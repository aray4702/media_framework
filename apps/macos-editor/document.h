#pragma once

// The editor's document: an mf::Scene plus the edits the UI makes to it. Every edit leaves each
// track valid (scene_graph_spec.md §6): items in start order, overlapping only across a
// transition, which sets the second item's start (R4) and is at most half of each item (R5).

#include <cstdint>
#include <map>
#include <string>

#include "mf/scene.h"

namespace editor {

struct Selection {
  int track = -1;           // -1: nothing (the project)
  int item = -1;            // -1: the track itself
  bool transition = false;  // the join into `item` from the one before it, not the item
  bool operator!=(const Selection& o) const { return track != o.track || item != o.item || transition != o.transition; }
  bool operator==(const Selection& o) const { return !(*this != o); }
};

class Document {
 public:
  static constexpr int64_t kMinDurationUs = 100000;
  static constexpr int64_t kStillDurationUs = 5000000;  // new image, text and color items

  Document();

  mf::Scene scene;
  mf::VideoFilter filter;  // the player's global filter

  mf::SceneTrack& track(int t) { return scene.tracks[t]; }
  mf::SceneItem& item(int t, int k) { return scene.tracks[t].items[k]; }
  int tracks() const { return int(scene.tracks.size()); }
  bool empty() const;  // no item on any track

  // Video tracks go on top of the others, audio tracks below them. Returns the new index, or
  // -1 when the scene already has 16 tracks.
  int addTrack(bool video);
  void removeTrack(int t);
  // Moves a track one step up (+1) or down (-1) among the tracks. Returns its new index.
  int moveTrack(int t, int delta);

  // Adds an item at `atUs`, or right after the item playing there; later items move along
  // to make room. `lengthUs`: the file's length (video, audio), which limits its duration.
  // Returns the item's index.
  int insertItem(int t, mf::SceneItem item, int64_t atUs, int64_t lengthUs = 0);
  void removeItem(int t, int k);

  // Moves an item, between the item before it and the one after it. An item joined to the
  // one before it by a transition can't move on its own: it starts where the transition says.
  void moveItem(int t, int k, int64_t startUs);
  // Up to the start of the next item, and within the file for video and audio.
  void setDuration(int t, int k, int64_t durationUs);
  // Changes the item's length at its start; video and audio play from later (or earlier) in the
  // file, never back past its start. Longer, the start first moves left into the free space
  // before the item (none when a transition joins it to the one before); past that, and when
  // shorter, it's a ripple trim: the start stays and the items after it move by as much.
  void trimStart(int t, int k, int64_t durationUs);
  void setIn(int t, int k, int64_t inUs);
  void setSpeed(int t, int k, double speed);
  int64_t maxDurationUs(int t, int k) const;

  // Copies video item k's sound (same file, `in`, speed and time) onto the lowest audio track
  // free for its time, or a new one, and mutes the video. Returns the new item's track and sets
  // *audioItem, or -1: not a video item, or 16 tracks already. *videoTrack follows the video
  // item when a new audio track shifts the tracks.
  int detachAudio(int* videoTrack, int k, int* audioItem);
  // Whether items k - 1 and k of track t meet where a transition can go: they have one, or the
  // second starts where the first ends and both are videos or images.
  bool junction(int t, int k);
  // The transition from item k - 1 into item k, or null.
  mf::SceneTransition* transitionInto(int t, int k);
  // Adds, changes or removes (kind empty) the transition into item k (k >= 1).
  void setTransition(int t, int k, const mf::SceneTransition* x);

 private:
  // Places each item after the one before it: at the transition's overlap when joined, else
  // no earlier than its end (moving it, and so later items, along).
  void normalize(int t);
  std::string newId(const char* prefix);

  std::map<std::string, int64_t> lengthUs_;  // by item id: the file's length (video, audio)
  int nextId_ = 1;
};

}  // namespace editor
