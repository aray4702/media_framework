#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mf/scene.h"

namespace mf {

// Where every item sits and plays (scene_graph_spec.md §2, §5), for the enabled tracks.
// Items are numbered in track order, then item order. Decodable items (video, audio) get a
// lane: a set of decoders and queues reused by items that never overlap (§7). Lanes are
// assigned greedily in start order, each item on a lane free a preroll margin before its start
// whenever the lane limit allows, so its decoder can start before the item does.
class SceneLayout {
 public:
  static constexpr int kMaxLanes = 8;
  // When lanes allow, an item's decoder is reserved from this far before its own start until its
  // end.  This is deliberately an item-local interval: a clip in the middle of an otherwise empty
  // timeline needs the same lead time as the incoming half of a cut.
  static constexpr int64_t kPrerollUs = 2000000;

  bool build(const Scene&, std::string* error);  // fails when more than kMaxLanes items play at once

  int items() const { return static_cast<int>(flat_.size()); }
  const SceneItem& item(int i) const { return scene_->tracks[flat_[i].track].items[flat_[i].index]; }
  const SceneTrack& trackOf(int i) const { return scene_->tracks[flat_[i].track]; }
  bool decodable(int i) const { return item(i).type == ItemType::Video || item(i).type == ItemType::Audio; }
  int64_t durationUs() const { return durationUs_; }
  bool active(int i, int64_t t) const { return t >= item(i).startUs && t < item(i).endUs(); }

  int lanes() const { return static_cast<int>(laneItems_.size()); }
  int laneOf(int i) const { return lane_[i]; }  // -1 when not decodable
  const std::vector<int>& laneItems(int lane) const { return laneItems_[lane]; }  // in start order

  // Media time of item i at timeline time t (t before the start maps to `in`), and back.
  int64_t mediaUs(int i, int64_t t) const;
  int64_t timelineUs(int i, int64_t mediaUs) const;

  // What a video track shows at t: its item, or the two items of a transition (outgoing first).
  // The two items of a transition are combined on their own before the track is drawn (§5.1).
  struct Visible {
    int item;
    int track = -1;                  // scene track index
    float offsetX = 0, offsetY = 0;  // push and slide, in output widths / heights
    float clip[4] = {0, 0, 1, 1};    // wipe: the visible part of the output
    float fade = 1;                  // crossfade: multiplies the opacity
    bool mix = false;                // crossfade: the pair is summed, (1 − p)·A + p·B, not drawn B over A
  };
  // Every enabled video track, bottom to top.
  void visibleAt(int64_t t, std::vector<Visible>* out) const;
  // An item's audio gain from the transitions it takes part in (1 outside them).
  double transitionGain(int i, int64_t t) const;

 private:
  struct Flat {
    int track, index;
    int in = -1, out = -1;  // index of the transition into / out of the item, in its track
  };
  const SceneTransition& transition(int i, int which) const { return trackOf(i).transitions[which]; }

  const Scene* scene_ = nullptr;
  std::vector<Flat> flat_;
  std::vector<std::vector<int>> trackItems_;  // per scene track: its items (empty when disabled)
  std::vector<int> lane_;
  std::vector<std::vector<int>> laneItems_;
  int64_t durationUs_ = 0;
};

}  // namespace mf
