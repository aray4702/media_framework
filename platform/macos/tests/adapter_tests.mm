// Checks the Mac OS demuxer and decoders on a real clip, without a window or audio output.
// Usage: macos_adapter_tests clip.mp4

#import <Foundation/Foundation.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "mf/macos.h"

using namespace mf;

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  line %d: CHECK failed: %s\n", __LINE__, #cond); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

// Decodes every video packet from the demuxer's current position; returns frame PTS in output
// order. Frames that fail to decode (damaged clips) are counted in *corrupt.
static std::vector<int64_t> decodeAllVideo(IDemuxer& demuxer, IVideoDecoder& decoder, int* packets, int* corrupt) {
  std::vector<int64_t> pts;
  *packets = *corrupt = 0;
  bool inputDone = false;
  for (int idle = 0; idle < 2000;) {
    bool progress = false;
    if (!inputDone) {
      Packet p;
      Result r = demuxer.read(kVideo, &p);
      if (r == Result::Eos) {
        decoder.signalEos();
        inputDone = true;
      } else {
        CHECK(r == Result::Ok);
        Result q;
        while ((q = decoder.queue(p)) == Result::Again) {
          VideoFrame f;
          Result d = decoder.dequeue(&f);
          if (d == Result::Ok) pts.push_back(f.ptsUs);
          else if (d == Result::CorruptFrame) ++*corrupt;
          else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (q == Result::CorruptFrame) ++*corrupt;
        ++*packets;
      }
      progress = true;
    }
    VideoFrame f;
    Result r = decoder.dequeue(&f);
    if (r == Result::Ok) {
      CHECK(f.image != nullptr);
      pts.push_back(f.ptsUs);
      progress = true;
    } else if (r == Result::CorruptFrame) {
      ++*corrupt;
      progress = true;
    } else if (r == Result::Eos) {
      break;
    }
    if (progress) {
      idle = 0;
    } else {
      ++idle;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return pts;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s clip.mp4\n", argv[0]);
    return 2;
  }
  auto demuxer = macos::createDemuxer();
  MediaInfo info;
  Result r = demuxer->open(macos::sourceFromPath(argv[1]), &info);
  std::fprintf(stderr, "open: %s duration=%.3fs video=%dx%d frame=%lldus supported=%d audio=%s\n", toString(r),
               info.durationUs / 1e6, info.video.width, info.video.height, (long long)info.video.frameDurationUs,
               info.video.supported, info.audio ? (info.audio->supported ? "AAC-LC" : "unsupported") : "none");
  if (r != Result::Ok) return 1;

  // All frames come out, in strictly increasing PTS order.
  auto decoder = macos::createVideoDecoder();
  CHECK(decoder->configure(info.video, [] {}) == Result::Ok);
  int packets = 0, corrupt = 0;
  std::vector<int64_t> pts = decodeAllVideo(*demuxer, *decoder, &packets, &corrupt);
  std::fprintf(stderr, "video: %d packets, %zu frames, %d failed to decode\n", packets, pts.size(), corrupt);
  CHECK(packets > 0);
  CHECK(int(pts.size()) + corrupt == packets);
  for (size_t i = 1; i < pts.size(); ++i) CHECK(pts[i] > pts[i - 1]);

  // Seek lands on a keyframe at or before the target.
  int64_t target = info.durationUs / 2;
  CHECK(demuxer->seekTo(target) == Result::Ok);
  Packet first;
  CHECK(demuxer->read(kVideo, &first) == Result::Ok);
  std::fprintf(stderr, "seek %.3fs -> keyframe %.3fs key=%d\n", target / 1e6, first.ptsUs / 1e6, first.key);
  CHECK(first.key);
  CHECK(first.ptsUs <= target);
  CHECK(target - first.ptsUs <= 10 * 1000000);

  // Audio decodes to about the track's duration of PCM.
  if (info.audio && info.audio->supported) {
    auto audio = macos::createAudioDecoder();
    CHECK(audio->configure(*info.audio) == Result::Ok);
    CHECK(demuxer->seekTo(0) == Result::Ok);
    int64_t frames = 0, lastPts = 0;
    int corrupt = 0;
    Packet p;
    while (demuxer->read(kAudio, &p) == Result::Ok) {
      PcmBuffer pcm;
      if (audio->decode(p, &pcm) != Result::Ok) ++corrupt;
      frames += int64_t(pcm.samples.size()) / info.audio->channels;
      lastPts = p.ptsUs;
    }
    double seconds = double(frames) / info.audio->sampleRate;
    std::fprintf(stderr, "audio: %.3fs of PCM, last packet %.3fs, %d corrupt\n", seconds, lastPts / 1e6, corrupt);
    CHECK(seconds > 0.5 * lastPts / 1e6);
  }

  std::fprintf(stderr, failures ? "FAILED (%d)\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
