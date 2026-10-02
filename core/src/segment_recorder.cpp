#include "mf/segment_recorder.h"

#include <algorithm>
#include <chrono>
#include <deque>

namespace mf {

namespace {

// The rates frames are kept at, as multiples of the nominal frame interval: 30, 20, 15, 10 fps for 30.
struct RateStep {
  int num, den;
};
constexpr RateStep kRates[] = {{1, 1}, {3, 2}, {2, 1}, {3, 1}};
constexpr int kRateSteps = int(sizeof(kRates) / sizeof(kRates[0]));

constexpr int64_t kFullForUs = 1000000;     // the queue at least half full this long: the encoder is too slow
constexpr int64_t kStepDownGapUs = 1000000;  // between steps down, for the last one to take effect
constexpr int64_t kStepUpAfterUs = 3000000;  // the queue nearly empty this long: try the next rate up
constexpr int64_t kMaxStepUpAfterUs = 30000000;
constexpr auto kRetryPoll = std::chrono::milliseconds(2);

int64_t steadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct SegmentRecorder::Segment {
  std::unique_ptr<IExportSink> sink;
  int width = 0, height = 0, fps = 30;
  int64_t frameUs = 33333;
  int sampleRate = 0, channels = 0;

  struct Frame {
    ComposedFrame frame;
    int64_t captureUs = 0;
  };
  std::deque<Frame> video;
  struct Chunk {
    std::vector<int16_t> pcm;
    int frames = 0;
    int64_t pos = 0;  // in samples from the first frame
  };
  std::deque<Chunk> audio;
  int64_t audioQueued = 0;  // samples per channel

  int64_t firstUs = -1;         // capture time of the first frame kept: the file's time 0
  int64_t lastKeptUs = -1;      // capture time of the last frame kept
  int64_t lastWrittenUs = -1;   // capture time of the last frame the encoder took
  int64_t nextDueUs = 0;        // the next frame is kept from about here (the rate's grid)
  int64_t audioPos = -1;        // the next audio sample's position; -1: none yet

  int step = 0;                 // index into kRates
  int64_t fullSinceUs = -1;     // capture time since which the queue has been at least half full
  int64_t calmSinceUs = -1;     // capture time since which the queue has held at most one frame
  int64_t lastStepUs = -1;      // capture time of the last change of rate
  int64_t lastStepUpUs = -1;
  int64_t stepUpAfterUs = kStepUpAfterUs;

  Stats stats;

  int64_t intervalUs() const { return frameUs * kRates[step].num / kRates[step].den; }
  bool pending() const { return !video.empty() || !audio.empty(); }
};

SegmentRecorder::SegmentRecorder(PlatformFactory& platform, Options options) : platform_(platform), options_(options) {
  thread_ = std::thread([this] { run(); });
}

// Each sink cancels its unfinished file.
SegmentRecorder::~SegmentRecorder() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    quit_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void SegmentRecorder::releaseFinished() {
  finishing_.erase(std::remove_if(finishing_.begin(), finishing_.end(), [](const Finishing& f) { return f.done->load(); }), finishing_.end());
}

Result SegmentRecorder::start(const ExportTarget& target, int width, int height, int fps, int sampleRate, int channels,
                              int videoBitrate) {
  std::lock_guard<std::mutex> lock(mu_);
  if (current_) return Result::InvalidState;
  releaseFinished();
  if (width < 2 || height < 2 || fps <= 0 || (sampleRate > 0 && (channels < 1 || channels > 2))) return Result::InvalidArgument;
  auto s = std::make_shared<Segment>();
  s->sink = platform_.createExportSink();
  if (!s->sink) return Result::Unsupported;
  ExportSettings settings;
  settings.width = width & ~1;  // encoders take even sizes
  settings.height = height & ~1;
  settings.fps = fps;
  settings.videoBitrate = videoBitrate;
  settings.realtime = true;  // a live camera: keep up rather than wait
  Result r = s->sink->open(target, settings, sampleRate, sampleRate > 0 ? channels : 0);
  if (r != Result::Ok) return r;
  s->width = settings.width;
  s->height = settings.height;
  s->fps = fps;
  s->frameUs = 1000000 / fps;
  s->sampleRate = sampleRate > 0 ? sampleRate : 0;
  s->channels = s->sampleRate > 0 ? channels : 0;
  s->stats.fps = fps;
  current_ = last_ = s;
  recording_ = true;
  return Result::Ok;
}

// Writes what the encoder takes, in order: audio first (it's small), then frames, each at its
// capture time. Stops at the first refusal; what's left is retried later.
void SegmentRecorder::drainLocked(Segment& s) {
  while (!s.audio.empty()) {
    Segment::Chunk& c = s.audio.front();
    Result r = s.sink->writeAudio(c.pcm.data(), c.frames, c.pos * 1000000 / s.sampleRate);
    if (r == Result::Again) break;
    if (r != Result::Ok) s.stats.audioDropped += c.frames;
    s.audioQueued -= c.frames;
    s.audio.pop_front();
  }
  while (!s.video.empty()) {
    Segment::Frame& f = s.video.front();
    f.frame.ptsUs = f.captureUs - s.firstUs;
    Result r = s.sink->writeVideo(f.frame);
    if (r == Result::Again) break;
    if (r == Result::Ok) {
      ++s.stats.written;
      s.lastWrittenUs = f.captureUs;
    } else {
      ++s.stats.dropped;
    }
    s.video.pop_front();
  }
}

void SegmentRecorder::video(const VideoFrame& camera) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!current_ || !recording_ || !camera.image) return;
  lastCallNs_ = steadyNs();
  admitLocked(*current_, camera);
  if (current_->pending()) wake_.notify_one();
}

void SegmentRecorder::admitLocked(Segment& s, const VideoFrame& camera) {
  int64_t t = camera.ptsUs;
  if (s.firstUs >= 0) {
    if (t <= s.lastKeptUs) return;  // not after the last one
    // Off the rate's grid (a burst, a faster camera, a lower rate): skipped. A quarter of a frame
    // of leeway for the camera's timing jitter.
    if (t < s.nextDueUs - s.frameUs / 4) {
      ++s.stats.skipped;
      return;
    }
  }
  int64_t interval = s.intervalUs();
  // On the grid; after a gap of more than a step (a stall), the grid restarts at this frame.
  s.nextDueUs = s.firstUs < 0 || t - s.nextDueUs >= interval ? t + interval : s.nextDueUs + interval;
  if (s.firstUs < 0) s.firstUs = t;
  s.lastKeptUs = t;

  Segment::Frame f;
  f.captureUs = t;
  f.frame.width = s.width;
  f.frame.height = s.height;
  ComposedLayer l;
  l.kind = ComposedLayer::Kind::Video;
  l.frame = camera;
  l.fit = Fit::Fill;  // the camera's own size: exactly the frame
  f.frame.layers.push_back(std::move(l));
  s.video.push_back(std::move(f));
  drainLocked(s);
  thinLocked(s);
  s.stats.maxPending = std::max(s.stats.maxPending, int(s.video.size()));
  adaptRateLocked(s, t);
}

// Over the limit, the waiting frame whose loss leaves the smallest gap goes: drops spread out
// evenly instead of freezing the picture. Never the segment's first frame (time 0) or the newest.
void SegmentRecorder::thinLocked(Segment& s) {
  size_t limit = size_t(std::max(2, options_.maxPendingFrames));
  while (s.video.size() > limit) {
    size_t best = 0;
    int64_t bestGap = INT64_MAX;
    for (size_t i = 0; i + 1 < s.video.size(); ++i) {
      int64_t prev = i > 0 ? s.video[i - 1].captureUs : s.lastWrittenUs;
      if (prev < 0) continue;  // the first frame
      int64_t gap = s.video[i + 1].captureUs - prev;
      if (gap < bestGap) {
        bestGap = gap;
        best = i;
      }
    }
    s.video.erase(s.video.begin() + std::ptrdiff_t(best));
    ++s.stats.dropped;
  }
}

// A queue that stays at least half full means the encoder can't keep up: the next rate down. One
// that stays nearly empty, the next rate up; after a step up that didn't hold, it waits longer.
// Times are capture times, so a burst of frames delivered at once doesn't count as a slow encoder.
void SegmentRecorder::adaptRateLocked(Segment& s, int64_t t) {
  size_t depth = s.video.size();
  size_t half = size_t(std::max(1, options_.maxPendingFrames / 2));
  if (depth >= half) {
    if (s.fullSinceUs < 0) s.fullSinceUs = t;
  } else {
    s.fullSinceUs = -1;
  }
  if (depth <= 1) {
    if (s.calmSinceUs < 0) s.calmSinceUs = t;
  } else {
    s.calmSinceUs = -1;
  }
  bool slow = s.fullSinceUs >= 0 && t - s.fullSinceUs >= kFullForUs;
  if (slow && s.step + 1 < kRateSteps && (s.lastStepUs < 0 || t - s.lastStepUs >= kStepDownGapUs)) {
    if (s.lastStepUpUs >= 0 && t - s.lastStepUpUs < 2 * s.stepUpAfterUs) {
      s.stepUpAfterUs = std::min(2 * s.stepUpAfterUs, kMaxStepUpAfterUs);
    }
    ++s.step;
    ++s.stats.rateReductions;
    s.lastStepUs = t;
    s.fullSinceUs = s.calmSinceUs = -1;
  } else if (s.step > 0 && s.calmSinceUs >= 0 && t - s.calmSinceUs >= s.stepUpAfterUs && t - s.lastStepUs >= s.stepUpAfterUs) {
    --s.step;
    s.lastStepUs = s.lastStepUpUs = t;
    s.calmSinceUs = -1;
  }
  s.stats.fps = int(int64_t(s.fps) * kRates[s.step].den / kRates[s.step].num);
}

void SegmentRecorder::audio(const int16_t* pcm, int frames, int64_t hostTimeNs) {
  std::unique_lock<std::mutex> lock(mu_);
  if (!current_ || !recording_) return;
  Segment& s = *current_;
  if (s.sampleRate == 0 || s.firstUs < 0 || frames <= 0) return;  // audio starts with the first frame
  lastCallNs_ = steadyNs();
  int skip = 0;
  if (s.audioPos < 0) {  // the first chunk: placed by its time, what came before the first frame cut off
    int64_t at = (hostTimeNs / 1000 - s.firstUs) * s.sampleRate / 1000000;
    if (at < 0) skip = int(std::min<int64_t>(-at, frames));
    s.audioPos = std::max<int64_t>(0, at);
  }
  if (skip >= frames) {
    s.audioPos = -1;  // all before the first frame: the next chunk is placed by its time
    return;
  }
  // Later chunks follow on: the microphone's clock is steady, and gaps would be heard. A chunk lost
  // to overflow still takes its time (silence), so what follows stays in sync.
  Segment::Chunk c;
  c.frames = frames - skip;
  c.pos = s.audioPos;
  c.pcm.assign(pcm + size_t(skip) * s.channels, pcm + size_t(frames) * s.channels);
  s.audioPos += c.frames;
  s.audioQueued += c.frames;
  s.audio.push_back(std::move(c));
  drainLocked(s);
  int64_t limit = options_.maxPendingAudioUs * s.sampleRate / 1000000;
  while (s.audioQueued > limit && s.audio.size() > 1) {  // the oldest goes: what's heard now stays
    s.stats.audioDropped += s.audio.front().frames;
    s.audioQueued -= s.audio.front().frames;
    s.audio.pop_front();
  }
  if (s.pending()) wake_.notify_one();
}

void SegmentRecorder::stop(std::function<void(Result, int64_t)> done) {
  std::shared_ptr<Segment> s;
  {
    std::lock_guard<std::mutex> lock(mu_);
    s = std::move(current_);
    if (s) {
      recording_ = false;
      releaseFinished();
      drainLocked(*s);
      if (s->pending()) {  // the encoder is busy: the thread finishes the file
        draining_.push_back({s, std::move(done)});
        wake_.notify_one();
        return;
      }
    }
  }
  // Outside the lock: a sink may finish at once, and `done` may call back.
  if (!s) return done(Result::InvalidState, 0);
  finish(s, done);
}

void SegmentRecorder::finish(const std::shared_ptr<Segment>& s, const std::function<void(Result, int64_t)>& done) {
  int64_t duration = s->lastWrittenUs < 0 ? 0 : s->lastWrittenUs - s->firstUs + s->frameUs;
  if (duration == 0) {
    s->sink.reset();  // nothing written: no file worth keeping
    return done(Result::WriteFailed, 0);
  }
  auto finished = std::make_shared<std::atomic<bool>>(false);
  IExportSink* sink = s->sink.get();
  {
    std::lock_guard<std::mutex> lock(mu_);
    finishing_.push_back({std::move(s->sink), finished});
  }
  sink->endAudio();
  sink->finish([done, duration, finished](Result r) {
    done(r, r == Result::Ok ? duration : 0);
    *finished = true;
  });
}

// Retries a refused write once nobody else has for a moment, and finishes stopped segments.
void SegmentRecorder::run() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!quit_) {
    std::vector<Draining> ready;
    for (auto it = draining_.begin(); it != draining_.end();) {
      drainLocked(*it->segment);
      if (it->segment->pending()) {
        ++it;
      } else {
        ready.push_back(std::move(*it));
        it = draining_.erase(it);
      }
    }
    if (!ready.empty()) {
      lock.unlock();
      for (const Draining& d : ready) finish(d.segment, d.done);
      lock.lock();
      continue;
    }
    bool waiting = current_ && current_->pending();
    if (waiting && steadyNs() - lastCallNs_ >= options_.idleRetryUs * 1000) {
      drainLocked(*current_);
      waiting = current_->pending();
    }
    if (waiting || !draining_.empty()) {
      wake_.wait_for(lock, kRetryPoll);
    } else {
      wake_.wait(lock);
    }
  }
}

int64_t SegmentRecorder::durationUs() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !last_ || last_->firstUs < 0 ? 0 : last_->lastKeptUs - last_->firstUs + last_->frameUs;
}

int SegmentRecorder::pendingVideoFrames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_ ? int(last_->video.size()) : 0;
}

SegmentRecorder::Stats SegmentRecorder::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_ ? last_->stats : Stats();
}

}  // namespace mf
