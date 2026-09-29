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
  int track = -1;  // -1: nothing (the project)
  int item = -1;   // -1: the track itself
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
  // Changes the item's length at its start (a ripple trim): it keeps its place on the timeline,
  // video and audio play from later (or earlier) in the file, and the items after it on the
  // track move by the same amount. Not back past the file's start.
  void trimStart(int t, int k, int64_t durationUs);
  void setIn(int t, int k, int64_t inUs);
  void setSpeed(int t, int k, double speed);
  int64_t maxDurationUs(int t, int k) const;

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
