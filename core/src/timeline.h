#pragma once

#include <cstdint>
#include <vector>

#include "mf/types.h"

namespace mf {

// Where each clip sits on the timeline (§2.4). Clip i+1 starts `transitionUs` before clip i
// ends, so the two overlap for the transition. The transition is at most half the shortest
// clip, so at most two clips are active at any time and clip i+2 starts after clip i ends:
// clip i always plays on lane i % 2.
class TimelineLayout {
 public:
  void build(const std::vector<int64_t>& clipDurationsUs, const Transition&);

  int clips() const { return static_cast<int>(startUs_.size()); }
  int64_t durationUs() const { return durationUs_; }
  int64_t transitionUs() const { return transitionUs_; }
  int64_t startUs(int clip) const { return startUs_[clip]; }
  int64_t endUs(int clip) const { return endUs_[clip]; }
  bool active(int clip, int64_t t) const { return t >= startUs_[clip] && t < endUs_[clip]; }

  int firstActive(int64_t t) const;  // the earliest clip active at t (the last clip past the end)
  int lastActive(int64_t t) const;   // the latest clip active at t: it leads the output

  // How far clip c's incoming transition has run at t: 0 at its start, 1 once complete.
  float progress(int clip, int64_t t) const;
  // Horizontal offset of clip c at t, in output widths (the slide).
  float offsetX(int clip, int64_t t) const;
  // Audio gain of clip c at t: fades in and out over the transitions, so an overlap sums to 1.
  float gain(int clip, int64_t t) const;

 private:
  std::vector<int64_t> startUs_, endUs_;
  int64_t transitionUs_ = 0, durationUs_ = 0;
  TransitionKind kind_ = TransitionKind::Cut;
};

}  // namespace mf
