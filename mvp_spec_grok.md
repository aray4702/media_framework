# MVP: local MP4 playback

One C++ pipeline plays one local file in a macOS window: picture and sound together, pause, seek, quit. Adapter interfaces exist so iOS, Android, and Web can be added later. They are not in this slice.

## Done when

From a path argument, the window shows the video and the speakers play the audio.

- Play starts at time zero and keeps audio and video on one clock.
- Pause freezes both. Play continues from that position.
- Seek while paused moves to that time and shows the corresponding video frame. Play continues from there.
- A file that cannot be opened, or has no supported video track, stops in the error state.
- Shutdown from any non-terminal state releases the file and the devices.

That is the whole slice. Quality targets in `reqs.md` (jank under 1%, A/V offset under 40 ms, seek under 100 ms, time to first frame) are not pass/fail gates for this slice. See the issue list.

## Out of this slice

Image, audio-only, 3D, text. Remote files, streaming, DRM. Background playback and platform lifecycle. A plugin loader. Android, iOS, and Web ports. Worker threads and inter-thread queues. Seek while playing. Metrics. RGBA conversion. Playback rate, volume, looping, subtitles, track selection.

## Contract



### Media

Non-fragmented MP4 with one `avc1` H.264 video track (8-bit 4:2:0) and, if present, one `mp4a` AAC-LC audio track, mono or stereo, 44100 or 48000 Hz.

Reject at `open` with `kUnsupported`: fragmented MP4, any other codec, more than one video or audio track, width or height above 4096, or a compressed sample above 8 MiB.

The demuxer may ignore a simple start trim in the edit list. Any other edit list is `kUnsupported`.

If the track header matrix is a 0, 90, 180, or 270 degree rotation, the display adapter applies it. Other matrices are drawn as square pixels on the coded frame.

### Public API

All methods run on the thread that constructed the pipeline. A call from another thread returns `kInvalidState` and does not change state. Timestamps are microseconds on the media timeline, starting at 0.

```text
open(path) -> error
play() -> error
pause() -> error
seek(position_us) -> error
shutdown() -> error
tick() -> error

state() -> State
duration_us() -> int64   // 0 outside READY and PLAY
position_us() -> int64   // 0 outside READY and PLAY
```

`duration_us` and `position_us` are reads. The written API had no way to place a seek, so they are part of this slice.

`tick` is also new. The host calls it on the creator thread while the window is open, about once per display refresh. It is a no-op outside `PLAY`. During `PLAY` it presents video against the master clock and keeps the speaker fed.

`open` and `play` return when the state has changed. They do not wait on a time-to-first-frame budget. The first frame is submitted inside `play` before it returns.

Error callback, set at construction: `void(Error, void* user)`. Return values are the result of the call. The callback is only for a fatal failure discovered later inside `tick`. It runs on the creator thread.

### States

```text
START  --open-->  READY | ERROR
READY  --play-->  PLAY  | ERROR
READY  --seek-->  READY | ERROR
PLAY   --pause--> READY
START | READY | PLAY | ERROR  --shutdown-->  SHUTDOWN
```

Any other call returns `kInvalidState` and leaves the state unchanged. One instance opens one file. `SHUTDOWN` and `ERROR` do not reopen. The caller destroys the instance and constructs another.

Seek from `PLAY` is rejected. To scrub, the host pauses, seeks, and, if playback had been running, calls play again.

### Errors


| Code            | When                                                   |
| --------------- | ------------------------------------------------------ |
| `kOk`           | Call succeeded                                         |
| `kInvalidState` | Illegal transition, or wrong thread                    |
| `kIo`           | Path cannot be opened or read                          |
| `kUnsupported`  | No decodable H.264 track, or file outside the contract |
| `kSeekDenied`   | Position outside `[0, duration]`                       |
| `kOutOfMemory`  | A size field would exceed the caps above               |


`kIo`, `kUnsupported`, and `kOutOfMemory` from `open` or from `tick` move the pipeline to `ERROR`.

A decode failure on one packet stays in the current state. Video repeats the last presented frame, or shows black if none exists yet. Audio drops that packet.

## Shape

Common C++ owns the state machine, the MP4 demuxer, and the clock. The host injects three adapters. Nothing else is virtual.

```text
file -> MP4 demuxer -- H.264 packets --> video decoder --> display
                    -- AAC packets  --> audio decoder --> speaker
```

The demuxer emits encoded packets. Decoders emit frames. There is no separate parser object: the AVC decoder config and the AAC audio-specific config are the decoder's `open` input.

Video frames stay in the decoder's native buffer. On this host that is a `CVPixelBuffer`. The pipeline does not convert to RGBA.

Audio frames are host-endian signed 16-bit interleaved PCM. The speaker opens at the stream's sample rate and channel count. No resampler.

### Adapters

```text
VideoDecoder
  open(codec config) -> error
  decode(packet) -> zero or one frame
  flush()

Display
  present(native frame)

Speaker
  open(sample_rate, channels) -> error
  write(pcm)
  position_us() -> int64
  pause(); resume(); flush()
```

macOS implementations: VideoToolbox, a layer-backed view, AudioQueue. The audio device calls the speaker adapter on the device thread. That adapter is the only object that shares memory with another thread: one ring buffer, one mutex. Pipeline methods stay on the creator thread.

`Speaker::position_us` is the media time of the sample the device is playing, minus the adapter's queued buffer. That subtraction is the output latency for this slice.

### Clock

If an audio track was opened, the speaker position is the master clock. Otherwise the master is a monotonic clock that starts at `play`, stops advancing across pause, and jumps on seek.

Inside `tick`, for the next decoded video frame of duration `D` and timestamp `pts`:

- `pts <= master`: present it.
- `master - pts > D`: drop it and try the next one.
- `pts > master`: keep it for a later tick.

On `play`, decode and present the first video frame at the current position, then return.

On `seek` (state is `READY`): flush decoder and speaker, find the previous sync sample, decode forward, drop frames before the target, present the first video frame at or after the target, drop audio before that time. Position becomes that frame's timestamp. State stays `READY`.

## Host

A macOS window. The path is the first argument. Space toggles play and pause. Left and right seek by two seconds, using pause, seek, and play when the pipeline was playing. The run loop calls `tick` once per refresh.

macOS is not in the supported-device list. It is the development host because VideoToolbox and AudioQueue are the same services an iOS adapter will wrap. The iOS, Android, and Web adapters are later steps.

## Acceptance

A fixture file, `testdata/sample.mp4`: under ten seconds, 30 fps H.264, AAC-LC stereo at 48 kHz, keyframe interval at most one second, non-fragmented.

1. `open` reaches `READY`, and `duration_us` is the file duration.
2. `play` shows frames and sound. Pause, then play, continues the same timeline.
3. While paused, `seek` to the middle of the file shows the frame at that time. `play` continues from there.
4. `seek` while playing returns `kInvalidState`.
5. `open` on a missing path and on a file with no H.264 track returns a fatal code and `ERROR`.
6. `shutdown` from `PLAY` reaches `SHUTDOWN` without a hang.



## Issue list

1. **The thread diagram is ambiguous, and it is too much for a first loop.** "A separate thread for file reader, audio/video parser and demuxer" can be one thread or three, plus the later decoder, render, and audio threads. That is four to six threads, queues, and condition variables, and it makes seek a cancellation problem. **Proposal:** this slice stays on the creator thread and adds `tick`. The audio device callback stays inside the speaker adapter. Split threads when a single `tick` can no longer decode a frame before the next refresh.
2. **Seek is specified in two ways that do not match.** The diagram allows seek only from `READY`. The prose treats scrubbing as a burst of seeks with a 100 ms budget, which is the in-play gesture. **Proposal:** follow the diagram. The host pauses before a scrub. Add `PLAY → seek → PLAY` later.
3. **"Honor only the latest scrub timestamp" assumes overlapping seeks.** A synchronous `seek` on one thread finishes before the next call, so the latest call already wins. **Proposal:** no seek queue in this slice.
4. **The pipeline wording disagrees with itself.** "The media reader produces frames" but frames exist only after decode. "A presentation device that sends internal frames to presentation devices" names one role twice. **Proposal:** demuxer emits packets, decoders emit frames, display and speaker present them.
5. **RGBA as the internal video format fights hardware decode.** It is marked optional and justified by a video graph, which is outside v1. Hardware decoders produce YUV. A mandatory RGBA copy spends the CPU this design is trying to avoid. **Proposal:** keep the decoder's native buffer. Signed 16-bit PCM stays, because there is one audio consumer and the device is opened at the stream rate.
6. **The smoothness numbers are not yet measurable, and 40 ms conflicts with 24 fps.** Dropped-frame rate has no window or denominator. Jank uses an undefined "expected interval," and a dropped frame also creates a 2.0-frame gap, so one late frame counts twice. One 24 fps frame is 41.7 ms, so an offset under 40 ms is stricter than showing the right frame. Seek latency never says which two events it measures, and a long GOP cannot be decoded in 100 ms. Time to first frame has no number. **Proposal:** this slice does not count them. When counters are added: expected interval is the source frame duration; a drop is a decoded frame never presented; a jank is a presented frame late by more than half a frame; the offset target is one frame duration, and 40 ms applies only when the frame duration is 33 ms or less; seek latency is the `seek` call until the new frame is presented, best effort.
7. **The API cannot drive a timeline.** Open, play, pause, seek, and shutdown never reveal duration, position, or state. **Proposal:** the three reads above.
8. **Fatal errors have two exits and no recovery.** A synchronous call both returns a state and is described as reporting through a callback. `ERROR` can only shut down. **Proposal:** the return code is the result of the call. The callback is only for a failure found later in `tick`. A fatal error ends the instance.
9. **Codec, container, and "malicious file" are unbounded.** Full demuxer hardening is its own project. **Proposal:** the media contract and the size caps above. Tripping a cap is `kOutOfMemory` or `kUnsupported`. A bad packet is skipped, not replaced with a designed placeholder asset. The requirements say "use a placeholder" without defining one; last video frame, or black, is the stand-in.
10. **There is no desktop target, and three ports are not the minimum.** The product list is Android 8, iOS 13, and the latest two majors of four browsers. This machine is macOS. A web port is either WASM plus WebCodecs, or an HTML video element that never uses this pipeline. **Proposal:** ship the macOS host described above. Port iOS next, on the same Apple adapters. Then Android. Web last.
11. **The master clock is specified. The thing that wakes the pipeline is not.** **Proposal:** `tick`, as above.
12. **The creator-thread rule cannot cover the audio device.** The OS calls that callback. **Proposal:** the rule binds Pipeline methods. The speaker adapter is the shared boundary.

