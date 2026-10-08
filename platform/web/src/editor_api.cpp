// The web editor's model: the macOS editor's editor::Document (apps/macos-editor/document.h),
// unchanged, as a C API for the page (platform/web/editor). It runs on the page's thread. The page
// reads the scene as a document (ed_scene) to draw the timeline and the properties, and changes it
// through the Document's edits, which keep every track valid (scene_graph_spec.md §6). Items,
// tracks and the output are passed as JSON in the document's own format, parsed by the core.

#include <emscripten.h>

#include <optional>
#include <string>

#include "../../../apps/macos-editor/document.h"
#include "mf/scene.h"
#include "mp4_demuxer.h"

namespace {

std::string out;  // the last string returned; valid until the next call that returns one

const char* give(std::string s) {
  out = std::move(s);
  return out.c_str();
}

// Sources stay names: the page resolves them for the player (web_api.cpp).
mf::MediaSource noSource(const std::string&) { return {}; }

// A document around `tracksJson` (and `outputJson`), parsed by the core: what an item, a track or
// the output is, as the document format says, with its checks.
std::optional<mf::Scene> parseWrapped(const std::string& tracksJson, const std::string& outputJson, std::string* error) {
  std::string doc = "{\"version\": 1, \"output\": " + outputJson + ", \"tracks\": [" + tracksJson + "]}";
  mf::Scene s;
  if (mf::parseScene(doc, noSource, &s, error) != mf::Result::Ok) return std::nullopt;
  return s;
}

const char* kSmallOutput = "{\"width\": 16, \"height\": 16, \"fps\": 30}";

std::optional<mf::SceneItem> parseItem(const std::string& itemJson, bool videoTrack, std::string* error) {
  auto s = parseWrapped(std::string("{\"kind\": \"") + (videoTrack ? "video" : "audio") + "\", \"items\": [" + itemJson + "]}",
                        kSmallOutput, error);
  if (!s || s->tracks.empty() || s->tracks[0].items.empty()) return std::nullopt;
  return s->tracks[0].items[0];
}

std::string lastError;

}  // namespace

using editor::Document;

extern "C" {

EMSCRIPTEN_KEEPALIVE Document* ed_new() { return new Document; }
EMSCRIPTEN_KEEPALIVE void ed_delete(Document* d) { delete d; }

// Why the last call that can fail failed.
EMSCRIPTEN_KEEPALIVE const char* ed_error() { return lastError.c_str(); }

// A saved project. Its media lengths must be given again (ed_set_length). 0: invalid.
EMSCRIPTEN_KEEPALIVE int ed_load(Document* d, const char* json) {
  mf::Scene s;
  if (mf::parseScene(json, noSource, &s, &lastError) != mf::Result::Ok) return 0;
  d->load(std::move(s));
  return 1;
}

// The scene as a document (serializeScene): items keep their ids; fields at their defaults are left out.
EMSCRIPTEN_KEEPALIVE const char* ed_scene(Document* d) { return give(mf::serializeScene(d->scene)); }

EMSCRIPTEN_KEEPALIVE void ed_set_length(Document* d, const char* itemId, double lengthUs) { d->setLength(itemId, int64_t(lengthUs)); }

EMSCRIPTEN_KEEPALIVE int ed_add_track(Document* d, int video) { return d->addTrack(video); }
EMSCRIPTEN_KEEPALIVE void ed_remove_track(Document* d, int t) { d->removeTrack(t); }
EMSCRIPTEN_KEEPALIVE int ed_move_track(Document* d, int t, int delta) { return d->moveTrack(t, delta); }

// An item (JSON, as in a document) at atUs on track t, or right after the item playing there.
// Returns its index, or -1 (lastError says why).
EMSCRIPTEN_KEEPALIVE int ed_insert_item(Document* d, int t, const char* itemJson, double atUs, double lengthUs) {
  if (t < 0 || t >= d->tracks()) return -1;
  std::string asVisual;
  auto item = parseItem(itemJson, true, &asVisual);              // a visual item belongs on a video track,
  if (!item) item = parseItem(itemJson, false, &lastError);  // an audio item on an audio one
  if (!item) {
    if (std::string(itemJson).find("\"audio\"") == std::string::npos) lastError = asVisual;  // why it isn't a visual item
    return -1;
  }
  return d->insertItem(t, *item, int64_t(atUs), int64_t(lengthUs));
}

EMSCRIPTEN_KEEPALIVE void ed_remove_item(Document* d, int t, int k) { d->removeItem(t, k); }
EMSCRIPTEN_KEEPALIVE void ed_move_item(Document* d, int t, int k, double startUs) { d->moveItem(t, k, int64_t(startUs)); }

// Moves item k of track t onto track `to` at startUs. Returns (to << 16) | its new index, or -1.
EMSCRIPTEN_KEEPALIVE int ed_move_to_track(Document* d, int t, int k, int to, double startUs) {
  int track = t, index = d->moveToTrack(&track, k, to, int64_t(startUs));
  return index < 0 ? -1 : (track << 16) | index;
}

EMSCRIPTEN_KEEPALIVE void ed_set_duration(Document* d, int t, int k, double us) { d->setDuration(t, k, int64_t(us)); }
EMSCRIPTEN_KEEPALIVE void ed_trim_start(Document* d, int t, int k, double us) { d->trimStart(t, k, int64_t(us)); }
EMSCRIPTEN_KEEPALIVE void ed_set_in(Document* d, int t, int k, double us) { d->setIn(t, k, int64_t(us)); }
EMSCRIPTEN_KEEPALIVE void ed_set_speed(Document* d, int t, int k, double speed) { d->setSpeed(t, k, speed); }
EMSCRIPTEN_KEEPALIVE double ed_max_duration(Document* d, int t, int k) { return double(d->maxDurationUs(t, k)); }
EMSCRIPTEN_KEEPALIVE int ed_junction(Document* d, int t, int k) { return d->junction(t, k); }
EMSCRIPTEN_KEEPALIVE int ed_free_track(Document* d, int video, double startUs, double endUs) {
  return d->freeTrack(video, int64_t(startUs), int64_t(endUs));
}

// Detaches a video item's sound onto an audio track. Returns (track << 16) | the audio item, or -1.
// The video item's track may shift (a new audio track goes below the video ones): ed_scene shows it.
EMSCRIPTEN_KEEPALIVE int ed_detach_audio(Document* d, int t, int k) {
  int videoTrack = t, audioItem = -1;
  int audioTrack = d->detachAudio(&videoTrack, k, &audioItem);
  return audioTrack < 0 ? -1 : (audioTrack << 16) | audioItem;
}

// The transition into item k: kind 0 cut .. 4 wipe (SceneTransitionKind), -1 none (removes it).
EMSCRIPTEN_KEEPALIVE void ed_set_transition(Document* d, int t, int k, int kind, int direction, double durationUs) {
  if (kind < 0) return d->setTransition(t, k, nullptr);
  mf::SceneTransition x;
  if (const mf::SceneTransition* old = d->transitionInto(t, k)) x = *old;
  x.kind = mf::SceneTransitionKind(kind);
  x.direction = mf::Direction(direction);
  x.durationUs = kind == int(mf::SceneTransitionKind::Cut) ? 0 : int64_t(durationUs);
  d->setTransition(t, k, &x);
}

// An item's look and sound from JSON (as in a document): everything but its timing, file and id,
// which only the Document's edits change, so every track stays valid. 0: invalid (lastError).
EMSCRIPTEN_KEEPALIVE int ed_set_item(Document* d, int t, int k, const char* itemJson) {
  mf::SceneItem& cur = d->item(t, k);
  auto item = parseItem(itemJson, cur.type != mf::ItemType::Audio, &lastError);
  if (!item) return 0;
  item->id = cur.id;
  item->type = cur.type;
  item->startUs = cur.startUs;
  item->durationUs = cur.durationUs;
  item->src = cur.src;
  item->source = cur.source;
  item->inUs = cur.inUs;
  item->speed = cur.speed;
  cur = *item;
  return 1;
}

// A track's settings from JSON: enabled, opacity, gain, effects. Its items and transitions stay.
EMSCRIPTEN_KEEPALIVE int ed_set_track(Document* d, int t, const char* trackJson) {
  mf::SceneTrack& cur = d->track(t);
  std::string json = trackJson;
  auto s = parseWrapped(json, kSmallOutput, &lastError);
  if (!s || s->tracks.empty()) return 0;
  const mf::SceneTrack& in = s->tracks[0];
  cur.enabled = in.enabled;
  cur.opacity = in.opacity;
  cur.gain = in.gain;
  cur.effects = in.effects;
  return 1;
}

// The output (size, frame rate, background) from JSON.
EMSCRIPTEN_KEEPALIVE int ed_set_output(Document* d, const char* outputJson) {
  auto s = parseWrapped("", outputJson, &lastError);
  if (!s) return 0;
  d->scene.output = s->output;
  return 1;
}

// What a media file is, from its bytes (MP4 or M4A), as JSON: {"durationUs", "video": {"width",
// "height"} or null, "audio": true/false}, or {"error": ...}.
EMSCRIPTEN_KEEPALIVE const char* ed_probe(const uint8_t* bytes, int length) {
  auto demuxer = mf::web::createMp4Demuxer();
  mf::MediaInfo info;
  mf::Result r = demuxer->open(mf::web::mediaSource(mf::web::memorySource(std::vector<uint8_t>(bytes, bytes + length))), &info);
  if (r != mf::Result::Ok) return give(std::string("{\"error\": \"") + mf::toString(r) + "\"}");
  std::string video = info.video.supported ? "{\"width\": " + std::to_string(info.video.width) + ", \"height\": " +
                                                 std::to_string(info.video.height) + "}"
                                           : "null";
  bool audio = info.audio && info.audio->supported;
  return give("{\"durationUs\": " + std::to_string(info.durationUs) + ", \"video\": " + video + ", \"audio\": " + (audio ? "true" : "false") + "}");
}

}  // extern "C"
