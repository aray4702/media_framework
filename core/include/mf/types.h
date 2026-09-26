#pragma once

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
  bool eos = false;
  std::shared_ptr<void> image;  // Mac OS: CVPixelBufferRef
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

enum class StageId { Source, VideoDecode, VideoRender, Audio };
constexpr int kStageCount = 4;

}  // namespace mf
