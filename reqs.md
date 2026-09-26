A cross-platform media framework or playback engine — how you abstracted platform differences (iOS, Android, Web), managed synchronisation, and maintained performance under constraint

- goal is to build a cross-platform media framework for different platforms on different devices.
[design] major device types are Android, iOS, and Web. they have different video decoding capabilities, and pipeline should make best use of their hardward capabilites for decoding. Encapusulate platform specifics in decoder adapter, speaker adapter, and display adapter.

- functionalitis include play various medias, e.g., image, audio, video, 3d, text, etc.
    local media/remote media? vod media/streaming media?
    range of media types.
[design] in v1, limit to video only, other meida types are non-goal. The pipeline will be extended to support more media types.
[design] in v1, limit to local media file, and other media sources are non-goal.
[design] in v1, limit to CFR, and VFR is no-goal.
[design] in v1, edit lists and aac priming is no-goal.
[design] in v1, track head matrixs in no-goal.

- video and audio shall be played in real-time. user experience looks for smooth playing, a/v synchronised, fast time to first frame.
[design] dropped frame rate is the percentage of dropped frames that don't go to display.
[design] a jank is detected when a video frame display interval is >1.5 expected interval. jank rate is percentage of janks.
[design] a/v offset is time offset of video frame and audio frame that are displayed at the same time. include audio device output latency in the calculation.
[design] by smooth playing, pipeline should keep the video fps capped by display refresh rate in playing, and jank rate be <1%, and audio video |offset| <40ms

- jump to a time in past or future.
[design] pipleline need a seek api with a timestampe argument, which makes the pipelien to decode audio and video frames at the timestamp and render the new video frame.
the performance should responsive. If the seek is not allowed, return error code with reason.
[design] pipeline make best efforts towards responsive user experience, i.e. latency < 100ms

- user scrub the timeline.
this can be considered mutiple seeks in a row.
[design] only latest timestamp is honored during scrubbing. pipeline make best efforts towards responsive user experience, i.e. latency < 100ms

[design] pipeline architecture includes a media reader which produces frames in internal format, and a presentation device that sends internal frames to presentation devices. media data enter the pipeline, goes through multiple steps, and finally presented on the display.
different media has different specific pipelines. for video file, the pipeline is composed of multiple components below:
file/stream reader -> demuxer
    -> audio parser -> audio decoder -> audio frame render -> speaker
    -> video parser -> video decoder -> video frame render -> display
[design] pipeline stages (reader/demuxer/parser, video decode, video render, audio decode and play) are non-blocking steps connected by bounded queues, and are driven by a per-platform scheduler. the scheduler owns the threading model:
    - native (Android, iOS, Mac OS, Windows, Linux): one thread per stage, and mutex/cv to synchronize access to the queues.
    - Browser: a worker event loop for the stages, plus an AudioWorklet for audio output.
every scheduler shall guarantee:
    1. decode, render, and I/O never run on the caller thread.
    2. a slow stage does not block rendering of frames already decoded.
    3. the real-time audio path never takes locks or allocates memory.
    4. queue memory is bounded.

[design] if audio track exists, audio and video tracks need be time synchronized.
[design] if audio track exists, use audio clock as master clock to drive the playing and let video synchronized to audio clock. if no audio track, use system clock to drive video playing.

[design] abstract common logic in c++ and use adapters to adapt to specifics of pipelines components.

[design] pipelien supports apis - open, play, pause, seek, shutdown
[design] pipeline support states - START, READY, PLAY, ERROR, SHUTDOWN. state transitions are shown below:
   START-->open-->READY/ERROR
   READY-->play-->PLAY/ERROR
        -->seek-->READY/ERROR
   PLAY-->pause-->READY
   START/READY/PLAY/ERROR-->shutdown-->SHUTDOWN
other transistions are rejected with error code.

[design](optional) for video frame use RGBA format as internal frame format of canvas. for audio frame use signed 16bits PCM as internal format. this is useful for complext pipeline which handle video graph.

[design] pipleline should only be allowed to call in the thread that created the pipeline instance.

[design] list of fatal errors: file cannot be openned, no video decoder that supports the video, malicious media file that allocats out of bound memory. one these errors are encountered, pipeline enter error state, and error is reported to caller in callback function.
list of non-fatal errors: corrupted audio and video frames. pipeline should skip corrupted audio and video frames and use placeholder in lieu.

[design] in v2, pipeline should adopt a plugin mechanism to support future media types.
[design] in v1, limit to non-protected meida and DRM support is no-goal.

- Recommended minimum supported platform versions:
    - Android: 8.0 (API level 26)
    - iOS: 13.0
    - Web: Latest two major versions of Chrome, Firefox, Safari, and Edge

- in v1, background play is no-goal.
- in v1, integration with platform lifecycle e.g. resume/pause is no-goal

[design] the architecture should be compatible with Android, iOS, Mac OS, Windows, Linux, and Browser