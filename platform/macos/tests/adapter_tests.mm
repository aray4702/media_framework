// Checks the Mac OS demuxer and decoders on a real clip, without a window or audio output, and
// the compositor's track groups (scene_graph_spec.md §5.1) by reading back rendered pixels.
// Usage: macos_adapter_tests clip.mp4

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/metal_compositor.h"
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

// --- Compositor -------------------------------------------------------------------------------

using Rgb = std::array<float, 3>;

// A full-canvas color layer, drawn at `offsetX` output widths.
static ComposedLayer colorLayer(Color color, float opacity, int group, Blend blend = Blend::Normal, float offsetX = 0) {
  ComposedLayer l;
  l.kind = ComposedLayer::Kind::Color;
  l.color = color;
  l.opacity = opacity;
  l.group = group;
  l.blend = blend;
  l.offsetX = offsetX;
  return l;
}

// Renders a 64x32 frame on a green background and returns the pixel at (x, 16).
static std::vector<Rgb> render(id<MTLDevice> device, ComposedFrame frame, std::initializer_list<int> xs) {
  frame.width = 64;
  frame.height = 32;
  frame.background = {0, 1, 0, 1};
  macos::MetalCompositor compositor;
  CHECK(compositor.init(device, MTLPixelFormatBGRA8Unorm));
  MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:32 mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  d.storageMode = MTLStorageModeShared;
  id<MTLTexture> target = [device newTextureWithDescriptor:d];
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> cmd = [queue commandBuffer];
  compositor.encode(frame, target, cmd);
  [cmd commit];
  [cmd waitUntilCompleted];
  std::vector<Rgb> out;
  for (int x : xs) {
    uint8_t bgra[4];
    [target getBytes:bgra bytesPerRow:64 * 4 fromRegion:MTLRegionMake2D(NSUInteger(x), 16, 1, 1) mipmapLevel:0];
    out.push_back({bgra[2] / 255.f, bgra[1] / 255.f, bgra[0] / 255.f});
  }
  return out;
}

static ComposedGroup track(float opacity = 1) {
  ComposedGroup g;
  g.track = 1;
  g.opacity = opacity;
  return g;
}

static bool near(const Rgb& a, const Rgb& b) {
  for (int i = 0; i < 3; ++i) {
    if (std::abs(a[i] - b[i]) > 3 / 255.f) return false;
  }
  return true;
}

static void compositorTests() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    std::fprintf(stderr, "compositor: no Metal device, skipped\n");
    return;
  }
  const Color red{1, 0, 0, 1}, blue{0, 0, 1, 1};

  // A push on a half-opacity track: B over A inside the track, then the track at 0.5 over the
  // green below. Drawn one by one, B's half would also show A through it.
  ComposedFrame push;
  push.layers = {colorLayer(red, 1, 0), colorLayer(blue, 1, 0, Blend::Normal, 0.5f)};
  push.groups.push_back(track(0.5f));
  std::vector<Rgb> p = render(device, push, {8, 56});
  CHECK(near(p[0], {0.5f, 0.5f, 0}));  // A over green
  CHECK(near(p[1], {0, 0.5f, 0.5f}));  // B only, over green

  // A crossfade a quarter through, with B half transparent: (1 − p)·A + p·B, then over green.
  ComposedFrame fade;
  fade.layers = {colorLayer(red, 0.75f, 0, Blend::Add), colorLayer({0, 0, 1, 0.5f}, 0.25f, 0, Blend::Add)};
  fade.groups.push_back(track());
  Rgb f = render(device, fade, {32})[0];
  CHECK(near(f, {0.75f, 0.125f, 0.125f}));  // alpha 0.875, so 0.125 of the green shows

  // Track effects on the combined image: brightness −0.5, and the right half cropped away.
  ComposedFrame effects;
  effects.layers = {colorLayer(red, 1, 0)};
  ComposedGroup g = track();
  g.effects.brightness = -0.5f;
  g.effects.crop[2] = 0.5f;
  effects.groups.push_back(g);
  std::vector<Rgb> e = render(device, effects, {8, 56});
  CHECK(near(e[0], {0.5f, 0, 0}));
  CHECK(near(e[1], {0, 1, 0}));  // cropped: the background

  // A track blurred as a whole: its edge is soft, while each layer alone has none.
  ComposedFrame blur;
  blur.layers = {colorLayer(red, 1, 0, Blend::Normal, -0.5f)};  // the left half
  ComposedGroup b = track();
  b.effects.blur = 0.1f;  // σ = 3.2 px
  blur.groups.push_back(b);
  std::vector<Rgb> r = render(device, blur, {4, 31, 32, 60});
  CHECK(near(r[0], {1, 0, 0}) && near(r[3], {0, 1, 0}));
  CHECK(r[1][0] > 0.3f && r[1][0] < 0.8f && r[2][0] > 0.2f && r[2][0] < 0.7f);  // mixed across the edge
  std::fprintf(stderr, "compositor: push %.2f,%.2f,%.2f  fade %.2f,%.2f,%.2f  blur edge %.2f,%.2f\n", p[1][0], p[1][1], p[1][2],
               f[0], f[1], f[2], r[1][0], r[2][0]);
}

int main(int argc, char** argv) {
  compositorTests();
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
