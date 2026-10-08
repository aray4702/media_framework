#include "drivers.h"

namespace mf {

// A step is stored only if T2 hasn't set the lane's step meanwhile (its next item's start step).
void DecodeControl::tick(Context& ctx, int64_t t, const std::vector<int>& visible, const std::vector<bool>& behind,
                         bool holdExpired) {
  size_t n = ctx.lanes.size();
  lanes_.resize(n);
  std::vector<int> before(n);
  for (size_t li = 0; li < n; ++li) {
    const Lane& lane = *ctx.lanes[li];
    DecodeRate::Lane& r = lanes_[li];
    int item = li < visible.size() ? visible[li] : -1;
    int64_t frameUs = item >= 0 ? ctx.items[item].info.video.frameDurationUs : int64_t(ctx.fpsDen) * 1000000 / ctx.fpsNum;
    r.active = lane.decodingVideo.load() || item >= 0;
    r.behind = li < behind.size() && behind[li];
    r.ahead = lane.decodedToUs.load() - t >= 2 * frameUs;
    r.step = before[li] = lane.decodeStep.load();
  }
  DecodeRate::Tick tick = rate_.tick(t, lanes_, holdExpired);
  if (tick.reducedUs > 0) ctx.metrics.addDecodeReduced(tick.reducedUs);
  if (tick.change != DecodeRate::Change::None) {
    ctx.metrics.countDecodeStep(tick.change == DecodeRate::Change::Down);
    for (size_t li = 0; li < n; ++li) {
      int expected = before[li];
      if (lanes_[li].step != expected) ctx.lanes[li]->decodeStep.compare_exchange_strong(expected, lanes_[li].step);
    }
  }
  ctx.decodeStartStep = rate_.startStep(lanes_);
}

}  // namespace mf
