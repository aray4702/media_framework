// Checks the camera recording's model: segments recorded and removed last first, the maximum
// length, effects taken at Record, the music's position, the microphone's default, what Done
// inserts into the project, and recovery from the journal.

#include <cstdio>
#include <cstdlib>

#include "capture_session.h"

using editor::CaptureSession;
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

static void checkValid(const mf::Scene& s) {
  std::string error;
  mf::Result r = mf::validateScene(s, &error);
  if (r != mf::Result::Ok) std::printf("invalid: %s\n", error.c_str());
  CHECK(r == mf::Result::Ok);
}

static mf::SceneOutput portrait() {
  mf::SceneOutput o;
  o.width = 1080;
  o.height = 1920;
  return o;
}

static bool record(CaptureSession& s, const std::string& file, int64_t us) { return s.startSegment() && s.stopSegment(file, us); }

static void segments() {
  CaptureSession s(portrait(), CaptureSession::kUnlimited, true);
  CHECK(s.doc.scene.output.width == 1080 && s.camera().fit == mf::Fit::Cover && s.mirror());
  CHECK(s.canRecord() && !s.recording() && s.totalUs() == 0);
  CHECK(record(s, "a.mp4", 2 * kS));
  CHECK(record(s, "b.mp4", 3 * kS));
  CHECK(!record(s, "c.mp4", CaptureSession::kMinSegmentUs - 1));  // too short: dropped
  CHECK(s.segments().size() == 2 && s.totalUs() == 5 * kS);

  CHECK(s.startSegment());
  CHECK(!s.startSegment() && !s.removeLast());  // one at a time; nothing removed while recording
  s.cancelSegment();
  CHECK(s.segments().size() == 2 && !s.recording());

  // Removed last first.
  auto last = s.removeLast();
  CHECK(last && last->file == "b.mp4");
  last = s.removeLast();
  CHECK(last && last->file == "a.mp4");
  CHECK(!s.removeLast() && s.totalUs() == 0);
}

static void maximum() {
  CaptureSession s(portrait(), 15 * kS, false);
  CHECK(s.remainingUs() == 15 * kS);
  CHECK(record(s, "a.mp4", 10 * kS));
  CHECK(record(s, "b.mp4", 8 * kS));  // cut to the room left
  CHECK(s.segments().back().durationUs == 5 * kS && s.totalUs() == 15 * kS);
  CHECK(s.full() && !s.canRecord() && !s.startSegment());
  s.removeLast();
  CHECK(s.canRecord() && s.remainingUs() == 5 * kS);
  s.setMaxDurationUs(CaptureSession::kUnlimited);
  CHECK(s.remainingUs() > 1000 * kS);
  int choices = 0;
  for (int64_t us : CaptureSession::kMaxChoicesUs) choices += us >= 0;
  CHECK(choices == 5);
}

static void effects() {
  CaptureSession s(portrait(), CaptureSession::kUnlimited, true);
  CHECK(record(s, "plain.mp4", kS));  // no effects
  s.effects().blur = true;
  s.effects().blurRadius = mf::Animatable(0.02);
  s.setMirror(false);
  CHECK(record(s, "blurred.mp4", kS));
  s.effects().blur = false;  // changed afterwards: the segments keep theirs
  CHECK(!s.segments()[0].effects.any() && s.segments()[0].mirror);
  CHECK(s.segments()[1].effects.blur && !s.segments()[1].mirror);
  s.removeLast();
  CHECK(!s.effects().blur && !s.mirror());  // removing a segment leaves the current effects
}

static void music() {
  CaptureSession s(portrait(), CaptureSession::kUnlimited, true);
  CHECK(s.recordMicrophone());
  s.setMusic("song.mp3", 20 * kS, 4 * kS);
  CHECK(!s.recordMicrophone() && s.musicPositionUs() == 4 * kS);  // music: the microphone goes off
  record(s, "a.mp4", 3 * kS);
  record(s, "b.mp4", 2 * kS);
  CHECK(s.musicPositionUs() == 9 * kS);
  s.removeLast();
  CHECK(s.musicPositionUs() == 7 * kS);  // back by the removed segment
  record(s, "c.mp4", 30 * kS);
  CHECK(s.musicPositionUs() == 20 * kS);  // no further than the file
  s.clearMusic();
  CHECK(s.recordMicrophone() && !s.hasMusic());
  s.setRecordMicrophone(true);
  s.setMusic("song.mp3", 20 * kS);
  CHECK(s.recordMicrophone());  // chosen: music no longer changes it
}

static mf::SceneItem sticker() {
  mf::SceneItem it;
  it.type = mf::ItemType::Image;
  it.src = "star.png";
  it.durationUs = 5 * kS;
  it.transform.x = mf::Animatable(0.3);
  return it;
}

static mf::SceneItem caption() {
  mf::SceneItem it;
  it.type = mf::ItemType::Text;
  it.text = "Hi";
  it.durationUs = 5 * kS;
  return it;
}

static void insert() {
  CaptureSession s(portrait(), CaptureSession::kUnlimited, true);
  Document project;
  std::string error;
  CHECK(s.insertInto(project, 0, {}, &error) == -1 && !error.empty());  // nothing recorded

  CHECK(s.addOverlay(sticker()) == 1 && s.addOverlay(caption()) == 2);
  CHECK(s.doc.item(1, 0).durationUs == CaptureSession::kPreviewUs);
  checkValid(s.doc.scene);  // the preview's scene
  s.setMusic("song.mp3", 20 * kS, 1 * kS);
  record(s, "a.mp4", 2 * kS);
  s.effects().blur = true;
  s.effects().blurRadius = mf::Animatable(0.01);
  s.setMirror(false);
  record(s, "b.mp4", 3 * kS);

  // The project has a clip already: the recording goes in at 4 s.
  mf::SceneItem clip;
  clip.type = mf::ItemType::Video;
  clip.src = "old.mp4";
  clip.durationUs = 10 * kS;
  project.insertItem(0, clip, 0, 10 * kS);
  int resolved = 0;
  int t = s.insertInto(project, 4 * kS, [&](const std::string&) { ++resolved, void(); return mf::MediaSource{}; }, &error);
  CHECK(t >= 0);
  if (t < 0) return;
  checkValid(project.scene);
  CHECK(resolved == 3);  // two segments and the music
  const mf::SceneTrack& segs = project.track(t);
  CHECK(segs.video && segs.items.size() == 2 && !segs.effects.any());
  CHECK(segs.items[0].src == "a.mp4" && segs.items[0].startUs == 4 * kS && segs.items[0].durationUs == 2 * kS);
  CHECK(segs.items[1].src == "b.mp4" && segs.items[1].startUs == 6 * kS && segs.items[1].durationUs == 3 * kS);
  CHECK(!segs.items[0].effects.any() && segs.items[0].transform.flipX);  // each with its own
  CHECK(segs.items[1].effects.blur && !segs.items[1].transform.flipX);
  CHECK(segs.items[0].fit == mf::Fit::Cover);
  // The overlays above, over the recording.
  CHECK(project.tracks() == t + 3);
  const mf::SceneItem& star = project.item(t + 1, 0);
  CHECK(star.src == "star.png" && star.startUs == 4 * kS && star.durationUs == 5 * kS && star.transform.x.value == 0.3);
  CHECK(project.item(t + 2, 0).text == "Hi" && project.item(t + 2, 0).durationUs == 5 * kS);
  // The music on an audio track, from where recording started it.
  CHECK(!project.track(0).video && project.track(0).items.size() == 1);
  const mf::SceneItem& song = project.item(0, 0);
  CHECK(song.src == "song.mp3" && song.inUs == 1 * kS && song.startUs == 4 * kS && song.durationUs == 5 * kS);
  CHECK(project.item(1, 0).src == "old.mp4");  // the clip, below

  // Too many tracks: the project stays as it was.
  Document full;
  while (full.addTrack(true) >= 0) {
  }
  int before = full.tracks();
  CHECK(s.insertInto(full, 0, {}, &error) == -1 && full.tracks() == before);
}

static void journal() {
  CaptureSession s(portrait(), 30 * kS, true);
  s.addOverlay(caption());
  s.setMusic("song.mp3", 20 * kS, 2 * kS);
  record(s, "a.mp4", 2 * kS);
  s.effects().colorAdjust = true;
  s.effects().saturation = mf::Animatable(1.5);
  record(s, "b.mp4", kS);
  s.setMirror(false);

  mf::Scene j = s.journal();
  checkValid(j);
  // Written and read back as a document.
  mf::Scene parsed;
  std::string error;
  mf::Result r = mf::parseScene(mf::serializeScene(j), {}, &parsed, &error);
  if (r != mf::Result::Ok) std::printf("journal: %s\n", error.c_str());
  CHECK(r == mf::Result::Ok);

  CaptureSession back(mf::SceneOutput{}, 30 * kS, true);
  CHECK(back.restore(parsed));
  CHECK(back.doc.scene.output.width == 1080);
  CHECK(back.segments().size() == 2 && back.totalUs() == 3 * kS);
  CHECK(back.segments()[0].file == "a.mp4" && back.segments()[0].mirror && !back.segments()[0].effects.any());
  CHECK(back.segments()[1].effects.colorAdjust && back.segments()[1].effects.saturation.value == 1.5);
  CHECK(!back.mirror() && back.effects().colorAdjust);  // the current ones
  CHECK(back.doc.tracks() == 2 && back.doc.item(1, 0).text == "Hi");
  CHECK(back.hasMusic() && back.musicPositionUs() == 5 * kS);
  checkValid(back.doc.scene);

  mf::Scene other;
  CHECK(!back.restore(other));  // not a session
}

int main() {
  segments();
  maximum();
  effects();
  music();
  insert();
  journal();
  std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
