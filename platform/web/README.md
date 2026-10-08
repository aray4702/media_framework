# Web platform

The portable core compiled to WebAssembly, with browser adapters: WebCodecs decoders, a WebGPU display, an AudioWorklet speaker, a portable MP4 demuxer, and a cooperative scheduler that runs every pipeline stage on the page's thread. See the browser port plan in the design docs for the reasoning and the remaining steps.

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

Open `http://localhost:8000/platform/web/app/?scene=stacked2` and press Play. Scenes: `single`, `sync` (a flash and a beep each second), `stacked2` (picture in picture), `stacked4` (four lanes in a grid, one at 60 fps). `seconds=N` sets how long they play.

## Measure

`tools/run_spike.mjs` plays scenes in Chrome and prints each one's metrics as JSON; `tools/summarize.py` shortens them:

```sh
cd platform/web/tools && npm install
SECONDS=8 node run_spike.mjs single sync stacked2 stacked4 | python3 summarize.py
```

`HEADLESS=0` shows the browser; `SCREENSHOT=<dir>` saves a picture of each scene mid-playback.

## Tests

The MP4 demuxer is plain C++, built natively too and checked against the macOS (AVFoundation) demuxer, packet for packet:

```sh
cmake --build build --target web_demuxer_tests && build/platform/web/web_demuxer_tests clips/*.mp4
```

## Limits (step 1)

- Video layers only: no images, text, effects, transitions' clipping or rotation in the display yet.
- Everything runs on the page's thread, not a worker.
- No export or camera.
- A decode error closes a WebCodecs decoder, so a corrupt frame fails playback instead of being skipped.
