#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mf {

enum class Result {
  Ok,
  InvalidState,
  WrongThread,
  InvalidArgument,
  FileOpenFailed,
  UnsupportedFormat,
  NoDecoder,
  MalformedMedia,
  DecoderFailed,
  AudioDeviceFailed,
  Unsupported,  // the platform has no implementation (e.g. no export sink)
  WriteFailed,  // export: encoding or writing the output file failed
  PermissionDenied,  // the camera or microphone isn't allowed (System Settings > Privacy)
  CaptureFailed,     // no camera, or it stopped
  // Adapter-to-core only; the Player never returns these.
  Again,         // try again later (input full / no output yet)
  Eos,           // end of track
  CorruptFrame,  // one frame failed to decode; playback continues
};
const char* toString(Result);

enum class State { Start, Ready, Play, Error, Shutdown };
const char* toString(State);

enum class Warning { AudioUnsupported, RotationIgnored };
const char* toString(Warning);

constexpr int kVideo = 0;
constexpr int kAudio = 1;

struct Packet {
  int track = kVideo;
  int64_t ptsUs = 0;
  int64_t dtsUs = 0;
  bool key = false;
  bool eos = false;  // end-of-track marker, carries no data
  uint32_t serial = 0;
  int item = 0;  // scene item the packet belongs to; ptsUs/dtsUs are in that item's media time
  std::vector<uint8_t> data;
  size_t bytes() const { return data.size(); }
};

struct TrackInfo {
  bool supported = false;  // codec is in MVP scope (H.264; AAC-LC or MP3)
  int64_t frameDurationUs = 0;
  int width = 0, height = 0;
  bool rotated = false;  // non-identity track matrix (A21)
  int sampleRate = 0, channels = 0;
  std::shared_ptr<void> format;  // platform codec config (Mac OS: CMFormatDescriptionRef)
};

struct MediaInfo {
  int64_t durationUs = 0;
  TrackInfo video;
  std::optional<TrackInfo> audio;
};

// A decoded frame stays in the platform's surface; `image` releases it with the last reference.
struct VideoFrame {
  int64_t ptsUs = 0;
  uint32_t serial = 0;
  int item = 0;  // scene item; ptsUs is in its media time
  bool eos = false;
  std::shared_ptr<void> image;  // Mac OS: CVPixelBufferRef (NV12 from the decoder, BGRA for a still image)
  size_t bytes() const { return 0; }
};

// --- Composition (§2.4) ---

// Applied to the output in RGB: rgb' = (rgb - 0.5) * contrast + 0.5 + brightness.
struct VideoFilter {
  float brightness = 0.0f;  // -1 to 1; 0 leaves the image unchanged
  float contrast = 1.0f;    // 0 to 2; 1 leaves the image unchanged
  bool operator==(const VideoFilter& o) const { return brightness == o.brightness && contrast == o.contrast; }
};

// What sets the output times while playing (§2.5). Export always uses a fixed frame grid.
enum class OutputDriver {
  Auto,         // LeadingClip when the only visual item is one video, else Vsync
  LeadingClip,  // one output frame per frame of the highest-fps active clip, paced by AvSync
  Vsync,        // one output frame per display refresh, at the clock time it will be seen
};

// Export output (§2.5). Frames are composed at n / fps and written with the platform encoder.
enum class VideoCodec { H264, HEVC };

// How a scene goes into a frame of another aspect ratio: all of it, with bands of its background
// color along the short side (Fit), or covering the frame, cropped along the long side (Fill).
enum class FrameFit { Fit, Fill };

struct ExportSettings {
  int width = 1920, height = 1080;  // set by Exporter::start: the frame size below, or the scene's output size
  int fps = 30;                      // set from the scene's output by Exporter::start
  // The file's frame size; 0: the scene's output size. The scene is still composed at its own
  // size and then scaled to this (by frameFit when the aspect ratio differs), so every item keeps
  // its size relative to the frame, including items at their natural pixel size (fit none).
  int frameWidth = 0, frameHeight = 0;
  FrameFit frameFit = FrameFit::Fit;
  // Fill: which part stays where the scene is cut, 0 (left, top) to 1 (right, bottom); 0.5 centers.
  float cropX = 0.5f, cropY = 0.5f;
  int videoBitrate = 10000000;
  int audioBitrate = 192000;
  VideoCodec codec = VideoCodec::H264;  // the container comes from the target (Mac OS: the file's extension)
  // A live source (a camera): encode with low latency, trading quality for keeping up, and take a
  // frame that is just a camera image of the file's size as it is.
  bool realtime = false;
};

// Opaque handle made by the platform layer. Mac OS: NSURL of the output file.
struct ExportTarget {
  std::shared_ptr<void> native;
};

// --- Scene rendering (scene_graph_spec.md §4, §5) ---

struct Color {
  float r = 0, g = 0, b = 0, a = 1;  // sRGB, straight alpha, 0 to 1
  bool operator==(const Color& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

enum class Fit { Contain, Cover, Fill, None };
enum class Blend { Normal, Add, Multiply, Screen };
enum class TextAlign { Left, Center, Right };

struct TextStyle {
  std::string font = "system";  // "system", "system-bold", or a family name
  float size = 0.05f;           // line height, as a fraction of the output height
  Color color{1, 1, 1, 1};
  TextAlign align = TextAlign::Center;
  bool hasBox = false;  // a background box behind the text
  Color box{0, 0, 0, 0.55f};
  float maxWidth = 0.9f;  // fraction of the output width; longer text wraps
  bool operator==(const TextStyle& o) const {
    return font == o.font && size == o.size && color == o.color && align == o.align && hasBox == o.hasBox && box == o.box &&
           maxWidth == o.maxWidth;
  }
};

// A plugin effect (mf/effects.h) as drawn in one output frame.
struct ComposedPluginEffect {
  std::string type;
  std::vector<float> params;  // in the order of the type's EffectInfo
  bool operator==(const ComposedPluginEffect& o) const { return type == o.type && params == o.params; }
};

// Effects (§4.4) as drawn in one output frame, applied crop → chromaKey → colorAdjust → plugins → blur.
struct ComposedEffects {
  float crop[4] = {0, 0, 0, 0};              // left, top, right, bottom fractions removed
  float brightness = 0, contrast = 1, saturation = 1;
  float blur = 0;                            // Gaussian sigma, as a fraction of the output height
  bool chromaKey = false;
  Color keyColor;
  float keyTolerance = 0.15f, keySoftness = 0.1f;
  std::vector<ComposedPluginEffect> plugins;  // in document order
  bool operator==(const ComposedEffects& o) const {
    return std::equal(crop, crop + 4, o.crop) && brightness == o.brightness && contrast == o.contrast &&
           saturation == o.saturation && blur == o.blur && chromaKey == o.chromaKey && keyColor == o.keyColor &&
           keyTolerance == o.keyTolerance && keySoftness == o.keySoftness && plugins == o.plugins;
  }
};

// One item as drawn in one output frame, with every value evaluated at that frame's time.
struct ComposedLayer {
  enum class Kind { Video, Image, Text, Color };
  Kind kind = Kind::Video;
  int item = -1;                             // scene item index
  int group = -1;                            // ComposedFrame::groups index, or -1: drawn onto the canvas
  VideoFrame frame;                          // Video, Image
  std::shared_ptr<const std::string> text;   // Text
  TextStyle style;                           // Text
  Color color;                               // Color: fills the output

  // Geometry (§4.1): fit the (cropped) natural size, then place the anchor at (x, y).
  Fit fit = Fit::Contain;
  float x = 0.5f, y = 0.5f;                  // fractions of the output
  float anchorX = 0.5f, anchorY = 0.5f;      // fractions of the item's box
  float scale = 1, rotation = 0;             // rotation in degrees, clockwise
  bool flipX = false;                        // the image mirrored left to right within its box
  float offsetX = 0, offsetY = 0;            // transition (push, slide), in output widths / heights
  float clip[4] = {0, 0, 1, 1};              // visible output region x0, y0, x1, y1 (wipe)

  float opacity = 1;
  Blend blend = Blend::Normal;  // in a group: Normal (over) or Add (crossfade mix)
  ComposedEffects effects;
};

// A track drawn on its own first (scene_graph_spec.md §5.1): its layers are combined over a
// transparent image, then the track's effects and opacity apply, and the result is blended
// onto the canvas. Used while two of its items are visible (a transition) or when the track
// has effects; otherwise its layer is drawn onto the canvas directly, which looks the same.
struct ComposedGroup {
  int track = -1;  // scene track index
  float opacity = 1;
  Blend blend = Blend::Normal;  // onto the canvas: the top item's
  ComposedEffects effects;      // on the combined image, in output coordinates
  bool operator==(const ComposedGroup& o) const {
    return track == o.track && opacity == o.opacity && blend == o.blend && effects == o.effects;
  }
};

// One output frame as the display (or the export sink) should draw it: layers bottom to top
// on a canvas of the scene's output size. Playback letterboxes the canvas into the view.
struct ComposedFrame {
  int64_t ptsUs = 0;  // timeline time
  uint32_t serial = 0;
  bool eos = false;
  int64_t frameDurationUs = 0;  // of the leading item, for frame pacing
  int width = 0, height = 0;    // canvas
  Color background;
  std::vector<ComposedLayer> layers;  // a group's layers are next to each other
  std::vector<ComposedGroup> groups;
  VideoFilter filter;       // global, applied to video and image layers after their own effects
  int64_t presentAtNs = 0;  // Vsync driver: the vsync this frame was composed for; 0 = paced by AvSync
  size_t bytes() const { return 0; }
};

struct PcmBuffer {
  int64_t ptsUs = 0;
  std::vector<int16_t> samples;  // S16 interleaved
};

// Opaque handles made by the platform layer (A15).
struct MediaSource {
  std::shared_ptr<void> native;  // Mac OS: NSURL
};
struct RenderTarget {
  void* native = nullptr;  // Mac OS: NSView* backed by a CAMetalLayer
};

struct Progress {
  enum class Kind { Did, Idle, WaitUntil };
  Kind kind = Kind::Idle;
  int64_t deadlineNs = 0;
  static Progress did() { return {Kind::Did, 0}; }
  static Progress idle() { return {Kind::Idle, 0}; }
  static Progress waitUntil(int64_t ns) { return {Kind::WaitUntil, ns}; }
};

enum class StageId { Source, VideoDecode, Composition, VideoRender, Audio };
constexpr int kStageCount = 5;

}  // namespace mf
