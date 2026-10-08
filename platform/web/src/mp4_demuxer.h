#pragma once

// A portable MP4 demuxer: the browser has no AVFoundation, so the web platform parses MP4 itself.
// Plain C++ with no platform headers, so it is tested natively against the macOS demuxer.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mf/adapters.h"

namespace mf::web {

// The bytes of a file. Reads are synchronous: in the browser the file is held in memory, or read
// from a worker where synchronous file reads are allowed.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual int64_t size() const = 0;
  virtual bool read(int64_t offset, void* out, size_t bytes) = 0;
};
std::shared_ptr<ByteSource> memorySource(std::vector<uint8_t> bytes);

// The MediaSource the MP4 demuxer opens: native holds a ByteSource.
MediaSource mediaSource(std::shared_ptr<ByteSource>);

// An image the page has decoded (an ImageBitmap, by its handle in library_mf.js). A ByteSource
// with no bytes, so a video item pointed at it fails to open instead of misreading it.
struct ImageSource : ByteSource {
  ImageSource(int handle, int width, int height) : handle(handle), width(width), height(height) {}
  int64_t size() const override { return 0; }
  bool read(int64_t, void*, size_t) override { return false; }
  int handle, width, height;
};

// TrackInfo::format on the web platform: what a WebCodecs decoder's configure() takes.
struct CodecConfig {
  std::string codec;                 // "avc1.64002a", "mp4a.40.2", "mp3"
  std::vector<uint8_t> description;  // avcC, or the AAC AudioSpecificConfig
};

std::unique_ptr<IDemuxer> createMp4Demuxer();

}  // namespace mf::web
