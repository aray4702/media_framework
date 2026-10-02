#pragma once

// Records one segment of a camera recording into a file: the camera's frames as they are (no
// overlays or effects: those stay editable) and, optionally, the microphone. Time starts at the
// segment's first frame: frames and audio are placed by their capture times relative to it, so
// they stay in sync however late they reach the encoder.
//
// The camera and the encoder don't run at the same pace:
//  - Bursts (the camera delivering frames after a stall, the encoder pausing): frames wait in a
//    short queue and are retried. It is bounded by a frame count, as each frame holds one of the
//    camera's buffers. When it overflows, the frame whose loss leaves the smallest gap is dropped,
//    so the motion stays even.
//  - A camera faster than the nominal rate, or bursts of frames closer together than a frame:
//    frames are kept on a grid of the nominal frame interval and the rest skipped.
//  - A slow encoder: when the queue stays full, the recording steps down to a lower frame rate
//    (30 → 20 → 15 → 10 fps for 30), keeping frames evenly, and steps back up once the encoder
//    keeps up again.
//  - Audio waits in its own longer queue; a chunk lost to overflow becomes silence, so what follows
//    stays in sync.
// Nothing here blocks the caller: refused writes are retried by later calls or, once the recorder
// has been idle a moment, by its own thread, which also finishes stopped segments.
//
// Frames and audio come from the camera's and the microphone's threads; start and stop from the
// owner's. All are thread-safe.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "mf/adapters.h"

namespace mf {

class SegmentRecorder {
 public:
  struct Options {
    int maxPendingFrames = 8;            // frames waiting for the encoder (each holds a camera buffer)
    int64_t maxPendingAudioUs = 2000000;  // audio waiting for the encoder
    int64_t idleRetryUs = 10000;          // the recorder's thread retries after this long without a call
  };
  // What the current (or last) segment did with the camera's frames.
  struct Stats {
    int written = 0;            // frames in the file
    int dropped = 0;            // frames lost: the queue overflowed, or the encoder failed them
    int skipped = 0;            // frames left out to keep the frame rate (bursts, a lower rate)
    int64_t audioDropped = 0;   // audio frames (samples per channel) lost to overflow: silence
    int fps = 0;                // the rate frames are being kept at; below the nominal one under load
    int rateReductions = 0;     // times the rate stepped down
    int maxPending = 0;         // the most frames that waited at once
  };

  explicit SegmentRecorder(PlatformFactory& platform) : SegmentRecorder(platform, Options()) {}
  SegmentRecorder(PlatformFactory& platform, Options options);
  ~SegmentRecorder();  // cancels files still finishing: their `done` never runs

  // Opens the file. width × height: the camera's frame size; fps: its nominal rate (the
  // encoder's key frame interval, and the last frame's length). sampleRate 0: no audio.
  Result start(const ExportTarget&, int width, int height, int fps, int sampleRate, int channels,
               int videoBitrate = 12000000);
  void video(const VideoFrame& camera);
  void audio(const int16_t* pcm, int frames, int64_t hostTimeNs);
  // Ends the segment and returns at once. `done` runs on any thread once the file is complete, with
  // Ok and the segment's length (first frame to the end of the last), or an error. Frames still
  // waiting are written first; the next segment can start meanwhile.
  void stop(std::function<void(Result, int64_t durationUs)> done);

  bool recording() const { return recording_; }
  int64_t durationUs() const;  // so far
  int pendingVideoFrames() const;  // waiting for the encoder
  int droppedFrames() const { return stats().dropped; }
  Stats stats() const;

 private:
  struct Segment;
  struct Finishing {
    std::unique_ptr<IExportSink> sink;
    std::shared_ptr<std::atomic<bool>> done;
  };

  void run();  // the recorder's thread
  void drainLocked(Segment&);
  void admitLocked(Segment&, const VideoFrame&);
  void thinLocked(Segment&);
  void adaptRateLocked(Segment&, int64_t captureUs);
  void finish(const std::shared_ptr<Segment>&, const std::function<void(Result, int64_t)>& done);  // without mu_
  void releaseFinished();

  PlatformFactory& platform_;
  const Options options_;
  mutable std::mutex mu_;
  std::condition_variable wake_;
  std::shared_ptr<Segment> current_;
  std::shared_ptr<Segment> last_;  // the current segment, or the last one once it's stopped: stats
  // Stopped segments with frames or audio still to write; the thread finishes them.
  struct Draining {
    std::shared_ptr<Segment> segment;
    std::function<void(Result, int64_t)> done;
  };
  std::vector<Draining> draining_;
  // Sinks of stopped segments, until their files are complete. Released on the owner's thread
  // (start, stop, the destructor): never from their own callback.
  std::vector<Finishing> finishing_;
  std::atomic<bool> recording_{false};
  int64_t lastCallNs_ = 0;  // steady clock: the last write attempt from a caller
  bool quit_ = false;
  std::thread thread_;
};

}  // namespace mf
