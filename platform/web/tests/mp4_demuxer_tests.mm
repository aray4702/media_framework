// Checks the portable MP4 demuxer against the macOS (AVFoundation) one on real files: the same
// media info, and the same packets, byte for byte, from the start and after seeks.
// Usage: web_demuxer_tests clip.mp4...

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/mp4_demuxer.h"
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

static std::vector<Packet> readAll(IDemuxer& d, int track, size_t limit) {
  std::vector<Packet> out;
  Packet p;
  while (out.size() < limit && d.read(track, &p) == Result::Ok) out.push_back(p);
  return out;
}

// The first difference between two packet lists, or none. `align`: macOS may start earlier (it
// restarts audio a packet or two before the one playing at a seek's time), so its packets before
// this list's first are skipped. Reading further than macOS (it gives up on a damaged file sooner)
// is noted, not a failure; reading less is.
static void comparePackets(const char* what, std::vector<Packet> mac, std::vector<Packet> web, bool align = false) {
  if (align && !web.empty()) {
    size_t skip = 0;
    while (skip < mac.size() && mac[skip].ptsUs < web.front().ptsUs) ++skip;
    mac.erase(mac.begin(), mac.begin() + std::ptrdiff_t(skip));
    size_t n = std::min(mac.size(), web.size());
    mac.resize(n);
    web.resize(n);
  }
  if (web.size() > mac.size()) {
    std::fprintf(stderr, "  note: %s: reads %zu packets, macOS %zu\n", what, web.size(), mac.size());
  } else {
    CHECK(mac.size() == web.size(), "%s: %zu packets on macOS, %zu here", what, mac.size(), web.size());
  }
  for (size_t i = 0; i < std::min(mac.size(), web.size()); ++i) {
    const Packet &a = mac[i], &b = web[i];
    bool same = a.ptsUs == b.ptsUs && a.dtsUs == b.dtsUs && a.key == b.key && a.data == b.data && a.disposable == b.disposable;
    if (!same) {
      CHECK(false, "%s packet %zu: macOS pts %lld dts %lld key %d disp %d %zu bytes; here pts %lld dts %lld key %d disp %d %zu bytes%s",
            what, i, (long long)a.ptsUs, (long long)a.dtsUs, a.key, a.disposable, a.data.size(), (long long)b.ptsUs,
            (long long)b.dtsUs, b.key, b.disposable, b.data.size(), a.data == b.data ? "" : " (data differs)");
      return;
    }
  }
}

static void checkClip(const char* path) {
  std::fprintf(stderr, "%s\n", path);
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto mac = macos::createDemuxer();
  auto web = web::createMp4Demuxer();
  MediaInfo a, b;
  Result ra = mac->open(macos::sourceFromPath(path), &a);
  Result rb = web->open(web::mediaSource(web::memorySource(bytes)), &b);
  CHECK(ra == rb, "open: macOS %s, here %s", toString(ra), toString(rb));
  if (ra != Result::Ok || rb != Result::Ok) return;

  // macOS measures the duration precisely from the media; here it is the movie header's.
  CHECK(std::llabs(a.durationUs - b.durationUs) <= 25000, "duration %lld vs %lld", (long long)a.durationUs, (long long)b.durationUs);
  CHECK(a.video.supported == b.video.supported && a.video.width == b.video.width && a.video.height == b.video.height,
        "video %dx%d vs %dx%d", a.video.width, a.video.height, b.video.width, b.video.height);
  CHECK(a.video.frameDurationUs == b.video.frameDurationUs, "frame duration %lld vs %lld", (long long)a.video.frameDurationUs,
        (long long)b.video.frameDurationUs);
  CHECK(a.video.maxBFrames == b.video.maxBFrames, "B-frame run %d vs %d", a.video.maxBFrames, b.video.maxBFrames);
  CHECK(a.video.rotated == b.video.rotated, "rotated %d vs %d", a.video.rotated, b.video.rotated);
  CHECK(bool(a.audio) == bool(b.audio), "audio %d vs %d", bool(a.audio), bool(b.audio));
  if (a.audio && b.audio) {
    CHECK(a.audio->supported == b.audio->supported && a.audio->sampleRate == b.audio->sampleRate && a.audio->channels == b.audio->channels,
          "audio %d Hz %d ch vs %d Hz %d ch", a.audio->sampleRate, a.audio->channels, b.audio->sampleRate, b.audio->channels);
  }
  if (b.video.supported) {
    auto config = std::static_pointer_cast<web::CodecConfig>(b.video.format);
    std::fprintf(stderr, "  video %s, %zu-byte avcC\n", config->codec.c_str(), config->description.size());
  }
  if (b.audio) std::fprintf(stderr, "  audio %s\n", std::static_pointer_cast<web::CodecConfig>(b.audio->format)->codec.c_str());

  for (int track : {kVideo, kAudio}) {
    const char* name = track == kVideo ? "video" : "audio";
    comparePackets(name, readAll(*mac, track, 1u << 20), readAll(*web, track, 1u << 20));
  }
  for (int64_t target : {int64_t{0}, int64_t{1250000}, a.durationUs / 2, a.durationUs - 300000}) {
    CHECK(mac->seekTo(target) == Result::Ok && web->seekTo(target) == Result::Ok, "seek %lld", (long long)target);
    for (int track : {kVideo, kAudio}) {
      std::string what = std::string(track == kVideo ? "video" : "audio") + " after seek to " + std::to_string(target);
      comparePackets(what.c_str(), readAll(*mac, track, 16), readAll(*web, track, 16), track == kAudio);
    }
  }
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) checkClip(argv[i]);
  std::fprintf(stderr, failures ? "%d FAILED\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
