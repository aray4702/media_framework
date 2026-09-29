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
  d.insertItem(0, still(mf::ItemType::Color), 0);   // [0, 5)
  d.insertItem(0, v, 6 * kS, 8 * kS);               // [6, 14), in 0
  d.insertItem(0, still(mf::ItemType::Color), 15 * kS);  // [15, 20)
  auto at = [&](int k) { return d.item(0, k).startUs; };

  d.trimStart(0, 1, 6 * kS);  // 2 s cut off its start: it stays, reads the file from 2 s; the next moves back 2 s
  CHECK(at(1) == 6 * kS && d.item(0, 1).durationUs == 6 * kS && d.item(0, 1).inUs == 2 * kS && at(2) == 13 * kS);
  d.trimStart(0, 1, 20 * kS);  // longer: at most back to the file's start
  CHECK(at(1) == 6 * kS && d.item(0, 1).durationUs == 8 * kS && d.item(0, 1).inUs == 0 && at(2) == 15 * kS);
  d.trimStart(0, 1, 0);  // shorter: at least the shortest item
  CHECK(d.item(0, 1).durationUs == Document::kMinDurationUs && at(2) == 7100000);
  d.trimStart(0, 1, 8 * kS);
  CHECK(at(2) == 15 * kS);
  checkValid(d);

  d.trimStart(0, 0, 3 * kS);  // a still: its length only, and the rest moves back
  CHECK(at(0) == 0 && d.item(0, 0).durationUs == 3 * kS && d.item(0, 0).inUs == 0 && at(1) == 4 * kS && at(2) == 13 * kS);
  checkValid(d);

  mf::SceneTransition x;
  x.kind = mf::SceneTransitionKind::Crossfade;
  x.durationUs = kS;
  d.setTransition(0, 1, &x);  // the video now starts at 2 s
  CHECK(at(1) == 2 * kS && at(2) == 13 * kS);
  d.trimStart(0, 1, 7 * kS);  // joined: it still keeps its place
  CHECK(at(1) == 2 * kS && d.item(0, 1).inUs == kS && at(2) == 12 * kS);
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
  tracks();
  std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
