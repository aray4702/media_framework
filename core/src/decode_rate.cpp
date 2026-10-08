#include "decode_rate.h"

#include <algorithm>
#include <cmath>

namespace mf {

DecodeRate::Tick DecodeRate::tick(int64_t t, std::vector<Lane>& lanes, bool holdExpired) {
  Tick out;
  int lo = kMaxStep, hi = 0, active = 0;
  double sum = 0;
  bool behind = false;
  for (const Lane& l : lanes) {
    if (!l.active) continue;
    lo = std::min(lo, l.step);
    hi = std::max(hi, l.step);
    sum += l.step;
    ++active;
    behind |= l.behind;
  }
  int64_t dt = lastTickUs_ >= 0 && t > lastTickUs_ ? t - lastTickUs_ : 0;
  lastTickUs_ = t;
  if (active == 0) {  // nothing decoding: no evidence either way
    behindSinceUs_ = aheadSinceUs_ = -1;
    return out;
  }
  if (hi > 0) out.reducedUs = dt;
  momentum_ += (1 - std::exp(-double(dt) / double(kMomentumUs))) * (sum / active - momentum_);

  // Ahead: nothing is late, and the most reduced lanes have decoded well past t.
  bool ahead = !behind;
  for (const Lane& l : lanes) ahead &= !l.active || l.step != hi || l.ahead;
  behindSinceUs_ = behind ? (behindSinceUs_ < 0 ? t : behindSinceUs_) : -1;
  aheadSinceUs_ = ahead ? (aheadSinceUs_ < 0 ? t : aheadSinceUs_) : -1;

  bool slow = holdExpired || (behindSinceUs_ >= 0 && t - behindSinceUs_ >= kBehindForUs);
  if (slow && lo < kMaxStep && (lastStepUs_ < 0 || t - lastStepUs_ >= kStepGapUs)) {
    if (lastStepUpUs_ >= 0 && t - lastStepUpUs_ < 2 * stepUpAfterUs_) {
      stepUpAfterUs_ = std::min(2 * stepUpAfterUs_, kMaxStepUpAfterUs);  // the last step up didn't hold
    }
    for (Lane& l : lanes) l.step += l.active && l.step == lo;  // the least reduced lanes
    lastStepUs_ = t;
    behindSinceUs_ = aheadSinceUs_ = -1;
    out.change = Change::Down;
  } else if (hi > 0 && aheadSinceUs_ >= 0 && t - aheadSinceUs_ >= stepUpAfterUs_ &&
             (lastStepUs_ < 0 || t - lastStepUs_ >= stepUpAfterUs_)) {
    for (Lane& l : lanes) l.step -= l.active && l.step == hi;  // the most reduced lanes
    lastStepUs_ = lastStepUpUs_ = t;
    aheadSinceUs_ = -1;
    out.change = Change::Up;
  }
  return out;
}

int DecodeRate::startStep(const std::vector<Lane>& lanes) const {
  int s = std::clamp(int(std::lround(momentum_)), 0, kMaxStep);
  int lo = kMaxStep, hi = 0;
  bool any = false;
  for (const Lane& l : lanes) {
    if (!l.active) continue;
    lo = std::min(lo, l.step);
    hi = std::max(hi, l.step);
    any = true;
  }
  if (!any) return s;
  int floor = std::max(0, hi - 1), ceiling = std::min(kMaxStep, lo + 1);
  return floor <= ceiling ? std::clamp(s, floor, ceiling) : floor;  // the lanes are at most one apart: floor <= ceiling
}

void DecodeRate::restart() {
  lastTickUs_ = behindSinceUs_ = aheadSinceUs_ = lastStepUs_ = lastStepUpUs_ = -1;
}

}  // namespace mf
