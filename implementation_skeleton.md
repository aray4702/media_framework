# Implementation Skeleton

Pseudocode for the source in `core/`, `platform/macos/` and `apps/macos-demo/`. It shows the functional logic only: error handling, validation and fatal/warning branches are left out, and tests are not covered.

## Layout

```text
core/include/mf/   types.h, adapters.h (interfaces), player.h, audio_ring.h, thread_scheduler.h
core/src/          player.cpp, pipeline.{h,cpp} (4 stages), av_sync, master_clock,
                   audio_ring, bounded_queue.h, metrics, thread_scheduler, types
platform/macos/    mac_platform, avf_demuxer, vt_video_decoder, at_audio_decoder,
                   au_speaker, metal_display
apps/macos-demo/   main.mm (AppKit UI + --autotest)
```

## Data flow

```text
          T1 Source                 T2 VideoDecode            T3 VideoRender          display link
Demuxer ──► videoPackets ──► VT decoder ──► frames(4) ──► AvSync ──► Display.present ──► Metal draw
        └─► audioPackets ──► T4 Audio: AAC decode ──► AudioRing ──► AU render callback (speaker)
                                                           └──► MasterClock (audio clock) ──► T3
```

## Core: interfaces ([adapters.h](core/include/mf/adapters.h))

```text
IClock        nowNs()
IDemuxer      open(src) -> MediaInfo; peekDtsUs(track); read(track) -> Packet; seekTo(us)
IVideoDecoder configure(track, onOutput); queue(pkt); signalEos(); dequeue() -> frame; flush()
IAudioDecoder configure(track); decode(pkt) -> PCM; flush()
IDisplay      attach(view, onPresented); present(frame, atNs); visible; vsyncPeriodNs; latencyNs; vsyncGridNs
ISpeaker      open(rate, ch, ring); start(); pause()
Stage         pump() -> Did | Idle | WaitUntil(ns)
IScheduler    start(stages); wake(id); stop()
PlatformFactory  creates all of the above + clock()
```

## Player ([player.cpp](core/src/player.cpp))

```text
create(): ctx = Context(factory); stages = makeStages(ctx); scheduler.start(stages)

open(src, view):  display.attach(view, onPresented); ctx.source = src
                  metrics.startTtff(); ctx.openRequested = true; wake(Source)
play():           state = Play; if ended: requestSeek(0)
                  ctx.playing = true; wakeAll()
pause():          state = Ready; stopPlaying()
seek(us):         requestSeek(clamp(us, 0, duration)); wake(Source)
shutdown():       halted = true; scheduler.stop(); speaker.pause(); release adapters; print metrics
position():       Play ? master.nowUs() : shownPtsUs

onSeekDone(serial, pts):  serial == 1 (preroll) -> state = Ready; onFirstFrame
                          else -> onSeekCompleted(pts)
onEnd():                  state = Ready; ended = true; stopPlaying(); shownPts = duration; onEnded
onPresented(pts, ns):     av = pts - clockAt(ns); metrics.presented(pts, ns, av)
stopPlaying():            playing = false; master.stop(); wake(VideoRender)
```

State machine: `Start → (preroll) → Ready ⇄ Play`, and any state can go to `Error` or `Shutdown`.

## Context ([pipeline.h](core/src/pipeline.h))

These are the pieces shared between the Player and the stages:

- Adapters.
- Queues:
  - `videoPackets`: capped at 60 items, 32 MB or 2 s.
  - `audioPackets`: capped at 120 items, 1 MB or 2 s.
  - `frames`: capped at 4.
- `ring`, `master`, `metrics`.
- `serial` (the latest seek started) and `shownSerial` (the latest seek completed).
- `playing`, `outputRunning`.
- A single pending-seek slot: `requestSeek`/`takeSeek`, where the latest request wins.

Queue hooks: when an item is pushed, the consumer stage is woken. When space frees up, the producer stage is woken.

## Stages ([pipeline.cpp](core/src/pipeline.cpp))

### T1 SourceStage

```text
pump:
  if !opened && openRequested: probe()
  if shownSerial == serial && (seek = takeSeek()): startSeek(seek)
  else readOne()

probe:      info = demuxer.open(src)
            videoDecoder.configure(info.video, wake(VideoDecode))
            if audio: audioDecoder.configure; ring.open(rate, ch, 200ms); speaker.open(ring)
            master.setAudio(hasAudio); requestSeek(0)          // preroll the first frame
startSeek:  setSeekTarget; flush packet queues; demuxer.seekTo(target); serial++; wakeAll
readOne:    among tracks that aren't at EOS and whose queue isn't full, pick the lowest DTS
            read the packet, stamp it with the serial, push it   (at EOS: push an EOS marker)
```

### T2 VideoDecodeStage

```text
pump:
  if serial changed: decoder.flush(); drop held input/output
  drainOutput():  dequeue frame (same serial) -> frames.tryPush
                  Eos -> push an EOS frame;  CorruptFrame -> skipToKey
  feedInput():    pop packet (same serial)
                  eos -> decoder.signalEos()
                  skipToKey && !key -> drop
                  else decoder.queue(pkt)                  // Again -> keep it and retry later
```

### T3 VideoRenderStage

```text
pump:
  if serial changed: reset current/candidate
  updateOutput(seekDone): playing && seek done -> speaker.start + master.start + sync.reset
                          else -> speaker.pause + master.stop
  current = frames.pop()
  seeking      -> seekStep()
  current.eos  -> checkEnd()
  else         -> renderStep()

seekStep (exact seek):   keep the last frame with pts <= target as the candidate
                         the first frame past the target, a newer pending seek, or EOS -> complete(candidate)
complete(frame):         display.present(frame, now); shownPts = pts; master.reset(pts, serial)
                         shownSerial = serial; onSeekDone; wake(Source)
renderStep:              hidden -> drop
                         clock = master.nowUs() + displayLatency
                         d = AvSync.decide(pts, clock, now+lead, vsync, frameDur, grid)
                         LateDrop / RateCapDrop -> count;  Wait -> waitUntil(min(at, now+50ms))
                         Present -> metrics.planned; display.present(frame, d.atNs)
checkEnd:                video ended && (no audio || ring.drained) -> onEnd()
```

### T4 AudioStage

```text
pump:
  if serial changed: decoder.flush(); trim = seekTarget; ring.flush(trim, serial)
  if pending pcm: ring.write(rest)                 // ring full -> poll every 5 ms while playing
  pkt = audioPackets.pop()
  eos -> ring.markEos()
  else pcm = decoder.decode(pkt); skip samples before trim; hold pcm for the next write
```

## Timing pieces

### AvSync.decide ([av_sync.cpp](core/src/av_sync.cpp))

```text
delta = pts - clock
delta < -frameDur                -> LateDrop
target = now + delta; slot = round((target - anchor) / vsync)
slot <= lastSlot                 -> RateCapDrop
delta > vsync                    -> Wait(target - vsync)
first frame: anchor = target; presentAnchor = snap(target) to the vsync grid
lastSlot = slot                  -> Present(presentAnchor + slot*vsync)
```

### MasterClock ([master_clock.cpp](core/src/master_clock.cpp))

```text
nowUs: !running          -> base
       using audio       -> ring.clockUs() (held at the base until this seek's audio is heard;
                            once the ring is drained, switch to the steady clock)
       otherwise         -> base + (now - baseNs)
start/stop freeze or unfreeze the value; reset(base, serial) happens after a seek
```

### AudioRing ([audio_ring.cpp](core/src/audio_ring.cpp))

A lock-free ring buffer with one producer and one consumer.

```text
write (T4):      copy into the ring, advance write_
flush (T4):      publish a seqlock record {gen+1, writeIndex, basePts, tag}
consume (RT cb): on a new gen, jump read_ to that record's writeIndex; copy out; pad with silence
                 publish a seqlock snapshot {gen, framesBefore, frames, audibleNs}
clockUs:         basePts + (framesBefore + (now - audibleNs)*rate), clamped to what's been delivered
drained:         eos && read == write && the last delivered frame has been heard
```

### BoundedQueue ([bounded_queue.h](core/src/bounded_queue.h))

A mutex-guarded deque. It counts as full when it hits any of its count, bytes or duration caps. An empty queue always accepts one item.

### ThreadScheduler ([thread_scheduler.cpp](core/src/thread_scheduler.cpp))

One thread per stage.

```text
loop: p = stage.pump()
      Did -> loop again;  Idle -> cv.wait(woken);  WaitUntil -> cv.wait_for(deadline)
```

### Metrics ([metrics.cpp](core/src/metrics.cpp))

Counters plus 1 ms histograms.

- `planned()` records a frame's slot; `presented()` matches it by pts.
- Jank is counted when the actual vsync gap is bigger than the planned slot gap.
- Also tracks the A/V offset, TTFF and seek latency.

## macOS adapters

### [mac_platform.mm](platform/macos/src/mac_platform.mm)

- The factory. Host clock is `CLOCK_UPTIME_RAW`. Uses `ThreadScheduler` and names the threads; the render and audio threads get the `USER_INTERACTIVE` QoS class.

### [avf_demuxer.mm](platform/macos/src/avf_demuxer.mm)

- `open`: loads the AVURLAsset keys and fills in the video/audio `TrackInfo`, then `startReader(0)`.
- `fill(track)`: `copyNextSampleBuffer`, then splits it into one `Packet` per sample (pts, dts, key flag, bytes).
- `seekTo`: an `AVSampleCursor` steps back to the sync sample, then a new `AVAssetReader` starts there.

### [vt_video_decoder.mm](platform/macos/src/vt_video_decoder.mm)

- `queue`: tags the frame with an id (generation, serial), capped at 4 in flight, then `VTDecompressionSessionDecodeFrame` (async).
- callback: if it's the current generation, the frame goes into the `reorder_` multimap (keyed by pts); then `onOutput()`.
- `dequeue`: pops the smallest pts once there are more than DPB-depth frames, or while draining.
- `flush`: `++generation` and clears the reorder map.

### [at_audio_decoder.cpp](platform/macos/src/at_audio_decoder.cpp)

- Uses `AudioConverter` (AAC to S16) with the magic cookie. `decode` feeds exactly one packet through `FillComplexBuffer`.

### [au_speaker.cpp](platform/macos/src/au_speaker.cpp)

- A DefaultOutput AudioUnit (S16). The render callback calls `ring.consume(out, frames, hostTime + outputLatency)`.

### [metal_display.mm](platform/macos/src/metal_display.mm)

- `present`: queues `{frame, atNs}` (keeps at most 4; overflow is reported as not shown).
- On each CVDisplayLink vsync:
  - update period, grid and latency
  - take the newest frame due by this vsync (older due frames are reported late)
  - `draw`
- `draw`: NV12 → Metal Y/UV textures, then a BT.709 shader draws an aspect-fit quad; `presentDrawable` and `addPresentedHandler` report the actual present time.
- Window occlusion sets `visible`.

## Demo app ([main.mm](apps/macos-demo/main.mm))

- **VideoView**: an NSView backed by a `CAMetalLayer`.
- **Controller**:
  - Builds the window: video view, Open and Play buttons, seek slider, labels.
  - `openPath`: creates a Player and calls `open` with the file and the view.
  - `togglePlay`: plays or pauses.
  - `sliderMoved`: pauses while the slider is dragged, calls `seek` on each move, and resumes on mouse-up.
  - `tick` (30 Hz): updates the time label and the slider.
- **Listener**: forwards every player callback to the main thread with `dispatch_async`.
- **`--autotest` mode**:
  1. Plays for N seconds.
  2. Scrubs: 50 random seeks over 2 s.
  3. Runs 10 single seeks, 300 ms apart.
  4. Prints metrics and peak memory, then exits.
