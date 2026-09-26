# Implementation Skeleton

An outline of the code in `core/`, `platform/macos/` and `apps/macos-demo/`. It covers functional logic only. Error checks, validation, fatal/warning paths, logging and metrics formatting are left out. Tests are not covered.

## 1. Architecture

### Layers

```text
apps/macos-demo      AppKit UI (Controller, VideoView, Listener)
        │  Player API (owner thread)            ▲ PlayerListener callbacks (internal threads)
        ▼                                       │
core  Player ── Impl (PipelineEvents) ── Context (shared state) ── 4 Stages
        │                                          │
        │            adapter interfaces (adapters.h): IDemuxer, IVideoDecoder,
        │            IAudioDecoder, ISpeaker, IDisplay, IScheduler, IClock
        ▼                                          ▲
platform/macos   PlatformFactory → AVAssetReader, VideoToolbox, AudioConverter,
                                   AudioUnit, CAMetalLayer + CVDisplayLink, ThreadScheduler
```

The core is portable C++17. It sees platform objects only through the interfaces and opaque handles (`MediaSource`, `RenderTarget`, and the `shared_ptr<void>` format and image fields).

### Pipeline

```text
          T1 Source                 T2 VideoDecode            T3 VideoRender          display link
Demuxer ──► videoPackets ──► VT decoder ──► frames(4) ──► AvSync ──► Display.present ──► Metal draw
        └─► audioPackets ──► T4 Audio: AAC decode ──► AudioRing ──► AU render callback (speaker)
                                                           └──► MasterClock (audio clock) ──► T3
```

### Components

| Component | File | Role |
| --- | --- | --- |
| Player / Impl | [player.cpp](core/src/player.cpp) | Public API, state machine, receives pipeline events |
| Context | [pipeline.h](core/src/pipeline.h) | Owns adapters, queues, ring, clock, metrics; seek slot; serials |
| SourceStage (T1) | [pipeline.cpp](core/src/pipeline.cpp) | Probe, start seeks, demux by lowest DTS |
| VideoDecodeStage (T2) | [pipeline.cpp](core/src/pipeline.cpp) | Packets → decoder → frames in PTS order |
| VideoRenderStage (T3) | [pipeline.cpp](core/src/pipeline.cpp) | Complete seeks, A/V sync, present/drop, start/stop output, detect end |
| AudioStage (T4) | [pipeline.cpp](core/src/pipeline.cpp) | Decode AAC, trim to seek target, write to the ring |
| AvSync | [av_sync.cpp](core/src/av_sync.cpp) | Per-frame present/drop/wait decision on the vsync grid |
| MasterClock | [master_clock.cpp](core/src/master_clock.cpp) | Audio clock, or steady clock when there's no audio or it has ended |
| AudioRing | [audio_ring.cpp](core/src/audio_ring.cpp) | Lock-free SPSC PCM ring that also carries the audio clock |
| BoundedQueue | [bounded_queue.h](core/src/bounded_queue.h) | Non-blocking queue capped by count, bytes and duration, with wake hooks |
| ThreadScheduler | [thread_scheduler.cpp](core/src/thread_scheduler.cpp) | One thread per stage; runs `pump()` and waits on a CV |
| Metrics | [metrics.cpp](core/src/metrics.cpp) | Drops, jank, A/V offset, TTFF, seek latency |
| MacPlatform | [mac_platform.mm](platform/macos/src/mac_platform.mm) | Factory, host clock, thread names and QoS |
| AvfDemuxer | [avf_demuxer.mm](platform/macos/src/avf_demuxer.mm) | AVAssetReader, one output per track |
| VtVideoDecoder | [vt_video_decoder.mm](platform/macos/src/vt_video_decoder.mm) | Async VTDecompressionSession + PTS reorder |
| AtAudioDecoder | [at_audio_decoder.cpp](platform/macos/src/at_audio_decoder.cpp) | AudioConverter AAC → S16, synchronous |
| AuSpeaker | [au_speaker.cpp](platform/macos/src/au_speaker.cpp) | DefaultOutput AudioUnit; render callback pulls from the ring |
| MetalDisplay | [metal_display.mm](platform/macos/src/metal_display.mm) | Pending-frame queue drained on each vsync; NV12 → RGB draw |

### Adapter interfaces ([adapters.h](core/include/mf/adapters.h))

```text
IClock        nowNs()
IDemuxer      open(src) -> MediaInfo; peekDtsUs(track); read(track) -> Packet; seekTo(us)
IVideoDecoder configure(track, onOutput); queue(pkt); signalEos(); dequeue() -> frame; flush()
IAudioDecoder configure(track); decode(pkt) -> PCM; flush()
IDisplay      attach(view, onPresented); present(frame, atNs); visible; vsyncPeriodNs; latencyNs; vsyncGridNs
ISpeaker      open(rate, ch, ring); start(); pause()
Stage         pump() -> Did | Idle | WaitUntil(ns)          // never blocks
IScheduler    start(stages); wake(id); stop()
PlatformFactory  create*() for each adapter + clock()
```

## 2. Component ownership and lifetime

### Ownership tree

```text
Controller (app, main thread)
├── unique_ptr<PlatformFactory>   lives for the whole app; owns the HostClock
├── unique_ptr<Listener>          raw pointer handed to the Player (not owned by it)
└── unique_ptr<Player>            one per opened file; replaced on each open
    └── unique_ptr<Impl>          (is the PipelineEvents sink)
        ├── Context ctx           (by value)
        │   ├── IClock&           → factory's clock (borrowed)
        │   ├── PipelineEvents&   → Impl (borrowed)
        │   ├── unique_ptr demuxer, videoDecoder, audioDecoder, speaker, display, scheduler
        │   ├── videoPackets, audioPackets, frames   (BoundedQueues)
        │   ├── AudioRing ring    (buffer allocated in probe, before playback)
        │   ├── MasterClock master (holds const AudioRing&)
        │   └── Metrics
        └── array<unique_ptr<Stage>, 4> stages      (each holds Context&)
```

### Resource lifetimes

- **Decoded frames:** a `VideoFrame` holds a retained `CVPixelBuffer` in a `shared_ptr<void>`. The frame is copied through the reorder map, the `frames` queue, T3's `current_`/`candidate_`, and the display's pending queue. The Metal completion handler keeps its own copy, so the buffer is released only after the GPU has finished with it.
- **Media handles:** `MediaSource` holds a retained `NSURL`. `TrackInfo.format` holds a retained `CMFormatDescription`, and `VtVideoDecoder` keeps its own `formatHolder_` reference.
- **AVAssetReader:** a new one is created on open and on every seek. The asset and tracks live as long as the demuxer.
- **VT session:** created in `configure()`. It is recreated once if it becomes invalid (after sleep or a GPU switch), and destroyed in the decoder's destructor, which waits for in-flight frames.
- **Metal `Sink`:** held in a `shared_ptr` that outlives `MetalDisplay`, because GPU presented handlers can fire after the display is destroyed. The destructor clears `sink->fn` under the sink's lock.
- **CVDisplayLink:** started in `attach()`. `CVDisplayLinkStop` in the destructor waits for any running callback.

### Construction and destruction order

```text
Impl():   ctx(factory)          // creates every adapter + scheduler, installs queue wake hooks
          stages = makeStages(ctx)
          scheduler.start(stages)   // threads start; all stages idle

~Impl():  shutdown()             // joins threads, releases adapters (see §4)
          ~stages
          ~ctx                   // queues, ring, metrics, scheduler object
```

Adapters are released before the buffers they call back into (ring, metrics, scheduler hooks), so no late callback can touch freed state.

## 3. State and state transitions

### Player state (`Impl::state`, guarded by `stateMu`)

```text
            open()                       preroll (serial 1) shown
  Start ───────────► Start (probing) ─────────────────────────► Ready ◄──────────┐
                                                                  │  ▲           │ pause()
                                                           play() │  │ onEnd()   │
                                                                  ▼  │ (ended=1) │
                                                                  Play ──────────┘
  any state except Error/Shutdown ── onFatal ──► Error
  any state ── shutdown() ──► Shutdown   (terminal, idempotent)
```

The only allowed calls are:
- `open()` in Start, once.
- `play()` in Ready. If `ended` is set, it first requests a seek to 0.
- `pause()` in Play.
- `seek()` in Ready. It clears `ended` and finishes asynchronously with `onSeekCompleted`.

### Pipeline state (in Context)

| Field | Meaning | Written by |
| --- | --- | --- |
| `openRequested` | Tells T1 to probe | owner (`open`) |
| `hasAudio`, `info`, `durationUs` | Probe result, read-only afterwards | T1 |
| pending seek slot | Latest requested seek, or none | owner (`seek`/`play`), T1 (`probe`) |
| seek target | Seek currently being executed | T1 (`startSeek`) |
| `serial` | Latest seek started | T1 |
| `shownSerial` | Latest seek completed | T3 |
| `shownPtsUs` | Last frame shown | T3, owner (`onEnd`) |
| `playing` | User wants playback | owner / events, under `playMu` |
| `outputRunning` | Speaker started and clock running | T3 (and shutdown / fatal), under `playMu` |
| `halted` | Fatal error or shutdown; stages go idle | Player |

Derived states:
- **Seeking:** `shownSerial != serial`.
- **Output should run:** `playing && !seeking && no pending seek`. T3 applies this on every pump.

### Seek lifecycle

```text
requested    owner: pending = {target, now}                      (latest wins; overwrites)
   │ T1: when shownSerial == serial (previous seek finished)
started      T1: target = pending; flush packet queues; demuxer.seekTo; serial++; wakeAll
   │ each stage sees serial != its local serial_
flushing     T2: decoder.flush()   T3: drop current/candidate   T4: decoder.flush, ring.flush(target, serial)
   │ T3 finds the frame to show
completed    T3: present frame; master.reset(pts, serial); shownSerial = serial; onSeekDone; wake T1
```

Serial 1 is the preroll started by `probe()`. When it completes, the Player moves from Start to Ready.

### Stage-local state

- **T1:**
  - `opened_`: whether `probe()` has run.
  - `eos_[2]`: per-track end of stream.
  - `serial_`: stamped on each packet.
- **T2:**
  - `serial_`
  - `input_`: packet held because the decoder was full.
  - `output_`: frame held because the frames queue was full.
  - `skipToKey_`: after a corrupt frame, skip to the next keyframe.
  - `eosSent_`
- **T3:**
  - `serial_`
  - `current_`: frame being handled.
  - `candidate_`: best frame so far for an exact seek.
  - `videoEnded_`
  - `outputRunning_`: copy of `ctx.outputRunning`.
  - AvSync anchor and last slot.
- **T4:**
  - `serial_`
  - `pcm_`/`offset_`: partly written PCM.
  - `trimUs_`: seek target for sample trimming.
- **MasterClock:**
  - `running_`
  - `useAudio_`: switches to false when audio drains.
  - `baseUs_`/`baseNs_`
  - `serial_`
- **AudioRing:**
  - flush generation
  - consumer's `seenGen_`
  - `eos_`

## 4. Startup and shutdown

### Startup

```text
main thread (owner)
 1. app: platform = createPlatform()
 2. openPath: player = Player::create(platform, listener)
      Context: create adapters + scheduler; hook queues to stage wakes
      makeStages; scheduler.start → spawn T1..T4 (named, QoS set), each pumps once → idle
 3. player.open(source, view)                                  // returns immediately
      display.attach(view): Metal device/queue/pipeline, texture cache, CVDisplayLink start,
                            occlusion + screen observers
      metrics.startTtff; openRequested = true; wake(T1)

T1
 4. probe():  demuxer.open (load asset keys, describe tracks, start reader at 0)
              videoDecoder.configure (VT session; onOutput = wake T2)
              audio: audioDecoder.configure; ring.open(200 ms); speaker.open (AU init, latency)
              hasAudio; master.setAudio; requestSeek(0)
 5. startSeek(serial 1) → demux packets

T2 → T3
 6. decode → frames → T3 seekStep → complete(first frame) → present now
 7. onSeekDone(1) → state Ready → listener.onFirstFrame, onStateChanged(Ready)

owner
 8. play(): state Play; playing = true; wakeAll
T3
 9. updateOutput: speaker.start; master.start(now); sync.reset; metrics.discontinuity → renderStep
```

### Shutdown

```text
owner: shutdown()    (also called from ~Impl; idempotent)
 1. state = Shutdown; callbacksOn = false          // no more listener callbacks
 2. halted = true                                  // every pump returns Idle
 3. scheduler.stop(): running = false; wake all; join T1..T4
 4. under playMu: speaker.pause if running; outputRunning = false
 5. release adapters in order:
      speaker       AudioOutputUnitStop, uninitialize, dispose   (RT callback stops)
      display       remove observers; CVDisplayLinkStop (waits for callback); sink.fn = null
      videoDecoder  wait for async frames; invalidate session
      audioDecoder  dispose converter
      demuxer       release reader / asset
 6. print metrics
then ~Impl destroys stages and Context (queues, ring, scheduler object)
```

Other paths:
- **Fatal error (`onFatal`, any stage):** sets `halted`, `playing = false`, pauses the speaker, stops the master clock, and sets state to Error. The threads stay alive and idle until `shutdown()`.
- **App:** `openPath` shuts down the previous Player before creating a new one. `applicationWillTerminate` calls `shutdown()`.
- **Deadlock rule:** listener callbacks run on internal threads and must never wait on the owner thread, because `shutdown()` joins those threads. The demo posts every callback to the main queue with `dispatch_async`.

## 5. Concurrency and synchronization

### Threads

| Thread | QoS | Runs |
| --- | --- | --- |
| Owner (main) | — | Player API, AppKit, window occlusion / screen notifications for the display |
| T1 `mf.source` | USER_INITIATED | SourceStage: probe (blocking asset key loads happen here), seeks, demux |
| T2 `mf.video-decode` | USER_INITIATED | VideoDecodeStage |
| T3 `mf.video-render` | USER_INTERACTIVE | VideoRenderStage; `onSeekDone`, `onEnd` |
| T4 `mf.audio` | USER_INTERACTIVE | AudioStage |
| VideoToolbox callback | system | `decoded()` → reorder map → `onOutput` (wakes T2) |
| CoreAudio render (real-time) | real-time | `ring.consume` (no locks, no allocation) |
| CVDisplayLink | system | `vsync()` → pick frame → `draw` (`nextDrawable` may block, but only here) |
| Metal handlers | system | presented handler → `Sink` → `Player.onPresented` → metrics |

### Scheduling model ([thread_scheduler.cpp](core/src/thread_scheduler.cpp))

```text
worker loop:  p = stage.pump()                 // never blocks
              Did       -> pump again
              Idle      -> cv.wait until woken
              WaitUntil -> cv.wait_for(deadline) or until woken
              woken = false
wake(id):     lock; woken = true; notify        // flag set under lock, so no wake is lost
```

The stages wake each other:
- Queue push wakes the consumer; queue pop or flush wakes the producer (hooks run outside the queue lock).
  - `videoPackets`: T1 → T2
  - `audioPackets`: T1 → T4
  - `frames`: T2 → T3
- The VT output callback wakes T2.
- A completed seek (T3) wakes T1.
- `play()`, `startSeek` and the seek request each wake the stages they affect.

### Locks and atomics

| Primitive | Protects | Contenders |
| --- | --- | --- |
| `Impl::stateMu` | `state`, `ended`, `openCalled` | owner, T1/T3 (events) |
| `ctx.playMu` | `playing` writes, `outputRunning`, speaker start/pause and master start/stop together | owner, T3 |
| `Context::seekMu_` | pending seek, seek target | owner, T1, T3, T4 |
| `BoundedQueue::mu_` (×3) | deque + byte count | producer / consumer stage |
| `MasterClock::mu_` | clock base, `running`, `useAudio` | T3, owner, display handler |
| `Metrics::mu_` | counters, histograms, plan ring | T2, T3, Metal handler, owner |
| `Worker::mu` + cv (×4) | `woken` flag | any waker, the worker |
| `VtVideoDecoder::mu_` | in-flight map, reorder map, generation, errors, eos | T2, VT callback |
| `MetalDisplay::mu_` | pending frames | T3, display link |
| `Sink::mu` | presented callback | Metal handlers, display destructor |
| Atomics | `serial`, `shownSerial`, `shownPtsUs`, `halted`, `playing`, `hasAudio`, `durationUs`, `openRequested`, `hasPending_`, `callbacksOn`, display `visible_`/`vsyncNs_`/`latencyNs_`/`gridNs_` | — |

### AudioRing (lock-free SPSC)

- **Indices:**
  - `write_` is advanced only by T4.
  - `read_` is advanced only by the real-time consumer.
  - Both use acquire/release ordering.
- **Flush:** the producer never moves `read_`. It publishes a flush record `{gen+1, writeIndex, basePts, tag}` under a seqlock. On its next callback, the consumer sees the new generation and jumps `read_` to that record's `writeIndex`.
- **Clock snapshot:** after each callback that delivered audio, the consumer publishes `{gen, framesBefore, frames, audibleNs}` under a second seqlock. Any thread can read it.

### Invalidation without blocking

- Packets and frames carry the `serial` they were read under. Each stage drops items whose serial doesn't match its current one.
- `VtVideoDecoder::flush()` increments `generation_` and clears the reorder map without waiting. Frames from an old generation that arrive late are discarded in the callback.
- **Publication order in `startSeek`:** the target is set before `serial` is incremented, so a stage that sees the new serial reads the correct target.

## 6. Timing and ordering

### Time bases

- **Host time (ns):** `CLOCK_UPTIME_RAW`. It uses the same base as `mach_absolute_time`, CoreAudio `mHostTime` and Core Animation / `CVTimeStamp`.
- **Media time (µs):** packet and frame PTS/DTS, seek targets, clock values.

### Master clock ([master_clock.cpp](core/src/master_clock.cpp))

```text
nowUs(now):
  !running      -> baseUs                                     (paused: frozen)
  useAudio      -> ring.clockUs(now) if its tag == serial, else baseUs
                   (held at the seek base until this seek's audio is actually heard)
                   ring drained -> switch to steady: baseUs = v, baseNs = now, useAudio = false
  else          -> baseUs + (now - baseNs)/1000               (steady clock)
start(now): running, baseNs = now      stop(now): baseUs = value(now), not running
reset(pts, serial): baseUs = pts; useAudio = hasAudio        (after each seek)
```

### Audio clock ([audio_ring.cpp](core/src/audio_ring.cpp))

- The speaker's render callback passes `audibleHostNs = callbackHostTime + output latency`. The latency is device + stream + AudioUnit latency, measured once in `open()`.
- `clockUs(now) = basePts + clamp(framesBefore + (now - audibleNs)·rate, 0, framesBefore + frames) / rate`
  - It extrapolates up to ±1 s.
  - It never moves past the last frame delivered, so the clock holds during an underrun.
- `drained(now)` is true when:
  - EOS has been marked,
  - `read == write`,
  - and `now` is past the point where the last delivered frame is heard.

### Ordering through the pipeline

- **Demux:** each call reads one packet, from the track with the lowest DTS among tracks that have queue space and haven't ended. Each track has its own AVAssetReader output, so a full video queue never stalls audio.
- **Video decode:** VideoToolbox returns frames in decode order. `dequeue()` releases frames in PTS order once the reorder map holds more than the DPB depth (worked out from the avcC level and frame size; 0 for baseline), or when draining at EOS.
- **Audio:** decoded synchronously in packet order. After a seek, samples before `trimUs_` are dropped. The ring's flush base is set to the seek target.

### Video presentation

**AvSync** ([av_sync.cpp](core/src/av_sync.cpp)) makes the decision for each frame:

```text
lead  = display.latencyNs()                    // how early a frame must be handed over
clock = master.nowUs(now) + lead
delta = pts - clock
delta < -frameDur                -> LateDrop
target = now + lead + delta; slot = round((target - anchor) / vsync)
anchored && slot <= lastSlot     -> RateCapDrop       (at most one frame per vsync)
delta > vsync                    -> Wait until target - vsync - lead   (capped at now + 50 ms)
first present: anchor = target; presentAnchor = target snapped to the real vsync grid
lastSlot = slot                  -> Present at presentAnchor + slot·vsync
vsync period changed             -> re-anchor
```

**MetalDisplay** ([metal_display.mm](platform/macos/src/metal_display.mm)) runs on the display link:

```text
present(frame, atNs): push {frame, atNs}; more than 4 pending -> oldest reported not shown
vsync(now, output):   period, grid = output time, latency = output - now + period
                      show = latest pending with atNs <= output + period/2; earlier ones -> late
                      draw(show) → presentDrawable; presented handler reports actual time
```

### Seek ordering

- **Latest wins:** there is one pending slot, and a new request overwrites the old one.
- **One at a time:** the next seek starts only after the previous one has been shown (`shownSerial == serial`).
- **Exact seek:** the decoder starts from the sync sample at or before the target. T3 keeps the last frame with `pts <= target` and shows it when:
  - the first frame past the target arrives,
  - or the track reaches EOS.

  Frames before the chosen one are counted as `decodeOnly`.
- **Scrubbing:** if a newer seek is already pending, T3 shows the best frame it has right away instead of decoding up to the target.
- Output restarts only when no seek is in flight or pending. On restart, AvSync is reset and metrics mark a discontinuity.

### End of stream

T3 reports the end only when both conditions hold:
- The video EOS frame has been reached.
- There's no audio, or the ring is drained. While waiting for the drain, T3 polls every 10 ms.

On end, the Player calls `stopPlaying()`, sets `shownPts = duration` and `ended = true`, and moves to Ready.

## 7. Performance-critical path

### Video, per frame

```text
T1   copyNextSampleBuffer → split → memcpy sample bytes into Packet.data       (copy 1)
T2   CMBlockBuffer alloc + copy → CMSampleBuffer → VTDecodeFrame (async, HW)    (copy 2)
VT   decoded into IOSurface-backed NV12 CVPixelBuffer                            (zero-copy from here)
     → reorder multimap → onOutput wakes T2 → frames queue
T3   AvSync.decide: O(1) arithmetic; present() only enqueues under a short lock
DL   CVMetalTextureCache wraps Y and UV planes as textures (no copy)
     one draw call: triangle strip + BT.709 NV12→RGB fragment shader, aspect-fit
     nextDrawable (may block up to 1 s) runs only on the display-link thread, never on T3
```

### Audio real-time path

`consume()` runs in the render callback. It does:
- two seqlock reads, which never block;
- a memcpy (two chunks if the data wraps around the ring);
- a silence pad on underrun;
- one seqlock write.

It takes no locks, makes no allocations and makes no system calls. The ring holds 200 ms. When the ring is full, T4 polls every 5 ms while playing, because the real-time thread can't signal it.

### Bounded buffering

These caps bound both memory and latency:

| Buffer | Cap |
| --- | --- |
| `videoPackets` | 60 packets / 32 MB / 2 s |
| `audioPackets` | 120 packets / 1 MB / 2 s |
| VT in flight | 4 |
| Reorder map | DPB depth (≤ 16) |
| `frames` | 4 |
| Display pending | 4 |
| CAMetalLayer drawables | 3 |
| AudioRing | 200 ms |
| Metrics | fixed 2001-bin histograms, 16-entry plan ring |

### Latency and wakeups

- Stages are driven by condition variables and sleep between wakes. Only a few waits use timeouts:
  - T3's clock wait is capped at 50 ms, so a clock that is holding still gets re-checked.
  - T4 polls every 5 ms when the ring is full.
  - The end check polls every 10 ms.
- Seek latency is kept low because:
  - the decoder flush never waits for in-flight frames;
  - a new AVAssetReader starts at the sync sample;
  - scrubbing shows intermediate frames without decoding up to the target.
- Thread priority: T3 and T4 run at USER_INTERACTIVE QoS; T1 and T2 at USER_INITIATED.
