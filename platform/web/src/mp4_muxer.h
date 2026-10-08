#pragma once

// A portable MP4 muxer for export on the web: browsers encode (WebCodecs) but don't write files.
// Plain C++ with no platform headers, so it is tested natively (re-muxing real files' packets).
// H.264 video and AAC audio; samples are written as they come, and the index (moov) at the end.

#include <cstdint>
#include <memory>
#include <vector>

namespace mf::web {

// Where the file's bytes go: appended, with a patch of bytes already written (the mdat size).
class ByteWriter {
 public:
  virtual ~ByteWriter() = default;
  virtual bool append(const uint8_t* data, size_t size) = 0;
  virtual bool patch(uint64_t offset, const uint8_t* data, size_t size) = 0;
  virtual uint64_t size() const = 0;
};

// A ByteWriter that keeps the file in memory.
class MemoryWriter : public ByteWriter {
 public:
  bool append(const uint8_t* data, size_t size) override;
  bool patch(uint64_t offset, const uint8_t* data, size_t size) override;
  uint64_t size() const override { return bytes.size(); }
  std::vector<uint8_t> bytes;
};

class Mp4Muxer {
 public:
  struct Track {
    bool video = true;
    int width = 0, height = 0;          // video
    int sampleRate = 0, channels = 0;   // audio
    std::vector<uint8_t> config;        // video: avcC; audio: the AAC AudioSpecificConfig
  };

  explicit Mp4Muxer(ByteWriter& out) : out_(out) {}

  // Before the first sample. Returns the track's index.
  int addTrack(const Track&);
  // A sample, in decode order. Times in µs: the presentation time, and how long it lasts (0: until
  // the next sample's, or for the last, the one before it).
  bool write(int track, const uint8_t* data, size_t size, int64_t ptsUs, int64_t durationUs, bool key);
  // Writes the index. False when a write failed (the file is then incomplete).
  bool finish();

 private:
  struct Sample {
    uint64_t offset;
    uint32_t size;
    int64_t ptsUs, durationUs;
    bool key;
  };
  struct State {
    Track track;
    uint32_t timescale;
    std::vector<Sample> samples;
  };
  struct Timing {
    std::vector<int64_t> pts, dts, dur;  // in the track's timescale, in decode order
    int64_t end = 0;                     // the presentation end: the last frame shown, plus its duration
  };
  bool start();
  static Timing timing(const State& t);
  std::vector<uint8_t> trak(const State& t, uint32_t id, uint64_t movieDurationMs) const;

  ByteWriter& out_;
  std::vector<State> tracks_;
  bool started_ = false, ok_ = true;
  uint64_t mdatStart_ = 0;
};

}  // namespace mf::web
