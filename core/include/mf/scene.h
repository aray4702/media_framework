#pragma once

// The scene graph (scene_graph_spec.md): tracks of video, image, text, color and audio items,
// transitions between items, per-item effects, and keyframe animation of any number.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mf/types.h"

namespace mf {

// Maps progress u (0 to 1) to eased progress (§4.3).
struct Easing {
  enum class Kind { Linear, Hold, Bezier };
  Kind kind = Kind::Linear;
  float x1 = 0, y1 = 0, x2 = 1, y2 = 1;  // Bezier control points (CSS cubic-bezier)
  static Easing bezier(float x1, float y1, float x2, float y2) { return {Kind::Bezier, x1, y1, x2, y2}; }
  double apply(double u) const;
};

struct Keyframe {
  int64_t timeUs = 0;  // relative to the item's start
  double value = 0;
  std::optional<Easing> easing;  // from this key to the next; else the property's
};

// A constant, or keyframes evaluated at item-local time.
struct Animatable {
  Animatable(double v = 0) : value(v) {}
  double value;
  std::vector<Keyframe> keys;
  Easing easing;
  bool animated() const { return !keys.empty(); }
  double at(int64_t localUs) const;
};

struct SceneTransform {
  Animatable x{0.5}, y{0.5}, scale{1}, rotation{0};
  float anchorX = 0.5f, anchorY = 0.5f;
  bool flipX = false;  // the image mirrored left to right within its box (e.g. a front camera)
};

// An effect from a plugin (mf/effects.h): its type, and a value for every parameter of the
// type's EffectInfo, in that order.
struct ScenePluginEffect {
  std::string type;
  std::vector<Animatable> params;
};

// Each effect at most once per item (or track), applied crop → chromaKey → colorAdjust →
// plugin effects (in document order) → blur (§4.4).
struct SceneEffects {
  bool colorAdjust = false;
  Animatable brightness{0}, contrast{1}, saturation{1};
  bool blur = false;
  Animatable blurRadius{0};
  bool crop = false;
  Animatable cropLeft{0}, cropTop{0}, cropRight{0}, cropBottom{0};
  bool chromaKey = false;
  Color keyColor;
  float keyTolerance = 0.15f, keySoftness = 0.1f;
  std::vector<ScenePluginEffect> plugins;
  bool any() const { return colorAdjust || blur || crop || chromaKey || !plugins.empty(); }
  int count() const { return int(colorAdjust) + int(blur) + int(crop) + int(chromaKey) + int(plugins.size()); }
};

enum class ItemType { Video, Image, Text, Color, Audio };

struct SceneItem {
  std::string id;
  ItemType type = ItemType::Video;
  // durationUs 0 (video and audio items): to the end of the file, from `in` at `speed`. The
  // player fills it in when it probes the file, then checks the rules that need it.
  int64_t startUs = 0, durationUs = 0;
  int64_t endUs() const { return startUs + durationUs; }
  bool toEnd() const { return durationUs == 0 && (type == ItemType::Video || type == ItemType::Audio); }

  // Video, audio, image: the file, and (video, audio) where it's read from.
  std::string src;
  MediaSource source;  // resolved from src
  int64_t inUs = 0;
  double speed = 1;

  // Visual items.
  SceneTransform transform;
  Animatable opacity{1};
  Blend blend = Blend::Normal;
  Fit fit = Fit::Contain;
  SceneEffects effects;
  std::string text;  // Text
  TextStyle style;   // Text
  Color color;       // Color

  // Audio items, and the audio of video items.
  bool mute = false;
  Animatable gain{1}, pan{0};
};

enum class SceneTransitionKind { Cut, Crossfade, Push, Slide, Wipe };
enum class Direction { Left, Right, Up, Down };
enum class AudioFade { EqualGain, EqualPower, Cut };

// Joins items[from] and items[from + 1] of its track, which overlap by durationUs.
struct SceneTransition {
  std::string id;
  int from = 0;
  SceneTransitionKind kind = SceneTransitionKind::Crossfade;
  Direction direction = Direction::Left;
  int64_t durationUs = 0;
  Easing easing;
  AudioFade audio = AudioFade::EqualGain;
};

struct SceneTrack {
  std::string id;
  bool video = true;
  bool enabled = true;
  float opacity = 1;  // video
  float gain = 1;     // audio
  // Video: applied to the track's combined image (§5.1), in output coordinates. Keyframe
  // times are scene time.
  SceneEffects effects;
  std::vector<SceneItem> items;  // in start order
  std::vector<SceneTransition> transitions;
};

// 0 (scenes built in code, for playback): taken from the media when probed.
struct SceneOutput {
  int width = 1920, height = 1080;  // 0: the first video item's size
  int fpsNum = 30, fpsDen = 1;       // 0: the first video item's rate
  int sampleRate = 48000, channels = 2;  // 0: the first audio's format
  Color background{0, 0, 0, 1};
};

struct Scene {
  SceneOutput output;
  std::vector<SceneTrack> tracks;  // video tracks composite bottom (first) to top
  std::string metadata;            // the document's free-form "metadata", as JSON text ("": none)
  int64_t durationUs() const;      // the latest end of an item on an enabled track
};

// Turns a document's `src` into a platform MediaSource (Mac OS: mf::macos::sourceFromPath).
using SourceResolver = std::function<MediaSource(const std::string& src)>;

// Parses a document and checks the schema and rules R1–R8 (§6). On failure returns
// InvalidArgument, and `error` says where and why (e.g. "tracks[0].items[2]: ...").
Result parseScene(const std::string& json, const SourceResolver&, Scene* out, std::string* error);

// Checks the rules (R2–R8) on a scene built in code. open() and Exporter::start() call it.
// Rules that need the length of an item with duration 0 are checked once it is probed.
Result validateScene(const Scene&, std::string* error);

// The scene as a document (the inverse of parseScene): fields at their defaults are left out,
// times are seconds to the microsecond, other numbers keep 6 significant digits. `srcFor` maps
// each item's src to what the document says (e.g. a path relative to the document's folder);
// none keeps it as it is. Of the metadata, only the document's (Scene::metadata) is kept.
std::string serializeScene(const Scene&, const std::function<std::string(const std::string& src)>& srcFor = {});

}  // namespace mf
