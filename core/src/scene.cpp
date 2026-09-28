#include "mf/scene.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "json.h"

namespace mf {

// --- Easing and keyframes (§4.3) ---------------------------------------------------------------

double Easing::apply(double u) const {
  u = std::clamp(u, 0.0, 1.0);
  if (kind == Kind::Linear) return u;
  if (kind == Kind::Hold) return u < 1 ? 0 : 1;
  // CSS cubic-bezier: solve x(s) = u for s by bisection (x is monotonic), return y(s).
  auto bez = [](double a, double b, double s) { return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s; };
  double lo = 0, hi = 1, s = u;
  for (int i = 0; i < 40; ++i) {
    s = (lo + hi) / 2;
    if (bez(x1, x2, s) < u) lo = s;
    else hi = s;
  }
  return bez(y1, y2, s);
}

double Animatable::at(int64_t localUs) const {
  if (keys.empty()) return value;
  if (localUs <= keys.front().timeUs) return keys.front().value;
  if (localUs >= keys.back().timeUs) return keys.back().value;
  auto next = std::upper_bound(keys.begin(), keys.end(), localUs, [](int64_t t, const Keyframe& k) { return t < k.timeUs; });
  const Keyframe& b = *next;
  const Keyframe& a = *(next - 1);
  double u = double(localUs - a.timeUs) / double(b.timeUs - a.timeUs);
  return a.value + (b.value - a.value) * a.easing.value_or(easing).apply(u);
}

int64_t Scene::durationUs() const {
  int64_t end = 0;
  for (const SceneTrack& track : tracks) {
    if (!track.enabled) continue;
    for (const SceneItem& item : track.items) end = std::max(end, item.endUs());
  }
  return end;
}

namespace {

using json::Value;

// --- Parsing: the schema -----------------------------------------------------------------------

class Reader {
 public:
  explicit Reader(const SourceResolver& resolve) : resolve_(resolve) {}

  bool fail(const std::string& path, const std::string& message) {
    if (error_.empty()) error_ = path + ": " + message;
    return false;
  }
  const std::string& error() const { return error_; }

  bool object(const Value& v, const std::string& path, std::initializer_list<const char*> allowed) {
    if (!v.isObject()) return fail(path, "must be an object");
    for (const auto& [key, value] : v.object) {
      bool known = false;
      for (const char* a : allowed) known |= key == a;
      if (!known) return fail(path, "unknown field '" + key + "'");
    }
    return true;
  }

  bool number(const Value* v, const std::string& path, double* out, double lo, double hi) {
    if (!v) return true;  // absent: keep the default
    if (!v->isNumber()) return fail(path, "must be a number");
    if (v->number < lo || v->number > hi) return fail(path, "must be between " + fmt(lo) + " and " + fmt(hi));
    *out = v->number;
    return true;
  }
  bool numberF(const Value* v, const std::string& path, float* out, double lo, double hi) {
    double d = *out;
    if (!number(v, path, &d, lo, hi)) return false;
    *out = float(d);
    return true;
  }
  bool integer(const Value* v, const std::string& path, int* out, int lo, int hi) {
    double d = *out;
    if (!number(v, path, &d, lo, hi)) return false;
    if (d != std::floor(d)) return fail(path, "must be a whole number");
    *out = int(d);
    return true;
  }
  bool boolean(const Value* v, const std::string& path, bool* out) {
    if (!v) return true;
    if (!v->isBool()) return fail(path, "must be true or false");
    *out = v->boolean;
    return true;
  }
  bool string(const Value* v, const std::string& path, std::string* out, size_t maxLength = 4096) {
    if (!v) return true;
    if (!v->isString() || v->string.empty()) return fail(path, "must be a non-empty string");
    if (v->string.size() > maxLength) return fail(path, "is longer than " + std::to_string(maxLength) + " bytes");
    *out = v->string;
    return true;
  }
  template <typename E>
  bool choice(const Value* v, const std::string& path, E* out, std::initializer_list<std::pair<const char*, E>> options) {
    if (!v) return true;
    std::string names;
    for (const auto& [name, value] : options) {
      if (v->isString() && v->string == name) {
        *out = value;
        return true;
      }
      names += names.empty() ? name : std::string(", ") + name;
    }
    return fail(path, "must be one of " + names);
  }

  bool id(const Value* v, const std::string& path, std::string* out) {
    if (!v) return true;
    if (!v->isString() || v->string.empty() || v->string.size() > 64) return fail(path, "must be 1 to 64 characters");
    for (char c : v->string) {
      if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') return fail(path, "may use only letters, digits, _ . -");
    }
    if (!ids_.insert(v->string).second) return fail(path, "id '" + v->string + "' is used twice (R1)");
    *out = v->string;
    return true;
  }

  // Seconds as a number, or an exact rational "num/den" (§2).
  bool time(const Value* v, const std::string& path, int64_t* out, bool positive) {
    if (!v) return true;
    long double seconds;
    if (v->isNumber()) {
      seconds = v->number;
    } else if (v->isString()) {
      size_t slash = v->string.find('/');
      char* end = nullptr;
      long long num = slash == std::string::npos ? -1 : std::strtoll(v->string.c_str(), &end, 10);
      long long den = num < 0 || end != v->string.c_str() + slash ? 0 : std::strtoll(v->string.c_str() + slash + 1, &end, 10);
      if (den <= 0 || *end != 0) return fail(path, "must be seconds or \"num/den\"");
      seconds = (long double)num / (long double)den;
    } else {
      return fail(path, "must be seconds or \"num/den\"");
    }
    if (seconds < 0 || (positive && seconds <= 0) || seconds > 86400) {
      return fail(path, positive ? "must be greater than 0 (and at most a day)" : "must be 0 or more (and at most a day)");
    }
    *out = std::llround(seconds * 1000000.0L);
    return true;
  }

  bool rate(const Value* v, const std::string& path, int* num, int* den) {
    if (!v) return true;
    if (v->isNumber()) {
      if (v->number <= 0 || v->number > 240) return fail(path, "must be above 0 and at most 240");
      if (v->number == std::floor(v->number)) {
        *num = int(v->number);
        *den = 1;
      } else {
        *num = int(std::llround(v->number * 1000));
        *den = 1000;
      }
      return true;
    }
    long long n = 0, d = 0;
    if (v->isString() && std::sscanf(v->string.c_str(), "%lld/%lld", &n, &d) == 2 && n > 0 && d > 0 && n <= 240 * d &&
        n < 1000000 && d < 1000000) {
      *num = int(n);
      *den = int(d);
      return true;
    }
    return fail(path, "must be a frame rate: a number or \"num/den\"");
  }

  bool color(const Value* v, const std::string& path, Color* out) {
    if (!v) return true;
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    const std::string& s = v->isString() ? v->string : std::string();
    if (s.size() != 7 && s.size() != 9) return fail(path, "must be #RRGGBB or #RRGGBBAA");
    float c[4] = {0, 0, 0, 1};
    for (size_t k = 1, n = 0; k < s.size(); k += 2, ++n) {
      int hi = hex(s[k]), lo = hex(s[k + 1]);
      if (s[0] != '#' || hi < 0 || lo < 0) return fail(path, "must be #RRGGBB or #RRGGBBAA");
      c[n] = float(hi * 16 + lo) / 255.0f;
    }
    *out = {c[0], c[1], c[2], c[3]};
    return true;
  }

  bool easing(const Value* v, const std::string& path, Easing* out) {
    if (!v) return true;
    if (v->isString()) {
      return choice(v, path, out,
                    {{"linear", Easing{}}, {"hold", Easing{Easing::Kind::Hold}}, {"easeIn", Easing::bezier(0.42f, 0, 1, 1)},
                     {"easeOut", Easing::bezier(0, 0, 0.58f, 1)}, {"easeInOut", Easing::bezier(0.42f, 0, 0.58f, 1)}});
    }
    if (!object(*v, path, {"cubicBezier"})) return false;
    const Value* b = v->find("cubicBezier");
    if (!b || !b->isArray() || b->array.size() != 4) return fail(path + ".cubicBezier", "must be [x1, y1, x2, y2]");
    float p[4];
    for (int k = 0; k < 4; ++k) {
      double lo = k % 2 == 0 ? 0 : -10, hi = k % 2 == 0 ? 1 : 10;
      if (!numberF(&b->array[k], path + ".cubicBezier[" + std::to_string(k) + "]", &p[k], lo, hi)) return false;
    }
    *out = Easing::bezier(p[0], p[1], p[2], p[3]);
    return true;
  }

  bool animatable(const Value* v, const std::string& path, Animatable* out) {
    if (!v) return true;
    if (v->isNumber()) {
      *out = Animatable(v->number);
      return true;
    }
    if (!object(*v, path, {"keys", "easing"})) return false;
    Animatable a;
    if (!easing(v->find("easing"), path + ".easing", &a.easing)) return false;
    const Value* keys = v->find("keys");
    if (!keys || !keys->isArray() || keys->array.empty() || keys->array.size() > 1024) {
      return fail(path + ".keys", "must be 1 to 1024 keys");
    }
    for (size_t k = 0; k < keys->array.size(); ++k) {
      const Value& key = keys->array[k];
      std::string kp = path + ".keys[" + std::to_string(k) + "]";
      if (!key.isArray() || key.array.size() < 2 || key.array.size() > 3) return fail(kp, "must be [time, value] or [time, value, easing]");
      Keyframe f;
      if (!time(&key.array[0], kp + "[0]", &f.timeUs, false)) return false;
      if (!key.array[1].isNumber()) return fail(kp + "[1]", "must be a number");
      f.value = key.array[1].number;
      if (key.array.size() == 3) {
        Easing e;
        if (!easing(&key.array[2], kp + "[2]", &e)) return false;
        f.easing = e;
      }
      a.keys.push_back(f);
    }
    *out = std::move(a);
    return true;
  }

  bool transform(const Value* v, const std::string& path, SceneTransform* out) {
    if (!v) return true;
    if (!object(*v, path, {"x", "y", "anchor", "scale", "rotation"})) return false;
    if (!animatable(v->find("x"), path + ".x", &out->x) || !animatable(v->find("y"), path + ".y", &out->y) ||
        !animatable(v->find("scale"), path + ".scale", &out->scale) ||
        !animatable(v->find("rotation"), path + ".rotation", &out->rotation)) {
      return false;
    }
    if (const Value* a = v->find("anchor")) {
      if (!a->isArray() || a->array.size() != 2) return fail(path + ".anchor", "must be [x, y]");
      if (!numberF(&a->array[0], path + ".anchor[0]", &out->anchorX, -10, 10) ||
          !numberF(&a->array[1], path + ".anchor[1]", &out->anchorY, -10, 10)) {
        return false;
      }
    }
    return true;
  }

  bool effects(const Value* v, const std::string& path, SceneEffects* out) {
    if (!v) return true;
    if (!v->isArray() || v->array.size() > 4) return fail(path, "must be a list of at most 4 effects");
    for (size_t k = 0; k < v->array.size(); ++k) {
      const Value& e = v->array[k];
      std::string ep = path + "[" + std::to_string(k) + "]";
      const Value* type = e.isObject() ? e.find("type") : nullptr;
      if (!type || !type->isString()) return fail(ep, "needs a type");
      const std::string& t = type->string;
      bool* seen = t == "colorAdjust" ? &out->colorAdjust : t == "blur" ? &out->blur : t == "crop" ? &out->crop : t == "chromaKey" ? &out->chromaKey : nullptr;
      if (!seen) return fail(ep + ".type", "must be one of colorAdjust, blur, crop, chromaKey");
      if (*seen) return fail(ep, "an item has at most one " + t + " effect");
      *seen = true;
      if (t == "colorAdjust") {
        if (!object(e, ep, {"type", "brightness", "contrast", "saturation"}) ||
            !animatable(e.find("brightness"), ep + ".brightness", &out->brightness) ||
            !animatable(e.find("contrast"), ep + ".contrast", &out->contrast) ||
            !animatable(e.find("saturation"), ep + ".saturation", &out->saturation)) {
          return false;
        }
      } else if (t == "blur") {
        if (!object(e, ep, {"type", "radius"})) return false;
        if (!e.find("radius")) return fail(ep, "needs radius");
        if (!animatable(e.find("radius"), ep + ".radius", &out->blurRadius)) return false;
      } else if (t == "crop") {
        if (!object(e, ep, {"type", "left", "top", "right", "bottom"}) ||
            !animatable(e.find("left"), ep + ".left", &out->cropLeft) || !animatable(e.find("top"), ep + ".top", &out->cropTop) ||
            !animatable(e.find("right"), ep + ".right", &out->cropRight) ||
            !animatable(e.find("bottom"), ep + ".bottom", &out->cropBottom)) {
          return false;
        }
      } else {
        if (!object(e, ep, {"type", "color", "tolerance", "softness"})) return false;
        if (!e.find("color")) return fail(ep, "needs color");
        if (!color(e.find("color"), ep + ".color", &out->keyColor) ||
            !numberF(e.find("tolerance"), ep + ".tolerance", &out->keyTolerance, 0, 1) ||
            !numberF(e.find("softness"), ep + ".softness", &out->keySoftness, 0, 1)) {
          return false;
        }
      }
    }
    return true;
  }

  bool style(const Value* v, const std::string& path, TextStyle* out) {
    if (!v) return true;
    if (!object(*v, path, {"font", "size", "color", "align", "box", "maxWidth"})) return false;
    if (v->find("box")) out->hasBox = true;
    return string(v->find("font"), path + ".font", &out->font, 256) && numberF(v->find("size"), path + ".size", &out->size, 0.001, 1) &&
           color(v->find("color"), path + ".color", &out->color) &&
           choice(v->find("align"), path + ".align", &out->align,
                  {{"left", TextAlign::Left}, {"center", TextAlign::Center}, {"right", TextAlign::Right}}) &&
           color(v->find("box"), path + ".box", &out->box) && numberF(v->find("maxWidth"), path + ".maxWidth", &out->maxWidth, 0.01, 1);
  }

  bool item(const Value& v, const std::string& path, bool videoTrack, SceneItem* out) {
    const Value* type = v.isObject() ? v.find("type") : nullptr;
    if (!type || !type->isString()) return fail(path, "needs a type");
    const std::string& t = type->string;
    bool ok;
    if (videoTrack && t == "video") {
      out->type = ItemType::Video;
      ok = object(v, path, {"type", "id", "start", "duration", "metadata", "src", "in", "speed", "transform", "opacity", "blend", "fit", "effects", "audio"});
    } else if (videoTrack && t == "image") {
      out->type = ItemType::Image;
      ok = object(v, path, {"type", "id", "start", "duration", "metadata", "src", "transform", "opacity", "blend", "fit", "effects"});
    } else if (videoTrack && t == "text") {
      out->type = ItemType::Text;
      ok = object(v, path, {"type", "id", "start", "duration", "metadata", "text", "style", "transform", "opacity", "blend", "fit", "effects"});
    } else if (videoTrack && t == "color") {
      out->type = ItemType::Color;
      ok = object(v, path, {"type", "id", "start", "duration", "metadata", "color", "transform", "opacity", "blend", "fit", "effects"});
    } else if (!videoTrack && t == "audio") {
      out->type = ItemType::Audio;
      ok = object(v, path, {"type", "id", "start", "duration", "metadata", "src", "in", "speed", "gain", "pan"});
    } else {
      return fail(path + ".type", videoTrack ? "must be one of video, image, text, color, transition" : "must be audio or transition");
    }
    if (!ok) return false;
    if (!v.find("start") || !v.find("duration")) return fail(path, "needs start and duration");
    if (!id(v.find("id"), path + ".id", &out->id) || !time(v.find("start"), path + ".start", &out->startUs, false) ||
        !time(v.find("duration"), path + ".duration", &out->durationUs, true)) {
      return false;
    }
    if (t == "video" || t == "audio" || t == "image") {
      if (!v.find("src")) return fail(path, "needs src");
      if (!string(v.find("src"), path + ".src", &out->src)) return false;
      out->source = resolve_ ? resolve_(out->src) : MediaSource{};
    }
    if (t == "video" || t == "audio") {
      if (!time(v.find("in"), path + ".in", &out->inUs, false) || !number(v.find("speed"), path + ".speed", &out->speed, 0.01, 16)) return false;
    }
    if (videoTrack) {
      if (!transform(v.find("transform"), path + ".transform", &out->transform) ||
          !animatable(v.find("opacity"), path + ".opacity", &out->opacity) ||
          !choice(v.find("blend"), path + ".blend", &out->blend,
                  {{"normal", Blend::Normal}, {"add", Blend::Add}, {"multiply", Blend::Multiply}, {"screen", Blend::Screen}}) ||
          !choice(v.find("fit"), path + ".fit", &out->fit,
                  {{"contain", Fit::Contain}, {"cover", Fit::Cover}, {"fill", Fit::Fill}, {"none", Fit::None}}) ||
          !effects(v.find("effects"), path + ".effects", &out->effects)) {
        return false;
      }
    }
    if (t == "text") {
      if (!v.find("text")) return fail(path, "needs text");
      if (!string(v.find("text"), path + ".text", &out->text, 1000) || !style(v.find("style"), path + ".style", &out->style)) return false;
    }
    if (t == "color") {
      if (!v.find("color")) return fail(path, "needs color");
      if (!color(v.find("color"), path + ".color", &out->color)) return false;
    }
    if (t == "video") {
      if (const Value* a = v.find("audio")) {
        if (!object(*a, path + ".audio", {"mute", "gain"}) || !boolean(a->find("mute"), path + ".audio.mute", &out->mute) ||
            !animatable(a->find("gain"), path + ".audio.gain", &out->gain)) {
          return false;
        }
      }
    }
    if (t == "audio") {
      if (!animatable(v.find("gain"), path + ".gain", &out->gain) || !animatable(v.find("pan"), path + ".pan", &out->pan)) return false;
    }
    return true;
  }

  bool transition(const Value& v, const std::string& path, SceneTransition* out) {
    if (!object(v, path, {"type", "id", "kind", "direction", "duration", "easing", "audio", "metadata"})) return false;
    if (!v.find("kind") || !v.find("duration")) return fail(path, "needs kind and duration");
    return id(v.find("id"), path + ".id", &out->id) &&
           choice(v.find("kind"), path + ".kind", &out->kind,
                  {{"cut", SceneTransitionKind::Cut}, {"crossfade", SceneTransitionKind::Crossfade}, {"push", SceneTransitionKind::Push},
                   {"slide", SceneTransitionKind::Slide}, {"wipe", SceneTransitionKind::Wipe}}) &&
           choice(v.find("direction"), path + ".direction", &out->direction,
                  {{"left", Direction::Left}, {"right", Direction::Right}, {"up", Direction::Up}, {"down", Direction::Down}}) &&
           time(v.find("duration"), path + ".duration", &out->durationUs, false) && easing(v.find("easing"), path + ".easing", &out->easing) &&
           choice(v.find("audio"), path + ".audio", &out->audio,
                  {{"equalGain", AudioFade::EqualGain}, {"equalPower", AudioFade::EqualPower}, {"cut", AudioFade::Cut}});
  }

  bool track(const Value& v, const std::string& path, SceneTrack* out) {
    const Value* kind = v.isObject() ? v.find("kind") : nullptr;
    if (!kind || !kind->isString() || (kind->string != "video" && kind->string != "audio")) return fail(path + ".kind", "must be video or audio");
    out->video = kind->string == "video";
    if (!object(v, path, {"id", "kind", "enabled", "items", "metadata", out->video ? "opacity" : "gain"})) return false;
    if (!id(v.find("id"), path + ".id", &out->id) || !boolean(v.find("enabled"), path + ".enabled", &out->enabled) ||
        !numberF(v.find("opacity"), path + ".opacity", &out->opacity, 0, 1) || !numberF(v.find("gain"), path + ".gain", &out->gain, 0, 4)) {
      return false;
    }
    const Value* items = v.find("items");
    if (!items || !items->isArray()) return fail(path + ".items", "must be a list");
    bool afterItem = false;
    for (size_t k = 0; k < items->array.size(); ++k) {
      const Value& e = items->array[k];
      std::string ep = path + ".items[" + std::to_string(k) + "]";
      const Value* type = e.isObject() ? e.find("type") : nullptr;
      if (type && type->isString() && type->string == "transition") {
        bool followed = k + 1 < items->array.size();
        if (!afterItem || !followed) return fail(ep, "a transition must sit between two items (R3)");
        SceneTransition tr;
        if (!transition(e, ep, &tr)) return false;
        tr.from = int(out->items.size()) - 1;
        out->transitions.push_back(tr);
        afterItem = false;
        continue;
      }
      SceneItem it;
      if (!item(e, ep, out->video, &it)) return false;
      out->items.push_back(std::move(it));
      afterItem = true;
    }
    return true;
  }

  bool document(const Value& v, Scene* out) {
    if (!object(v, "$", {"version", "output", "tracks", "metadata"})) return false;
    const Value* version = v.find("version");
    if (!version || !version->isNumber() || version->number != 1) return fail("$.version", "must be 1");
    const Value* o = v.find("output");
    if (!o) return fail("$", "needs output");
    if (!object(*o, "output", {"width", "height", "fps", "sampleRate", "channels", "background"})) return false;
    if (!o->find("width") || !o->find("height") || !o->find("fps")) return fail("output", "needs width, height and fps");
    SceneOutput& out_ = out->output;
    if (!integer(o->find("width"), "output.width", &out_.width, 2, 8192) || !integer(o->find("height"), "output.height", &out_.height, 2, 8192) ||
        !rate(o->find("fps"), "output.fps", &out_.fpsNum, &out_.fpsDen) ||
        !integer(o->find("sampleRate"), "output.sampleRate", &out_.sampleRate, 8000, 192000) ||
        !integer(o->find("channels"), "output.channels", &out_.channels, 1, 2) || !color(o->find("background"), "output.background", &out_.background)) {
      return false;
    }
    if (out_.width % 2 || out_.height % 2) return fail("output", "width and height must be even");
    if (out_.sampleRate != 44100 && out_.sampleRate != 48000) return fail("output.sampleRate", "must be 44100 or 48000");
    const Value* tracks = v.find("tracks");
    if (!tracks || !tracks->isArray() || tracks->array.empty() || tracks->array.size() > 16) return fail("$.tracks", "must be 1 to 16 tracks");
    for (size_t k = 0; k < tracks->array.size(); ++k) {
      SceneTrack t;
      if (!track(tracks->array[k], "tracks[" + std::to_string(k) + "]", &t)) return false;
      out->tracks.push_back(std::move(t));
    }
    return true;
  }

 private:
  static std::string fmt(double d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", d);
    return buf;
  }

  const SourceResolver& resolve_;
  std::set<std::string> ids_;
  std::string error_;
};

// --- Rules R2–R8 -------------------------------------------------------------------------------

struct Checker {
  std::string error;

  bool fail(const std::string& path, const std::string& message) {
    if (error.empty()) error = path + ": " + message;
    return false;
  }

  bool range(const Animatable& a, const std::string& path, int64_t duration, double lo, double hi) {
    if (a.keys.empty()) {
      if (a.value < lo || a.value > hi) return fail(path, "must be between " + fmt(lo) + " and " + fmt(hi) + " (R7)");
      return true;
    }
    for (size_t k = 0; k < a.keys.size(); ++k) {
      const Keyframe& f = a.keys[k];
      std::string kp = path + ".keys[" + std::to_string(k) + "]";
      if (f.value < lo || f.value > hi) return fail(kp, "must be between " + fmt(lo) + " and " + fmt(hi) + " (R7)");
      if (f.timeUs < 0 || f.timeUs > duration) return fail(kp, "must lie within the item (R6)");
      if (k > 0 && f.timeUs <= a.keys[k - 1].timeUs) return fail(kp, "key times must increase (R6)");
    }
    return true;
  }

  static std::string fmt(double d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", d);
    return buf;
  }

  bool item(const SceneItem& it, const std::string& p) {
    int64_t d = it.durationUs;
    if (d <= 0 || it.startUs < 0) return fail(p, "needs start >= 0 and duration > 0");
    if (it.speed <= 0 || it.speed > 16 || it.inUs < 0) return fail(p, "needs in >= 0 and speed in (0, 16]");
    bool visual = it.type != ItemType::Audio;
    if (visual) {
      const SceneTransform& tr = it.transform;
      const SceneEffects& e = it.effects;
      if (!range(tr.x, p + ".transform.x", d, -100, 100) || !range(tr.y, p + ".transform.y", d, -100, 100) ||
          !range(tr.scale, p + ".transform.scale", d, 0.0001, 100) || !range(tr.rotation, p + ".transform.rotation", d, -1e6, 1e6) ||
          !range(it.opacity, p + ".opacity", d, 0, 1) || !range(e.brightness, p + ".effects.brightness", d, -1, 1) ||
          !range(e.contrast, p + ".effects.contrast", d, 0, 2) || !range(e.saturation, p + ".effects.saturation", d, 0, 2) ||
          !range(e.blurRadius, p + ".effects.radius", d, 0, 0.1) || !range(e.cropLeft, p + ".effects.left", d, 0, 1) ||
          !range(e.cropTop, p + ".effects.top", d, 0, 1) || !range(e.cropRight, p + ".effects.right", d, 0, 1) ||
          !range(e.cropBottom, p + ".effects.bottom", d, 0, 1)) {
        return false;
      }
      if (e.crop && !e.cropLeft.animated() && !e.cropRight.animated() && e.cropLeft.value + e.cropRight.value >= 1) {
        return fail(p + ".effects", "crop removes the whole width (R7)");
      }
      if (e.crop && !e.cropTop.animated() && !e.cropBottom.animated() && e.cropTop.value + e.cropBottom.value >= 1) {
        return fail(p + ".effects", "crop removes the whole height (R7)");
      }
      if (it.type == ItemType::Text && (it.text.empty() || it.style.size <= 0)) return fail(p, "text needs text and a size");
    }
    if (it.type == ItemType::Video || it.type == ItemType::Audio) {
      if (!range(it.gain, p + ".gain", d, 0, 4) || !range(it.pan, p + ".pan", d, -1, 1)) return false;
    }
    return true;
  }

  bool track(const SceneTrack& t, const std::string& p) {
    for (size_t k = 0; k < t.items.size(); ++k) {
      const SceneItem& it = t.items[k];
      std::string ip = p + ".items[" + std::to_string(k) + "]";
      bool audioItem = it.type == ItemType::Audio;
      if (audioItem == t.video) return fail(ip, t.video ? "a video track can't hold audio items" : "an audio track holds only audio items");
      if (!item(it, ip)) return false;
    }
    std::vector<const SceneTransition*> after(t.items.size(), nullptr);
    for (const SceneTransition& tr : t.transitions) {
      if (tr.from < 0 || tr.from + 1 >= int(t.items.size()) || after[tr.from]) return fail(p, "a transition must sit between two items (R3)");
      after[tr.from] = &tr;
    }
    for (size_t k = 1; k < t.items.size(); ++k) {
      const SceneItem &a = t.items[k - 1], &b = t.items[k];
      std::string ip = p + ".items[" + std::to_string(k) + "]";
      if (b.startUs < a.startUs) return fail(ip, "items must be in start order (R2)");
      const SceneTransition* tr = after[k - 1];
      if (!tr) {
        if (b.startUs < a.endUs()) return fail(ip, "overlaps the item before it without a transition between them (R2)");
        continue;
      }
      if (tr->kind == SceneTransitionKind::Cut && tr->durationUs != 0) return fail(p, "a cut has duration 0 (R5)");
      if (b.startUs != a.endUs() - tr->durationUs) {
        return fail(ip, "must start exactly " + fmt(tr->durationUs / 1e6) + " s before the item before it ends (R4)");
      }
      if (tr->durationUs * 2 > std::min(a.durationUs, b.durationUs)) {
        return fail(ip, "the transition before it is longer than half of an item it joins (R5)");
      }
    }
    return true;
  }

  bool scene(const Scene& s) {
    const SceneOutput& o = s.output;
    if (o.width < 0 || o.height < 0 || o.width > 8192 || o.height > 8192 || o.width % 2 || o.height % 2) {
      return fail("output", "width and height must be even, at most 8192");
    }
    if (o.fpsNum < 0 || o.fpsDen <= 0 || (o.fpsNum > 0 && o.fpsNum > 240LL * o.fpsDen)) return fail("output.fps", "must be above 0 and at most 240");
    if (o.channels < 0 || o.channels > 2 || o.sampleRate < 0 || o.sampleRate > 192000) return fail("output", "bad audio format");
    if (s.tracks.empty() || s.tracks.size() > 16) return fail("$.tracks", "must be 1 to 16 tracks");
    size_t total = 0;
    for (size_t k = 0; k < s.tracks.size(); ++k) {
      if (!track(s.tracks[k], "tracks[" + std::to_string(k) + "]")) return false;
      total += s.tracks[k].items.size();
    }
    if (total > 1000) return fail("$.tracks", "more than 1000 items");
    if (s.durationUs() <= 0) return fail("$.tracks", "nothing to play: every enabled track is empty (R8)");
    return true;
  }
};

}  // namespace

Result parseScene(const std::string& text, const SourceResolver& resolve, Scene* out, std::string* error) {
  json::Value doc;
  std::string parseError;
  if (!json::parse(text, &doc, &parseError)) {
    if (error) *error = "not valid JSON: " + parseError;
    return Result::InvalidArgument;
  }
  Scene scene;
  Reader reader(resolve);
  if (!reader.document(doc, &scene)) {
    if (error) *error = reader.error();
    return Result::InvalidArgument;
  }
  Result r = validateScene(scene, error);
  if (r == Result::Ok) *out = std::move(scene);
  return r;
}

Result validateScene(const Scene& scene, std::string* error) {
  Checker checker;
  if (checker.scene(scene)) return Result::Ok;
  if (error) *error = checker.error;
  return Result::InvalidArgument;
}

}  // namespace mf
