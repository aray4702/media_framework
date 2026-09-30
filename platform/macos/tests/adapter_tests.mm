// Checks the Mac OS demuxer and decoders on a real clip, without a window or audio output, and
// the compositor's track groups (scene_graph_spec.md §5.1) by reading back rendered pixels.
// Usage: macos_adapter_tests clip.mp4

#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "../src/metal_compositor.h"
#include "mf/effects.h"
#include "mf/macos.h"
#include "mf/segment_recorder.h"

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

// The 64 × 32 canvas (red left half, green background) into a target of another size.
static std::vector<Rgb> renderFramed(id<MTLDevice> device, int tw, int th, const macos::MetalCompositor::Framing& framing,
                                     std::initializer_list<std::pair<int, int>> points) {
  ComposedFrame frame;
  frame.width = 64;
  frame.height = 32;
  frame.background = {0, 1, 0, 1};
  frame.layers = {colorLayer({1, 0, 0, 1}, 1, -1, Blend::Normal, -0.5f)};  // the left half
  macos::MetalCompositor compositor;
  CHECK(compositor.init(device, MTLPixelFormatBGRA8Unorm));
  MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                               width:NSUInteger(tw)
                                                                              height:NSUInteger(th)
                                                                           mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  d.storageMode = MTLStorageModeShared;
  id<MTLTexture> target = [device newTextureWithDescriptor:d];
  id<MTLCommandBuffer> cmd = [[device newCommandQueue] commandBuffer];
  compositor.encode(frame, target, cmd, framing);
  [cmd commit];
  [cmd waitUntilCompleted];
  std::vector<Rgb> out;
  for (auto [x, y] : points) {
    uint8_t bgra[4];
    [target getBytes:bgra bytesPerRow:NSUInteger(tw) * 4 fromRegion:MTLRegionMake2D(NSUInteger(x), NSUInteger(y), 1, 1) mipmapLevel:0];
    out.push_back({bgra[2] / 255.f, bgra[1] / 255.f, bgra[0] / 255.f});
  }
  return out;
}

// A 2:1 canvas into a square target: fit shows it all between bands (black, or the background),
// fill covers the target and crops the sides, keeping the part the crop position says.
static void framingTests() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) return;
  const Rgb red{1, 0, 0}, green{0, 1, 0}, black{0, 0, 0};
  macos::MetalCompositor::Framing fit;
  std::vector<Rgb> a = renderFramed(device, 32, 32, fit, {{16, 2}, {4, 16}, {28, 16}});  // canvas 32 × 16 in rows 8 to 24
  CHECK(near(a[0], black) && near(a[1], red) && near(a[2], green));
  fit.backgroundBands = true;
  CHECK(near(renderFramed(device, 32, 32, fit, {{16, 2}})[0], green));  // an export's bands: the background
  macos::MetalCompositor::Framing fill;
  fill.fill = true;
  std::vector<Rgb> c = renderFramed(device, 32, 32, fill, {{4, 16}, {28, 16}, {4, 1}});  // canvas x 16 to 48: the middle
  CHECK(near(c[0], red) && near(c[1], green) && near(c[2], red));  // no band: the top row is canvas
  fill.cropX = 0;  // the left edge
  std::vector<Rgb> l = renderFramed(device, 32, 32, fill, {{4, 16}, {28, 16}});
  CHECK(near(l[0], red) && near(l[1], red));
  fill.cropX = 1;  // the right edge
  std::vector<Rgb> r = renderFramed(device, 32, 32, fill, {{4, 16}, {28, 16}});
  CHECK(near(r[0], green) && near(r[1], green));
  std::fprintf(stderr, "framing: fit band %.0f,%.0f,%.0f  fill center %.0f/%.0f  left %.0f  right %.0f\n", a[0][0], a[0][1], a[0][2],
               c[0][0], c[1][1], l[1][0], r[0][1]);
}

// flipX: an image, red on its left half and blue on its right, drawn filling the canvas, is
// mirrored within its box; with its left quarter cropped, the crop is of the image, before the flip.
static void flipTests() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) return;
  NSDictionary* attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}, (id)kCVPixelBufferMetalCompatibilityKey : @YES};
  CVPixelBufferRef pixels = nullptr;
  CHECK(CVPixelBufferCreate(nullptr, 16, 8, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &pixels) == kCVReturnSuccess);
  if (!pixels) return;
  CVPixelBufferLockBaseAddress(pixels, 0);
  auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
  size_t stride = CVPixelBufferGetBytesPerRow(pixels);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t* p = base + y * stride + x * 4;  // B, G, R, A
      bool left = x < 8;
      p[0] = left ? 0 : 255, p[1] = 0, p[2] = left ? 255 : 0, p[3] = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
  auto image = [&](bool flip, float cropLeft, float blur = 0) {
    ComposedLayer l;
    l.kind = ComposedLayer::Kind::Image;
    l.frame.image = std::shared_ptr<void>(CVPixelBufferRetain(pixels), [](void* p) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p)); });
    l.fit = Fit::Fill;
    l.flipX = flip;
    l.effects.crop[0] = cropLeft;
    l.effects.blur = blur;
    ComposedFrame f;
    f.layers = {l};
    return render(device, f, {8, 56});
  };
  const Rgb red{1, 0, 0}, blue{0, 0, 1};
  std::vector<Rgb> plain = image(false, 0), flipped = image(true, 0), cropped = image(true, 0.25f);
  CHECK(near(plain[0], red) && near(plain[1], blue));
  CHECK(near(flipped[0], blue) && near(flipped[1], red));
  CHECK(near(cropped[0], blue) && near(cropped[1], red));  // a third red (x 4 to 8 of 4 to 16), mirrored to the right
  // Through an effects texture (a tiny blur takes the offscreen path): still mirrored.
  std::vector<Rgb> offscreen = image(true, 0, 0.001f);
  CHECK(near(offscreen[0], blue) && near(offscreen[1], red));
  CVPixelBufferRelease(pixels);
}

// The beauty plugin, built into the build tree's plugins folder, loaded and drawn by the compositor.
static void pluginTests() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) return;
  std::vector<std::string> errors;
  std::vector<std::string> types = macos::loadEffectPlugins(&errors);
  for (const std::string& e : errors) std::fprintf(stderr, "  plugin: %s\n", e.c_str());
  CHECK(errors.empty());
  const EffectInfo* info = findEffect("beauty");
  CHECK(info && info->params.size() == 3 && info->param("whiten") == 1);
  if (!info) return;
  CHECK(macos::loadEffectPlugins(&errors).empty() && errors.empty());  // loaded once: again is a no-op

  auto beauty = [](float smooth, float whiten, float sharpen) {
    ComposedEffects e;
    e.plugins.push_back({"beauty", {smooth, whiten, sharpen}});
    return e;
  };
  // Skin: whitened by the log curve, log(c·(k − 1) + 1) / log(k) with k = 1 + 4·whiten.
  Color skin{0.85f, 0.62f, 0.50f, 1};
  auto lift = [](float c) { return std::log(c * 4 + 1) / std::log(5.0f); };
  Rgb whitened{lift(skin.r), lift(skin.g), lift(skin.b)};
  ComposedFrame f;
  f.layers = {colorLayer(skin, 1, -1)};
  f.layers[0].effects = beauty(0, 1, 0);
  CHECK(near(render(device, f, {32})[0], whitened));
  // On a track's combined image too.
  f.layers[0].effects = {};
  f.layers[0].group = 0;
  f.groups = {track()};
  f.groups[0].effects = beauty(0, 1, 0);
  CHECK(near(render(device, f, {32})[0], whitened));
  // Not skin, and flat: smoothing and sharpening leave it as it is.
  Color blue{0.2f, 0.4f, 0.9f, 1};
  ComposedFrame g;
  g.layers = {colorLayer(blue, 1, -1)};
  g.layers[0].effects = beauty(1, 1, 1);
  CHECK(near(render(device, g, {32})[0], Rgb{blue.r, blue.g, blue.b}));
  // An effect no plugin has is skipped; the plugin effect still runs before the blur.
  g.layers[0].effects.plugins.insert(g.layers[0].effects.plugins.begin(), ComposedPluginEffect{"missing", {1}});
  g.layers[0].effects.blur = 0.01f;
  CHECK(near(render(device, g, {32})[0], Rgb{blue.r, blue.g, blue.b}));
}

// The camera: devices listed; frames streamed only when access was granted before (the test never
// asks, so it never shows the system prompt), NV12 on the host clock, and none after stop().
static void cameraTests() {
  auto camera = macos::createCamera();
  std::vector<CameraDevice> devices = camera->devices();
  for (const CameraDevice& d : devices) std::fprintf(stderr, "camera: %s%s\n", d.name.c_str(), d.front ? " (front)" : "");
  for (const CameraDevice& d : devices) CHECK(!d.id.empty() && !d.name.empty());
  if (devices.empty()) return (void)std::fprintf(stderr, "camera: none, skipped\n");
  if (macos::cameraAccess(false) != macos::CameraAccess::Granted) {
    CHECK(camera->start("", [](const VideoFrame&) {}, nullptr) == Result::PermissionDenied);
    return (void)std::fprintf(stderr, "camera: access not granted, streaming skipped\n");
  }
  std::atomic<int> frames{0}, nv12{0};
  std::atomic<int64_t> lastPts{0};
  CHECK(camera->start(devices[0].id,
                      [&](const VideoFrame& f) {
                        auto pixels = static_cast<CVPixelBufferRef>(f.image.get());
                        if (pixels && CVPixelBufferGetPixelFormatType(pixels) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) ++nv12;
                        lastPts = f.ptsUs;
                        ++frames;
                      },
                      nullptr) == Result::Ok);
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // the camera takes a moment to start
  camera->stop();
  int count = frames;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  std::fprintf(stderr, "camera: %d frames in 2.5 s\n", count);
  CHECK(count >= 10);
  CHECK(nv12 == count);
  CHECK(std::llabs(macos::hostNowNs() / 1000 - lastPts) < 1000000);  // on the host clock
  CHECK(frames == count);  // none after stop()
}

// A segment recorded into a real file: 30 synthetic frames (320 × 240) with the microphone, read
// back: about a second long, at the frame size, with audio.
static void segmentTests() {
  NSString* path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"mf_segment_test.mp4"];
  auto platform = macos::createPlatform();
  SegmentRecorder rec(*platform);
  CHECK(rec.start(macos::exportTargetFromPath(path.UTF8String), 320, 240, 30, 48000, 1) == Result::Ok);
  NSDictionary* attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}, (id)kCVPixelBufferMetalCompatibilityKey : @YES};
  CVPixelBufferRef pixels = nullptr;
  CVPixelBufferCreate(nullptr, 320, 240, kCVPixelFormatType_32BGRA, (__bridge CFDictionaryRef)attrs, &pixels);
  CHECK(pixels != nullptr);
  if (!pixels) return;
  VideoFrame f;
  f.image = std::shared_ptr<void>(pixels, [](void* p) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p)); });
  int w = 0, h = 0;
  CHECK(macos::frameSize(f, &w, &h) && w == 320 && h == 240);
  std::vector<int16_t> pcm(1600, 1000);  // 1/30 s at 48 kHz
  int64_t t0 = macos::hostNowNs() / 1000;
  for (int i = 0; i < 30; ++i) {  // as a camera delivers: a frame and its audio every 1/30 s
    f.ptsUs = t0 + i * 33333;
    rec.video(f);
    rec.audio(pcm.data(), 1600, f.ptsUs * 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  Result result = Result::Again;
  int64_t length = 0;
  rec.stop([&](Result r, int64_t us) {
    result = r;
    length = us;
    dispatch_semaphore_signal(done);
  });
  CHECK(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) == 0);
  std::fprintf(stderr, "segment: %s, %.3f s, %d frames dropped\n", toString(result), length / 1e6, rec.droppedFrames());
  CHECK(result == Result::Ok && rec.droppedFrames() == 0 && length == 30 * 33333);
  auto demuxer = macos::createDemuxer();
  MediaInfo info;
  CHECK(demuxer->open(macos::sourceFromPath(path.UTF8String), &info) == Result::Ok);
  std::fprintf(stderr, "segment file: %dx%d, %.3f s, audio %s\n", info.video.width, info.video.height, info.durationUs / 1e6,
               info.audio ? "yes" : "no");
  CHECK(info.video.width == 320 && info.video.height == 240);
  CHECK(std::llabs(info.durationUs - 1000000) < 50000);
  CHECK(info.audio && info.audio->supported);
  [NSFileManager.defaultManager removeItemAtPath:path error:nil];
}

int main(int argc, char** argv) {
  compositorTests();
  framingTests();
  flipTests();
  pluginTests();
  cameraTests();
  segmentTests();
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s clip.mp4|audio.m4a\n", argv[0]);
    return 2;
  }
  auto demuxer = macos::createDemuxer();
  MediaInfo info;
  Result r = demuxer->open(macos::sourceFromPath(argv[1]), &info);
  std::fprintf(stderr, "open: %s duration=%.3fs video=%dx%d frame=%lldus supported=%d audio=%s\n", toString(r),
               info.durationUs / 1e6, info.video.width, info.video.height, (long long)info.video.frameDurationUs,
               info.video.supported, info.audio ? (info.audio->supported ? "AAC-LC or MP3" : "unsupported") : "none");
  if (r != Result::Ok) return 1;
  bool video = info.video.width > 0;  // an audio-only file has no video to check
  CHECK(video || info.audio);

  // All frames come out, in strictly increasing PTS order.
  if (video) {
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
  } else {
    // Audio only: a seek starts reading at the target.
    int64_t target = info.durationUs / 2;
    CHECK(demuxer->seekTo(target) == Result::Ok);
    Packet first;
    CHECK(demuxer->read(kAudio, &first) == Result::Ok);
    std::fprintf(stderr, "audio only: seek %.3fs -> packet %.3fs\n", target / 1e6, first.ptsUs / 1e6);
    CHECK(std::llabs(first.ptsUs - target) <= 100000);
  }

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
