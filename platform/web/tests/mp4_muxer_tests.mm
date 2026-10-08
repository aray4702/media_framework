// Checks the portable MP4 muxer on real files: every clip's packets, re-muxed into a new file, read
// back by the macOS (AVFoundation) demuxer as they were in the source: the same presentation times,
// keyframes and bytes, B-frames included, in decode order with increasing decode times. (Decode
// times are rebuilt from the presentation times, so they may differ from the source's by rounding.)
// Usage: web_muxer_tests clip.mp4... (KEEP=1 keeps the muxed files)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include "../src/mp4_demuxer.h"
#include "../src/mp4_muxer.h"
#include "mf/macos.h"

using namespace mf;

static int failures = 0;
#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (!(cond)) {                                          \
      std::fprintf(stderr, "  CHECK failed: %s: ", #cond);  \
      std::fprintf(stderr, __VA_ARGS__);                    \
      std::fprintf(stderr, "\n");                           \
      ++failures;                                           \
    }                                                       \
  } while (0)

static std::vector<Packet> readAll(IDemuxer& d, int track) {
  std::vector<Packet> out;
  Packet p;
  while (d.read(track, &p) == Result::Ok) out.push_back(p);
  return out;
}

static void checkClip(const char* path) {
  std::fprintf(stderr, "%s\n", path);
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto source = web::createMp4Demuxer();
  MediaInfo info;
  CHECK(source->open(web::mediaSource(web::memorySource(bytes)), &info) == Result::Ok, "open");
  std::vector<Packet> packets[2] = {info.video.supported ? readAll(*source, kVideo) : std::vector<Packet>{},
                                    info.audio ? readAll(*source, kAudio) : std::vector<Packet>{}};

  web::MemoryWriter file;
  web::Mp4Muxer muxer(file);
  int ids[2] = {-1, -1};
  if (!packets[kVideo].empty()) {
    web::Mp4Muxer::Track t;
    t.width = info.video.width;
    t.height = info.video.height;
    t.config = std::static_pointer_cast<web::CodecConfig>(info.video.format)->description;
    ids[kVideo] = muxer.addTrack(t);
  }
  if (!packets[kAudio].empty()) {
    web::Mp4Muxer::Track t;
    t.video = false;
    t.sampleRate = info.audio->sampleRate;
    t.channels = info.audio->channels;
    t.config = std::static_pointer_cast<web::CodecConfig>(info.audio->format)->description;
    ids[kAudio] = muxer.addTrack(t);
  }
  // Interleaved by decode time, as an encoder's output would come.
  size_t next[2] = {0, 0};
  for (;;) {
    int track = -1;
    for (int k : {kVideo, kAudio}) {
      if (next[k] < packets[k].size() && (track < 0 || packets[k][next[k]].dtsUs < packets[track][next[track]].dtsUs)) track = k;
    }
    if (track < 0) break;
    const Packet& p = packets[track][next[track]++];
    int64_t duration = track == kVideo && next[track] == packets[track].size() ? info.video.frameDurationUs : 0;
    CHECK(muxer.write(ids[track], p.data.data(), p.data.size(), p.ptsUs, duration, p.key), "write");
  }
  CHECK(muxer.finish(), "finish");

  std::string out = "/tmp/mf_muxer_test_" + std::to_string(getpid()) + ".mp4";
  std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(file.bytes.data()), std::streamsize(file.bytes.size()));
  auto mac = macos::createDemuxer();
  MediaInfo back;
  CHECK(mac->open(macos::sourceFromPath(out), &back) == Result::Ok, "macOS can't open the muxed file");
  CHECK(back.video.width == info.video.width && back.video.height == info.video.height, "size %dx%d", back.video.width, back.video.height);
  CHECK(bool(back.audio) == bool(info.audio), "audio");
  for (int track : {kVideo, kAudio}) {
    if (packets[track].empty()) continue;
    std::vector<Packet> got = readAll(*mac, track);
    CHECK(got.size() == packets[track].size(), "%s: %zu packets back, %zu written", track == kVideo ? "video" : "audio", got.size(),
          packets[track].size());
    for (size_t i = 0; i < std::min(got.size(), packets[track].size()); ++i) {
      const Packet &a = packets[track][i], &b = got[i];
      bool ordered = i == 0 || got[i - 1].dtsUs < b.dtsUs;
      if (a.ptsUs != b.ptsUs || !ordered || b.dtsUs > b.ptsUs || a.key != b.key || a.data != b.data) {
        CHECK(false, "%s packet %zu: wrote pts %lld dts %lld key %d, read pts %lld dts %lld key %d%s", track == kVideo ? "video" : "audio",
              i, (long long)a.ptsUs, (long long)a.dtsUs, a.key, (long long)b.ptsUs, (long long)b.dtsUs, b.key,
              a.data == b.data ? "" : " (data differs)");
        break;
      }
    }
  }
  std::fprintf(stderr, "  %zu video and %zu audio packets, %zu bytes\n", packets[kVideo].size(), packets[kAudio].size(), file.bytes.size());
  if (getenv("KEEP")) std::fprintf(stderr, "  kept %s\n", out.c_str());  // KEEP=1: for checking with other tools
  else unlink(out.c_str());
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) checkClip(argv[i]);
  std::fprintf(stderr, failures ? "%d FAILED\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
