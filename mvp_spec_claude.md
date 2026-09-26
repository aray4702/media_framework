# Media Framework — MVP Spec (v0)

Source: [reqs.md](reqs.md). This spec defines the smallest version that works end to end and still exercises the core architecture: the C++ core, platform adapters, threads with queues, the state machine, A/V sync, seek/scrub, and the metrics. Everything else is under [Next steps](#7-next-steps).

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
| Keyframe seek, scrub coalescing                                                                                                             | Frame-accurate seek                                                      |
| Metrics: dropped-frame rate, jank rate, A/V offset, TTFF, seek latency                                                                      | Telemetry upload, dashboards                                             |
| Minimal demo app: `NSWindow` with a `CAMetalLayer`-backed view, open file, play/pause, seek bar                                             | Production UI                                                            |


**Why Mac OS first:** most of the pipeline runs on C APIs that C++calls directly (`VideoToolbox`, `CoreMedia`, `AudioToolbox`/`AudioUnit`), and only the demuxer and display need a thin Objective-C++ layer (`AVAssetReader`, `CAMetalLayer`). There's no device to deploy to, Instruments gives good profiling, and the adapters carry over almost unchanged to iOS. That keeps the MVP close to one language while the adapter boundaries stay real.

---

## 2. Architecture

```
            caller thread (owner)                      
                 │ open/play/pause/seek/shutdown        
                 ▼                                      
        ┌──────── Player (C++ core: state machine, clock, queues) ────────┐
        │                                                                 │
 T1 Source:  IDemuxer ──► [video pkt Q] ──► T2 VideoDecode: IVideoDecoder ──► [frame Q] ──► T3 VideoRender: IDisplay
                     └──► [audio pkt Q] ──► T4 Audio: IAudioDecoder ──► ISpeaker (blocking write)
        │                                                                 │
        └──────────── MasterClock = ISpeaker position | steady_clock ─────┘
```

### 2.1 Core (portable C++17, no platform headers)

- `Player`: public API, state machine, owner-thread check, seek coalescing, and a serial number for each seek.
- **Stages** (`SourceStage`, `VideoDecodeStage`, `VideoRenderStage`, `AudioStage`): each one is a **non-blocking** `pump()` step. It tries to pop, does its work, tries to push, and returns a `Progress` value. Stages never sleep or wait. Waiting belongs to the scheduler (§2.3), so the same stage code runs on native threads and on the browser's event loop (A12).
- `BoundedQueue<T>`: fixed capacity, non-blocking `tryPush`/`tryPop`, `flush()`, and a hook that notifies the scheduler when an item or a free slot becomes available. The native thread scheduler guards it with a mutex. Capacities: video packets 60, audio packets 120, decoded video frames 4.
- `MasterClock`: follows the audio clock when an audio track exists, otherwise `std::chrono::steady_clock`.
- `AvSync`: decides for each frame whether to present or drop, and when to present it (§4).
- `Metrics`: counters and histograms. Dumped to the log on `shutdown` and exposed through a query.

### 2.2 Adapter interfaces (MVP implementations are Mac OS-only)

```cpp
struct Packet   { int track; int64_t ptsUs; bool key; std::vector<uint8_t> data; uint32_t serial; };
struct MediaInfo{ int64_t durationUs; TrackInfo video; std::optional<TrackInfo> audio; };

class IDemuxer {            // Mac OS: AVAssetReader, passthrough track outputs (outputSettings = nil)
  virtual Result open(const std::string& path, MediaInfo* out) = 0;
  virtual Result read(Packet* out) = 0;             // Ok | Eos | error
  virtual Result seekTo(int64_t us) = 0;            // previous sync sample; recreates the reader (A13)
};
class IVideoDecoder {       // Mac OS: VTDecompressionSession, output to IOSurface-backed CVPixelBuffers
  virtual Result configure(const TrackInfo&, IDisplay*) = 0;
  virtual Result queue(const Packet&) = 0;                      // Ok | Again (too many frames in flight)
  virtual Result dequeue(VideoFrame* out) = 0;                  // Ok | Again; never blocks. opaque handle + ptsUs, PTS order
  virtual void   flush() = 0;
};
class IDisplay {            // Mac OS: CAMetalLayer via presentDrawable:atTime:
  virtual void present(VideoFrame&, int64_t systemTimeNs) = 0;
  virtual void drop(VideoFrame&) = 0;
  virtual int64_t vsyncPeriodNs() const = 0;
};
class IAudioDecoder { /* same shape as IVideoDecoder; output is S16 interleaved PCM. Mac OS: AudioConverter */ };
class ISpeaker {            // Mac OS: default-output AudioUnit, pulling from a lock-free SPSC ring
  virtual Result open(int sampleRate, int channels) = 0;
  virtual int  write(const int16_t*, int frames) = 0;   // frames accepted (0 = full); never blocks
  virtual void start() = 0; virtual void pause() = 0; virtual void flush() = 0;
  virtual int64_t presentedPtsUs() const = 0;   // PTS audible now; includes output latency
};

enum class Progress { Did, NeedInput, NeedOutput, WaitUntil };   // WaitUntil carries a deadline
class IScheduler {          // Mac OS: 4 threads (§2.3). Web: event loop (§7.1)
  virtual void start(std::array<Stage*, 4>) = 0;
  virtual void wake(StageId) = 0;               // input arrived / output space freed / command
  virtual void stop() = 0;                      // joins or cancels; stages are idle afterwards
};
struct PlatformFactory { /* creates the five adapters above and the IScheduler */ };
```

Frame formats: video frames stay **opaque, in the platform's surface buffers** (IOSurface-backed `CVPixelBuffer`s, mapped to Metal textures through `CVMetalTextureCache`), with no RGBA conversion. Audio is **S16 PCM**, which `AudioConverter` decodes to directly. See issue A7.

VideoToolbox returns frames in decode order, so the video decoder adapter reorders them by PTS. The reorder depth comes from the SPS (`max_num_reorder_frames`, falling back to the DPB size), and the buffer is drained on EOS and cleared on flush.

### 2.3 Threads: the Mac OS scheduler (as reqs.md specifies)

Threading is part of the platform scheduler, not the core contract. The MVP's Mac OS scheduler runs one thread per stage. T3 and T4 run at `QOS_CLASS_USER_INTERACTIVE`. Each thread runs `while (running) { p = stage.pump(); wait(p); }`, where `wait` does the following:

- `Did`: loop again straight away.
- `NeedInput` / `NeedOutput` on a queue: wait on the stage's CV until `wake()` is called.
- `NeedInput` / `NeedOutput` on the decoder: wait on the CV. The `VTDecompressionSession` output callback calls `wake()`, so the decoder needs no polling.
- `NeedInput` / `NeedOutput` on the speaker: do a timed CV wait of about 5 ms. The AudioUnit render callback runs on a real-time thread and must not take the CV's mutex, so the ring buffer is polled.
- `WaitUntil(t)`: timed CV wait until `t`. Pause, seek and shutdown call `wake()`, which ends the wait early.


| Thread          | Stage work                                                                                                                                                            |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Caller          | API calls only. Every call is non-blocking except `open` and `shutdown`.                                                                                              |
| T1 Source       | Handles pending seeks, demuxes, and pushes packets to the A and V packet queues. The parser stage is a pass-through in the MVP (A3).                                  |
| T2 Video decode | Takes a packet, queues it into the decoder, dequeues output in PTS order, and pushes it to the frame queue.                                                           |
| T3 Video render | Takes a frame, runs `AvSync`, then presents or drops it.                                                                                                              |
| T4 Audio        | Takes a packet, decodes it, and writes to the `ISpeaker` ring buffer. When the ring is full, the thread waits a few ms. The speaker position drives the master clock. |


Host unit tests use a **single-threaded manual scheduler** that calls `pump()` in a fixed order with a fake clock. This makes the tests for sync, seek and the state machine deterministic.

Callbacks run **on internal threads**. The listener must hand work over to its own thread before it calls any API (A11).

---

## 3. API and state machine

```cpp
class Player {
 public:
  static std::unique_ptr<Player> create(PlatformFactory&, PlayerListener*);  // binds owner thread
  Result open(const std::string& path, void* nativeLayer);   // CAMetalLayer*. sync: probe + configure; then prerolls 1st frame async
  Result play();
  Result pause();
  Result seek(int64_t positionUs);    // sync validation; completion via onSeekCompleted
  Result shutdown();                  // sync: stops & joins threads, releases adapters
  State   state() const;
  int64_t durationUs() const;
  int64_t positionUs() const;         // = master clock (or last shown frame when not playing)
};
struct PlayerListener {
  virtual void onStateChanged(State) = 0;
  virtual void onError(Result code, const std::string& reason) = 0;   // fatal, async
  virtual void onFirstFrame() = 0;                  // after open (TTFF)
  virtual void onSeekCompleted(int64_t shownPtsUs) = 0;
  virtual void onEnded() = 0;
};
```

`Result` codes: `Ok, InvalidState, WrongThread, InvalidArgument, FileOpenFailed, UnsupportedFormat, NoDecoder, MalformedMedia, DecoderFailed, AudioDeviceFailed`.

### Transitions (MVP)


| From                   | Call or event     | To            | Notes                                                                        |
| ---------------------- | ----------------- | ------------- | ---------------------------------------------------------------------------- |
| START                  | `open` ok / fail  | READY / ERROR | READY shows the first frame (preroll = internal `seek(0)`)                   |
| READY                  | `play`            | PLAY          |                                                                              |
| READY                  | `seek(t)`         | READY         | Coalesced. Shows the frame at the keyframe ≤ t                               |
| PLAY                   | `pause`           | READY         | Audio pauses. The clock freezes                                              |
| PLAY                   | end of stream     | READY         | Fires `onEnded`. Position = duration (A5)                                    |
| PLAY                   | async fatal error | ERROR         | Fires `onError`                                                              |
| START/READY/PLAY/ERROR | `shutdown`        | SHUTDOWN      |                                                                              |
| SHUTDOWN               | `shutdown`        | SHUTDOWN      | Returns `Ok`: idempotent (A6)                                                |
| any other              | any               | unchanged     | Returns `InvalidState`. A call from a non-owner thread returns `WrongThread` |


Seek in PLAY is **rejected** in the MVP, following reqs.md literally. To scrub, the app calls `pause → seek* → play` (A4).

---

## 4. Sync, render and seek algorithms

**Master clock**

- With audio: `clock = speaker.presentedPtsUs()`. The render callback publishes, through atomics, the `AudioTimeStamp` (`mSampleTime`, `mHostTime`) of each buffer it pulls. The clock extrapolates that to the current `mach_absolute_time`, subtracts the device output latency (`kAudioDevicePropertyLatency` + safety offset + stream latency + buffer size), and adds the PTS of the first sample written since the last flush. It therefore includes output latency.
- Without audio: `clock = basePts + (steady_now − baseTime)`. `basePts` and `baseTime` are reset on play and on seek. The clock freezes on pause.

**Video render (T3), per frame**

1. `delta = frame.pts − clock.now()`.
2. If `delta < −frameDuration`, the frame is late: **drop** it and count it as dropped.
3. Compute the frame's vsync slot: `slot = round((target − anchor) / vsyncPeriod)`. `anchor` is the first presented target after play or seek. If `slot == lastSlot`, the frame rate is above the refresh rate: **drop** it and count it as rate-capped, not dropped (A9). Otherwise set `lastSlot = slot`. Comparing slots instead of raw time gaps keeps timestamp jitter from causing drops (A19).
4. If `delta > 1 vsync`, return `WaitUntil(now + delta − 1 vsync)` and keep the frame. The scheduler wakes the stage then, or earlier on pause, seek or shutdown.
5. Otherwise, **present** it with `presentDrawable:atTime:(now + delta)`. Core Animation aligns the frame to vsync.

**Seek / scrub** (serial-number pattern)

1. The caller thread validates the state and clamps `t` to `[0, duration]`. It writes `t` into a **single-slot** `pendingSeek`, overwriting any earlier value, and returns `Ok`.
2. T1 takes the latest `pendingSeek` and does `++serial`. It aborts and flushes all queues, then signals T2, T3 and T4 to flush their decoders and the speaker. It calls `demuxer.seekTo(t)` and resumes pushing packets tagged with the new serial.
3. Downstream threads drop any item with a stale serial.
4. T3 presents the first frame of the new serial immediately, even when paused, and fires `onSeekCompleted`. If another seek is already pending, T1 begins it right away. Because each new request overwrites the slot, only the latest one is honored.

**Errors**

- Fatal errors (state becomes ERROR, `onError` fires, threads stop): file fails to open, unsupported container or codec, no decoder, decoder fails to configure or reports an unrecoverable error, audio device fails, malformed media (A8).
- Non-fatal errors: a decode error on a frame. For video, hold the last good frame and skip packets up to the next keyframe. For audio, output silence for that packet's duration. Count both.

---

## 5. Metrics and acceptance criteria

All criteria are measured on a single reference machine (a base M1 Mac running Mac OS 13 or later, driving an external display at a fixed 60 Hz) with the test clips listed below.


| Metric                | Definition (MVP)                                                                                                                                                                                                                                              | Target                              |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| Dropped-frame rate    | late drops / (presented + late drops). Excludes rate-cap drops and seek flushes                                                                                                                                                                               | < 1%                                |
| Jank rate             | presented intervals > 1.5 × `max(frameDuration, vsyncPeriod)` / presented intervals. Excludes the first frame after play and after a seek                                                                                                                     | < 1%                                |
| A/V offset            | `frame.pts − audioClock` at the frame's scheduled present time. Reported as mean                                                                                                                                                                              | x                                   |
| End-to-end A/V offset | On the flash/beep sync clip, the time between the flash on screen and the beep at the speaker, measured with a camera and mic or a light sensor. This catches content offsets such as AAC priming, which the pipeline's own A/V offset metric can't see (A20) |                                     |
| TTFF                  | `open()` called → first frame presented                                                                                                                                                                                                                       | < 500 ms (A10)                      |
| Seek latency          | `seek()` called → `onSeekCompleted`. Reported as p50/p95                                                                                                                                                                                                      | p95 < 100 ms at GOP ≤ 1 s           |
| Scrub                 | 50 seeks in 2 s. Last target shown; no crash or leak                                                                                                                                                                                                          | Last shown ≤ 100 ms after last call |


**Test clips:** all CFR, unrotated (identity track matrix), with no edit list. 1080p30, 1080p60, 720p24 and 4K30 (H.264 + AAC); a flash/beep A/V sync clip, 1080p30 + AAC 48 kHz; 1080p60 in a 1000 Hz timescale (jittered PTS; expect 0 rate-cap drops); 120 fps (expect ~50% rate-capped, 0% dropped); a video-only clip; a clip with a truncated tail; a clip with corrupted mdat bytes; a clip whose header is fuzzed. The 4K30 clip checks behavior only and is not held to the targets.

**Tests:**

- Core unit tests run on the host with fake adapters and the manual scheduler. They cover the state table, queue flush, seek coalescing, AvSync decisions and the clock.
- Integration tests (XCTest) run on the reference machine and check the targets above.

---

## 6. Milestones

1. **M1:** Core skeleton, state machine, non-blocking stages and queues, the Mac OS thread scheduler and the manual test scheduler. Video-only playback on the system clock.
2. **M2:** Audio path, audio master clock and A/V offset metric.
3. **M3:** Seek, scrub coalescing, preroll on open, EOS.
4. **M4:** Error handling, the full metrics set, test clips, acceptance run.

Repo layout: `core/` (C++plus host tests), `platform/macos/` (adapters in C++ and Objective-C++), `apps/macos-demo/`.

---

## 7. Next steps (deferred requirements)

- iOS: reuses the Mac OS adapters (AVAssetReader, VideoToolbox, AudioUnit via RemoteIO, CAMetalLayer). New work is `AVAudioSession` setup and interruptions.
- Android: `AMediaExtractor`, `AMediaCodec`, AAudio (or Oboe on API 26/27), `ANativeWindow` via `releaseOutputBufferAtTime`, behind a thin JNI shim. Reuses the native thread scheduler; the decoder is polled because synchronous-mode `AMediaCodec` has no notification below API 28.
- Web: WASM core with WebCodecs, AudioWorklet and OffscreenCanvas/WebGL, driven by an event-loop scheduler. See §7.1. Add a portable demuxer at this point, e.g. libavformat or a hardened custom MP4 parser.
- Other desktops (Windows, Linux), per reqs.md. These reuse the native thread scheduler and core unchanged. Only the adapters are new:
  - Windows: Media Foundation / D3D11, WASAPI
  - Linux: VA-API, PipeWire/ALSA
- VFR content, edit lists, AAC priming trim, and track-matrix rotation (A20, A21).
- Seek while in PLAY, and frame-accurate seek.
- Decoder capability probing, ranking, and software fallback. More codecs and containers.
- Plugin mechanism (v2), other media types, remote/streaming sources, DRM, background play, lifecycle integration. These are all listed as non-goals in reqs.md.
- An optional RGBA/S16 "canvas" path for effects and compositing graphs.

### 7.1 Web threading model (next step)

**Why the native thread model doesn't work here:**

- The main thread can't block.
- WebCodecs delivers frames through callbacks on the event loop. A worker blocked on a CV never returns to its event loop, so those callbacks **never fire**.
- WebCodecs already decodes on the browser's own threads, so a dedicated decode thread adds nothing.
- Audio is *pulled* by an `AudioWorklet` on a real-time thread, which must never take a lock.

So the web scheduler runs the same core stages from an event loop, not from threads.

```text
Main thread (owner)            Media Worker (1 dedicated worker)                AudioWorklet (RT)
─────────────────────          ─────────────────────────────────────           ─────────────────
Player JS facade  ──postMsg──► C++ core (WASM, single-threaded)
state mirror, UI  ◄─events───    Source: FileReaderSync + demux
                                 VideoDecoder (WebCodecs) ──► frame queue
                                 rAF loop on OffscreenCanvas ──► draw
                                 AudioDecoder (WebCodecs) ──► PCM ──► SAB ring buffer ──► process()
                                 clock ◄──────────── frames consumed / outputLatency ◄───┘
```


| Native thread   | Web equivalent                                                                                                                                                                                                                 |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Caller          | Main thread. A JS facade sends commands by `postMessage`. All APIs return Promises: they validate synchronously and complete asynchronously (A11). `play()` must be called from a user gesture because of the autoplay policy. |
| T1 Source       | A worker task. It reads with `FileReaderSync` from a `File`/`Blob` (no paths) and demuxes in WASM with a portable demuxer (A2). It runs whenever the decoders drain (`dequeue` event, `decodeQueueSize` below a cap).          |
| T2 Video decode | WebCodecs `VideoDecoder`. Its `output` callback pushes to the frame queue and calls `wake`.                                                                                                                                    |
| T3 Video render | A `requestAnimationFrame` loop in the worker, drawing to `OffscreenCanvas` with WebGL `texImage2D(VideoFrame)`. A `WaitUntil` result simply waits for the next rAF.                                                            |
| T4 Audio        | `AudioDecoder` in the worker writes PCM into a lock-free SPSC ring buffer in `SharedArrayBuffer`. The `AudioWorklet` pulls from it.                                                                                            |


The worker has one thread, so queues need no locks. The audio ring buffer is the only cross-thread structure, and it uses atomics.

**Sync:**

- **Audio clock:** the worklet publishes `framesConsumed` on every `process()` call. `clock = basePts + (framesConsumed − baseFrame) / rate − outputLatency`, extrapolated with `performance.now()`. Cross-check it against `AudioContext.getOutputTimestamp()`. Browsers without `outputLatency` fall back to `baseLatency` plus a constant tuned per browser.
- **Render:** in each rAF callback, predict `displayTime ≈ rAF timestamp + vsyncPeriod`. The vsync period is the median gap between rAF timestamps. Draw the **latest** frame with `pts ≤ clock(displayTime)`. Earlier due frames are `close()`d: rate-capped if they fell within the last slot, late drops otherwise. rAF gives one frame per vsync, so the slot rule (A19) holds automatically.
- **Frame budget:** `close()` every `VideoFrame` right away. Holding more than about 3–4 frames stalls the hardware decoder.

**Risks and fallbacks:**


| Risk                                                                               | Fallback                                                                                                                |
| ---------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| The host page can't send COOP/COEP headers, so `SharedArrayBuffer` isn't available | Send PCM to the worklet by `postMessage`, transferring the buffers. This adds latency and copies, but works everywhere. |
| Some Safari versions lack `OffscreenCanvas` or rAF in workers (verify)             | Transfer `VideoFrame`s to the main thread and render there.                                                             |
| `AudioDecoder` support in Safari is recent (verify)                                | Decode AAC in WASM inside the worker.                                                                                   |
| A WebCodecs decode error closes the decoder                                        | For a non-fatal corrupt frame: `reset()`, `configure()`, and resume at the next keyframe (A14).                         |
| rAF stops in a hidden tab while audio keeps going                                  | Pause on `visibilitychange`. Background play is a non-goal.                                                             |


---

## 8. Issues found in reqs.md (with proposals)


| #   | Type                   | Issue                                                                                                                                                                                                                                                                                                                                       | Why it matters                                                                                                  | Proposal (MVP decision in **bold**)                                                                                                                                                                                                                                              |
| --- | ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A1  | Unbounded              | "Video" names no container or codec.                                                                                                                                                                                                                                                                                                        | The decoder and demuxer matrix is open-ended.                                                                   | **MP4 + H.264 + AAC-LC only**, then extend.                                                                                                                                                                                                                                      |
| A2  | Gap                    | The demuxer is in the pipeline but is not one of the adapters (only decoder, speaker and display are). Writing a C++ MP4 demuxer is a lot of work and adds security risk.                                                                                                                                                                   | A robust demuxer is a project in its own right. The "malicious file" requirement makes a hand-rolled one risky. | **Add an** `IDemuxer` **adapter. MVP uses** `AMediaExtractor`**,** which runs sandboxed in Android's media process. Choose a portable demuxer when iOS/Web are added.                                                                                                            |
| A3  | Unnecessary complexity | The parser stage. With MP4 plus platform decoders, codec config goes in through the format (csd/avcC), so no parsing is needed.                                                                                                                                                                                                             | A stage that does nothing still costs a thread and a queue.                                                     | **Pass-through in the MVP**, run on T1. Keep the slot for elementary streams and codecs that need it.                                                                                                                                                                            |
| A4  | Inconsistency          | Seek is allowed only from READY, yet scrubbing and "jump to a time" happen naturally during playback.                                                                                                                                                                                                                                       | Every player UI would then have to do pause, seek, play itself.                                                 | **MVP follows the table.** Next step: allow `seek` in PLAY, staying in PLAY.                                                                                                                                                                                                     |
| A5  | Gap                    | No end-of-stream behavior or state.                                                                                                                                                                                                                                                                                                         | PLAY with nothing left to play is undefined.                                                                    | **At EOS: PLAY → READY and fire** `onEnded`. `play()` from EOS restarts at 0.                                                                                                                                                                                                    |
| A6  | Gap                    | `shutdown` from SHUTDOWN is "rejected". No mention of freeing resources on ERROR.                                                                                                                                                                                                                                                           | Cleanup code in callers becomes fragile.                                                                        | **Make** `shutdown` **idempotent.** ERROR → `shutdown` is the only way out.                                                                                                                                                                                                      |
| A7  | Conflict               | Internal RGBA/S16 frames conflict with "make best use of hardware". A GPU→CPU copy and color conversion of 4K60 video costs about 2 GB/s.                                                                                                                                                                                                   | Hurts the jank target and battery.                                                                              | **Keep video frames opaque, in platform surfaces (zero-copy).** Audio uses S16. Treat RGBA as an optional path for later.                                                                                                                                                        |
| A8  | Unbounded              | "Malicious file that allocates out-of-bound memory" can't be detected once it has happened.                                                                                                                                                                                                                                                 | It's a security property, not an error you can detect.                                                          | Treat it as `MalformedMedia`. Validate sizes against file size and limits (max sample 16 MB, max 8K resolution). Rely on the platform sandbox (A2). Fuzz-test later.                                                                                                             |
| A9  | Inconsistency          | Capping fps at the refresh rate means dropping frames on purpose, and those drops would count as "dropped frames" and hurt the metric. The dropped-frame rate also has no denominator or target.                                                                                                                                            | The metric could fail even when behavior is correct.                                                            | **Count rate-cap drops separately.** Dropped rate = late drops / (presented + late drops), target < 1%.                                                                                                                                                                          |
| A10 | Missing / unbounded    | "Fast TTFF" has no number and no start or end point. The jank "expected interval" is ambiguous when content fps ≠ refresh rate. Where the 100 ms seek target is measured from is not defined. Keyframe spacing (GOP) sets seek time and is not bounded.                                                                                     | Targets that can't be checked can't be accepted.                                                                | Use the definitions in §5. **TTFF < 500 ms. Seek measured from** `seek()` **call to frame shown, with GOP ≤ 1 s.** Longer GOPs are best effort.                                                                                                                                  |
| A11 | Gap                    | API calls are limited to the owner thread, but nothing says which thread callbacks run on. Nothing says whether APIs are sync or async.                                                                                                                                                                                                     | Deadlocks, or `WrongThread` errors inside callbacks.                                                            | **Callbacks run on internal threads, and the listener hands work to its own thread.** `open`/`shutdown` are sync; `seek` is validated sync and completes async. Next step: post callbacks through a platform looper.                                                             |
| A12 | Conflict               | The fixed 4-thread mutex/CV model doesn't carry over to Web. Threads there need SharedArrayBuffer plus COOP/COEP headers, and audio runs in an AudioWorklet. On Android, the AAudio callback thread must not lock a mutex.                                                                                                                  | Priority inversion, and a hard Web port. Blocking stages would have to be rewritten for the web.                | **Stages are non-blocking** `pump()` **steps. Waiting lives in a per-platform** `IScheduler` **(§2.1–2.3).** Android runs the 4 threads from reqs.md, and AAudio is written from T4, never from the callback. Web runs one worker's event loop plus an AudioWorklet (§7.1).      |
| A13 | Platform risk          | AAudio on API 26/27 has known bugs, and Oboe exists to work around them. The NDK can't report the actual time a frame reached the screen before API 33.                                                                                                                                                                                     | The API 26 floor and exact jank measurement are at risk.                                                        | **MVP uses AAudio and computes jank from scheduled present times.** Next step: Oboe, or the Java `OnFrameRenderedListener`, for true display timing.                                                                                                                             |
| A14 | Ambiguous              | "Placeholder" for corrupted frames is undefined. H.264 errors spread until the next keyframe.                                                                                                                                                                                                                                               | Behavior would differ from one implementation to the next.                                                      | **Video holds the last good frame until the next keyframe. Audio outputs silence.**                                                                                                                                                                                              |
| A15 | Gap                    | Missing APIs needed for a usable player: render target (surface), duration and position.                                                                                                                                                                                                                                                    | A seek bar can't be built without them.                                                                         | **Add** `nativeWindow` **to** `open`**, plus** `durationUs()` **and** `positionUs()`**.**                                                                                                                                                                                        |
| A16 | Ambiguous              | Not stated whether READY after `open` shows a frame.                                                                                                                                                                                                                                                                                        | Affects TTFF and what the user sees.                                                                            | **Preroll: show the first frame on** `open`. This reuses the seek path.                                                                                                                                                                                                          |
| A17 | Gap                    | The fatal error list leaves out decoder runtime failure, audio device failure and unsupported audio codec.                                                                                                                                                                                                                                  | Unhandled error paths.                                                                                          | Decoder and device failures are **fatal**. An **unsupported audio codec plays video only**, driven by the system clock, and fires a warning callback.                                                                                                                            |
| A19 | Gap                    | Nothing says how to decide that a frame is above the refresh rate. A naive `target − lastTarget < vsyncPeriod` check fails two ways. With 60 fps content in a millisecond timescale (gaps of 16/17 ms) on a 60 Hz display, it drops about ⅓ of frames. With 90 fps content, it shows only 45 fps.                                           | This would break the most common case (60 on 60) and under-use the display.                                     | **Quantize targets to vsync slots and drop only when a frame falls in the same slot as the previous one (§4 step 3).** Anchor error is at most half a vsync, which changes only which frame of a pair is shown. Next step: anchor to the real vsync phase from `AChoreographer`. |
| A20 | Conflict               | reqs.md makes AAC priming (and edit lists) a no-goal, but priming affects the ±40 ms A/V target. AAC files start with about 1024–2112 encoder-delay samples, roughly 21–44 ms at 48 kHz. Without trimming, audio content arrives late by that much. The pipeline's A/V offset metric compares scheduled times, so it can't see this offset. | The target could pass on paper while real sync is off by up to 44 ms.                                           | **Keep it a no-goal, but add the end-to-end sync-clip test (§5).** In M2, if the measured offset is over 15 ms, shift audio timestamps by the encoder delay. This is a small, local change, not full edit-list support.                                                          |
| A21 | Gap                    | Track header matrix is a no-goal. Most phone-recorded video carries a 90° rotation in that matrix.                                                                                                                                                                                                                                          | Those files will play sideways. That's acceptable for the MVP, but it should be known.                          | **Test clips are unrotated.** The demo app shows a "rotation not supported" warning when the matrix isn't identity. Next step: pass the rotation to `IDisplay` (`ANativeWindow_setBuffersTransform`).                                                                            |
| A18 | Minor                  | The A/V threshold is symmetric (±40 ms), but perception is asymmetric (ITU-R BT.1359: about +45 / −125 ms).                                                                                                                                                                                                                                 | The target is stricter than it needs to be.                                                                     | **Keep ±40 ms at p95** for the MVP. It's achievable with an audio master clock.                                                                                                                                                                                                  |


