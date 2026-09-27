#include <algorithm>

#include "drivers.h"

namespace mf {
namespace {
constexpr int64_t kMs = 1000000;  // ns
}  // namespace

void VsyncDriver::restart() {
  nextSlotNs_ = gridNs_ = 0;
  last_.reset();
}

Progress VsyncDriver::step(FrameSampler& s, CompositionOutput& out) {
  {
    std::lock_guard<std::mutex> lock(ctx_.playMu);
    if (!ctx_.outputRunning) {  // paused: T3 wakes us when output starts
      nextSlotNs_ = gridNs_ = 0;
      return Progress::idle();
    }
  }
  int64_t now = ctx_.hostClock.nowNs();
  int64_t period = std::max<int64_t>(ctx_.display->vsyncPeriodNs(), kMs);
  int64_t lead = ctx_.display->latencyNs();
  int64_t grid = ctx_.display->vsyncGridNs();
  if (grid == 0) grid = gridNs_ ? gridNs_ : (gridNs_ = now);  // no real vsync known: our own grid
  // The first refresh not yet composed that a frame handed over now can still reach.
  int64_t earliest = std::max(now + lead, nextSlotNs_);
  int64_t slot = grid + (earliest - grid + period - 1) / period * period;
  int64_t composeNs = slot - lead - period / 2;
  if (now < composeNs) return Progress::waitUntil(composeNs);
  nextSlotNs_ = slot + period / 2;

  int64_t t = ctx_.master.nowUs(now) + (slot - now) / 1000;
  if (last_ && t <= last_->ptsUs) return Progress::did();  // the clock is holding (audio not heard yet)
  const TimelineLayout& layout = s.layout();
  if (t >= layout.durationUs()) {
    out.finish();
    return Progress::did();
  }
  s.advanceAll(t);
  for (int c = layout.firstActive(t); c < layout.clips() && layout.active(c, t); ++c) {
    const VideoFrame* f = s.frameOf(c);
    if (!s.exactAt(c, t) && f && s.timeOf(*f) + ctx_.infos[c].video.frameDurationUs <= t) ctx_.metrics.countLateLayer();
  }
  ComposedFrame frame = s.composeAt(t);
  frame.presentAtNs = slot;
  if (frame.layerCount == 0 || unchanged(frame)) return Progress::did();  // the frame on screen stays
  last_ = frame;
  out.emit(std::move(frame));
  return Progress::did();
}

// Nothing visible differs from the last output: same frames, offsets, caption and filter.
bool VsyncDriver::unchanged(const ComposedFrame& f) const {
  if (!last_ || last_->layerCount != f.layerCount || last_->text != f.text || !(last_->filter == f.filter)) return false;
  for (int i = 0; i < f.layerCount; ++i) {
    const ComposedFrame::Layer &a = last_->layers[i], &b = f.layers[i];
    if (a.frame.clip != b.frame.clip || a.frame.ptsUs != b.frame.ptsUs || a.offsetX != b.offsetX) return false;
  }
  return true;
}

}  // namespace mf
