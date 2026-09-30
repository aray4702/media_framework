#include "mf/segment_recorder.h"

#include <algorithm>

namespace mf {

SegmentRecorder::~SegmentRecorder() = default;  // each sink cancels its unfinished file

void SegmentRecorder::releaseFinished() {
  finishing_.erase(std::remove_if(finishing_.begin(), finishing_.end(), [](const Finishing& f) { return f.done->load(); }), finishing_.end());
}

Result SegmentRecorder::start(const ExportTarget& target, int width, int height, int fps, int sampleRate, int channels,
                              int videoBitrate) {
  std::lock_guard<std::mutex> lock(mu_);
  if (open_) return Result::InvalidState;
  releaseFinished();
  if (width < 2 || height < 2 || fps <= 0 || (sampleRate > 0 && (channels < 1 || channels > 2))) return Result::InvalidArgument;
  sink_ = platform_.createExportSink();
  if (!sink_) return Result::Unsupported;
  ExportSettings s;
  s.width = width & ~1;  // encoders take even sizes
  s.height = height & ~1;
  s.fps = fps;
  s.videoBitrate = videoBitrate;
  Result r = sink_->open(target, s, sampleRate, sampleRate > 0 ? channels : 0);
  if (r != Result::Ok) {
    sink_.reset();
    return r;
  }
  width_ = s.width;
  height_ = s.height;
  frameUs_ = 1000000 / fps;
  sampleRate_ = sampleRate > 0 ? sampleRate : 0;
  channels_ = sampleRate_ > 0 ? channels : 0;
  firstUs_ = lastUs_ = audioPos_ = -1;
  dropped_ = 0;
  open_ = true;
  recording_ = true;
  return Result::Ok;
}

void SegmentRecorder::video(const VideoFrame& camera) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!recording_ || !camera.image) return;
  if (firstUs_ >= 0 && camera.ptsUs <= lastUs_) return;  // not after the last one
  int64_t first = firstUs_ >= 0 ? firstUs_ : camera.ptsUs;
  ComposedFrame f;
  f.ptsUs = camera.ptsUs - first;
  f.width = width_;
  f.height = height_;
  ComposedLayer l;
  l.kind = ComposedLayer::Kind::Video;
  l.frame = camera;
  l.fit = Fit::Fill;  // the camera's own size: exactly the frame
  f.layers.push_back(std::move(l));
  if (sink_->writeVideo(f) != Result::Ok) {  // Again (the encoder is busy) or failed: this frame is dropped
    ++dropped_;
    return;
  }
  firstUs_ = first;
  lastUs_ = camera.ptsUs;
}

void SegmentRecorder::audio(const int16_t* pcm, int frames, int64_t hostTimeNs) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!recording_ || sampleRate_ == 0 || firstUs_ < 0 || frames <= 0) return;  // audio starts with the first frame
  int skip = 0;
  if (audioPos_ < 0) {  // the first chunk: placed by its time, what came before the first frame cut off
    int64_t at = (hostTimeNs / 1000 - firstUs_) * sampleRate_ / 1000000;
    if (at < 0) skip = int(std::min<int64_t>(-at, frames));
    audioPos_ = std::max<int64_t>(0, at);
  }
  if (skip >= frames) {
    audioPos_ = -1;  // all before the first frame: the next chunk is placed by its time
    return;
  }
  int n = frames - skip;
  // Later chunks follow on: the microphone's clock is steady, and gaps would be heard. A chunk the
  // encoder has no room for still takes its time (silence), so what follows stays in sync.
  sink_->writeAudio(pcm + size_t(skip) * channels_, n, audioPos_ * 1000000 / sampleRate_);
  audioPos_ += n;
}

void SegmentRecorder::stop(std::function<void(Result, int64_t)> done) {
  IExportSink* sink = nullptr;
  int64_t duration = 0;
  auto finished = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!open_) {
      duration = -1;
    } else {
      recording_ = false;
      open_ = false;
      releaseFinished();
      duration = firstUs_ < 0 ? 0 : lastUs_ - firstUs_ + frameUs_;
      if (duration == 0) {
        sink_.reset();  // nothing written: no file worth keeping
      } else {
        sink = sink_.get();
        finishing_.push_back({std::move(sink_), finished});
      }
    }
  }
  // Outside the lock: a sink may finish at once, and `done` may call back.
  if (duration < 0) return done(Result::InvalidState, 0);
  if (!sink) return done(Result::WriteFailed, 0);
  sink->endAudio();
  sink->finish([done, duration, finished](Result r) {
    done(r, r == Result::Ok ? duration : 0);
    *finished = true;
  });
}

int64_t SegmentRecorder::durationUs() const {
  std::lock_guard<std::mutex> lock(mu_);
  return firstUs_ < 0 ? 0 : lastUs_ - firstUs_ + frameUs_;
}

}  // namespace mf
