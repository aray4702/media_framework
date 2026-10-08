# Web platform

The portable core compiled to WebAssembly, with browser adapters: WebCodecs decoders and encoders, a WebGPU compositor, an AudioWorklet speaker, a portable MP4 demuxer and muxer, and a cooperative scheduler that runs every pipeline stage on one thread.

## Threads

| Thread | Runs |
| --- | --- |
| Player (a worker) | The player and its five stages (`CooperativeScheduler`), WebCodecs, the WebGPU compositor on the transferred canvas (`OffscreenCanvas`), image decoding, export |
| Page | The page's calls (`web_api.cpp`, carried out on the player's thread in order), the `AudioContext` (browsers allow it only here) |
| Audio | The AudioWorklet: C++ pulling PCM from the shared `AudioRing` |

Every time passed between them is absolute (`performance.timeOrigin + performance.now()`), since each thread has its own time origin.

## Build

Needs Emscripten (`brew install emscripten`).

```sh
platform/web/build.sh        # writes build-web/mf.js and build-web/mf.wasm
```

## Run

The page needs cross-origin isolation (shared memory between the page and the AudioWorklet), so serve the repository root with the COOP/COEP headers:

```sh
python3 platform/web/tools/serve.py          # http://localhost:8000
```

Open `http://localhost:8000/platform/web/app/?scene=stacked2` and press Play. Scenes: `single`, `sync` (a flash and a beep each second), `stacked2` (picture in picture), `stacked4` (four lanes in a grid, one at 60 fps), `features` (a crossfade, a wipe, an image, a caption, color adjust, crop, chroma key, blur, a blend mode), `corrupt` (a clip with damaged data). `seconds=N` sets how long they play.

## Editor

`platform/web/editor/` is the media editor in the browser: `http://localhost:8000/platform/web/editor/`. Its model is the macOS editor's `editor::Document` (`apps/macos-editor/document.cpp`), unchanged, as a C API (`src/editor_api.cpp`) on the page's thread; every edit goes through it, so every track stays valid. After an edit the scene goes to the player (`mf_apply`), which redraws the frame when only the look changed and otherwise reopens the scene at the playhead.

- **Media:** MP4 (H.264, AAC) and M4A files and images, from Add media… or dropped on the page. Click one to add it at the playhead: a video on the lowest free video track, an image, text or color on a layer above what plays there, audio on an audio track.
- **Text, emoji and colors:** the macOS editor's text presets (heading, subheading, body, caption), emoji as text, solid colors.
- **Timeline:** select items, tracks and joins; drag items along their track or onto another; trim either end (the start trims into the file); click the diamond between two items for a transition; scrub in the ruler. Space plays, Delete removes the selection.
- **Properties:** timing (start, duration, in, speed); position, scale, rotation, opacity, fit, blend; color adjust, blur, crop and chroma key; text words, font, size, color and box; sound (mute, gain, pan) and detaching a video's sound; transitions (kind, direction, duration); tracks (on, opacity or gain, order); the project (size, frame rate, background).
- **Projects:** Save downloads the scene document; Open takes the document together with its media files, matched by file name.
- **Export:** MP4 at the project's size, 720p or 1080p, downloaded when done.

`tools/run_editor.mjs` drives it in Chrome like a person (import, add, edit, drag, transition, play, export, save and reopen) and checks the scene and the player at each step.

Not yet, compared with the macOS editor: stickers, the camera and voice-over (step 5), editing in the preview (drag and resize on the picture), dragging from the sidebar onto the timeline, waveforms, effect plugins, export formats and framing (MOV, HEVC, fill, several outputs), resizable panes, and keeping media between sessions (projects reopen with their files picked again).

## Measure

`tools/run_spike.mjs` plays scenes in Chrome and prints each one's metrics as JSON; `tools/summarize.py` shortens them:

```sh
cd platform/web/tools && npm install
SECONDS=8 node run_spike.mjs single sync stacked2 stacked4 | python3 summarize.py
```

`HEADLESS=0` shows the browser; `SCREENSHOT=<dir>` saves pictures of each scene, at `SHOTS=1.5,3` seconds into playback (default: halfway).

## Export

`mf_export` renders a scene on the player's thread through the same compositor into an export-sized canvas, encodes it with WebCodecs (H.264 High, AAC-LC) and muxes it with the portable MP4 muxer. The file is kept in memory; `mf_export_data` / `mf_export_size` give it to the page once the report has the `exported` event. The test page exports instead of playing with `export=1`, and the runner saves the file with `EXPORT=<dir>`.

To compare with the macOS export of the same scenes (the runner also saves each scene's JSON, its sources as the clips' paths):

```sh
cd platform/web/tools && SECONDS=6 EXPORT=/tmp/exports node run_spike.mjs single features
cd ../../.. && for s in single features; do build/platform/web/export_reference /tmp/exports/$s.json /tmp/exports/$s-mac.mp4; done
python3 platform/web/tools/compare_exports.py /tmp/exports single features   # needs numpy
```

## Tests

The MP4 demuxer and muxer are plain C++, built natively too and checked against the macOS (AVFoundation) demuxer: the demuxer packet for packet, the muxer by re-muxing every clip's packets and reading them back:

```sh
cmake --build build --target web_demuxer_tests web_muxer_tests
build/platform/web/web_demuxer_tests clips/*.mp4
build/platform/web/web_muxer_tests clips/*.mp4
```

## Decode errors

A decode error closes a WebCodecs decoder. The adapters report it as one corrupt frame and make the decoder again: video at the next keyframe (the last good frame holds meanwhile), audio at the next packet (the gap plays as silence).

## Limits

- Effect plugins (`beauty`) are macOS only: each needs a WGSL version. A layer's plugin effects are skipped.
- An export is kept in memory until it is done: a long one at a high bitrate needs that much memory. Writing to a file as it goes (the File System Access API, or the origin's private storage) would remove that.
- The export frame size letterboxes (`frameFit` fit); `fill` cropping is not implemented on the web.
- No camera yet.
- The browser does not report when a frame reached the screen: present times are estimated as the refresh after the one a frame is drawn in.
