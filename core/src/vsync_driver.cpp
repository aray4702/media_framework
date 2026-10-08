#include <algorithm>

#include "drivers.h"

namespace mf {
namespace {
constexpr int64_t kMs = 1000000;  // ns
}  // namespace

void VsyncDriver::restart() {
  nextSlotNs_ = gridNs_ = 0;
  last_.reset();
  hold_.assign(size_t(ctx_.layout.items()), Hold::None);
  holdSinceUs_.assign(size_t(ctx_.layout.items()), 0);
  control_.restart();
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
  bool holdComposition = false, holdExpired = false;
  std::vector<int> visible(size_t(s.lanes()), -1);  // per lane: its visible video item
  std::vector<bool> behind(size_t(s.lanes()), false);
  for (int i : s.videoAt(t)) {
    const VideoFrame* f = s.frameOf(i);
    bool exact = s.exactAt(i, t);
    size_t li = size_t(ctx_.layout.laneOf(i));
    visible[li] = i;
    behind[li] = behind[li] || !exact;
    // A frame arriving after its item's deadline may be much older than the playhead.  Once an
    // item has missed its first visible frame, wait until the sampler has consumed all frames
    // through t (its next frame is after t) before replacing the previous complete picture.
    // An item that is exact without a frame will never get one for t (its first frame is later,
    // or none decodes): it never holds the other layers.  Nor does one held for kMaxHoldUs: its
    // late frames are shown until it catches up.
    Hold& h = hold_[size_t(i)];
    if (exact) {
      h = Hold::None;
    } else if (h == Hold::None && !f) {
      h = Hold::Holding;
      holdSinceUs_[size_t(i)] = t;
    }
    if (h == Hold::Holding) {
      if (t - holdSinceUs_[size_t(i)] < kMaxHoldUs) {
        holdComposition = true;
      } else {
        h = Hold::Expired;
        holdExpired = true;
        ctx_.metrics.countHoldExpiry();
      }
    }
    if (!exact && f && s.timeOf(*f) + ctx_.items[i].info.video.frameDurationUs <= t) ctx_.metrics.countLateLayer();
  }
  control_.tick(ctx_, t, visible, behind, holdExpired);
  // A genuine gap has no visible video and still composes the output background normally.
  if (holdComposition) return Progress::did();
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
