# Implementation Skeleton

An outline of the code in `core/`, `platform/macos/` and `apps/macos-demo/`. It covers functional logic only. Error checks, validation, fatal/warning paths, logging and metrics formatting are left out. Tests are not covered.

## 1. Architecture

### Layers

```text
apps/macos-demo      AppKit UI (Controller, VideoView, Listener), --export runs an Exporter headless
        │  Player / Exporter API (owner thread)       ▲ PlayerListener / ExportListener callbacks
        ▼                                             │ (internal threads)
core  Player | Exporter ── Impl (PipelineEvents) ── Context (shared state) ── 5 Stages
        │                   Scene ── SceneLayout (items, lanes, transitions)
        │            adapter interfaces (adapters.h): IDemuxer, IVideoDecoder, IAudioDecoder,
        │            ISpeaker, IDisplay, IExportSink, IImageLoader, IScheduler, IClock
        ▼                                             ▲
platform/macos   PlatformFactory → AVAssetReader, VideoToolbox, AudioConverter, AudioUnit,
                                   CAMetalLayer + CVDisplayLink, AVAssetWriter, ImageIO,
                                   MetalCompositor (shared by display and export), ThreadScheduler
```

The core is portable C++17. It sees platform objects only through the interfaces and opaque handles (`MediaSource`, `RenderTarget`, `ExportTarget`, and the `shared_ptr<void>` format and image fields).

### What gets played

The pipeline plays a **`Scene`** ([scene.h](../../core/include/mf/scene.h)): tracks of video, image, text, color and audio items, transitions between neighbouring items of a track, per-item effects, and keyframed values. It comes from a JSON document (`parseScene`, [scene_graph_spec.md](scene_graph_spec.md)) or is built in code (`validateScene`).

- A video or audio item with **duration 0** plays to the end of its file. T1 opens those files first, sets the durations, and validates again with them.
- `Player::open(MediaSource)` is a scene of one such item, with the output size, rate and audio format taken from the file.
- The demo's clip mode (`mf_demo a.mp4 b.mp4`) builds a scene itself: it opens each file for its length, places the clips back to back with pushes, and adds a caption track.

### Pipeline

```text
                 per lane (≤ 8; items on a lane never overlap)
T1 Source        T2 VideoDecode              TC Composition               T3 VideoRender        display link
Demuxer(item) ─► videoPackets ─► VT decoder ─► frames(4) ─┐
Demuxer(item) ─► videoPackets ─► VT decoder ─► frames(4) ─┼─► FrameSampler ─► driver ─► composed(4) ─► AvSync or
   ...                                                    ┘   composeAt(t)                         vsync slot ─► Display.present ─► MetalCompositor
         └─► audioPackets (per lane) ─► T4 Audio: decode, resample, mix ─► AudioRing ─► AU render callback (speaker)
                                                                               └──► MasterClock (audio clock) ─► TC, T3
Export:  T3 ─► IExportSink.writeVideo ─► MetalCompositor ─► AVAssetWriter (H.264)
         T4 ─► IExportSink.writeAudio ─────────────────────► AVAssetWriter (AAC)
```

### Components

| Component | File | Role |
| --- | --- | --- |
| Player / Impl | [player.cpp](../../core/src/player.cpp) | Public playback API, state machine, receives pipeline events |
| Exporter / Impl | [exporter.cpp](../../core/src/exporter.cpp) | Public export API; same pipeline with the export driver and sink |
| Scene, parseScene, validateScene | [scene.cpp](../../core/src/scene.cpp), [json.cpp](../../core/src/json.cpp) | Scene model, easing and keyframe evaluation, JSON parsing, rules R1–R8 |
| SceneLayout | [layout.cpp](../../core/src/layout.cpp) | Flattens enabled tracks into items; assigns lanes; media ↔ timeline time; what is visible at t; transition gains |
| Context | [pipeline.h](../../core/src/pipeline.h) | Owns adapters, lanes, items, queues, ring, clock, metrics; seek slot; serials |
| SourceStage (T1) | [pipeline.cpp](../../core/src/pipeline.cpp) | Probe, build the layout, start seeks, demux every lane by lowest timeline DTS |
| VideoDecodeStage (T2) | [pipeline.cpp](../../core/src/pipeline.cpp) | Per lane: packets → decoder → frames in PTS order; reconfigure per item |
| CompositionStage (TC) | [composition.cpp](../../core/src/composition.cpp) | Exact seek frame, then runs the driver; hands composed frames to T3 |
| FrameSampler | [composition.cpp](../../core/src/composition.cpp) | Per-lane head frame and each item's latest frame; `composeAt(t)` builds the layers |
| LeadingClipDriver | [leading_clip_driver.cpp](../../core/src/leading_clip_driver.cpp) | One output per frame of the highest-fps visible video |
| VsyncDriver | [vsync_driver.cpp](../../core/src/vsync_driver.cpp) | One output per display refresh, at the clock time it will be seen |
| ExportDriver | [export_driver.cpp](../../core/src/export_driver.cpp) | Outputs on the fixed n / fps grid, each once every layer is exact |
| VideoRenderStage (T3) | [pipeline.cpp](../../core/src/pipeline.cpp) | Complete seeks, A/V sync, present/drop, start/stop output, detect end; on export, write video |
| AudioStage (T4) | [pipeline.cpp](../../core/src/pipeline.cpp) | Per lane decode; resample, gain, pan, fades; mix into the ring (or the export sink) |
| AvSync | [av_sync.cpp](../../core/src/av_sync.cpp) | Per-frame present/drop/wait decision on the vsync grid (leading-clip driver) |
| MasterClock | [master_clock.cpp](../../core/src/master_clock.cpp) | Audio clock, or steady clock when there's no audio or it has ended |
| AudioRing | [audio_ring.cpp](../../core/src/audio_ring.cpp) | Lock-free SPSC PCM ring that also carries the audio clock |
| BoundedQueue | [bounded_queue.h](../../core/src/bounded_queue.h) | Non-blocking queue capped by count, bytes and duration, with wake hooks |
| ThreadScheduler | [thread_scheduler.cpp](../../core/src/thread_scheduler.cpp) | One thread per stage; runs `pump()` and waits on a CV |
| Metrics | [metrics.cpp](../../core/src/metrics.cpp) | Drops, late layers, jank, A/V offset, TTFF, seek latency |
| MacPlatform | [mac_platform.mm](../../platform/macos/src/mac_platform.mm) | Factory, host clock, thread names and QoS, `loadScene` (resolves `src` paths) |
| AvfDemuxer | [avf_demuxer.mm](../../platform/macos/src/avf_demuxer.mm) | AVAssetReader, one output per track |
| VtVideoDecoder | [vt_video_decoder.mm](../../platform/macos/src/vt_video_decoder.mm) | Async VTDecompressionSession + PTS reorder; session kept across items when the format allows |
| AtAudioDecoder | [at_audio_decoder.cpp](../../platform/macos/src/at_audio_decoder.cpp) | AudioConverter AAC → S16, synchronous |
| AuSpeaker | [au_speaker.cpp](../../platform/macos/src/au_speaker.cpp) | DefaultOutput AudioUnit; render callback pulls from the ring |
| MetalDisplay | [metal_display.mm](../../platform/macos/src/metal_display.mm) | Pending-frame queue drained on each vsync; draws with MetalCompositor |
| MetalCompositor | [metal_compositor.mm](../../platform/macos/src/metal_compositor.mm) | Draws a `ComposedFrame`: letterbox, layers with geometry, effects, blend, blur, text |
| ImageIoLoader | [image_loader.mm](../../platform/macos/src/image_loader.mm) | Still image → IOSurface-backed BGRA CVPixelBuffer (EXIF orientation applied) |
| AvfExportSink | [avf_export_sink.mm](../../platform/macos/src/avf_export_sink.mm) | AVAssetWriter: composes into writer pixel buffers (H.264), PCM → AAC |

### Adapter interfaces ([adapters.h](../../core/include/mf/adapters.h))

```text
IClock         nowNs()
IDemuxer       open(src) -> MediaInfo; peekDtsUs(track); read(track) -> Packet; seekTo(us)
IVideoDecoder  configure(track, onOutput)   // again for a lane's next item, once drained or flushed
               queue(pkt); signalEos(); dequeue() -> frame; flush()
IAudioDecoder  configure(track); decode(pkt) -> PCM; flush()
IDisplay       attach(view, onPresented); present(ComposedFrame, atNs); visible; vsyncPeriodNs; latencyNs; vsyncGridNs
ISpeaker       open(rate, ch, ring); start(); pause()
IImageLoader   load(src) -> VideoFrame (platform surface), width, height
IExportSink    open(target, settings, rate, ch); writeVideo(ComposedFrame); writeAudio(pcm, frames, pts)
               endAudio(); finish(done)                   // writes return Again instead of blocking
Stage          pump() -> Did | Idle | WaitUntil(ns)       // never blocks
IScheduler     start(stages); wake(id); stop()
PlatformFactory  create*() for each adapter + clock(); export sink and image loader may be null
```

## 2. Component ownership and lifetime

### Ownership tree

```text
Controller (app, main thread)
├── unique_ptr<PlatformFactory>   lives for the whole app; owns the HostClock
├── unique_ptr<Listener>          raw pointer handed to the Player (not owned by it)
└── unique_ptr<Player>            one per opened file or scene; replaced on each open
    └── unique_ptr<Impl>          (is the PipelineEvents sink)
        ├── Context ctx           (by value)
        │   ├── IClock&           → factory's clock (borrowed)
        │   ├── PipelineEvents&   → Impl (borrowed)
        │   ├── unique_ptr speaker, display        (playback)   | exportSink (export)
        │   ├── unique_ptr scheduler
        │   ├── Scene scene, SceneLayout layout (points into scene)
        │   ├── vector<ItemRuntime> items           (created in probe)
        │   │     unique_ptr<IDemuxer> (video/audio items), MediaInfo, image frame, text
        │   ├── vector<unique_ptr<Lane>> lanes      (created in probe, one per layout lane)
        │   │     unique_ptr videoDecoder, audioDecoder
        │   │     videoPackets, audioPackets, frames   (BoundedQueues)
        │   ├── composed                  (BoundedQueue<ComposedFrame>, TC → T3)
        │   ├── AudioRing ring    (buffer allocated in probe, before playback)
        │   ├── MasterClock master (holds const AudioRing&)
        │   └── Metrics
        └── array<unique_ptr<Stage>, 5> stages      (each holds Context&)
            └── CompositionStage: FrameSampler, unique_ptr<CompositionDriver> (made once probed)

Exporter (headless --export)
└── unique_ptr<Impl>   same Context (forExport = true: exportSink instead of speaker + display)
```

### Resource lifetimes

- **Decoded frames:** a `VideoFrame` holds a retained `CVPixelBuffer` in a `shared_ptr<void>`. It is copied through the reorder map, the lane's `frames` queue, the sampler's head and per-item latest frame, and into each `ComposedLayer` of every `ComposedFrame` that shows it. Those frames pass through the `composed` queue, T3's `current_`/`lastShown_`, and the display's pending queue. `MetalCompositor::encode` keeps a copy of the whole `ComposedFrame` (and the CVMetalTextures) until the command buffer completes, so a buffer is released only after the GPU has finished with it.
- **Image items:** decoded once while probing into `ItemRuntime.image` (BGRA CVPixelBuffer), which lives as long as the Context. Every layer that draws it shares that one buffer.
- **Text items:** `ItemRuntime.text` is a `shared_ptr<const string>`, shared by every layer. The compositor rasterizes text with Core Text and caches the textures (most recent first), keyed on text, style and size.
- **Media handles:** `MediaSource` holds a retained `NSURL`. `TrackInfo.format` holds a retained `CMFormatDescription`, and `VtVideoDecoder` keeps its own `formatHolder_` reference.
- **Demuxers:** one per video or audio item, opened in probe (those of duration-0 items first, to learn their lengths, and handed over). A new AVAssetReader is created on open and on every seek.
- **VT session:** created in `configure()`. It is reused for the next item on the lane when `VTDecompressionSessionCanAcceptFormatDescription` says so, otherwise recreated. It is recreated once if it becomes invalid (after sleep or a GPU switch), and destroyed in the decoder's destructor, which waits for in-flight frames.
- **Metal `Sink`:** held in a `shared_ptr` that outlives `MetalDisplay`, because GPU presented handlers can fire after the display is destroyed. The destructor clears `sink->fn` under the sink's lock.
- **CVDisplayLink:** started in `attach()`. `CVDisplayLinkStop` in the destructor waits for any running callback.
- **Export sink:** its destructor cancels, drains both dispatch queues, and waits for `finishWriting`'s completion, so no callback runs after it returns. An unfinished file is cancelled.

### Construction and destruction order

```text
Impl():   ctx(factory)          // creates speaker + display (or export sink) + scheduler,
                                //   hooks the composed queue to TC / T3 wakes
          stages = makeStages(ctx)   // Source, VideoDecode, Composition, VideoRender, Audio
          scheduler.start(stages)    // threads start; all stages idle

T1 probe: layout.build; items (demuxers, images); lanes (decoders, queues with wake hooks)

~Impl():  shutdown()             // joins threads, releases adapters (see §4)
          ~stages
          ~ctx                   // lanes, items, queues, ring, metrics, scheduler object
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
- `open()` in Start, once. `open(Scene)` validates the scene first and returns `InvalidArgument` with the reason. Media problems arrive later through `onError`.
- `play()` in Ready. If `ended` is set, it first requests a seek to 0.
- `pause()` in Play.
- `seek()` in Ready. It clears `ended` and finishes asynchronously with `onSeekCompleted`.
- `setFilter()` in any state before Shutdown. A paused frame is redrawn with the new filter.

### Exporter state

```text
start() ── started ── T1 probe ── frames + audio written ── sink.finish ── onEnd ──► finished, completed ── onCompleted
                                                     any stage ── onFatal ──► finished ── onError
shutdown() ── stopped   (cancels an unfinished file; idempotent)
```

`progress()` is `writtenUs / durationUs` until completed.

### Pipeline state (in Context)

| Field | Meaning | Written by |
| --- | --- | --- |
| `driver`, `autoDriver` | Output driver; Auto for a scene is resolved in probe | owner (`open`/`start`), T1 |
| `scene` | What to play; T1 fills in the lengths of duration-0 items | owner, T1 |
| `openRequested` | Tells T1 to probe | owner |
| `layout`, `items`, `lanes`, `width`/`height`, `fpsNum`/`fpsDen` | Probe result, read-only afterwards | T1 |
| `probed`, `hasAudio`, `durationUs` | Probe finished; some item's audio is mixed; timeline length | T1 |
| pending seek slot | Latest requested seek, or none | owner (`seek`/`play`), T1 (`probe`) |
| seek target | Seek being executed, with each lane's first item (`lanePos`) | T1 (`startSeek`) |
| `serial` | Latest seek started | T1 |
| `shownSerial` | Latest seek completed | T3 |
| `shownPtsUs` | Timeline time of the last frame shown | T3, owner (`onEnd`) |
| filter, `filterVersion` | Global filter; bumped on each change | owner (`setFilter`) |
| `playing` | User wants playback | owner / events, under `playMu` |
| `outputRunning` | Speaker started and clock running | T3 (and shutdown / fatal), under `playMu` |
| `audioWritten`, `writtenUs` | Export progress | T4, T3 |
| `halted` | Fatal error or shutdown; stages go idle | Player / Exporter |

Derived states:
- **Seeking:** `shownSerial != serial`.
- **Output should run:** `playing && !seeking && no pending seek`. T3 applies this on every pump.

### Seek lifecycle

```text
requested    owner: pending = {target, now}                      (latest wins; overwrites)
   │ T1: when shownSerial == serial (previous seek finished)
started      T1: flush every lane's packet queues; per lane, the first item not ended by the target
                 (lanePos) and demuxer.seekTo(its media time); target = pending; serial++; wakeAll
   │ each stage sees serial != its local serial
flushing     T2: per lane decoder.flush()
             TC: sampler.restart(serial, lanePos); driver.restart()
             T3: drop current
             T4: per lane decoder.flush; cursor = target; ring.flush(target, serial)
   │ TC finds the frame to show (seekStep) and emits it
completed    T3: present frame; master.reset(pts, serial); shownSerial = serial; onSeekDone; wake T1
```

Serial 1 is the preroll started by `probe()`. When it completes, the Player moves from Start to Ready. Export has only serial 1 and its driver skips the seek frame.

### Stage-local state

- **T1:**
  - `opened_`: whether `probe()` has run.
  - `reads_[lane]`: position in the lane's item list, per-track `eos[2]`.
  - `serial_`: stamped on each packet.
  - `preopened_`: demuxers of duration-0 items, opened before the layout exists.
- **T2 (per lane):**
  - `serial`
  - `item`: the item the decoder is configured for.
  - `drained`: nothing left in the decoder, so it can be reconfigured for the next item.
  - `input`: packet held because the decoder was full.
  - `output`: frame held because the lane's frames queue was full.
  - `skipToKey`: after a corrupt frame, skip to the next keyframe.
  - `eosSent`
- **TC:**
  - `serial_`, `targetUs_`, `seeking_`, `ended_`
  - `out_`: composed frames waiting for room in `composed`.
  - FrameSampler, per lane: the lane's video items (`seq`), the one being delivered (`pos`), `head` frame, each item's `latest` frame, `done`.
  - VsyncDriver: next slot, own grid when the display has none, `last_` output (to skip unchanged refreshes).
  - ExportDriver: frame `index_`.
- **T3:**
  - `serial_`
  - `current_`: frame being handled. `lastShown_`: for a filter redraw while paused.
  - `videoEnded_`
  - `outputRunning_`: copy of `ctx.outputRunning`.
  - AvSync anchor and last slot (leading-clip driver); slot anchor (vsync driver).
  - `finished_` (export).
- **T4:**
  - `serial_`
  - per lane: mixed items (`seq`), current `pos`, `ended`, configured decoder item, decoded samples `buf` from `bufStart` at the item's own rate.
  - `cursor_`/`endSample_`: timeline samples at the output rate.
  - `chunk_`/`offset_`: mixed PCM not yet fully written.
  - `eosMarked_`
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

### Startup (playback)

```text
main thread (owner)
 1. app: platform = createPlatform()
 2. openPaths / openScene: player = Player::create(platform, listener)
      Context: create speaker, display, scheduler; hook composed queue to TC / T3
      makeStages; scheduler.start → spawn T1, T2, TC, T3, T4 (named, QoS set), each pumps once → idle
 3. player.open(scene | source, view, driver)       // returns immediately
      validateScene (rules that need a duration-0 item's length wait for probe)
      display.attach(view): Metal device/queue, MetalCompositor init (pipelines per source and
                            blend mode, blur, texture cache), CVDisplayLink start,
                            occlusion + screen observers
      driver; metrics.startTtff;
      openRequested = true; wake(T1)

T1
 4. probe():  duration-0 items: open the demuxer, duration = (file length − in) / speed
              validateScene; layout.build (lanes ≤ 8, else fatal)
              per lane: video + audio decoder, queues with wake hooks
              per item: text → shared string; image → imageLoader.load;
                        video/audio → demuxer.open; video: configure the lane's decoder once
                        (so a rejected stream fails here); audio: configure, decide mixAudio
              canvas = scene.output or the first video's (or image's) size; fps likewise
              audio mixed: ring.open(rate, ch, 200 ms); speaker.open (AU init, latency)
              Auto driver: one visual item and it is video → LeadingClip, else Vsync
              hasAudio; master.setAudio; durationUs; probed = true; requestSeek(0)
 5. startSeek(serial 1) → demux packets on every lane

T2 → TC → T3
 6. decode → lane frames → TC seekStep → composeAt(first frame) → composed → T3 complete → present now
 7. onSeekDone(1) → state Ready → listener.onFirstFrame, onStateChanged(Ready)

owner
 8. play(): state Play; playing = true; wakeAll
T3
 9. updateOutput: speaker.start; master.start(now); sync.reset; metrics.discontinuity;
                  vsync driver: wake TC (it composes only while output runs)
```

### Startup (export)

```text
owner:  exporter = Exporter::create(platform, listener)       // Context without display/speaker
        exporter.start(scene, target, settings)   // size and fps from scene.output
          driver = Export; openRequested = true; wake(T1)
T1:     probe as above, but exportSink.open(target, settings, rate, ch) instead of the speaker
TC:     ExportDriver from t = 0 (no seek frame), frames on the n / fps grid
T3:     exportStep → sink.writeVideo                T4: mix → sink.writeAudio, then endAudio
T3:     video ended and audioWritten → sink.finish → onEnd → listener.onCompleted
```

### Shutdown

```text
owner: shutdown()    (also called from ~Impl; idempotent)
 1. state = Shutdown; callbacksOn = false          // no more listener callbacks
 2. halted = true                                  // every pump returns Idle
 3. scheduler.stop(): running = false; wake all; join T1, T2, TC, T3, T4
 4. under playMu: speaker.pause if running; outputRunning = false
 5. release adapters in order:
      speaker       AudioOutputUnitStop, uninitialize, dispose   (RT callback stops)
      display       remove observers; CVDisplayLinkStop (waits for callback); sink.fn = null
      per lane      videoDecoder (wait for async frames; invalidate session), audioDecoder
      per item      demuxer (release reader / asset)
 6. print metrics
then ~Impl destroys stages and Context (lanes, items, queues, ring, scheduler object)
```

The Exporter's `shutdown()` does the same with the export sink in place of the speaker and display. Resetting the sink cancels an unfinished file.

Other paths:
- **Fatal error (`onFatal`, any stage):** sets `halted`, `playing = false`, pauses the speaker, stops the master clock, and sets state to Error. The threads stay alive and idle until `shutdown()`. For export, `finished` is set and `onError` is called once.
- **App:** `openPaths` and `openScene` shut down the previous Player before creating a new one. `applicationWillTerminate` calls `shutdown()`.
- **Deadlock rule:** listener callbacks run on internal threads and must never wait on the owner thread, because `shutdown()` joins those threads. The demo posts every callback to the main queue with `dispatch_async`.

## 5. Concurrency and synchronization

### Threads

| Thread | QoS | Runs |
| --- | --- | --- |
| Owner (main) | — | Player / Exporter API, AppKit, window occlusion / screen notifications for the display |
| T1 `mf.source` | USER_INITIATED | SourceStage: probe (blocking asset key loads and image decodes happen here), seeks, demux |
| T2 `mf.video-decode` | USER_INITIATED | VideoDecodeStage, every lane |
| TC `mf.composition` | USER_INITIATED | CompositionStage: exact seek, driver steps, `composeAt` |
| T3 `mf.video-render` | USER_INTERACTIVE | VideoRenderStage; `onSeekDone`, `onEnd` |
| T4 `mf.audio` | USER_INTERACTIVE | AudioStage |
| VideoToolbox callback | system | `decoded()` → reorder map → `onOutput` (wakes T2) |
| CoreAudio render (real-time) | real-time | `ring.consume` (no locks, no allocation) |
| CVDisplayLink | system | `vsync()` → pick frame → MetalCompositor encode (`nextDrawable` may block, but only here) |
| Metal handlers | system | presented handler → `Sink` → `Player.onPresented` → metrics |
| `mf.export-video` / `mf.export-audio` | dispatch serial queues | export sink: compose into writer buffers and append; append audio |

### Scheduling model ([thread_scheduler.cpp](../../core/src/thread_scheduler.cpp))

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
  - `videoPackets` (per lane): T1 → T2
  - `audioPackets` (per lane): T1 → T4
  - `frames` (per lane): T2 → TC
  - `composed`: TC → T3
- The VT output callback wakes T2.
- A completed seek (T3) wakes T1.
- T3 wakes TC when output starts with the vsync driver; T4 wakes T3 when export audio is complete.
- `play()`, `setFilter()`, `startSeek` and the seek request each wake the stages they affect.

### Locks and atomics

| Primitive | Protects | Contenders |
| --- | --- | --- |
| `Impl::stateMu` | `state`, `ended`, `openCalled` | owner, T1/T3 (events) |
| `ctx.playMu` | `playing` writes, `outputRunning`, speaker start/pause and master start/stop together | owner, T3, TC (vsync driver reads `outputRunning`) |
| `Context::seekMu_` | pending seek, seek target | owner, T1, TC, T3, T4 |
| `Context::filterMu_` | global filter | owner, TC, T3 |
| `BoundedQueue::mu_` (3 per lane + `composed`) | deque + byte count | producer / consumer stage |
| `MasterClock::mu_` | clock base, `running`, `useAudio` | TC, T3, owner, display handler |
| `Metrics::mu_` | counters, histograms, plan ring | T2, TC, T3, Metal handler, owner |
| `Worker::mu` + cv (×5) | `woken` flag | any waker, the worker |
| `VtVideoDecoder::mu_` | in-flight map, reorder map, generation, errors, eos | T2, VT callback |
| `MetalDisplay::mu_` | pending frames | T3, display link |
| `Sink::mu` | presented callback | Metal handlers, display destructor |
| Atomics | `serial`, `shownSerial`, `shownPtsUs`, `halted`, `playing`, `probed`, `hasAudio`, `durationUs`, `openRequested`, `filterVersion`, `audioWritten`, `writtenUs`, `hasPending_`, `callbacksOn`, display `visible_`/`vsyncNs_`/`latencyNs_`/`gridNs_`, export sink `queuedVideo_`/`queuedAudio_`/`failed_`/`cancelled_` | — |

The probe results (`layout`, `items`, `lanes`, canvas, fps) are written by T1 before `probed` is set and never change afterwards, so the other stages read them without locks once they see `probed`.

### AudioRing (lock-free SPSC)

- **Indices:**
  - `write_` is advanced only by T4.
  - `read_` is advanced only by the real-time consumer.
  - Both use acquire/release ordering.
- **Flush:** the producer never moves `read_`. It publishes a flush record `{gen+1, writeIndex, basePts, tag}` under a seqlock. On its next callback, the consumer sees the new generation and jumps `read_` to that record's `writeIndex`.
- **Clock snapshot:** after each callback that delivered audio, the consumer publishes `{gen, framesBefore, frames, audibleNs}` under a second seqlock. Any thread can read it.

### Invalidation without blocking

- Packets and frames carry the `serial` they were read under, and the item they belong to. Each stage drops items whose serial doesn't match its current one.
- `VtVideoDecoder::flush()` increments `generation_` and clears the reorder map without waiting. Frames from an old generation that arrive late are discarded in the callback.
- **Publication order in `startSeek`:** the target (with `lanePos`) is set before `serial` is incremented, so a stage that sees the new serial reads the correct target.

## 6. Timing and ordering

### Time bases

- **Host time (ns):** `CLOCK_UPTIME_RAW`. It uses the same base as `mach_absolute_time`, CoreAudio `mHostTime` and Core Animation / `CVTimeStamp`.
- **Timeline time (µs):** seek targets, clock values, `ComposedFrame.ptsUs`, item start/end.
- **Media time (µs):** packet and frame PTS/DTS, in each item's file. `SceneLayout` maps between them: `media = in + (t − start) × speed`, and back.
- **Item-local time:** `t − start`, at which keyframes are evaluated.

### Master clock ([master_clock.cpp](../../core/src/master_clock.cpp))

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

### Audio clock ([audio_ring.cpp](../../core/src/audio_ring.cpp))

- The speaker's render callback passes `audibleHostNs = callbackHostTime + output latency`. The latency is device + stream + AudioUnit latency, measured once in `open()`.
- `clockUs(now) = basePts + clamp(framesBefore + (now - audibleNs)·rate, 0, framesBefore + frames) / rate`
  - It extrapolates up to ±1 s.
  - It never moves past the last frame delivered, so the clock holds during an underrun.
- `drained(now)` is true when:
  - EOS has been marked,
  - `read == write`,
  - and `now` is past the point where the last delivered frame is heard.

### Layout ([layout.cpp](../../core/src/layout.cpp))

- **Items** of enabled tracks are numbered in track order, then item order.
- **Lanes:** video and audio items are sorted by start and coloured greedily, so items sharing a lane never overlap. Each item's interval is first widened by 1 s of preroll, so its decoder can start before it plays; if that needs more than 8 lanes it retries without the margin, and fails above 8.
- **`visibleAt(t)`:** per enabled video track, bottom to top, the item playing at t, or both items of a transition with their offset (push, slide), clip rect (wipe) or fades (crossfade: `1 − p` and `p`, summed), eased.
- **Groups (`composeAt`):** a track whose pair is visible, or that has effects, becomes a `ComposedGroup` (track opacity, top item's blend, track effects at scene time). Its layers carry only their own opacity, and blend `Normal` (B over A) or `Add` (crossfade). Any other track's layer carries the track opacity and its item's blend, and is drawn onto the canvas directly.
- **`transitionGain(i, t)`:** fade in and out over the item's transitions (equal gain, equal power or cut).

### Ordering through the pipeline

- **Demux:** each call reads one packet, from the lane and track with the lowest timeline DTS among those that have queue space and haven't ended. A lane reads its items in order; once both tracks of an item end (or pass its end by 1 s, for B-frame reordering), it seeks the next item's demuxer to its start. Audio of an unmixed item is read and dropped, to keep the reader's tracks balanced.
- **Video decode:** per lane. VideoToolbox returns frames in decode order. `dequeue()` releases frames in PTS order once the reorder map holds more than the DPB depth (worked out from the avcC level and frame size; 0 for baseline), or when draining at EOS. Each item's EOS goes through as an EOS frame, and the decoder is reconfigured for the next item only once drained.
- **Composition:** the FrameSampler consumes each lane's frames in order, keeps each item's latest frame at or before the output time, and drops frames past an item's end. A layer is **exact at t** when its next frame is later than t, or its item has ended.
- **Audio:** decoded synchronously in packet order, per lane, into a float buffer at the item's own rate. Packets within 1 ms of the previous one continue it exactly; gaps become silence. The mix waits until every item sounding in the next 1024-frame chunk is decoded that far.

### Output drivers ([drivers.h](../../core/src/drivers.h))

**LeadingClip** (the default for a single video). Frames go in timeline order, once no other lane can still produce an earlier one. Only a frame of the leading item (highest frame rate among the visible videos, the upper one on a tie) produces an output, so every other layer is exact at that time. T3 paces them with AvSync.

**Vsync** (the default for anything else). Composes only while output runs:

```text
slot      = first vsync on the display's grid (or our own) ≥ max(now + latency, nextSlot)
compose at  slot − latency − period/2          (WaitUntil before that)
t         = master.nowUs(now) + (slot − now)   // the clock time the frame will be seen
t ≤ last output         -> skip (clock holding)
advanceAll(t); a visible layer whose decode is behind keeps its last frame (counted as a late layer)
nothing visible changed -> skip (the frame on screen stays)
emit frame with presentAtNs = slot
```

**Export:** `t = index · fpsDen / fpsNum`; wait until every visible layer is exact at t; emit; next index. It starts at 0 without a seek frame.

### Video presentation

**AvSync** ([av_sync.cpp](../../core/src/av_sync.cpp)), for leading-clip frames (`presentAtNs == 0`):

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

**Scheduled** (vsync-driver frames): present at `presentAtNs`, or count it late if that vsync has already passed. The filter is applied at present time, so a change shows at once; while paused, T3 redraws the last frame when `filterVersion` changes. A hidden window drops frames (`countHidden`).

**MetalDisplay** ([metal_display.mm](../../platform/macos/src/metal_display.mm)) runs on the display link:

```text
present(frame, atNs): push {frame, atNs}; more than 4 pending -> oldest reported not shown
vsync(now, output):   period, grid = output time, latency = output - now + period
                      show = latest pending with atNs <= output + period/2; earlier ones -> late
                      compositor.encode(show, drawable) → presentDrawable; presented handler reports actual time
```

**MetalCompositor** ([metal_compositor.mm](../../platform/macos/src/metal_compositor.mm)), shared by the display and the export sink:

```text
canvas (scene output size) letterboxed into the target; clear; per layer bottom to top:
  prepare: textures (NV12 → R8 + RG8, BGRA image, cached text texture, or none for a color),
           crop, fit to the canvas, scale + rotate about the anchor at (x, y) + transition offset
  blurred layers first: source + effects into a texture, two Gaussian passes (≤ 64 taps a side)
  groups next: the group's layers into a transparent, target-sized texture (kept across frames),
        over or plus; then its crop, and its blur like a blurred layer
  draw: scissor to the wipe clip; fragment shader: chroma key → color adjust → global filter
        (video and image) → opacity; pipeline per blend mode (normal, add, multiply, screen)
        a group is drawn once, where its first layer would be: its texture with the track's
        chroma key → color adjust → opacity, in the top item's blend mode
```

### Seek ordering

- **Latest wins:** there is one pending slot, and a new request overwrites the old one.
- **One at a time:** the next seek starts only after the previous one has been shown (`shownSerial == serial`).
- **Exact seek (TC):** each lane starts from the sync sample at or before its item's media time for the target. The frame shown is the leading item's last frame at or before the target (its first frame if none is), composed once every visible layer is exact at that time. Frames replaced in the meantime are counted as `decodeOnly`. If nothing can be shown, the seek completes without a frame.
- **Scrubbing:** if a newer seek is already pending, TC shows whatever it has decoded right away instead of waiting for exact layers.
- Output restarts only when no seek is in flight or pending. On restart, AvSync is reset and metrics mark a discontinuity.

### End of stream

TC emits an EOS frame when the driver reaches the end: every lane has delivered all its items (leading clip), or `t ≥ duration` (vsync, export). T3 reports the end only when both conditions hold:
- The EOS frame has been reached.
- There's no audio, or the ring is drained. While waiting for the drain, T3 polls every 10 ms.

On end, the Player calls `stopPlaying()`, sets `shownPts = duration` and `ended = true`, and moves to Ready. On export, T3 waits for `audioWritten` instead, then calls `sink.finish`.

## 7. Performance-critical path

### Video, per frame

```text
T1   copyNextSampleBuffer → split → memcpy sample bytes into Packet.data       (copy 1)
T2   CMBlockBuffer alloc + copy → CMSampleBuffer → VTDecodeFrame (async, HW)    (copy 2)
VT   decoded into IOSurface-backed NV12 CVPixelBuffer                            (zero-copy from here)
     → reorder multimap → onOutput wakes T2 → lane frames queue
TC   composeAt(t): per visible item, a ComposedLayer holding a reference to its frame and
     its keyframed values evaluated at t; no pixels touched
T3   AvSync.decide: O(1) arithmetic; present() only enqueues under a short lock
DL   CVMetalTextureCache wraps Y and UV planes (and image BGRA) as textures (no copy)
     one draw per layer (plus two passes for a blurred one), BT.709 NV12→RGB in the shader
     nextDrawable (may block up to 1 s) runs only on the display-link thread, never on T3
Export: composes into the writer pool's IOSurface buffer and waits for the GPU on the
     export queue only; the pipeline threads never wait
```

### Audio real-time path

`consume()` runs in the render callback. It does:
- two seqlock reads, which never block;
- a memcpy (two chunks if the data wraps around the ring);
- a silence pad on underrun;
- one seqlock write.

It takes no locks, makes no allocations and makes no system calls. The ring holds 200 ms. When the ring is full, T4 polls every 5 ms while playing, because the real-time thread can't signal it. Mixing (resampling by linear interpolation, gain and pan ramps) runs on T4, 1024 frames at a time.

### Bounded buffering

These caps bound both memory and latency:

| Buffer | Cap |
| --- | --- |
| Lanes | 8 |
| `videoPackets` (per lane) | 60 packets / 32 MB / 2 s |
| `audioPackets` (per lane) | 120 packets / 1 MB / 2 s |
| VT in flight (per lane) | 4 |
| Reorder map (per lane) | DPB depth (≤ 16) |
| `frames` (per lane) | 4 |
| `composed` | 4 |
| Display pending | 4 |
| CAMetalLayer drawables | 3 |
| AudioRing | 200 ms |
| Export sink queued | 3 video frames, 8 audio chunks |
| Metrics | fixed 2001-bin histograms, 16-entry plan ring |

### Latency and wakeups

- Stages are driven by condition variables and sleep between wakes. Only a few waits use timeouts:
  - T3's clock wait is capped at 50 ms, so a clock that is holding still gets re-checked.
  - The vsync driver sleeps until half a refresh before each slot's hand-over time.
  - T4 polls every 5 ms when the ring is full.
  - The end check polls every 10 ms.
  - On export, T3 and T4 poll every 2 ms while the sink returns `Again`.
- Seek latency is kept low because:
  - the decoder flush never waits for in-flight frames;
  - a new AVAssetReader starts at the sync sample;
  - scrubbing shows intermediate frames without decoding up to the target.
- Lanes are assigned with 1 s of preroll, so an item's decoder usually starts before the item is visible.
- Thread priority: T3 and T4 run at USER_INTERACTIVE QoS; T1, T2 and TC at USER_INITIATED.
