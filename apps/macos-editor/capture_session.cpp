#include "capture_session.h"

#include <algorithm>
#include <limits>

namespace editor {
namespace {

// The journal's tracks, by id.
const char* const kCameraId = "capture-camera";
const char* const kSegmentsId = "capture-segments";
const char* const kMusicId = "capture-music";

mf::SceneItem cameraItem(bool mirror) {
  mf::SceneItem it;
  it.type = mf::ItemType::Video;
  it.id = "camera";
  it.src = "camera";  // stands for the live picture: never read
  it.durationUs = CaptureSession::kPreviewUs;
  it.fit = mf::Fit::Cover;  // fills the frame, like the recording
  it.transform.flipX = mirror;
  return it;
}

}  // namespace

CaptureSession::CaptureSession(const mf::SceneOutput& project, int64_t maxDurationUs, bool mirror) : maxUs_(maxDurationUs) {
  doc.scene.output = project;
  doc.track(kCameraTrack).items.push_back(cameraItem(mirror));
}

int64_t CaptureSession::totalUs() const {
  int64_t total = 0;
  for (const Segment& s : segments_) total += s.durationUs;
  return total;
}

int64_t CaptureSession::remainingUs() const {
  if (maxUs_ == kUnlimited) return std::numeric_limits<int64_t>::max();
  return std::max<int64_t>(0, maxUs_ - totalUs());
}

bool CaptureSession::startSegment() {
  if (!canRecord()) return false;
  recording_ = true;
  Segment s;
  s.effects = camera().effects;
  s.mirror = mirror();
  segments_.push_back(std::move(s));  // its file and length come when it stops
  return true;
}

bool CaptureSession::stopSegment(const std::string& file, int64_t durationUs) {
  if (!recording_) return false;
  recording_ = false;
  Segment s = std::move(segments_.back());
  segments_.pop_back();
  s.file = file;
  s.durationUs = std::min(durationUs, remainingUs());
  if (s.durationUs < kMinSegmentUs) return false;
  segments_.push_back(std::move(s));
  return true;
}

void CaptureSession::cancelSegment() {
  if (!recording_) return;
  recording_ = false;
  segments_.pop_back();
}

std::optional<CaptureSession::Segment> CaptureSession::removeLast() {
  if (recording_ || segments_.empty()) return std::nullopt;
  Segment s = std::move(segments_.back());
  segments_.pop_back();
  return s;
}

void CaptureSession::setMusic(const std::string& src, int64_t lengthUs, int64_t inUs) {
  musicSrc_ = src;
  musicLengthUs_ = lengthUs;
  musicInUs_ = std::clamp<int64_t>(inUs, 0, std::max<int64_t>(0, lengthUs));
  if (!microphoneChosen_) microphone_ = false;
}

void CaptureSession::clearMusic() {
  musicSrc_.clear();
  musicLengthUs_ = musicInUs_ = 0;
  if (!microphoneChosen_) microphone_ = true;
}

int64_t CaptureSession::musicPositionUs() const {
  int64_t pos = musicInUs_ + totalUs();
  return musicLengthUs_ > 0 ? std::min(pos, musicLengthUs_) : pos;
}

int CaptureSession::addOverlay(mf::SceneItem item) {
  int t = doc.addTrack(true);
  if (t < 0) return -1;
  item.durationUs = kPreviewUs;
  doc.insertItem(t, std::move(item), 0);
  return t;
}

int CaptureSession::insertInto(Document& project, int64_t atUs, const mf::SourceResolver& resolve, std::string* error) const {
  auto fail = [&](const char* message) {
    if (error) *error = message;
    return -1;
  };
  if (segments_.empty()) return fail("Nothing was recorded.");
  int64_t total = totalUs(), end = atUs + total;
  std::vector<const mf::SceneItem*> overlays;
  for (int t = kCameraTrack + 1; t < int(doc.scene.tracks.size()); ++t) {
    for (const mf::SceneItem& it : doc.scene.tracks[t].items) overlays.push_back(&it);
  }
  int64_t musicUs = hasMusic() ? std::min(total, musicLengthUs_ - musicInUs_) : 0;
  bool music = musicUs > 0;
  bool newAudioTrack = music && project.freeTrack(false, atUs, atUs + musicUs) < 0;
  if (project.tracks() + 1 + int(overlays.size()) + int(newAudioTrack) > 16) return fail("The project has too many tracks for the recording.");

  // The music first: a new audio track goes in below the video ones, moving their indices.
  if (music) {
    int t = newAudioTrack ? project.addTrack(false) : project.freeTrack(false, atUs, atUs + musicUs);
    mf::SceneItem it;
    it.type = mf::ItemType::Audio;
    it.src = musicSrc_;
    it.source = resolve ? resolve(musicSrc_) : mf::MediaSource{};
    it.inUs = musicInUs_;
    it.durationUs = musicUs;
    project.insertItem(t, std::move(it), atUs, musicLengthUs_);
  }
  int segmentsTrack = project.addTrack(true);
  int64_t at = atUs;
  for (const Segment& s : segments_) {
    mf::SceneItem it;
    it.type = mf::ItemType::Video;
    it.src = s.file;
    it.source = resolve ? resolve(s.file) : mf::MediaSource{};
    it.durationUs = s.durationUs;
    it.fit = mf::Fit::Cover;
    it.transform.flipX = s.mirror;
    it.effects = s.effects;
    project.insertItem(segmentsTrack, std::move(it), at, s.durationUs);
    at += s.durationUs;
  }
  for (const mf::SceneItem* o : overlays) {
    mf::SceneItem it = *o;
    it.durationUs = end - atUs;
    project.insertItem(project.addTrack(true), std::move(it), atUs);
  }
  return segmentsTrack;
}

mf::Scene CaptureSession::journal() const {
  mf::Scene s = doc.scene;
  s.tracks[kCameraTrack].id = kCameraId;
  s.tracks[kCameraTrack].enabled = false;  // the live picture: nothing to play
  mf::SceneTrack segments;
  segments.id = kSegmentsId;
  int64_t at = 0;
  for (const Segment& seg : segments_) {
    mf::SceneItem it;
    it.type = mf::ItemType::Video;
    it.src = seg.file;
    it.startUs = at;
    it.durationUs = seg.durationUs;
    it.fit = mf::Fit::Cover;
    it.transform.flipX = seg.mirror;
    it.effects = seg.effects;
    segments.items.push_back(std::move(it));
    at += seg.durationUs;
  }
  s.tracks.insert(s.tracks.begin() + kCameraTrack + 1, std::move(segments));
  if (hasMusic()) {
    mf::SceneTrack music;
    music.id = kMusicId;
    music.video = false;
    mf::SceneItem it;
    it.type = mf::ItemType::Audio;
    it.src = musicSrc_;
    it.inUs = musicInUs_;
    it.durationUs = std::max<int64_t>(1, musicLengthUs_ - musicInUs_);  // the file's length is in + duration
    music.items.push_back(std::move(it));
    s.tracks.insert(s.tracks.begin(), std::move(music));  // audio tracks go below the video ones
  }
  return s;
}

bool CaptureSession::restore(const mf::Scene& journal) {
  const mf::SceneTrack *camera = nullptr, *segments = nullptr, *music = nullptr;
  std::vector<const mf::SceneTrack*> overlays;
  for (const mf::SceneTrack& t : journal.tracks) {
    if (t.id == kCameraId) camera = &t;
    else if (t.id == kSegmentsId) segments = &t;
    else if (t.id == kMusicId) music = &t;
    else if (t.video) overlays.push_back(&t);
  }
  if (!camera || camera->items.size() != 1 || !segments) return false;

  Document fresh;
  fresh.scene.output = journal.output;
  fresh.track(kCameraTrack).items.push_back(cameraItem(camera->items[0].transform.flipX));
  fresh.item(kCameraTrack, 0).effects = camera->items[0].effects;
  doc = std::move(fresh);
  for (const mf::SceneTrack* t : overlays) {
    for (const mf::SceneItem& it : t->items) addOverlay(it);
  }
  segments_.clear();
  for (const mf::SceneItem& it : segments->items) segments_.push_back({it.src, it.durationUs, it.effects, it.transform.flipX});
  recording_ = false;
  bool chosen = microphoneChosen_;
  microphoneChosen_ = true;  // restoring the music leaves the microphone as it is
  if (music && !music->items.empty()) {
    const mf::SceneItem& m = music->items[0];
    setMusic(m.src, m.inUs + m.durationUs, m.inUs);
  } else {
    clearMusic();
  }
  microphoneChosen_ = chosen;
  return true;
}

}  // namespace editor
