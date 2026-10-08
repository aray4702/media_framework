# Web platform

The portable core compiled to WebAssembly, with browser adapters: WebCodecs decoders, a WebGPU compositor, an AudioWorklet speaker, a portable MP4 demuxer, and a cooperative scheduler that runs every pipeline stage on one thread.

## Threads

| Thread | Runs |
| --- | --- |
| Player (a worker) | The player and its five stages (`CooperativeScheduler`), WebCodecs, the WebGPU compositor on the transferred canvas (`OffscreenCanvas`), image decoding |
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

## Measure

`tools/run_spike.mjs` plays scenes in Chrome and prints each one's metrics as JSON; `tools/summarize.py` shortens them:

```sh
cd platform/web/tools && npm install
SECONDS=8 node run_spike.mjs single sync stacked2 stacked4 | python3 summarize.py
```

`HEADLESS=0` shows the browser; `SCREENSHOT=<dir>` saves pictures of each scene, at `SHOTS=1.5,3` seconds into playback (default: halfway).

## Tests

The MP4 demuxer is plain C++, built natively too and checked against the macOS (AVFoundation) demuxer, packet for packet:

```sh
cmake --build build --target web_demuxer_tests && build/platform/web/web_demuxer_tests clips/*.mp4
```

## Decode errors

A decode error closes a WebCodecs decoder. The adapters report it as one corrupt frame and make the decoder again: video at the next keyframe (the last good frame holds meanwhile), audio at the next packet (the gap plays as silence).

## Limits

- Effect plugins (`beauty`) are macOS only: each needs a WGSL version. A layer's plugin effects are skipped.
- No export or camera yet.
- The browser does not report when a frame reached the screen: present times are estimated as the refresh after the one a frame is drawn in.
