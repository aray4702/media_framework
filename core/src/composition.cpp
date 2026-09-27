#include "composition.h"

#include <algorithm>
#include <deque>

#include "drivers.h"

namespace mf {

// --- FrameSampler ---------------------------------------------------------------------------

void FrameSampler::restart(uint32_t serial, const PendingSeek& target) {
  serial_ = serial;
  for (int li = 0; li < kLanes; ++li) {
    lanes_[li] = LaneView{};
    lanes_[li].clip = target.laneClip[li];
    lanes_[li].done = lanes_[li].clip >= layout().clips();
  }
}

bool FrameSampler::pop(int li) {
  LaneView& lane = lanes_[li];
  if (lane.head || lane.done) return false;
  VideoFrame f;
  if (!ctx_.lanes[li].frames.tryPop(&f)) return false;
  if (f.serial != serial_) return true;
  if (f.eos) {  // the lane's clip has no more frames; its next clip is two ahead
    lane.clip = f.clip + kLanes;
    lane.done = lane.clip >= layout().clips();
    return true;
  }
  lane.head = std::move(f);
  return true;
}

void FrameSampler::take(int li) {
  LaneView& lane = lanes_[li];
  VideoFrame f = std::move(*lane.head);
  lane.head.reset();
  if (!layout().active(f.clip, timeOf(f))) return;  // past the clip's end: cut by the next clip
  if (seeking_ && lane.held && lane.held->clip == f.clip) ctx_.metrics.countDecodeOnly();
  lane.held = std::move(f);
}

void FrameSampler::advance(int li, int64_t t) {
  LaneView& lane = lanes_[li];
  for (;;) {
    if (!lane.head && !pop(li)) return;
    if (!lane.head) continue;
    if (timeOf(*lane.head) > t) return;
    take(li);
  }
}

void FrameSampler::advanceAll(int64_t t) {
  for (int li = 0; li < kLanes; ++li) advance(li, t);
}

bool FrameSampler::exactAt(int c, int64_t t) const {
  const LaneView& lane = lanes_[Context::laneOf(c)];
  if (lane.done || lane.clip > c) return true;
  return lane.clip == c && lane.head && timeOf(*lane.head) > t;
}

bool FrameSampler::allExactAt(int64_t t) const {
  for (int c = layout().firstActive(t); c < layout().clips() && layout().active(c, t); ++c) {
    if (!exactAt(c, t)) return false;
  }
  return true;
}

bool FrameSampler::mayDeliver(int c) const {
  const LaneView& lane = lanes_[Context::laneOf(c)];
  return !lane.done && lane.clip <= c;
}

const VideoFrame* FrameSampler::frameOf(int c) const {
  const std::optional<VideoFrame>& held = lanes_[Context::laneOf(c)].held;
  return held && held->clip == c ? &*held : nullptr;
}

int64_t FrameSampler::floorOf(int li) const {
  const LaneView& lane = lanes_[li];
  if (lane.done) return kNever;
  int64_t floor = layout().startUs(lane.clip);
  if (lane.held && lane.held->clip == lane.clip) floor = std::max(floor, timeOf(*lane.held) + 1);
  return floor;
}

bool FrameSampler::done() const {
  return lanes_[0].done && lanes_[1].done && !lanes_[0].head && !lanes_[1].head;
}

ComposedFrame FrameSampler::composeAt(int64_t t) const {
  ComposedFrame out;
  out.ptsUs = t;
  out.serial = serial_;
  out.frameDurationUs = ctx_.infos[layout().leadClip(t)].video.frameDurationUs;
  for (int c = layout().firstActive(t); c < layout().clips() && layout().active(c, t); ++c) {
    if (const VideoFrame* f = frameOf(c)) out.layers[out.layerCount++] = {*f, layout().offsetX(c, t)};
  }
  out.text = ctx_.captionAt(t);
  out.filter = ctx_.filter();
  return out;
}

namespace {

// ---------------------------------------------------------------------------------------
// TC: composition (§2.4, §2.5). Hands composed frames to T3 in order. After each seek it shows
// the exact seek frame, then lets the driver pick the output times.
class CompositionStage : public Stage, private CompositionOutput {
 public:
  explicit CompositionStage(Context& c) : ctx_(c), sampler_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.probed) return Progress::idle();
    if (!driver_) driver_ = makeDriver();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) restart(serial);
    if (serial_ == 0) return Progress::idle();  // the preroll seek hasn't started yet
    while (!out_.empty()) {
      if (!ctx_.composed.tryPush(out_.front())) return Progress::idle();  // T3 frees a slot and wakes us
      out_.pop_front();
    }
    if (ended_) return Progress::idle();
    if (seeking_) return seekStep();
    return driver_->step(sampler_, *this);
  }

 private:
  std::unique_ptr<CompositionDriver> makeDriver() {
    switch (ctx_.driver) {
      case Driver::LeadingClip: return std::make_unique<LeadingClipDriver>();
      case Driver::Vsync: return std::make_unique<VsyncDriver>(ctx_);
      case Driver::Export: return std::make_unique<ExportDriver>(ctx_.exportSettings.fps);
    }
    return nullptr;
  }

  void restart(uint32_t serial) {
    serial_ = serial;
    PendingSeek target = ctx_.seekTarget();
    targetUs_ = std::clamp<int64_t>(target.targetUs, 0, ctx_.layout.durationUs() - 1);
    sampler_.restart(serial, target);
    driver_->restart();
    out_.clear();
    ended_ = false;
    setSeeking(driver_->startsWithSeek());
  }

  void setSeeking(bool seeking) {
    seeking_ = seeking;
    sampler_.setSeeking(seeking);
  }

  void emit(ComposedFrame f) override { out_.push_back(std::move(f)); }

  void finish() override {
    ComposedFrame eos;
    eos.eos = true;
    eos.serial = serial_;
    out_.push_back(std::move(eos));
    ended_ = true;
  }

  // Exact seek (§4), for every driver that starts with one: the leading clip's last frame at
  // or before the target (its first frame, if none is), with every other active clip's latest
  // frame at that time. While a newer seek is pending (scrubbing), shows whatever is decoded.
  Progress seekStep() {
    FrameSampler& s = sampler_;
    bool scrub = ctx_.hasPendingSeek();
    s.advanceAll(targetUs_);
    if (!s.allExactAt(targetUs_) && !scrub) return Progress::idle();  // the lanes' frame queues wake us

    int lead = s.layout().leadClip(targetUs_);
    const std::optional<VideoFrame>& head = s.head(Context::laneOf(lead));
    std::optional<int64_t> t;
    if (const VideoFrame* f = s.frameOf(lead)) {
      t = s.timeOf(*f);
    } else if (head && head->clip == lead) {
      t = s.timeOf(*head);  // nothing at or before the target: the first frame after it
    } else if (s.mayDeliver(lead) && !scrub) {
      return Progress::idle();
    }
    if (t && *t > targetUs_) {
      s.advanceAll(*t);
      if (!s.allExactAt(*t) && !scrub) return Progress::idle();
    }
    ComposedFrame out = s.composeAt(t.value_or(targetUs_));
    if (out.layerCount == 0) {
      if (!s.done()) return Progress::idle();  // nothing decoded yet, even when scrubbing
      setSeeking(false);
      finish();  // nothing to show: the seek completes without a frame (A5)
      return Progress::did();
    }
    setSeeking(false);
    driver_->seekShown(out);
    emit(std::move(out));
    return Progress::did();
  }

  Context& ctx_;
  FrameSampler sampler_;
  std::unique_ptr<CompositionDriver> driver_;  // made once probing is done: the driver is set by then
  uint32_t serial_ = 0;
  int64_t targetUs_ = 0;
  std::deque<ComposedFrame> out_;  // composed, waiting for room in the queue to T3
  bool seeking_ = false, ended_ = false;
};

}  // namespace

std::unique_ptr<Stage> makeCompositionStage(Context& ctx) { return std::make_unique<CompositionStage>(ctx); }

}  // namespace mf
