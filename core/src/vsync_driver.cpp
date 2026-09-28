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
  if (t >= s.layout().durationUs()) {
    out.finish();
    return Progress::did();
  }
  s.advanceAll(t);
  for (int i : s.videoAt(t)) {
    const VideoFrame* f = s.frameOf(i);
    if (!s.exactAt(i, t) && f && s.timeOf(*f) + ctx_.items[i].info.video.frameDurationUs <= t) ctx_.metrics.countLateLayer();
  }
  ComposedFrame frame = s.composeAt(t);
  frame.presentAtNs = slot;
  if (unchanged(frame)) return Progress::did();  // the frame on screen stays
  last_ = frame;
  out.emit(std::move(frame));
  return Progress::did();
}

// Nothing visible differs from the last output: the same frames, and every value the same.
bool VsyncDriver::unchanged(const ComposedFrame& f) const {
  if (!last_ || last_->layers.size() != f.layers.size() || !(last_->filter == f.filter) || !(last_->groups == f.groups)) return false;
  for (size_t i = 0; i < f.layers.size(); ++i) {
    const ComposedLayer &a = last_->layers[i], &b = f.layers[i];
    if (a.kind != b.kind || a.item != b.item || a.frame.ptsUs != b.frame.ptsUs || a.text != b.text || !(a.color == b.color) ||
        a.x != b.x || a.y != b.y || a.scale != b.scale || a.rotation != b.rotation || a.offsetX != b.offsetX ||
        a.offsetY != b.offsetY || a.opacity != b.opacity || a.blend != b.blend || a.group != b.group ||
        !(a.effects == b.effects) || !std::equal(a.clip, a.clip + 4, b.clip)) {
      return false;
    }
  }
  return true;
}

}  // namespace mf
