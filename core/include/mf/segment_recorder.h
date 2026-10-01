#pragma once

// Records one segment of a camera recording into a file: the camera's frames as they are (no
// overlays or effects: those stay editable) and, optionally, the microphone. Time starts at the
// segment's first frame: frames and audio are placed by their capture times relative to it, so
// they stay in sync. Writes go through an IExportSink without blocking the caller: frames wait in
// a short queue and are retried while the encoder catches up; video in the file is timed at a
// steady frame rate.
//
// Frames and audio come from the camera's and the microphone's threads; start and stop from the
// owner's. All are thread-safe.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <deque>
#include <mutex>
#include <vector>

#include "mf/adapters.h"

namespace mf {

class SegmentRecorder {
 public:
  explicit SegmentRecorder(PlatformFactory& platform) : platform_(platform) {}
  ~SegmentRecorder();  // cancels files still finishing: their `done` never runs

  // Opens the file. width × height: the camera's frame size; fps: its nominal rate (the
  // encoder's key frame interval, and the last frame's length). sampleRate 0: no audio.
  Result start(const ExportTarget&, int width, int height, int fps, int sampleRate, int channels,
               int videoBitrate = 12000000);
  void video(const VideoFrame& camera);
  void audio(const int16_t* pcm, int frames, int64_t hostTimeNs);
  // Ends the file. `done` runs on any thread once it's complete, with Ok and the segment's length
  // (first frame to the end of the last), or an error. No frame is written after stop(), and the
  // next segment can start at once: the file finishes on its own.
  void stop(std::function<void(Result, int64_t durationUs)> done);

  bool recording() const { return recording_; }
  int64_t durationUs() const;  // so far
  int pendingVideoFrames() const;  // waiting for the encoder
  int droppedFrames() const { return dropped_; }

 private:
  PlatformFactory& platform_;
  std::unique_ptr<IExportSink> sink_;
  // Sinks of stopped segments, until their files are complete. Released on the owner's thread
  // (start, stop, the destructor): never from their own callback.
  struct Finishing {
    std::unique_ptr<IExportSink> sink;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Finishing> finishing_;
  void releaseFinished();
  void drainVideoLocked(std::unique_lock<std::mutex>& lock, bool block);
  mutable std::mutex mu_;
  std::deque<ComposedFrame> pendingVideo_;
  bool open_ = false;
  std::atomic<bool> recording_{false};
  int width_ = 0, height_ = 0, frameUs_ = 33333;
  int sampleRate_ = 0, channels_ = 0;
  int64_t firstUs_ = -1, lastUs_ = -1;  // capture times of the first and last frames accepted
  int64_t framesWritten_ = 0;           // encoded frames; file time is framesWritten_ * frameUs_
  int64_t audioPos_ = -1;               // the next audio sample's position; -1: none written yet
  std::atomic<int> dropped_{0};
};

}  // namespace mf
