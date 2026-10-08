# Porting the Media Editor to the Browser

Started October 7, 2026; all five steps done October 8, 2026. A record of the plan and its results, copied from the working doc ([Claude Doc](https://claude.ai/artifact/GCFF996jUVPmZpVLA1FxPc)). How to build, run and test the port is in [platform/web/README.md](../../platform/web/README.md).

A browser port is feasible. The portable core (about 6,600 lines of C++) compiles to WebAssembly almost unchanged; the work is about 2,400 lines of macOS adapters replaced by web adapters, about 6,100 lines of AppKit editor UI rewritten for the web, and three changes to the core. Target Chrome, Edge and Safari 26+ first, with Firefox partial.

## Architecture

The portable core moves to the browser as is; the UI and adapters are new.

| Layer | macOS today | Browser port |
| --- | --- | --- |
| Editor UI | AppKit views, 6,100 lines | New: a web UI on the same document model (step 4) |
| Portable core, reused | Pipeline stages, lane layout, A/V sync, seek and scrub, decode ladder: 6,600 lines. Editor document model and capture session: 700 lines. | The same code, compiled to WebAssembly. Changes for the browser: one-worker cooperative scheduler, asynchronous audio decoder |
| Adapters | macOS adapters, 2,400 lines: VideoToolbox decode; AVFoundation demuxing and export; Metal compositing and effect plugins; Core Audio output and camera capture | New web adapters replacing them: WebCodecs decode and encode; MP4 parser in WebAssembly, MP4 muxer; WebGPU compositing, WGSL effects; AudioWorklet output, getUserMedia camera |

Only the middle layer carries over: built natively on macOS, compiled to WebAssembly in the browser, behind the same adapter interfaces.

## Adapter mapping

Every platform API sits behind one of the core's adapter interfaces, so each row below is a new web adapter. Only the audio decoder's interface has to change shape.

| Interface | Browser API | Fit |
| --- | --- | --- |
| `IVideoDecoder` | WebCodecs `VideoDecoder` | Good: already queue/dequeue with an "output ready" callback |
| `IAudioDecoder` | WebCodecs `AudioDecoder` | Mismatch: the interface is synchronous, WebCodecs is asynchronous |
| `IDemuxer` | An MP4 parser compiled to WebAssembly, reading a `File` | Good in a worker, where file reads can be synchronous; AVFoundation's parser has no browser equivalent |
| `IDisplay` and compositor | WebGPU, with `importExternalTexture` for decoded frames | Works; the Metal shaders (about 600 lines) are rewritten in WGSL |
| `ISpeaker` and `AudioRing` | AudioWorklet reading the ring from shared memory | Good: the ring is already lock-free, one producer and one consumer |
| `IClock` | `performance.now()`, plus the AudioContext's output timestamp and latency | Good |
| `IExportSink` | WebCodecs `VideoEncoder` and `AudioEncoder`, an MP4 muxer, a file save | Works; the muxer is new code or a library |
| `IImageLoader` | `createImageBitmap` | Good |
| `ICamera` | `getUserMedia` and `MediaStreamTrackProcessor` | Chrome, Edge, Safari 18+ (workers only); not Firefox |
| Effect plugins | Plugins written as WGSL shaders | The registration API carries over; native `.dylib` plugins with Metal code do not |

## Core changes

The core needs three changes; none touches its playback logic.

1. **A cooperative scheduler.** Today each of the five stages has its own thread that sleeps on a condition variable. WebCodecs delivers decoded frames only when the worker's event loop runs, and a blocked WebAssembly thread never lets it. All five stages would instead run on one worker, pumped from its event loop, with timers for "wake me at X". This works because every stage's `pump()` already returns immediately. `SegmentRecorder`, which has its own blocking thread, needs the same treatment.
2. **An asynchronous audio decoder interface.** `IAudioDecoder` takes the queue/dequeue shape of `IVideoDecoder`, and the audio stage pulls decoded audio instead of decoding inline. The macOS adapter implements it trivially.
3. **New meanings for the opaque handles.** `MediaSource` becomes a file handle and `RenderTarget` a canvas. The core already treats both as opaque, so its code does not change.

## Risks

The first two are unknowns that step 1 measures; the rest are known constraints.

| Risk | Impact | Response |
| --- | --- | --- |
| Decoder frame pool | Each lane queues up to 4 decoded frames and up to 8 lanes play at once; a browser hardware decoder may stall when that many are held | Measure in step 1; shrink the frame queues if needed |
| Cooperative scheduling on one worker | Composition, demuxing and audio mixing share one thread | Measure frame pacing in step 1 with 2 to 4 lanes |
| Cross-origin isolation | Shared memory between the worker and the AudioWorklet needs COOP/COEP headers, which constrains hosting and third-party content | Serve with those headers; no fallback planned |
| Approximate metrics | The browser does not report when a frame reached the screen | Estimate jank and A/V offset from `requestAnimationFrame` timing |
| Safari before 26 | No WebCodecs audio, so no AAC | Require Safari 26 |
| Firefox | Stable releases lack `importExternalTexture` (a copy per frame) and `MediaStreamTrackProcessor` (no camera); Android has no WebCodecs | Treat Firefox as partial |
| HEVC encoding | Limited in browsers | Export H.264 only |
| Reopening projects | Keeping access to local media across sessions needs the File System Access API, which only Chromium has | Elsewhere, copy media into the browser's private storage or re-pick files |

## Plan

The steps run in order, and each one ends with something that runs in a browser. Step 1 retires the unknowns before anything is built on them.

| Step | Delivers | Status |
| --- | --- | --- |
| 1. Spike | The core in WebAssembly on the page's thread with the cooperative scheduler, WebCodecs decode, WebGPU compositing and AudioWorklet output, playing a 2 to 4 lane scene with frame pacing, the frame pool and A/V offset measured | Done |
| 2. Full player | The pipeline moved to a worker, the full compositor (images, text, transitions, effects) in WGSL, corrupt frames skipped instead of failing | Done |
| 3. Export | WebCodecs encoders and an MP4 muxer, saved to a file | Done |
| 4. Editor UI | A web UI on the reused `Document` model | Done |
| 5. Camera and voice | Recording through `getUserMedia` and WebCodecs in Chrome and Edge (Safari untested, not Firefox) | Done |

## Step 1 results

The spike plays every test scene in Chrome with no late frames and no jank, and keeps audio and video within 1 ms at the 95th percentile with four lanes. Runs were in headless Chrome 154 on an Apple GPU (Metal 3) with hardware H.264 decoding, 8 s per scene, 1280 × 720 output at 60 Hz.

| Scene | Frames shown | Late or janky | A/V offset, 95th pct. | First frame |
| --- | --- | --- | --- | --- |
| One 1080p30 clip | 177 of 177 (6 s) | 0 | 5 ms | 188 ms |
| A/V sync clip (flash and beep) | 177 of 177 (6 s) | 0 | 3 ms | 37 ms |
| Two lanes, picture in picture | 248 of 248 (6 s) | 0 | 1 ms | 40 ms |
| Four lanes in a grid, one at 60 fps | 477 of 477, three runs | 0 | 1 ms | 52 ms (181 ms cold) |

- **Frame pool:** with four lanes, up to 28 decoded frames were held at once and up to 35 were inside the decoders, with no decoder stall. The risk did not show here; Safari and Windows still need a check.
- **Done early:** two core changes (the cooperative scheduler, the asynchronous audio decoder with end-of-stream draining) and the MP4 demuxer. The demuxer matches AVFoundation packet for packet on every test clip; it reads further only on the two damaged ones.
- **Changed from the plan:** the pipeline runs on the page's thread, not a worker, because the AudioContext must live on the page. Moving it to a worker is now step 2.
- **Found:** with threads, Emscripten's clock adds the page's time origin, so frames were compared against the wrong time base until the clock used `performance.now()`. A/V sync re-anchors on any change in the refresh period, so the display reports the period only when it changes by more than 5%.
- **Not yet:** images, text, effects and transition clipping in the display; export; camera. A decode error closes a WebCodecs decoder, so a corrupt frame fails playback.

## Step 2 results

The player now runs on its own thread with the full compositor, and every test scene plays to its end with no late frames and no jank. Same setup as step 1, 8 s per scene.

| Scene | Frames shown | Late or janky | A/V offset, 95th pct. | First frame |
| --- | --- | --- | --- | --- |
| One 1080p30 clip | 237 | 0 | 4 ms | 84 ms |
| A/V sync clip | 237 | 0 | 7 ms | 38 ms |
| Two lanes, picture in picture | 332 | 0 | 1 ms | 36 ms |
| Four lanes, one at 60 fps | 477 | 0 | 1 ms | 64 ms |
| Compositor features (6 s scene) | 222 | 0 | 1 ms | 35 ms |
| Damaged clip | 126, with 16 corrupt-frame skips | 0 | 1 ms | 35 ms |

- **Threads:** the player, its stages, WebCodecs and the compositor run on a worker that owns the canvas. The page keeps only the `AudioContext`, which browsers allow only there; the speaker sends its calls to it. Every time passed between threads is absolute, since each has its own time origin.
- **Compositor:** a WebGPU port of the Metal one. It covers fit, crop, rotation, flip, transition offsets, wipes, crossfades, blend modes, color adjust, chroma key, the global filter, blur, images and text captions. Effect plugins stay macOS only: each needs a WGSL version.
- **Damaged media:** a decode error closes a WebCodecs decoder, so it is reported as one corrupt frame and the decoder is made again. Video resumes at the next keyframe, holding the last good frame; audio resumes at the next packet.
- **Found:** one 50 ms stall in the worker's first animation frames moved a smoothed refresh period enough for A/V sync to re-anchor 15 times. The period is now the median of the last 15 intervals.

## Step 3 results

The browser exports every test scene to a valid MP4 at about 12× real time, matching the macOS export frame for frame. Each 6 s scene was exported on the web (WebCodecs) and with the macOS exporter (AVFoundation), and the two files compared with ffmpeg.

| Scene | Frames, web / macOS | Luma SSIM | Audio match after alignment | Web export time |
| --- | --- | --- | --- | --- |
| One 1080p30 clip | 180 / 180 | 0.997 | 50.7 dB | 0.49 s |
| A/V sync clip | 180 / 180 | 1.000 | 46.7 dB | 0.47 s |
| Two lanes | 180 / 180 | 0.997 | 50.7 dB | 0.48 s |
| Four lanes | 180 / 180 | 0.997 | 50.7 dB | 0.49 s |
| Compositor features | 180 / 180 | 0.988 | 49.9 dB | 0.49 s |

- **Muxer:** portable C++, like the demuxer. Re-muxing every test clip's packets and reading them back with AVFoundation gives the same presentation times, keyframes and bytes, including the B-frame clips. ffprobe reads its files and ffmpeg decodes them without errors.
- **Encoders:** the compositor draws each frame into an export-sized canvas for `VideoEncoder` (H.264 High); the mixed audio goes to `AudioEncoder` (AAC-LC). A busy encoder makes the core retry.
- **Colour:** the colour bars differ slightly (PSNR about 31 dB, flat across frames). Chrome tags its file BT.709; the macOS file is untagged, though it also uses BT.709. Metal converts decoded frames to RGB with BT.709, and the browser chooses its own conversion for these untagged sources. It is a colour-management question for both platforms, not a web bug.
- **Audio lag:** the 2112-sample audio offset is the macOS AAC encoder's start-up delay, which neither platform trims.
- **Not yet:** the export is held in memory until it ends, and the frame size only letterboxes (no fill cropping).

## Step 4 results

The editor runs in the browser on the macOS editor's own document model, unchanged, and a scripted session passes all 17 of its checks: import, add, edit, drag, transition, play, export, save and reopen. The UI is about 900 lines of plain JavaScript: page, controller, timeline and properties panel.

| Area | In the web editor |
| --- | --- |
| Media | MP4 and M4A files and images, added from a picker or dropped on the page |
| Adding | Click to add at the playhead: a video on a free video track, an overlay above what plays there, audio on an audio track; text presets, emoji, colors |
| Timeline | Select, drag along or across tracks, trim either end, transitions from the diamond between two items, scrub, zoom; Space and Delete |
| Properties | Timing, position, scale, rotation, opacity, fit, blend, color adjust, blur, crop, chroma key, text style, sound, detaching audio, transitions, tracks, project |
| Projects | Save downloads the document; Open takes it with its media files |
| Export | MP4 at the project's size, 720p or 1080p |

- **Model:** `editor::Document` (`apps/macos-editor/document.cpp`) compiles to WebAssembly as is and runs on the page's thread behind a small C API. Every edit goes through it, so every track stays valid.
- **Preview:** after an edit, a change to the look only redraws the frame on the player's thread; anything else reopens the scene at the playhead, as the macOS editor reloads its player.
- **Checked in the session:** dragging a clip 100 px moved it exactly 100 px of time; playback across the crossfade had no late frames or jank; the 19 s project exported in 2.6 s and decodes cleanly; the saved project reopened identical.
- **Found:** a decoder destroyed when the player reopened could still receive a late WebCodecs callback, which crashed the worker. Destroying a decoder now detaches its callbacks.
- **Not yet:** stickers, editing on the preview, dragging from the sidebar to the timeline, waveforms, effect plugins, export formats and framing, resizable panes, keeping media between sessions. The camera and voice-over are step 5.

## Step 5 results

The editor records the camera and voice-overs: 3 s of Chrome's fake camera came out as 60 of 60 frames with sound and nothing dropped, placed on the timeline where it started. Recording runs on the page's thread: `MediaStreamTrackProcessor` reads frames, WebCodecs encodes them (H.264 real-time, AAC), and the portable MP4 muxer writes the file, which is then imported like a dropped file.

| Check (headless Chrome, fake devices) | Result |
| --- | --- |
| Camera, 3 s at 1280×720, 20 fps | 60 frames, 0 dropped, 3.0 s, H.264 + AAC (read back by ffprobe) |
| Camera item | On a video track at the playhead (2 s) |
| Voice-over, 2 s from 1 s | M4A of 2.1–2.2 s on an audio track at 1 s; the timeline played during it and stopped with it |
| Playback across both recordings | No late frames or jank in 4 of 5 runs; 1 jank in one run |
| Step 4 editor test | Still passes (17 checks) |

- **Two clocks:** Chrome stamps camera frames and microphone audio on different clocks (audio on `performance.now`, video on the system's). Each track is moved onto the page's clock by the offset its first sample showed on arrival. Without this the camera recordings had no sound.
- **Encoder start-up:** configuring the hardware H.264 encoder on macOS held up camera frames for about 1.7 s, which cut 1.65 s from the start of a recording. The editor now configures the encoder when the camera preview opens.
- **Falling behind:** when the video encoder has more than 2 frames queued, frames are dropped and counted rather than queued. The count shows while recording.
- **Voice-over timing:** the item starts where playback started. How long the player takes to start, about one or two refreshes, is not subtracted.
- **Not done:** recording in a worker (the only option for Safari, untested), the macOS editor's camera window layouts, and picking a device.

## Sources

Browser support facts come from these search results, read as summaries rather than page by page; confirm a version before relying on it.

- [WebCodecs browser support (testmuai)](https://www.testmuai.com/learning-hub/webcodecs-browser-support/)
- [Remotion: processing video with WebCodecs](https://www.remotion.dev/docs/media-parser/webcodecs)
- [WebGPU hits critical mass: all major browsers ship it](https://www.webgpu.com/news/webgpu-hits-critical-mass-all-major-browsers/)
- [WebGPU browser support in 2026](https://webo360solutions.com/blog/webgpu-browser-support/)
- [web.dev: WebGPU supported in major browsers](https://web.dev/blog/webgpu-supported-major-browsers)
- [caniuse: MediaStreamTrackProcessor](https://caniuse.com/mdn-api_mediastreamtrackprocessor)
- [Mozilla: unbundling MediaStreamTrackProcessor and VideoTrackGenerator](https://blog.mozilla.org/webrtc/unbundling-mediastreamtrackprocessor-and-videotrackgenerator/)
