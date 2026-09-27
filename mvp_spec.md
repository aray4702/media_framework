# Media Framework — MVP Spec (v0)

Source: [reqs.md](reqs.md). This spec defines the smallest version that works end to end and still exercises the core architecture: the C++ core, platform adapters, threads with queues, the state machine, A/V sync, seek/scrub, and the metrics. Everything else is under [Next steps](#7-next-steps-deferred-requirements).

---

## 1. MVP scope

| In scope                                                                                                                                    | Out of scope (deferred)                                                  |
| ------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| **One platform: Mac OS** (13 Ventura+, Apple silicon)                                                                                       | iOS, Web, Android, Windows, Linux                                        |
| Local file only, **MP4 container**                                                                                                          | Remote/streaming, other containers                                       |
| **Constant frame rate (CFR)** content. Timestamp rounding from the timescale (e.g. alternating 16/17 ms gaps at 60 fps) still counts as CFR | Variable frame rate (VFR)                                                |
| Samples play at their raw MP4 timestamps                                                                                                    | Edit lists, AAC priming trim (A20), track header matrix / rotation (A21) |
| Video **H.264**, audio **AAC-LC** (audio track optional)                                                                                    | HEVC/VP9/AV1, other audio codecs                                         |
| Hardware decoder chosen by the platform (default `VTDecompressionSession` for the codec)                                                    | Picking or ranking decoders by capability, software fallback             |
| APIs: `open`, `play`, `pause`, `seek`, `shutdown` + `duration`/`position` queries                                                           | Speed, volume, track selection, looping                                  |
| States: START, READY, PLAY, ERROR, SHUTDOWN                                                                                                 | Buffering state (not needed for local files)                             |
| Seek to the exact timestamp; scrub coalescing                                                                                               | Seek while in PLAY (A4)                                                  |
| Metrics: dropped-frame rate, jank rate, A/V offset, TTFF, seek latency, peak memory                                                         | Telemetry upload, dashboards                                             |
| Minimal demo app: `NSWindow` with a `CAMetalLayer`-backed view, open file, play/pause, seek bar                                             | Production UI                                                            |
| Composition (§2.4): up to 16 clips back to back, joined by a horizontal slide or a cut; one caption at the bottom; brightness and contrast  | Other transitions, per-clip effects, caption styling and positioning, picture-in-picture |

**Why Mac OS first:** most of the pipeline runs on C APIs that C++ calls directly (`VideoToolbox`, `CoreMedia`, `AudioToolbox`/`AudioUnit`), and only the demuxer and display need a thin Objective-C++ layer (`AVAssetReader`, `CAMetalLayer`). There's no device to deploy to, Instruments gives good profiling, and the adapters carry over almost unchanged to iOS. That keeps the MVP close to one language while the adapter boundaries stay real. The cost is that a Mac is not one of reqs.md's main device types and is far from constrained. §5 adds a constrained run, and A22 covers what the MVP can't prove.

---

## 2. Architecture

```text
            caller thread (owner)
                 │ open/play/pause/seek/shutdown
                 ▼
        ┌──────── Player (C++ core: state machine, clock, queues) ────────┐
        │                                                                 │
 T1 Source:  IDemuxer ─(video)─► [video pkt Q] ──► T2 VideoDecode: IVideoDecoder ──► [frame Q] ─┐
  (per clip,  (per track) ─(audio)─► [audio pkt Q] ─┐                                              ├─► TC Composition ──► [composed Q] ──► T3 VideoRender: IDisplay ──► presenter
   2 lanes)                                        └─► T4 Audio: IAudioDecoder ×2, mixer ──► ISpeaker ring ──► RT render callback
             each lane (clip i on lane i % 2) has its own packet queues, decoders and frame queue ─┘
        │                                                                 │
        └──────────── MasterClock = audio frames consumed | steady_clock ─┘
```

### 2.1 Core (portable C++17, no platform headers)

- `Player`: public API, state machine, owner-thread check, seek coalescing, and a serial number for each seek.
- **Stages** (`SourceStage`, `VideoDecodeStage`, `CompositionStage`, `VideoRenderStage`, `AudioStage`): each one is a **non-blocking** `pump()` step. It tries to pop, does its work, tries to push, and returns a `Progress` value. Stages never sleep or wait, and no adapter call they make may block. Waiting belongs to the scheduler (§2.3), so the same stage code runs on native threads and on the browser's event loop.
- `BoundedQueue<T>`: non-blocking `tryPush`/`tryPop`, `flush()`, and a hook that notifies the scheduler when an item or a free slot becomes available. The native thread scheduler guards it with a mutex. Each queue is capped by **count, bytes and duration**, whichever is reached first (A23; caps below).
- `MasterClock`: follows the audio clock when an audio track is playing, otherwise `std::chrono::steady_clock` (§4).
- `AvSync`: decides for each frame whether to present or drop, and when to present it (§4).
- `Metrics`: counters and histograms, fed with actual present times from the display. Dumped to the log on `shutdown` and exposed through a query.
- `TimelineLayout`: where each clip starts and ends on the timeline, which clip leads, and the slide offset and audio gain of a clip at a given time (§2.4).
- `Exporter`: the same pipeline with the export driver, writing to an `IExportSink` (§2.5).

**Why C++17.** reqs.md asks for the common logic in C++. The standard is set to C++17 because:

- **It's the newest standard that every target toolchain supports fully.** The core must build unchanged with Apple Clang (Mac OS, iOS), the Android NDK's Clang (API 26+), Emscripten (Web), MSVC (Windows) and GCC/Clang (Linux). C++20 support still differs between these compilers and their standard libraries, and older Apple and NDK targets lag behind.
- **It covers what the core needs:**
  - `std::optional` for the audio track, per-frame A/V samples and the pending seek.
  - Nested namespaces (`mf::macos`).
  - `std::shared_ptr<void>` with custom deleters, which passes platform handles through the core without exposing their types.
- **C++20 would be nice, not needed.**
  - `std::span` would suit PCM buffers.
  - `std::jthread`/`stop_token` would suit the scheduler.
  - Concepts would suit the adapter interfaces.
  - `std::atomic::wait` would suit some waits, but not on the real-time audio thread, which must never block anyway.

  None of these changes the design.
- **It doesn't push a standard onto embedders.** The public headers (`player.h`, `adapters.h`) need only C++17, so an app that embeds the player isn't forced onto a newer standard.

Revisit when the oldest supported NDK and Xcode versions ship complete C++20 libraries.

**Buffer caps (reqs guarantee 4):**

| Buffer                                  | Cap                                           |
| --------------------------------------- | --------------------------------------------- |
| Video packet queue (per lane)           | 60 packets, 32 MB, or 2 s                     |
| Audio packet queue (per lane)           | 120 packets, 1 MB, or 2 s                     |
| Decoded frame queue (per lane)          | 4 frames                                      |
| Composed frame queue                    | 4 frames (each holds 1 or 2 decoded frames)   |
| Decoder frames in flight (per lane)     | 4                                             |
| Reorder buffer (in the decoder adapter) | DPB size from the SPS (≤ 16; about 5 at 4K)   |
| Presenter + GPU                         | 1 waiting frame + 2 in use by command buffers |
| Audio PCM ring                          | 200 ms                                        |

Worst case this is about 80 MB at 1080p and about 230 MB at 4K for one lane. The second lane only fills during a transition or ahead of the next clip, and adds up to the same again. The decoder's pixel-buffer pool is sized to match, and each `CVPixelBuffer` is released when the command buffer that samples it completes.

### 2.2 Adapter interfaces (MVP implementations are Mac OS-only)

```cpp
struct Packet   { int track; int64_t ptsUs; bool key; std::vector<uint8_t> data; uint32_t serial; };
struct MediaInfo{ int64_t durationUs; TrackInfo video; std::optional<TrackInfo> audio; };

class IDemuxer {            // Mac OS: AVAssetReader, one passthrough track output per track (outputSettings = nil)
  virtual Result open(const MediaSource&, MediaInfo* out) = 0;
  virtual Result peekDtsUs(int track, int64_t* out) = 0;  // Ok | Eos; next packet's decode time
  virtual Result read(int track, Packet* out) = 0;        // Ok | Eos | error; tracks are read independently
  virtual Result seekTo(int64_t us, int64_t* keyPtsUs) = 0; // sync sample ≤ us via AVSampleCursor; recreates the reader (A13)
};
class IVideoDecoder {       // Mac OS: VTDecompressionSession, output to IOSurface-backed CVPixelBuffers
  virtual Result configure(const TrackInfo&, IDisplay*) = 0;
  virtual Result queue(const Packet&) = 0;                      // Ok | Again (4 frames in flight)
  virtual Result dequeue(VideoFrame* out) = 0;                  // Ok | Again; never blocks. opaque handle + ptsUs, PTS order
  virtual void   flush() = 0;                                   // never waits; bumps a generation (below)
};
class IDisplay {            // Mac OS: CAMetalLayer, drawn by a presenter queue (below)
  virtual void present(VideoFrame&, int64_t hostTimeNs) = 0;   // never blocks; latest frame wins
  virtual void drop(VideoFrame&) = 0;
  virtual bool visible() const = 0;                            // false when the window is occluded or minimized
  virtual int64_t vsyncPeriodNs() const = 0;                   // can change at run time (display switch)
  virtual void setPresentedListener(PresentedListener*) = 0;   // actual present time per frame, for Metrics
};
class IAudioDecoder { /* same shape as IVideoDecoder; output is S16 interleaved PCM. Mac OS: AudioConverter */ };
class ISpeaker {            // Mac OS: default-output AudioUnit, pulling from a lock-free SPSC ring
  virtual Result open(int sampleRate, int channels) = 0;
  virtual int  write(const int16_t*, int frames) = 0;   // frames accepted (0 = full); never blocks
  virtual Result start() = 0; virtual void pause() = 0;
  virtual void flush(int64_t nextPtsUs) = 0;             // producer side only; bumps the ring generation (§4)
  virtual std::optional<int64_t> presentedPtsUs() const = 0;  // PTS audible now; empty before the first callback
  virtual bool drained() const = 0;                       // every written frame has been played
};

enum class Progress { Did, NeedInput, NeedOutput, WaitUntil };   // WaitUntil carries a deadline
class IScheduler {          // Mac OS: 4 threads (§2.3). Web: event loop (§7.1)
  virtual void start(std::array<Stage*, 4>) = 0;
  virtual void wake(StageId) = 0;               // input arrived / output space freed / command
  virtual void stop() = 0;                      // joins or cancels; stages are idle afterwards
};
struct PlatformFactory {
  /* creates the five adapters above and the IScheduler, plus the opaque handles for open():
     Mac OS: MediaSource from a file URL (NSOpenPanel), RenderTarget from a CAMetalLayer*.
     Android later: MediaSource from a file descriptor. Web later: from a File. */
};
```

Frame formats: video frames stay **opaque, in the platform's surface buffers** (IOSurface-backed `CVPixelBuffer`s, mapped to Metal textures through `CVMetalTextureCache`), with no RGBA conversion. Audio is **S16 PCM**, which `AudioConverter` decodes to directly. See issue A7.

**Video decoder adapter (Mac OS).**

- VideoToolbox returns frames in decode order, so the adapter reorders them by PTS. The reorder depth comes from the SPS (`max_num_reorder_frames`, falling back to the DPB size), and the buffer is drained on EOS.
- `flush()` must not call `VTDecompressionSessionWaitForAsynchronousFrames`, which blocks. It bumps a generation number instead, and the output callback discards frames from older generations.
- The output callback calls `wake(VideoDecode)`, so T2 never polls.

**Display adapter (Mac OS).**

- `nextDrawable` can block for up to 1 s when drawables run out or the window is hidden. So `present()` hands the frame to a presenter, a serial dispatch queue that acquires the drawable, draws the frame aspect-fit, and calls `presentDrawable:atTime:`. `maximumDrawableCount` is 3.
- If the presenter is still busy, the waiting frame is replaced (latest wins) and the replaced frame counts as a late drop.
- The presenter registers `addPresentedHandler` on each drawable and reports the actual present time (`presentedTime`) to Metrics.
- When `visible()` is false, T3 drops frames without presenting them and counts them as hidden, which is excluded from the metrics.
- The adapter observes `NSWindowDidChangeScreenNotification` and republishes `vsyncPeriodNs()` from `NSScreen.minimumRefreshInterval`. T3 re-anchors its vsync slots when the period changes.
- The app sets `drawableSize` on the main thread when the view resizes. The presenter draws into whatever size it gets.

### 2.3 Threads: the Mac OS scheduler (as reqs.md specifies)

Threading is part of the platform scheduler, not the core contract. The MVP's Mac OS scheduler runs one thread per stage. T3 and T4 run at `QOS_CLASS_USER_INTERACTIVE`. Each thread runs `while (running) { p = stage.pump(); wait(p); }`, where `wait` does the following:

- `Did`: loop again straight away.
- `NeedInput` / `NeedOutput` on a queue: wait on the stage's CV until `wake()` is called.
- `NeedInput` / `NeedOutput` on the decoder: wait on the CV. The `VTDecompressionSession` output callback calls `wake()`, so the decoder needs no polling.
- `NeedInput` / `NeedOutput` on the speaker: do a timed CV wait of about 5 ms. The AudioUnit render callback runs on a real-time thread and must not take the CV's mutex, so the ring buffer is polled.
- `WaitUntil(t)`: timed CV wait until `t`. Pause, seek and shutdown call `wake()`, which ends the wait early.

| Thread          | Stage work                                                                                                                                                                                                                                                                                                                                       |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Caller          | API calls only. Every call returns without doing I/O or decoding. Only `shutdown` waits, to join the threads.                                                                                                                                                                                                                                    |
| T1 Source       | Probes and configures on `open`, handles pending seeks, and demuxes. Among the tracks whose queue has room, it reads from the one with the lowest next decode time. A full video queue therefore never stops audio from being read, so a badly interleaved file can't deadlock the pipeline. The parser stage is a pass-through in the MVP (A3). |
| T2 Video decode | For each lane: takes a packet, queues it into the lane's decoder, dequeues output in PTS order, and pushes it to the lane's frame queue.                                                                                                                                                                                                           |
| TC Composition  | Merges the two lanes' frames in timeline order and builds each `ComposedFrame`: layers, slide offsets, caption, filter. Picks the frame to show for an exact seek (§4).                                                                                                                                                                           |
| T3 Video render | Takes a composed frame, runs `AvSync`, then hands it to the presenter or drops it.                                                                                                                                                                                                                                                              |
| T4 Audio        | Takes packets from both lanes, decodes them, mixes them on the timeline (§2.4), and writes to the `ISpeaker` ring buffer. When the ring is full, the thread waits a few ms. The frames the render callback consumes drive the master clock.                                                                                                        |

On Mac OS, T2 is thin: VideoToolbox already decodes asynchronously, so T2 mostly moves packets in and frames out. reqs.md asks for one thread per stage, so it stays. Merging it into T1 is an option if reqs.md relaxes that rule.

**How the scheduler meets reqs.md's guarantees:**

1. Decode, render and I/O never run on the caller thread: `open` only validates. Probing, configuring and preroll run on T1–T3.
2. A slow stage doesn't block rendering of decoded frames: T3 depends only on the frame queue and the clock, and `present()` never blocks.
3. The real-time audio path never takes locks or allocates: the render callback only copies from the ring and publishes the clock through atomics. Ring memory is allocated in `open`.
4. Queue memory is bounded: every buffer has a count, byte or duration cap (§2.1).

Host unit tests use a **single-threaded manual scheduler** that calls `pump()` in a fixed order with a fake clock. This makes the tests for sync, seek and the state machine deterministic.

### 2.4 Composition: timeline, lanes and the composition stage

The player plays a **timeline**: up to 16 clips in order, a transition between each pair, captions and a filter.

```cpp
struct Timeline {
  std::vector<MediaSource> clips;   // 1 to 16, played in order
  Transition transition;            // Cut, SlideLeft or SlideRight, with durationUs (default 1 s)
  std::vector<TextOverlay> texts;   // {text, startUs, endUs} on the timeline; the first match is shown
  VideoFilter filter;               // brightness -1..1 (0 = none), contrast 0..2 (1 = none); live via setFilter
};
```

**Layout.** Clip `i+1` starts `T` before clip `i` ends, so the two overlap for the transition, and the timeline is `sum(durations) − (n−1)·T` long. `T` is capped at half the shortest clip. So at most two clips are active at any time, and clip `i+2` starts only after clip `i` ends. A cut is `T = 0`.

**Lanes.** Clip `i` always plays on **lane `i % 2`**. Each lane has its own packet queues, video and audio decoders and frame queue, so the incoming clip decodes alongside the outgoing one. T1 probes every clip on `open` (one demuxer each, for the durations and to fail early on unsupported media), then:

- **Seek to `t`:** the lane of the first clip active at `t` seeks into it, and the other lane seeks the next clip to `max(0, t − start)`.
- **End of a clip:** once a lane has read both tracks of clip `i` to the end, it moves on to clip `i+2` from 0.
- Packets carry their clip index. When a lane's next clip arrives, T2 waits for the previous clip to drain (its Eos), then reconfigures the decoder. VideoToolbox keeps the session when `VTDecompressionSessionCanAcceptFormatDescription` accepts the new format.

**Composition stage (TC).** It takes frames from both lanes in timeline order (`start[clip] + pts`). It moves on only when the other lane can't still deliver an earlier frame: that lane has a frame waiting, or it is still before its clip's start, or it has finished. For each frame:

- A frame past its clip's end is dropped: the next clip cuts it.
- A frame of the **outgoing** clip is kept as that lane's latest frame. It is shown under the leading clip's next frame.
- A frame of the **leading** clip (the latest one active at its time) becomes a `ComposedFrame`: the outgoing clip's latest frame and this one, each with its horizontal offset, plus the caption and the filter.

```cpp
struct ComposedFrame {
  int64_t ptsUs;  uint32_t serial;  bool eos;  int64_t frameDurationUs;   // of the leading clip
  int layerCount; struct { VideoFrame frame; float offsetX; } layers[2];  // outgoing first; offset in output widths
  std::shared_ptr<const std::string> text;  VideoFilter filter;
};
```

At transition progress `p = (t − start) / T`, slide-left puts the outgoing clip at `−p` and the incoming one at `1 − p`; slide-right mirrors this. Output frames follow the leading clip's frame rate, and AvSync paces them with its frame duration.

**Drawing.** `IDisplay::present` takes the `ComposedFrame` and draws it in one pass, so frames stay zero-copy: each layer from its decoder surface, aspect-fit and offset, with the filter applied in the fragment shader as `rgb' = (rgb − 0.5) · contrast + 0.5 + brightness`. The caption goes on top, centered at the bottom of the leading clip's picture and not sliding. On Mac OS it is rasterized once with Core Text into a texture and reused while the text and size stay the same. T3 stamps the latest filter onto each frame it presents, so a slider change shows on the next frame. While paused, it redraws the frame on screen.

**Audio.** T4 mixes on the timeline in 1024-frame chunks. Before mixing a chunk, every clip with audio in it must be decoded that far or have ended. Each clip is cut to its time on the timeline and scaled by a linear gain that fades in and out over the transitions. The two gains in an overlap sum to 1, so a slide is an equal-gain crossfade. Consecutive packets of a clip within 1 ms of each other are treated as one continuous stream, so timestamp rounding never doubles or drops a sample. The output format is that of the first clip with usable audio. Clips without audio, or with a different sample rate or channel count, play silent (`AudioUnsupported` warning for the latter; resampling is a next step). The master clock is the mixed audio, so it runs across clip boundaries.

### 2.5 Output drivers and export

The composition stage keeps, for each lane, the **latest frame at or before the output time** and composes the output at a time `t` with `composeAt(t)`: every active clip's frame with its offset at `t`, the caption at `t`, the current filter. Only the **driver** differs: it picks the output times and decides what happens when a layer's frame for `t` isn't decoded yet.

| Driver | Output times | A layer's frame isn't ready | Use |
| --- | --- | --- | --- |
| **LeadingClip** | Each frame of the leading clip: the highest frame rate among the active clips (the later clip on a tie) | Frames are handled in timeline order, so the other layers are always exact | `Auto` for one clip. Output = source frames, paced by AvSync |
| **Vsync** | Each display refresh: `t = clock(now) + (vsync − now)`, composed half a frame before the hand-over deadline | Hold its last frame and count a late layer; never wait | `Auto` for several clips. Slides and effects move every refresh |
| **Export** | Fixed grid `t = n / fps` | Wait until every layer has its exact frame | `Exporter` only |

- **Seek is the same for every driver.** The leading clip's last frame at or before the target (its first frame, if none is), with every other active clip's latest frame at that time. While a newer seek is pending, whatever is decoded is shown instead (scrub).
- **Vsync details.** The composed frame carries `presentAtNs`, the refresh it was composed for. T3 presents it for that refresh without AvSync, and drops it only if that refresh is more than a frame in the past (e.g. composed just before a pause). A refresh where nothing visible changes (same frames, offsets, caption and filter) produces no frame, so a 30 fps clip on a 60 Hz display presents 30 frames a second, not 60. While the clock holds (audio not heard yet), refreshes don't go back in time. TC composes only while output runs, so it is idle when paused.
- **Export** runs the same pipeline without a display or speaker. T3 writes composed frames and T4 the mixed audio chunks to an `IExportSink`, which returns `Again` while its encoder is busy (T3 and T4 then poll every 2 ms). Once both have written everything, the sink finishes the file, and `ExportListener::onCompleted` fires.

```cpp
class IExportSink {   // Mac OS: MetalCompositor into AVAssetWriter's BGRA buffers → H.264; PCM → AAC
  virtual Result open(const ExportTarget&, const ExportSettings&, int sampleRate, int channels) = 0;
  virtual Result writeVideo(const ComposedFrame&) = 0;                          // Ok | Again | WriteFailed
  virtual Result writeAudio(const int16_t* pcm, int frames, int64_t ptsUs) = 0;  // Ok | Again | WriteFailed
  virtual void finish(std::function<void(Result)> done) = 0;
};
class Exporter {       // same pipeline, driver = Export
  Result start(const Timeline&, const ExportTarget&, const ExportSettings&);  // width/height even, fps 1–240
  Result shutdown();   // cancels an unfinished file
  double progress() const;
};
```

On Mac OS, the display and the export sink draw with the same `MetalCompositor`, so an exported frame matches what playback shows. The sink encodes on one serial queue per track: `AVAssetWriter` holds one input back until the other catches up, so a single queue could wait on itself.

Callbacks run **on internal threads**. A callback must return quickly and **must never wait on the owner thread**, for example with `dispatch_sync` to the main queue or a lock the owner holds while calling the player. Otherwise `shutdown`, which joins those threads, deadlocks. The listener posts work to its own thread with `dispatch_async` (A11).

---

## 3. API and state machine

```cpp
class Player {
 public:
  static std::unique_ptr<Player> create(PlatformFactory&, PlayerListener*);  // binds owner thread
  Result open(const Timeline&, RenderTarget);  // validates and returns; probe, configure and preroll run on T1 (§2.4)
  Result open(MediaSource, RenderTarget);      // a timeline of one clip
  Result play();
  Result pause();
  Result seek(int64_t positionUs);    // sync validation; completion via onSeekCompleted
  Result shutdown();                  // sync: stops callbacks, wakes and joins threads, releases adapters
  Result setFilter(VideoFilter);      // any state before SHUTDOWN; InvalidArgument out of range
  State   state() const;
  int64_t durationUs() const;
  int64_t positionUs() const;         // = master clock in PLAY, else the PTS of the frame on screen
};
struct PlayerListener {
  virtual void onStateChanged(State) = 0;
  virtual void onError(Result code, const std::string& reason) = 0;     // fatal, async
  virtual void onWarning(Warning code, const std::string& reason) = 0;  // playback continues
  virtual void onFirstFrame() = 0;                  // after open (TTFF)
  virtual void onSeekCompleted(int64_t shownPtsUs) = 0;
  virtual void onEnded() = 0;
};
```

`Result` codes: `Ok, InvalidState, WrongThread, InvalidArgument, FileOpenFailed, UnsupportedFormat, NoDecoder, MalformedMedia, DecoderFailed, AudioDeviceFailed`. `open` itself returns only `Ok`, `InvalidState`, `WrongThread` or `InvalidArgument` (no clips or more than 16, a negative transition, a caption with `endUs <= startUs`, or a filter out of range). Probe and decode failures arrive through `onError`.

`Warning` codes: `AudioUnsupported` (video plays alone, A17), `RotationIgnored` (non-identity track matrix, A21).

### Transitions (MVP)

| From                   | Call or event                              | To            | Notes                                                                                                                                          |
| ---------------------- | ------------------------------------------ | ------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| START                  | `open`                                     | START         | Returns `Ok` and starts probing on T1. `play` and `seek` return `InvalidState` until READY                                                     |
| START                  | first frame shown / probe or preroll fails | READY / ERROR | READY fires after preroll (internal `seek(0)`) shows the first frame, with `onFirstFrame`. On failure: `onStateChanged(ERROR)`, then `onError` |
| READY                  | `play` ok / fails                          | PLAY / ERROR  | Fails if the audio device won't start                                                                                                          |
| READY                  | `seek(t)`                                  | READY         | Coalesced. Shows the frame at `t` (§4)                                                                                                         |
| READY                  | async fatal error                          | ERROR         | E.g. a decoder failure during a seek. Fires `onError`                                                                                          |
| PLAY                   | `pause`                                    | READY         | Audio pauses. The clock freezes                                                                                                                |
| PLAY                   | end of stream                              | READY         | Both tracks have finished. Fires `onEnded`, sets the `ended` flag, position = duration (A5)                                                    |
| PLAY                   | async fatal error                          | ERROR         | Fires `onError`                                                                                                                                |
| START/READY/PLAY/ERROR | `shutdown`                                 | SHUTDOWN      | From START, this cancels any probe in progress                                                                                                 |
| SHUTDOWN               | `shutdown`                                 | SHUTDOWN      | Returns `Ok`: idempotent (A6)                                                                                                                  |
| any other              | any                                        | unchanged     | Returns `InvalidState`. A call from a non-owner thread returns `WrongThread`                                                                   |

`play()` with the `ended` flag set restarts at 0. Any `seek` clears the flag, so end of stream → `seek(t)` → `play()` plays from `t`.

Seek in PLAY is **rejected** in the MVP, following reqs.md literally. To scrub, the app calls `pause → seek* → play` (A4).

---

## 4. Sync, render and seek algorithms

**Master clock**

- **With audio.** The render callback publishes a snapshot through a seqlock each time it runs:
  - `generation`, which changes on each ring flush.
  - `framesBefore`: content frames consumed before this buffer.
  - `n`: content frames in this buffer. Silence inserted on underrun is not counted.
  - `audibleHostTime`: when the buffer's first frame reaches the speaker. This is the callback's `mHostTime` plus the device output latency (`kAudioDevicePropertyLatency` + safety offset + stream latency + buffer size).
- The clock is then `clock(now) = basePts + (framesBefore + clamp((now − audibleHostTime) × rate, 0, n)) / rate`, where `basePts` is the PTS passed to the last `flush`.
  - The clamp stops the clock during an underrun instead of letting it run ahead of the audio actually heard.
  - The value includes output latency.
- **Before the first callback of a generation,** the clock holds at `basePts`. T3 keeps the first frame on screen and waits.
- **When the audio track ends** (the speaker reports `drained()` after the last packet) or audio is disabled (A17), the clock switches to `steady_clock` and continues from the last audio value. Video therefore keeps playing when audio is shorter than video.
- **Without audio:** `clock = basePts + (steady_now − baseTime)`. `basePts` and `baseTime` are reset on play and on seek. The clock freezes on pause.

**Audio ring flush.** Only the consumer may move the read index of a single-producer, single-consumer ring.

1. T4 stops writing.
2. T4 publishes `{generation + 1, writeIndexAtFlush, basePts}`.
3. T4 resumes writing data for the new generation.
4. At the start of each callback, the render callback checks the generation. If it changed, it sets `readIndex = writeIndexAtFlush`, which discards the old data, and resets its frame counts.

**Video render (T3), per frame**

1. `delta = frame.pts − clock.now()`.
2. If `delta < −frameDuration`, the frame is late: **drop** it and count it as dropped.
3. Compute the frame's vsync slot: `slot = round((target − anchor) / vsyncPeriod)`. `anchor` is the first presented target after play, seek or a vsync-period change. If `slot == lastSlot`, the frame rate is above the refresh rate: **drop** it and count it as rate-capped, not dropped (A9). Otherwise set `lastSlot = slot`. Comparing slots instead of raw time gaps keeps timestamp jitter from causing drops (A19).
4. If `delta > 1 vsync`, return `WaitUntil(now + delta − 1 vsync)` and keep the frame. The scheduler wakes the stage then, or earlier on pause, seek or shutdown.
5. Otherwise, **present** it at host time `now + delta`. The presenter calls `presentDrawable:atTime:`, and Core Animation aligns the frame to vsync.

**Seek / scrub** (serial-number pattern)

1. The caller thread validates the state and clamps `t` to `[0, duration]`. It writes `t` into a **single-slot** `pendingSeek`, overwriting any earlier value, clears `ended`, wakes T1 and returns `Ok`.
2. A seek is *in flight* while `shownSerial != serial`. T1 starts a pending seek only when none is in flight. It then:
   1. does `++serial`;
   2. flushes the packet queues;
   3. signals T2, TC, T3 and T4, which flush their decoders, the frame queues and the speaker on their own threads without waiting;
   4. calls `seekTo` on the demuxers of the clips around `t` (§2.4), which land on keyframes `k ≤ t`;
   5. pushes packets tagged with the new serial and `targetPts = t`.
3. Downstream threads drop any item with a stale serial.
4. **Exact seek.** Frames with `pts < t` are decoded but not shown, and are counted as decode-only. TC keeps the latest composed frame with timeline `pts ≤ t` and passes it on once the next one is past `t`, or at end of stream. T3 shows the first frame of the new serial. T4 starts mixing at `t`, trimming inside the first packet.
5. **Scrub.** If a newer seek is already pending when TC composes the in-flight seek's keyframe, TC passes it on right away and T3 completes that seek without decoding on to `t`. During a fast scrub, a new frame therefore appears about once per keyframe-seek latency. The last seek always runs as an exact seek, so the final position is exact, and only the latest request is honored.
6. On completion, T3 sets `shownSerial = serial`, fires `onSeekCompleted(shownPts)`, and wakes T1.

**Errors**

- **Fatal** (state becomes ERROR, `onError` fires, threads stop):
  - file fails to open;
  - unsupported container;
  - no decoder for the video codec;
  - video decoder fails to configure, reports an unrecoverable error, or is invalidated a second time;
  - audio device fails;
  - malformed media (A8).
- **Recovered:** if the VideoToolbox session is invalidated (`kVTInvalidSessionErr`, e.g. after sleep or a GPU switch), the adapter recreates it once and resumes at the next keyframe.
- **Warnings** (`onWarning`, playback continues): an unsupported audio codec plays video alone on the system clock (A17). A non-identity track matrix is ignored (A21).
- **Per-frame errors:**
  - Video: when VideoToolbox reports an error status or `kVTDecodeInfo_FrameDropped`, hold the last good frame and skip packets up to the next keyframe.
  - Audio: on a decode error, output silence for that packet's duration.
  - Both are counted as corrupt skips, which are reported separately and excluded from the dropped-frame rate.
  - VideoToolbox conceals many corruptions without reporting them. Those frames show artifacts and aren't counted (A14).

---

## 5. Metrics and acceptance criteria

**Reference machine:** a MacBook Air M1 (8 GB) running Mac OS 13 or later, using its built-in 60 Hz display. An external monitor adds its own input lag, which no API reports, and that would count against the A/V target. Every target is checked in two runs:

- **Normal:** on mains power.
- **Constrained:** on battery, with Low Power Mode on and 4 busy background CPU threads.

Passing on this machine doesn't show that the targets hold on Android, iOS or Web devices (A22).

| Metric                | Definition (MVP)                                                                                                                                                                                                                                                                                                                       | Target                                                                 |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------- |
| Dropped-frame rate    | late drops / (presented + late drops). Excludes rate-cap drops, seek flushes, decode-only frames, hidden frames and corrupt skips                                                                                                                                                                                                      | < 1%                                                                   |
| Jank rate             | From actual present times (`presentedTime`). An interval is jank when it is at least one vsync longer than planned: `round(actual interval / vsync) > planned slot difference`. When the content fps equals the refresh rate, this matches reqs.md's "> 1.5 × expected interval". Excludes the first frame after play and after a seek | < 1%                                                                   |
| A/V offset            | `frame.pts − audioClock(actual present time)` for each presented frame. Reported as the mean and p95 of the absolute value                                                                                                                                                                                                             | p95 of absolute offset < 40 ms                                         |
| End-to-end A/V offset | On the flash/beep sync clip, the time between the flash on screen and the beep at the speaker, measured with a camera and mic or a light sensor. This catches content offsets such as AAC priming, which the pipeline's own A/V offset metric can't see (A20)                                                                          | Absolute offset < 40 ms                                                |
| TTFF                  | `open()` called → first frame actually presented                                                                                                                                                                                                                                                                                       | < 500 ms (A10)                                                         |
| Seek latency          | `seek()` called → `onSeekCompleted` with the frame at `t` presented. Reported as p50/p95                                                                                                                                                                                                                                               | p95 < 100 ms at GOP ≤ 1 s, up to 1080p. 4K and longer GOPs best effort |
| Scrub                 | 50 seeks in 2 s. A frame appears at least every 200 ms during the scrub. The last target is shown exactly; no crash or leak                                                                                                                                                                                                            | Last target shown ≤ 150 ms after the last call                         |
| Peak memory           | Resident memory during 4K playback and the scrub test                                                                                                                                                                                                                                                                                  | Within the §2.1 budget + 50 MB                                         |

The scrub target allows for one keyframe seek already in flight plus the final exact seek. reqs.md's 100 ms for scrubbing is a best-effort goal.

**Test clips:** all CFR, unrotated (identity track matrix), with no edit list.

- 1080p30, 1080p60, 720p24 and 4K30 (H.264 + AAC). The 1080p30 clip also checks the jank boundary: one missed vsync there is exactly 1.5 × the frame interval.
- A flash/beep A/V sync clip, 1080p30 + AAC 48 kHz.
- 1080p60 in a 1000 Hz timescale (jittered PTS; expect 0 rate-cap drops).
- 120 fps (expect ~50% rate-capped, 0% dropped).
- A video-only clip.
- A clip whose audio ends 5 s before the video.
- A non-interleaved clip (all audio samples after all video samples).
- A 4 s GOP clip (seek is best effort).
- A clip with a truncated tail, a clip with corrupted mdat bytes, and a clip whose header is fuzzed.

The 4K30 clip checks behavior and memory only and is not held to the timing targets.

**Tests:**

- Core unit tests run on the host with fake adapters and the manual scheduler. They cover:
  - the state table, including async `open` and errors in READY;
  - queue caps and flush;
  - per-track reading with no deadlock on the non-interleaved clip;
  - seek coalescing, exact seek and scrub keyframe completion;
  - AvSync decisions, including a vsync-period change;
  - the clock: holding during underrun, the handoff to `steady_clock` at audio end, and ring flush generations.
- Integration tests (XCTest) run on the reference machine and check the targets above in both runs.

---

## 6. Milestones

1. **M1:** Core skeleton, state machine with async `open`, non-blocking stages, queues with count/byte/duration caps, per-track source reading, the Mac OS thread scheduler, the display presenter and the manual test scheduler. Video-only playback on the system clock.
2. **M2:** Audio path, ring flush generations, the audio master clock (underrun hold, handoff at audio end), and the A/V offset metric from actual present times.
3. **M3:** Exact seek, scrub coalescing with keyframe completion, preroll on open, end of stream for both tracks. Measure the cost of recreating the `AVAssetReader` (A13).
4. **M4:** Errors, warnings, recovery from session invalidation, the full metrics set, test clips, and acceptance in the normal and constrained runs.

Repo layout: `core/` (C++ plus host tests), `platform/macos/` (adapters in C++ and Objective-C++), `apps/macos-demo/`.

---

## 7. Next steps (deferred requirements)

- **iOS:** reuses the Mac OS adapters (`AVAssetReader`, VideoToolbox, AudioUnit via RemoteIO, `CAMetalLayer`).
  - New work: `AVAudioSession` setup and interruptions.
  - `AVSampleCursor` needs iOS 16. For iOS 13–15, find the sync sample with the portable demuxer or by reading forward from an earlier reader.
  - Repeat acceptance on the oldest device the iOS 13 floor allows (A22).
- **Android:** `AMediaExtractor`, `AMediaCodec`, AAudio (or Oboe on API 26/27), and `ANativeWindow` via `releaseOutputBufferAtTime`, behind a thin JNI shim.
  - Use one `AMediaExtractor` per track. A single cursor would bring back the interleaving stall.
  - Reuses the native thread scheduler. The decoder is polled, because synchronous-mode `AMediaCodec` has no notification below API 28.
  - `MediaSource` wraps a file descriptor.
- **Web:** WASM core with WebCodecs, AudioWorklet and OffscreenCanvas/WebGL, driven by an event-loop scheduler (§7.1). Add a portable demuxer at this point, e.g. libavformat or a hardened custom MP4 parser.
- **Other desktops** (Windows, Linux), per reqs.md. These reuse the native thread scheduler and core unchanged. Only the adapters are new:
  - Windows: Media Foundation / D3D11, WASAPI
  - Linux: VA-API, PipeWire/ALSA
- **Mac OS:** variable refresh (ProMotion) and real vsync phase through `CADisplayLink` on Mac OS 14+.
- VFR content, edit lists, AAC priming trim, and track-matrix rotation (A20, A21).
- Seek while in PLAY (A4).
- Decoder capability probing, ranking, and software fallback. More codecs and containers.
- Plugin mechanism (v2), other media types, remote/streaming sources, DRM, background play, lifecycle integration. These are all listed as non-goals in reqs.md.
- An optional RGBA/S16 "canvas" path for effects and compositing graphs.

### 7.1 Web threading model (next step)

reqs.md already sets the Web scheduler: one worker's event loop runs the stages, and an AudioWorklet plays the audio. These are the points the native design doesn't cover:

- **Why not threads:** WebCodecs delivers frames through callbacks on the event loop. A worker blocked on a CV never returns to its event loop, so those callbacks never fire.
- **Mapping:**
  - Caller: the main thread, with a Promise-based JS facade over `postMessage`. `play()` must be called from a user gesture.
  - T1: a worker task that reads a `File` with `FileReaderSync` and demuxes in WASM.
  - T2 and T4 decode: WebCodecs `VideoDecoder` and `AudioDecoder` callbacks.
  - T3: a `requestAnimationFrame` loop on `OffscreenCanvas` that draws the latest frame with `pts ≤ clock(displayTime)`.
  - Audio: a single-producer, single-consumer ring in `SharedArrayBuffer`, pulled by the AudioWorklet.
- **Clock:** the worklet publishes `framesConsumed`, and the clock subtracts `outputLatency` (falling back to `baseLatency` plus a constant tuned per browser).
- **Frame budget:** `close()` every `VideoFrame` right away. Holding more than about 3–4 frames stalls the hardware decoder.
- **Risks and fallbacks:**
  - No COOP/COEP headers: transfer PCM to the worklet by `postMessage`.
  - Safari gaps in `OffscreenCanvas` or `AudioDecoder`: render on the main thread, or decode AAC in WASM.
  - A decode error closes the decoder: call `reset()` and `configure()`, then resume at the next keyframe.
  - A hidden tab stops rAF: pause on `visibilitychange`.

---

## 8. Issues found in reqs.md (with proposals)

| #   | Type                   | Issue                                                                                                                                                                                                                                                                                                                                                                                                                                     | Why it matters                                                                                                  | Proposal (MVP decision in **bold**)                                                                                                                                                                                                                                                                                                            |
| --- | ---------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1  | Unbounded              | "Video" names no container or codec.                                                                                                                                                                                                                                                                                                                                                                                                      | The decoder and demuxer matrix is open-ended.                                                                   | **MP4 + H.264 + AAC-LC only**, then extend.                                                                                                                                                                                                                                                                                                    |
| A2  | Gap                    | The demuxer is in the pipeline but is not one of the adapters (only decoder, speaker and display are). Writing a C++ MP4 demuxer is a lot of work and adds security risk.                                                                                                                                                                                                                                                                 | A robust demuxer is a project in its own right. The "malicious file" requirement makes a hand-rolled one risky. | **Add an** `IDemuxer` **adapter. MVP uses** `AVAssetReader`**,** Apple's maintained and hardened MP4 parser, inside the App Sandbox. It parses in-process, so it's less isolated than an out-of-process demuxer. Tracks are read independently (§2.3). Choose a portable demuxer when Web is added.                                            |
| A3  | Unnecessary complexity | The parser stage. With MP4 plus platform decoders, codec config goes in through the format (avcC), so no parsing is needed.                                                                                                                                                                                                                                                                                                               | A stage that does nothing still costs a thread and a queue.                                                     | **Pass-through in the MVP**, run on T1. `AVAssetReader` passes the avcC through in the `CMVideoFormatDescription`. Keep the slot for elementary streams and codecs that need it.                                                                                                                                                               |
| A4  | Inconsistency          | Seek is allowed only from READY, yet scrubbing and "jump to a time" happen naturally during playback.                                                                                                                                                                                                                                                                                                                                     | Every player UI would then have to do pause, seek, play itself.                                                 | **MVP follows the table.** Next step: allow `seek` in PLAY, staying in PLAY.                                                                                                                                                                                                                                                                   |
| A5  | Gap                    | No end-of-stream behavior or state.                                                                                                                                                                                                                                                                                                                                                                                                       | PLAY with nothing left to play is undefined.                                                                    | **End of stream is when both tracks have finished: PLAY → READY and fire** `onEnded`. `play()` from end of stream restarts at 0 unless a seek came first.                                                                                                                                                                                      |
| A6  | Gap                    | `shutdown` from SHUTDOWN is "rejected". No mention of freeing resources on ERROR.                                                                                                                                                                                                                                                                                                                                                         | Cleanup code in callers becomes fragile.                                                                        | **Make** `shutdown` **idempotent.** ERROR → `shutdown` is the only way out.                                                                                                                                                                                                                                                                    |
| A7  | Conflict               | Internal RGBA/S16 frames conflict with "make best use of hardware". A GPU→CPU copy and color conversion of 4K60 video costs about 2 GB/s.                                                                                                                                                                                                                                                                                                 | Hurts the jank target and battery.                                                                              | **Keep video frames opaque, in platform surfaces (zero-copy: IOSurface-backed** `CVPixelBuffer` **→ Metal texture).** Audio uses S16. Treat RGBA as an optional path for later.                                                                                                                                                                |
| A8  | Unbounded              | "Malicious file that allocates out-of-bound memory" can't be detected once it has happened.                                                                                                                                                                                                                                                                                                                                               | It's a security property, not an error you can detect.                                                          | Treat it as `MalformedMedia`. Validate sizes against file size and limits (max sample 16 MB, max 8K resolution). Rely on Apple's parser and the App Sandbox (A2). Fuzz-test later.                                                                                                                                                             |
| A9  | Inconsistency          | Capping fps at the refresh rate means dropping frames on purpose, and those drops would count as "dropped frames" and hurt the metric. The dropped-frame rate also has no denominator or target.                                                                                                                                                                                                                                          | The metric could fail even when behavior is correct.                                                            | **Count rate-cap drops separately.** Dropped rate = late drops / (presented + late drops), target < 1%.                                                                                                                                                                                                                                        |
| A10 | Missing / unbounded    | "Fast TTFF" has no number and no start or end point. The jank "expected interval" is ambiguous when content fps ≠ refresh rate, and "> 1.5×" falls exactly on a common interval (one missed vsync at 30 fps on 60 Hz). Where the 100 ms seek target is measured from is not defined. Seeking "at the timestamp" means decoding from the keyframe to the target, so its cost grows with GOP length and resolution, and GOP is not bounded. | Targets that can't be checked can't be accepted.                                                                | Use the definitions in §5. **TTFF < 500 ms. Jank counted in vsync slots against the planned cadence. Seek measured from** `seek()` **call to the target frame shown: p95 < 100 ms at GOP ≤ 1 s up to 1080p.** 4K and longer GOPs are best effort. While scrubbing, an in-flight seek stops at its keyframe.                                    |
| A11 | Gap                    | API calls are limited to the owner thread, but nothing says which thread callbacks run on. Nothing says whether APIs are sync or async.                                                                                                                                                                                                                                                                                                   | Deadlocks, or `WrongThread` errors inside callbacks.                                                            | **Callbacks run on internal threads. They must return quickly and never wait on the owner thread.** Every API returns without I/O or decoding (`open` completes through callbacks). Only `shutdown` waits, to join threads. `seek` is validated sync and completes async. Next step: post callbacks to the owner's dispatch queue or run loop. |
| A12 | Resolved in reqs.md    | The original fixed 4-thread mutex/CV model didn't carry over to Web, and native audio callbacks run on real-time threads that must not lock.                                                                                                                                                                                                                                                                                              | —                                                                                                               | reqs.md now requires non-blocking stages driven by a per-platform scheduler, with four guarantees. The spec follows it (§2.1–2.3, §7.1). On Mac OS, T4 writes to a lock-free ring, and the render callback never locks or allocates.                                                                                                           |
| A13 | Platform risk          | `AVAssetReader` can't seek in place, so each seek builds a new reader, and in passthrough mode a reader started at an arbitrary time may begin on a frame that isn't a keyframe. `nextDrawable` can block for up to 1 s. ProMotion displays vary their refresh rate, and moving the window changes it. `CVDisplayLink` is deprecated in Mac OS 15, and its replacements need Mac OS 14.                                                   | Seek latency, non-blocking stages and the fixed-vsync slot rule are at risk.                                    | **Find the sync sample with** `AVSampleCursor` **and start the new reader there. Measure the cost in M3 against the seek target. Present from a separate presenter queue (§2.2). Re-read the vsync period on screen changes and measure on a fixed 60 Hz display.** Next step: `CADisplayLink` on Mac OS 14+, and variable refresh.            |
| A14 | Ambiguous              | "Placeholder" for corrupted frames is undefined. H.264 errors spread until the next keyframe.                                                                                                                                                                                                                                                                                                                                             | Behavior would differ from one implementation to the next.                                                      | **Video holds the last good frame until the next keyframe. Audio outputs silence.** Skips are counted separately from drops. Only errors the decoder reports are detected.                                                                                                                                                                     |
| A15 | Gap                    | Missing APIs needed for a usable player: render target (surface), duration and position. A file path isn't a portable input: Android uses content URIs and file descriptors, and Web uses `File` objects.                                                                                                                                                                                                                                 | A seek bar can't be built without them, and a path-based `open` won't port.                                     | **`open` takes a** `MediaSource` **and a** `RenderTarget` **(opaque handles made by the platform factory), plus** `durationUs()` **and** `positionUs()`**.**                                                                                                                                                                                   |
| A16 | Ambiguous              | Not stated whether READY after `open` shows a frame.                                                                                                                                                                                                                                                                                                                                                                                      | Affects TTFF and what the user sees.                                                                            | **Preroll: READY is entered once the first frame is shown.** This reuses the seek path.                                                                                                                                                                                                                                                        |
| A17 | Gap                    | The fatal error list leaves out decoder runtime failure, audio device failure and unsupported audio codec.                                                                                                                                                                                                                                                                                                                                | Unhandled error paths.                                                                                          | Decoder and device failures are **fatal**. An **unsupported audio codec plays video only**, driven by the system clock, and fires `onWarning(AudioUnsupported)`.                                                                                                                                                                               |
| A18 | Minor                  | The A/V threshold is symmetric (±40 ms), but perception is asymmetric (ITU-R BT.1359: about +45 / −125 ms).                                                                                                                                                                                                                                                                                                                               | The target is stricter than it needs to be.                                                                     | **Keep ±40 ms at p95** for the MVP. It's achievable with an audio master clock.                                                                                                                                                                                                                                                                |
| A19 | Gap                    | Nothing says how to decide that a frame is above the refresh rate. A naive `target − lastTarget < vsyncPeriod` check fails two ways. With 60 fps content in a millisecond timescale (gaps of 16/17 ms) on a 60 Hz display, it drops about ⅓ of frames. With 90 fps content, it shows only 45 fps.                                                                                                                                         | This would break the most common case (60 on 60) and under-use the display.                                     | **Quantize targets to vsync slots and drop only when a frame falls in the same slot as the previous one (§4 step 3).** Anchor error is at most half a vsync, which changes only which frame of a pair is shown. Next step: anchor to the real vsync phase from `CADisplayLink` (Mac OS 14+).                                                   |
| A20 | Conflict               | reqs.md makes AAC priming (and edit lists) a no-goal, but priming affects the ±40 ms A/V target. AAC files start with about 1024–2112 encoder-delay samples, roughly 21–44 ms at 48 kHz. Without trimming, audio content arrives late by that much. The pipeline's A/V offset metric compares clock times, so it can't see this offset.                                                                                                   | The target could pass on paper while real sync is off by up to 44 ms.                                           | **Keep it a no-goal, but add the end-to-end sync-clip test (§5).** In M2, if the measured offset is over 15 ms, shift audio timestamps by the encoder delay. This is a small, local change, not full edit-list support.                                                                                                                        |
| A21 | Risk                   | reqs.md makes the track header matrix a no-goal, but most phone-recorded video carries a 90° rotation in that matrix.                                                                                                                                                                                                                                                                                                                     | Those files will play sideways. That's acceptable for the MVP, but it should be known.                          | **Test clips are unrotated. The player fires** `onWarning(RotationIgnored)` **when the matrix isn't identity, and the demo app shows it.** Next step: pass the track's `preferredTransform` to `IDisplay` and apply it in the Metal render pass.                                                                                               |
| A22 | Unbounded              | reqs.md asks for "performance under constraint" on "different devices" but names no device class, power state or background load.                                                                                                                                                                                                                                                                                                         | Targets met on a fast desktop say little about phones or browsers.                                              | **MVP adds a constrained run (battery, Low Power Mode, background CPU load, §5).** Each port repeats acceptance on its weakest supported device, starting with the oldest device the iOS 13 floor allows.                                                                                                                                      |
| A23 | Unbounded              | "Queue memory is bounded" gives no budget, and packets and frames vary a lot in size (up to 16 MB per sample, about 12 MB per 4K frame).                                                                                                                                                                                                                                                                                                  | Counting items alone lets 60 large packets take about 1 GB.                                                     | **Cap every queue by count, bytes and duration, with a stated worst case (§2.1)**, checked by the peak-memory metric.                                                                                                                                                                                                                          |
