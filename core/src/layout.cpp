#include "layout.h"

#include <algorithm>
#include <cmath>

namespace mf {

bool SceneLayout::build(const Scene& scene, std::string* error) {
  scene_ = &scene;
  flat_.clear();
  trackItems_.assign(scene.tracks.size(), {});
  durationUs_ = 0;
  for (int t = 0; t < int(scene.tracks.size()); ++t) {
    const SceneTrack& track = scene.tracks[t];
    if (!track.enabled) continue;
    int first = items();
    for (int k = 0; k < int(track.items.size()); ++k) {
      trackItems_[t].push_back(items());
      flat_.push_back({t, k});
      durationUs_ = std::max(durationUs_, track.items[k].endUs());
    }
    for (int x = 0; x < int(track.transitions.size()); ++x) {
      int from = track.transitions[x].from;
      flat_[first + from].out = x;
      flat_[first + from + 1].in = x;
    }
  }

  // Greedy interval coloring: the fewest lanes such that items sharing one never overlap.
  std::vector<int> order;
  for (int i = 0; i < items(); ++i) {
    if (decodable(i)) order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return item(a).startUs < item(b).startUs; });
  for (int64_t margin : {kPrerollUs, int64_t{0}}) {
    lane_.assign(items(), -1);
    laneItems_.clear();
    std::vector<int64_t> laneEnd;
    for (int i : order) {
      int64_t from = item(i).startUs - margin;
      int lane = -1;
      for (int l = 0; l < int(laneEnd.size()) && lane < 0; ++l) {
        if (laneEnd[l] <= from) lane = l;
      }
      if (lane < 0) {
        lane = int(laneEnd.size());
        laneEnd.push_back(0);
        laneItems_.emplace_back();
      }
      lane_[i] = lane;
      laneEnd[lane] = item(i).endUs();
      laneItems_[lane].push_back(i);
    }
    if (lanes() <= kMaxLanes) return true;
  }
  if (error) *error = "more than " + std::to_string(kMaxLanes) + " video and audio items play at the same time (R11)";
  return false;
}

int64_t SceneLayout::mediaUs(int i, int64_t t) const {
  const SceneItem& it = item(i);
  return it.inUs + std::llround(double(std::max<int64_t>(0, t - it.startUs)) * it.speed);
}

int64_t SceneLayout::timelineUs(int i, int64_t m) const {
  const SceneItem& it = item(i);
  return it.startUs + std::llround(double(m - it.inUs) / it.speed);
}

void SceneLayout::visibleAt(int64_t t, std::vector<Visible>* out) const {
  out->clear();
  for (int tr = 0; tr < int(trackItems_.size()); ++tr) {
    if (!scene_->tracks[tr].video) continue;
    const std::vector<int>& list = trackItems_[tr];
    // Items are in start order and overlap only in pairs, so the last one started is found
    // first from the end, and the one before it is the other half of a transition.
    auto it = std::upper_bound(list.begin(), list.end(), t, [&](int64_t time, int i) { return time < item(i).startUs; });
    if (it == list.begin()) continue;
    int b = *(it - 1);
    if (!active(b, t)) continue;
    int a = it - 1 == list.begin() ? -1 : *(it - 2);
    if (a < 0 || !active(a, t) || flat_[a].out < 0) {
      out->push_back({b, tr});
      continue;
    }
    const SceneTransition& x = transition(a, flat_[a].out);
    double p = x.durationUs > 0 ? x.easing.apply(double(t - item(b).startUs) / double(x.durationUs)) : 1;
    float dir = x.direction == Direction::Left || x.direction == Direction::Up ? -1.0f : 1.0f;
    bool vertical = x.direction == Direction::Up || x.direction == Direction::Down;
    Visible va{a, tr}, vb{b, tr};
    switch (x.kind) {
      case SceneTransitionKind::Cut:
        break;
      case SceneTransitionKind::Crossfade:
        va.fade = float(1 - p);
        vb.fade = float(p);
        va.mix = vb.mix = true;
        break;
      case SceneTransitionKind::Push:
        (vertical ? va.offsetY : va.offsetX) = dir * float(p);
        (vertical ? vb.offsetY : vb.offsetX) = dir * float(p - 1);
        break;
      case SceneTransitionKind::Slide:
        (vertical ? vb.offsetY : vb.offsetX) = dir * float(p - 1);
        break;
      case SceneTransitionKind::Wipe: {  // B is revealed by an edge moving in `direction`
        int axis = vertical ? 1 : 0;
        if (dir < 0) vb.clip[axis] = float(1 - p);
        else vb.clip[axis + 2] = float(p);
        break;
      }
    }
    out->push_back(va);
    out->push_back(vb);
  }
}

double SceneLayout::transitionGain(int i, int64_t t) const {
  auto fade = [](AudioFade mode, double q, bool in) {
    switch (mode) {
      case AudioFade::EqualGain: return in ? q : 1 - q;
      case AudioFade::EqualPower: return in ? std::sin(q * M_PI / 2) : std::cos(q * M_PI / 2);
      case AudioFade::Cut: return (q >= 0.5) == in ? 1.0 : 0.0;
    }
    return 1.0;
  };
  double gain = 1;
  if (flat_[i].out >= 0) {  // fading out into the next item
    const SceneTransition& x = transition(i, flat_[i].out);
    int64_t start = item(i + 1).startUs;
    if (x.durationUs > 0 && t >= start) gain *= fade(x.audio, std::min(1.0, double(t - start) / double(x.durationUs)), false);
  }
  if (flat_[i].in >= 0) {  // fading in from the previous item
    const SceneTransition& x = transition(i, flat_[i].in);
    int64_t start = item(i).startUs;
    if (x.durationUs > 0 && t < start + x.durationUs) gain *= fade(x.audio, std::max(0.0, double(t - start) / double(x.durationUs)), true);
  }
  return gain;
}

}  // namespace mf
