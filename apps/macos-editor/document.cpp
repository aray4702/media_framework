#include "document.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace editor {

Document::Document() {
  scene.output.width = 1920;
  scene.output.height = 1080;
  scene.output.fpsNum = 30;
  addTrack(true);
}

bool Document::empty() const {
  for (const mf::SceneTrack& t : scene.tracks) {
    if (!t.items.empty()) return false;
  }
  return true;
}

int Document::addTrack(bool video) {
  if (tracks() >= 16) return -1;
  mf::SceneTrack track;
  track.video = video;
  track.id = newId("track");
  if (video) {
    scene.tracks.push_back(std::move(track));
    return tracks() - 1;
  }
  scene.tracks.insert(scene.tracks.begin(), std::move(track));
  return 0;
}

void Document::removeTrack(int t) { scene.tracks.erase(scene.tracks.begin() + t); }

int Document::moveTrack(int t, int delta) {
  int to = std::clamp(t + delta, 0, tracks() - 1);
  std::swap(scene.tracks[t], scene.tracks[to]);
  return to;
}

int Document::insertItem(int t, mf::SceneItem it, int64_t atUs, int64_t lengthUs) {
  std::vector<mf::SceneItem>& items = track(t).items;
  it.id = newId("item");
  if (lengthUs > 0) lengthUs_[it.id] = lengthUs;
  it.startUs = std::max<int64_t>(0, atUs);
  int k = 0;
  while (k < int(items.size()) && items[k].startUs <= it.startUs) ++k;
  if (k > 0) it.startUs = std::max(it.startUs, items[k - 1].endUs());  // after the item playing there
  items.insert(items.begin() + k, std::move(it));

  // A transition between the items on either side no longer joins neighbors.
  std::vector<mf::SceneTransition>& xs = track(t).transitions;
  xs.erase(std::remove_if(xs.begin(), xs.end(), [&](const mf::SceneTransition& x) { return x.from == k - 1; }), xs.end());
  for (mf::SceneTransition& x : xs) {
    if (x.from >= k) ++x.from;
  }
  normalize(t);
  return k;
}

void Document::removeItem(int t, int k) {
  lengthUs_.erase(item(t, k).id);
  std::vector<mf::SceneItem>& items = track(t).items;
  items.erase(items.begin() + k);
  std::vector<mf::SceneTransition>& xs = track(t).transitions;
  xs.erase(std::remove_if(xs.begin(), xs.end(), [&](const mf::SceneTransition& x) { return x.from == k - 1 || x.from == k; }),
           xs.end());
  for (mf::SceneTransition& x : xs) {
    if (x.from > k) --x.from;
  }
  normalize(t);
}

void Document::moveItem(int t, int k, int64_t startUs) {
  if (transitionInto(t, k)) return;
  std::vector<mf::SceneItem>& items = track(t).items;
  int64_t lo = k > 0 ? items[k - 1].endUs() : 0;
  int64_t hi = std::numeric_limits<int64_t>::max();
  if (k + 1 < int(items.size()) && !transitionInto(t, k + 1)) hi = items[k + 1].startUs - items[k].durationUs;
  items[k].startUs = std::max(lo, std::min(hi, startUs));
  normalize(t);
}

void Document::setDuration(int t, int k, int64_t durationUs) {
  std::vector<mf::SceneItem>& items = track(t).items;
  int64_t hi = maxDurationUs(t, k);
  if (k + 1 < int(items.size()) && !transitionInto(t, k + 1)) hi = std::min(hi, items[k + 1].startUs - items[k].startUs);
  items[k].durationUs = std::max(kMinDurationUs, std::min(hi, durationUs));
  normalize(t);
}

void Document::trimStart(int t, int k, int64_t durationUs) {
  mf::SceneItem& it = item(t, k);
  bool media = it.type == mf::ItemType::Video || it.type == mf::ItemType::Audio;
  int64_t hi = media ? it.durationUs + int64_t(std::llround(double(it.inUs) / it.speed))  // back to the file's start
                     : maxDurationUs(t, k);
  int64_t delta = it.durationUs - std::clamp(durationUs, kMinDurationUs, std::max(kMinDurationUs, hi));  // > 0: cut off
  it.durationUs -= delta;
  if (media) it.inUs = std::max<int64_t>(0, it.inUs + std::llround(double(delta) * it.speed));
  std::vector<mf::SceneItem>& items = track(t).items;
  if (delta < 0) {  // longer: into the free space before it first
    int64_t space = transitionInto(t, k) ? 0 : it.startUs - (k > 0 ? items[k - 1].endUs() : 0);
    int64_t left = std::min(-delta, std::max<int64_t>(0, space));
    it.startUs -= left;
    delta += left;
  }
  for (size_t j = k + 1; j < items.size(); ++j) items[j].startUs -= delta;
  normalize(t);
}

void Document::setIn(int t, int k, int64_t inUs) {
  auto len = lengthUs_.find(item(t, k).id);
  int64_t hi = len != lengthUs_.end() ? len->second - kMinDurationUs : 0;
  item(t, k).inUs = std::clamp<int64_t>(inUs, 0, std::max<int64_t>(0, hi));
  setDuration(t, k, item(t, k).durationUs);  // what's left of the file may be shorter
}

void Document::setSpeed(int t, int k, double speed) {
  item(t, k).speed = std::clamp(speed, 0.25, 4.0);
  setDuration(t, k, item(t, k).durationUs);
}

int64_t Document::maxDurationUs(int t, int k) const {
  const mf::SceneItem& it = scene.tracks[t].items[k];
  auto len = lengthUs_.find(it.id);
  if (len == lengthUs_.end()) return int64_t(3600) * 1000000;  // stills: an hour
  return std::max(kMinDurationUs, int64_t(std::llround(double(len->second - it.inUs) / it.speed)));
}

int Document::detachAudio(int* videoTrack, int k, int* audioItem) {
  mf::SceneItem sound = item(*videoTrack, k);  // a copy: adding a track moves the tracks
  if (sound.type != mf::ItemType::Video) return -1;
  auto len = lengthUs_.find(sound.id);
  int64_t length = len != lengthUs_.end() ? len->second : 0;
  int at = -1;
  for (int t = 0; t < tracks() && at < 0; ++t) {
    if (track(t).video) continue;
    bool busy = false;
    for (const mf::SceneItem& it : track(t).items) busy |= it.startUs < sound.endUs() && it.endUs() > sound.startUs;
    if (!busy) at = t;
  }
  if (at < 0) {
    at = addTrack(false);
    if (at < 0) return -1;
    ++*videoTrack;  // audio tracks go in below the video ones
  }
  sound.type = mf::ItemType::Audio;
  sound.mute = false;
  *audioItem = insertItem(at, sound, sound.startUs, length);
  item(*videoTrack, k).mute = true;
  return at;
}

bool Document::junction(int t, int k) {
  if (k < 1 || k >= int(track(t).items.size())) return false;
  if (transitionInto(t, k)) return true;
  auto picture = [](const mf::SceneItem& it) { return it.type == mf::ItemType::Video || it.type == mf::ItemType::Image; };
  const mf::SceneItem &a = item(t, k - 1), &b = item(t, k);
  return picture(a) && picture(b) && b.startUs == a.endUs();
}

mf::SceneTransition* Document::transitionInto(int t, int k) {
  for (mf::SceneTransition& x : track(t).transitions) {
    if (x.from == k - 1) return &x;
  }
  return nullptr;
}

void Document::setTransition(int t, int k, const mf::SceneTransition* x) {
  std::vector<mf::SceneTransition>& xs = track(t).transitions;
  if (mf::SceneTransition* old = transitionInto(t, k)) xs.erase(xs.begin() + (old - xs.data()));
  if (x && k >= 1) {
    mf::SceneTransition copy = *x;
    copy.from = k - 1;
    if (copy.id.empty()) copy.id = newId("transition");
    xs.push_back(copy);
    std::sort(xs.begin(), xs.end(), [](const mf::SceneTransition& a, const mf::SceneTransition& b) { return a.from < b.from; });
  }
  normalize(t);
}

void Document::normalize(int t) {
  std::vector<mf::SceneItem>& items = track(t).items;
  for (int k = 1; k < int(items.size()); ++k) {
    mf::SceneItem &a = items[k - 1], &b = items[k];
    if (mf::SceneTransition* x = transitionInto(t, k)) {
      int64_t half = std::min(a.durationUs, b.durationUs) / 2;
      x->durationUs = x->kind == mf::SceneTransitionKind::Cut ? 0 : std::clamp<int64_t>(x->durationUs, 0, half);
      b.startUs = a.endUs() - x->durationUs;
    } else {
      b.startUs = std::max(b.startUs, a.endUs());
    }
  }
}

std::string Document::newId(const char* prefix) { return prefix + std::to_string(nextId_++); }

}  // namespace editor
