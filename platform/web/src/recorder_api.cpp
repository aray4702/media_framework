// A recording's file on the page's thread (platform/web/editor/record.js): the encoded camera
// and microphone chunks (WebCodecs) muxed into an MP4 in memory with the portable muxer.

#include <emscripten.h>

#include <memory>

#include "mp4_muxer.h"

namespace {

struct Recording {
  mf::web::MemoryWriter file;
  mf::web::Mp4Muxer muxer{file};
};

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE Recording* rec_new() { return new Recording; }
EMSCRIPTEN_KEEPALIVE void rec_delete(Recording* r) { delete r; }

// A track: video (width, height, avcC) or audio (sample rate, channels, AudioSpecificConfig).
// Returns its index, for rec_write.
EMSCRIPTEN_KEEPALIVE int rec_add_track(Recording* r, int video, int width, int height, int sampleRate, int channels,
                                       const uint8_t* config, int configSize) {
  mf::web::Mp4Muxer::Track t;
  t.video = video;
  t.width = width;
  t.height = height;
  t.sampleRate = sampleRate;
  t.channels = channels;
  t.config.assign(config, config + configSize);
  return r->muxer.addTrack(t);
}

// A chunk, in decode order; times in µs from the recording's start.
EMSCRIPTEN_KEEPALIVE int rec_write(Recording* r, int track, const uint8_t* data, int size, double ptsUs, double durationUs, int key) {
  return r->muxer.write(track, data, size_t(size), int64_t(ptsUs), int64_t(durationUs), key);
}

// Writes the index: the file is then rec_data / rec_size. 0: a write failed.
EMSCRIPTEN_KEEPALIVE int rec_finish(Recording* r) { return r->muxer.finish(); }
EMSCRIPTEN_KEEPALIVE const uint8_t* rec_data(Recording* r) { return r->file.bytes.data(); }
EMSCRIPTEN_KEEPALIVE int rec_size(Recording* r) { return int(r->file.bytes.size()); }

}  // extern "C"
