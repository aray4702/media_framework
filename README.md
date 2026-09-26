# Media Framework (MVP)

Mac OS implementation of [mvp_spec_claude.md](mvp_spec_claude.md): a portable C++17 playback core with Mac OS adapters (AVAssetReader, VideoToolbox, AudioConverter, AudioUnit, CAMetalLayer + CVDisplayLink) and a minimal demo app.

## Build and test

```sh
cmake -S . -B build && cmake --build build -j
./build/core_tests                                   # core unit tests (fake adapters, manual scheduler)
scripts/make_clips.sh                                # test clips into clips/ (needs ffmpeg with libx264)
./build/platform/macos/macos_adapter_tests clips/1080p30.mp4   # real demuxer + decoders, headless
./build/apps/macos-demo/mf_demo clips/1080p30.mp4    # demo player
./build/apps/macos-demo/mf_demo --autotest clips/1080p30.mp4 5 # play 5 s, scrub, seek, print metrics
```

`--autotest` opens a window and plays audio (the generated clips use a quiet tone). It stands in for the XCTest integration tests in §5, which need a full Xcode install.

## Layout

- `core/include/mf/` public API: `player.h` (Player, listener, metrics), `adapters.h` (adapter interfaces), `audio_ring.h`, `thread_scheduler.h`.
- `core/src/` state machine and stages (`pipeline.cpp`), `player.cpp`, clock, A/V sync, metrics, bounded queue.
- `core/tests/` host tests with fake adapters and a single-threaded scheduler.
- `platform/macos/` Mac OS adapters and the platform factory.
- `apps/macos-demo/` the demo app.
