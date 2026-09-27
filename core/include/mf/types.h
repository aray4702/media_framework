#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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
  int clip = 0;  // timeline clip the packet belongs to; ptsUs/dtsUs are in that clip's time
  std::vector<uint8_t> data;
  size_t bytes() const { return data.size(); }
};

struct TrackInfo {
  bool supported = false;  // codec is in MVP scope (H.264 / AAC-LC)
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
  int clip = 0;  // ptsUs is in this clip's time
  bool eos = false;
  std::shared_ptr<void> image;  // Mac OS: CVPixelBufferRef
  size_t bytes() const { return 0; }
};

// --- Composition (§2.4) ---

// Applied to the output in RGB: rgb' = (rgb - 0.5) * contrast + 0.5 + brightness.
struct VideoFilter {
  float brightness = 0.0f;  // -1 to 1; 0 leaves the image unchanged
  float contrast = 1.0f;    // 0 to 2; 1 leaves the image unchanged
  bool operator==(const VideoFilter& o) const { return brightness == o.brightness && contrast == o.contrast; }
};

// Between each pair of clips. Slide: the outgoing clip moves out while the incoming one
// moves in behind it, over durationUs; the two clips overlap on the timeline by that long.
enum class TransitionKind { Cut, SlideLeft, SlideRight };
struct Transition {
  TransitionKind kind = TransitionKind::SlideLeft;
  int64_t durationUs = 1000000;  // shortened to half the shortest clip; ignored for Cut
};

// A caption at the bottom of the video, in timeline time [startUs, endUs).
struct TextOverlay {
  std::string text;
  int64_t startUs = 0;
  int64_t endUs = std::numeric_limits<int64_t>::max();
};

// One output frame as the display should draw it, built by the composition stage.
struct ComposedFrame {
  struct Layer {
    VideoFrame frame;
    float offsetX = 0;  // in output widths, + moves right; each layer is aspect-fit on its own
  };
  int64_t ptsUs = 0;  // timeline time
  uint32_t serial = 0;
  bool eos = false;
  int64_t frameDurationUs = 0;  // of the leading clip, for frame pacing
  int layerCount = 0;           // 1, or 2 during a transition (outgoing first)
  Layer layers[2];
  std::shared_ptr<const std::string> text;  // bottom caption, or null
  VideoFilter filter;
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
