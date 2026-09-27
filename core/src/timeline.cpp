#include "timeline.h"

#include <algorithm>

namespace mf {

void TimelineLayout::build(const std::vector<int64_t>& durations, const Transition& t,
                           std::vector<int64_t> frameDurationsUs) {
  kind_ = t.kind;
  frameUs_ = std::move(frameDurationsUs);
  int64_t shortest = durations.empty() ? 0 : *std::min_element(durations.begin(), durations.end());
  transitionUs_ = durations.size() < 2 || t.kind == TransitionKind::Cut ? 0 : std::clamp<int64_t>(t.durationUs, 0, shortest / 2);
  startUs_.clear();
  endUs_.clear();
  int64_t start = 0;
  for (int64_t d : durations) {
    startUs_.push_back(start);
    endUs_.push_back(start + d);
    start += d - transitionUs_;
  }
  durationUs_ = endUs_.empty() ? 0 : endUs_.back();
}

int TimelineLayout::firstActive(int64_t t) const {
  for (int c = 0; c < clips(); ++c) {
    if (t < endUs_[c]) return c;
  }
  return clips() - 1;
}

int TimelineLayout::lastActive(int64_t t) const {
  for (int c = clips() - 1; c > 0; --c) {
    if (t >= startUs_[c]) return c;
  }
  return 0;
}

int TimelineLayout::leadClip(int64_t t) const {
  int lead = lastActive(t);
  if (frameUs_.size() != startUs_.size()) return lead;
  for (int c = firstActive(t); c < lead; ++c) {
    if (active(c, t) && frameUs_[c] < frameUs_[lead]) lead = c;
  }
  return lead;
}

float TimelineLayout::progress(int c, int64_t t) const {
  if (c == 0 || transitionUs_ == 0) return 1.0f;
  return std::clamp(float(t - startUs_[c]) / float(transitionUs_), 0.0f, 1.0f);
}

float TimelineLayout::offsetX(int c, int64_t t) const {
  if (kind_ == TransitionKind::Cut) return 0;
  float dir = kind_ == TransitionKind::SlideLeft ? -1.0f : 1.0f;  // direction the content moves
  if (c + 1 < clips() && t >= startUs_[c + 1]) return dir * progress(c + 1, t);  // sliding out
  return -dir * (1.0f - progress(c, t));                                         // sliding in
}

float TimelineLayout::gain(int c, int64_t t) const {
  if (!active(c, t)) return 0;
  if (transitionUs_ == 0) return 1;
  float in = c == 0 ? 1.0f : float(t - startUs_[c]) / float(transitionUs_);
  float out = c + 1 == clips() ? 1.0f : float(endUs_[c] - t) / float(transitionUs_);
  return std::clamp(std::min(in, out), 0.0f, 1.0f);
}

}  // namespace mf
