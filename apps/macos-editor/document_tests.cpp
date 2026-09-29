// Checks that the editor's edits keep the scene valid (validateScene), and place items as the
// timeline shows them.

#include <cstdio>
#include <cstdlib>

#include "document.h"

using editor::Document;

static int failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

static constexpr int64_t kS = 1000000;

static void checkValid(const Document& d) {
  std::string error;
  mf::Result r = mf::validateScene(d.scene, &error);
  if (r != mf::Result::Ok) std::printf("invalid: %s\n", error.c_str());
  CHECK(r == mf::Result::Ok);
}

static mf::SceneItem still(mf::ItemType type) {
  mf::SceneItem it;
  it.type = type;
  it.durationUs = 5 * kS;
  if (type == mf::ItemType::Text) it.text = "Title";
  return it;
}

static void insertAndRemove() {
  Document d;
  int t = 0;
  CHECK(d.insertItem(t, still(mf::ItemType::Color), 0) == 0);
  CHECK(d.insertItem(t, still(mf::ItemType::Text), 2 * kS) == 1);  // inside the first: right after it
  CHECK(d.item(t, 1).startUs == 5 * kS);
  CHECK(d.insertItem(t, still(mf::ItemType::Color), 0) == 1);  // at 0, which the first starts at: after it
  CHECK(d.item(t, 1).startUs == 5 * kS && d.item(t, 2).startUs == 10 * kS);  // the text moved along
  checkValid(d);
  d.removeItem(t, 1);
  CHECK(d.item(t, 1).startUs == 10 * kS);  // no ripple on delete
  checkValid(d);
}

static void moveAndTrim() {
  Document d;
  d.insertItem(0, still(mf::ItemType::Color), 0);
  d.insertItem(0, still(mf::ItemType::Color), 10 * kS);
  d.moveItem(0, 1, 2 * kS);  // not over the first
  CHECK(d.item(0, 1).startUs == 5 * kS);
  d.moveItem(0, 0, 7 * kS);  // not over the second
  CHECK(d.item(0, 0).startUs == 0);
  d.moveItem(0, 1, 12 * kS);
  d.setDuration(0, 0, 20 * kS);  // up to the next item
  CHECK(d.item(0, 0).durationUs == 12 * kS);
  d.setDuration(0, 0, 0);
  CHECK(d.item(0, 0).durationUs == Document::kMinDurationUs);
  checkValid(d);
}

static void mediaLength() {
  Document d;
  mf::SceneItem v;
  v.type = mf::ItemType::Video;
  v.durationUs = 8 * kS;
  d.insertItem(0, v, 0, 8 * kS);
  d.setDuration(0, 0, 20 * kS);
  CHECK(d.item(0, 0).durationUs == 8 * kS);
  d.setIn(0, 0, 3 * kS);
  CHECK(d.item(0, 0).durationUs == 5 * kS);
  d.setSpeed(0, 0, 0.5);
  d.setDuration(0, 0, 20 * kS);
  CHECK(d.item(0, 0).durationUs == 10 * kS);
  checkValid(d);
}

static void trimStart() {
  Document d;
  mf::SceneItem v;
  v.type = mf::ItemType::Video;
  v.durationUs = 8 * kS;
  d.insertItem(0, still(mf::ItemType::Color), 0);        // [0, 5)
  d.insertItem(0, v, 6 * kS, 8 * kS);                    // [6, 14), in 0: 1 s free before it
  d.insertItem(0, still(mf::ItemType::Color), 15 * kS);  // [15, 20)
  auto at = [&](int k) { return d.item(0, k).startUs; };
  auto in = [&](int k) { return d.item(0, k).inUs; };

  d.trimStart(0, 1, 6 * kS);  // 2 s cut off its start: it stays, reads from 2 s; the next moves back
  CHECK(at(1) == 6 * kS && d.item(0, 1).durationUs == 6 * kS && in(1) == 2 * kS && at(2) == 13 * kS);
  d.trimStart(0, 1, 7 * kS);  // 1 s longer: into the free second before it; nothing else moves
  CHECK(at(1) == 5 * kS && d.item(0, 1).durationUs == 7 * kS && in(1) == kS && at(2) == 13 * kS);
  d.trimStart(0, 1, 20 * kS);  // no space left, and 1 s more of the file: a ripple
  CHECK(at(1) == 5 * kS && d.item(0, 1).durationUs == 8 * kS && in(1) == 0 && at(2) == 14 * kS);
  d.trimStart(0, 1, 0);  // shorter: at least the shortest item
  CHECK(at(1) == 5 * kS && d.item(0, 1).durationUs == Document::kMinDurationUs && at(2) == 6100000);
  d.trimStart(0, 1, 8 * kS);
  CHECK(at(1) == 5 * kS && in(1) == 0 && at(2) == 14 * kS);
  checkValid(d);

  d.trimStart(0, 0, 3 * kS);  // a still: its length only, and the rest moves back
  CHECK(at(0) == 0 && d.item(0, 0).durationUs == 3 * kS && in(0) == 0 && at(1) == 3 * kS && at(2) == 12 * kS);
  d.trimStart(0, 0, 4 * kS);  // at 0 s: no space before it, so a ripple
  CHECK(at(0) == 0 && d.item(0, 0).durationUs == 4 * kS && at(1) == 4 * kS && at(2) == 13 * kS);
  checkValid(d);

  mf::SceneTransition x;
  x.kind = mf::SceneTransitionKind::Crossfade;
  x.durationUs = kS;
  d.setTransition(0, 1, &x);  // the video now starts at 3 s
  CHECK(at(1) == 3 * kS && at(2) == 13 * kS);
  d.trimStart(0, 1, 7 * kS);  // joined: it keeps its place
  CHECK(at(1) == 3 * kS && in(1) == kS && at(2) == 12 * kS);
  d.trimStart(0, 1, 8 * kS);  // and grows by a ripple, not into the item before
  CHECK(at(1) == 3 * kS && in(1) == 0 && at(2) == 13 * kS);
  checkValid(d);
}

static void transitions() {
  Document d;
  d.insertItem(0, still(mf::ItemType::Color), 0);
  d.insertItem(0, still(mf::ItemType::Color), 7 * kS);
  d.insertItem(0, still(mf::ItemType::Color), 20 * kS);
  mf::SceneTransition x;
  x.kind = mf::SceneTransitionKind::Crossfade;
  x.durationUs = 10 * kS;  // longer than half of an item: shortened
  d.setTransition(0, 1, &x);
  CHECK(d.transitionInto(0, 1) && d.transitionInto(0, 1)->durationUs == 2500000);
  CHECK(d.item(0, 1).startUs == 2500000);
  checkValid(d);

  d.moveItem(0, 1, 9 * kS);  // joined: it doesn't move on its own
  CHECK(d.item(0, 1).startUs == 2500000);
  d.moveItem(0, 0, 1 * kS);  // the pair moves together
  CHECK(d.item(0, 1).startUs == 3500000);
  d.setDuration(0, 0, 3 * kS);  // shorter: so is the transition
  CHECK(d.transitionInto(0, 1)->durationUs == 1500000 && d.item(0, 1).startUs == 2500000);
  checkValid(d);

  d.insertItem(0, still(mf::ItemType::Text), 2 * kS);  // before the second: the transition goes
  CHECK(!d.transitionInto(0, 1) && !d.transitionInto(0, 2));
  CHECK(d.item(0, 1).type == mf::ItemType::Text && d.item(0, 1).startUs == 4 * kS && d.item(0, 2).startUs == 9 * kS);
  checkValid(d);

  d.setTransition(0, 2, &x);
  d.removeItem(0, 1);  // the transition into the removed item goes too
  CHECK(d.scene.tracks[0].transitions.empty());
  checkValid(d);

  mf::SceneTransition cut;
  cut.kind = mf::SceneTransitionKind::Cut;
  cut.durationUs = kS;
  d.setTransition(0, 1, &cut);
  CHECK(d.transitionInto(0, 1)->durationUs == 0);
  d.setTransition(0, 1, nullptr);
  CHECK(!d.transitionInto(0, 1));
  checkValid(d);
}

static void junctions() {
  Document d;
  mf::SceneItem image = still(mf::ItemType::Image);
  d.insertItem(0, image, 0);                               // [0, 5)
  d.insertItem(0, image, 5 * kS);                          // [5, 10): meets it
  d.insertItem(0, image, 11 * kS);                         // [11, 16): a gap
  d.insertItem(0, still(mf::ItemType::Text), 16 * kS);     // [16, 21): meets it, but text
  CHECK(!d.junction(0, 0) && d.junction(0, 1) && !d.junction(0, 2) && !d.junction(0, 3) && !d.junction(0, 4));
  mf::SceneTransition x;
  x.kind = mf::SceneTransitionKind::Crossfade;
  x.durationUs = kS;
  d.setTransition(0, 1, &x);  // overlapping now: still a junction
  CHECK(d.item(0, 1).startUs == 4 * kS && d.junction(0, 1));
  checkValid(d);
}

static void detach() {
  Document d;
  mf::SceneItem v;
  v.type = mf::ItemType::Video;
  v.src = "clip.mp4";
  v.durationUs = 8 * kS;
  v.inUs = kS;
  d.insertItem(0, v, 2 * kS, 10 * kS);  // [2, 10), from 1 s into the file
  int video = 0, k = -1;
  int a = d.detachAudio(&video, 0, &k);
  CHECK(a == 0 && video == 1 && k == 0);  // a new audio track, below the video one
  const mf::SceneItem& sound = d.item(a, k);
  CHECK(sound.type == mf::ItemType::Audio && sound.src == "clip.mp4" && sound.startUs == 2 * kS && sound.durationUs == 8 * kS &&
        sound.inUs == kS && !sound.mute);
  CHECK(d.item(video, 0).mute);
  d.setDuration(a, k, 20 * kS);  // the file's length still limits it: 10 s less 1 s in
  CHECK(d.item(a, k).durationUs == 9 * kS);
  int again = -1;
  d.insertItem(video, v, 20 * kS, 10 * kS);  // a second video, [20, 28): the audio track is free then
  CHECK(d.detachAudio(&video, 1, &again) == a && again == 1 && video == 1);
  CHECK(d.detachAudio(&video, 1, &again) == 0 && video == 2);  // busy now: another new audio track
  int none = 0;
  d.insertItem(video, still(mf::ItemType::Image), 40 * kS);
  CHECK(d.detachAudio(&video, 2, &none) == -1);  // not a video
  checkValid(d);
}

static void load() {
  Document d;
  mf::Scene s;
  mf::SceneTrack t;
  t.id = "clip7";  // numbered like the editor's own ids
  mf::SceneItem a = still(mf::ItemType::Color), b = still(mf::ItemType::Color);
  a.id = "item41";
  b.startUs = 5 * kS;  // no id
  t.items = {a, b};
  mf::SceneTransition x;
  x.from = 0;
  x.kind = mf::SceneTransitionKind::Cut;
  t.transitions = {x};
  s.tracks = {t};
  d.load(s);
  CHECK(d.tracks() == 1 && d.track(0).id == "clip7" && d.item(0, 0).id == "item41");
  CHECK(d.item(0, 1).id == "item42" && d.track(0).transitions[0].id == "transition43");  // past 41
  int k = d.insertItem(0, still(mf::ItemType::Text), 20 * kS);
  CHECK(d.item(0, k).id == "item44");
  mf::SceneItem v;
  v.type = mf::ItemType::Video;
  v.durationUs = 10 * kS;
  k = d.insertItem(0, v, 30 * kS);  // its length unknown until set
  d.setLength(d.item(0, k).id, 6 * kS);
  d.setDuration(0, k, 10 * kS);
  CHECK(d.item(0, k).durationUs == 6 * kS);
  checkValid(d);
}

static void tracks() {
  Document d;
  int a = d.addTrack(false);
  CHECK(a == 0 && !d.track(0).video && d.track(1).video);  // audio below the video tracks
  int v = d.addTrack(true);
  CHECK(v == 2);
  CHECK(d.moveTrack(v, -1) == 1 && d.track(2).id != d.track(1).id);
  d.removeTrack(0);
  CHECK(d.tracks() == 2);
  while (d.addTrack(true) >= 0) {
  }
  CHECK(d.tracks() == 16);
}

int main() {
  insertAndRemove();
  moveAndTrim();
  mediaLength();
  trimStart();
  transitions();
  junctions();
  detach();
  load();
  tracks();
  std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
