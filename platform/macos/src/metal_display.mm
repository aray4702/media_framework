// IDisplay on a CAMetalLayer, presented from the display link. Each vsync callback says when
// the frame drawn now will reach the screen; the frame due at that time is drawn and presented
// right away, so frames land on the real vsync grid. nextDrawable can block for up to 1 s, but
// only on the display-link thread, never on T3 (§2.2).

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach_time.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

#include "mf/macos.h"

// CVDisplayLink is deprecated in Mac OS 15; its replacement (CADisplayLink) needs Mac OS 14 (A13).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace mf::macos {
namespace {

// NV12 (BT.709, video range) to RGB, aspect-fit by `scale`.
NSString* const kShaders = @R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V vmain(uint id [[vertex_id]], constant float2& scale [[buffer(0)]]) {
  float2 p = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);
  V o;
  o.pos = float4(p * scale, 0.0, 1.0);
  o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
  return o;
}
fragment float4 fmain(V in [[stage_in]], texture2d<float> y [[texture(0)]], texture2d<float> uv [[texture(1)]]) {
  constexpr sampler s(filter::linear);
  float Y = (y.sample(s, in.uv).r - 16.0 / 255.0) * (255.0 / 219.0);
  float2 C = (uv.sample(s, in.uv).rg - 128.0 / 255.0) * (255.0 / 224.0);
  float3 rgb = float3(Y + 1.5748 * C.y, Y - 0.1873 * C.x - 0.4681 * C.y, Y + 1.8556 * C.x);
  return float4(saturate(rgb), 1.0);
}
)";

constexpr size_t kMaxPending = 4;

// Outlives the display: GPU callbacks may fire after it is destroyed.
struct Sink {
  std::mutex mu;
  IDisplay::PresentedFn fn;
  void report(int64_t pts, int64_t presentedNs) {
    std::lock_guard<std::mutex> lock(mu);
    if (fn) fn(pts, presentedNs);
  }
};

class MetalDisplay : public IDisplay {
 public:
  ~MetalDisplay() override {
    for (id observer : observers_) [[NSNotificationCenter defaultCenter] removeObserver:observer];
    if (link_) {
      CVDisplayLinkStop(link_);  // waits for a running callback
      CVDisplayLinkRelease(link_);
    }
    {
      std::lock_guard<std::mutex> lock(sink_->mu);
      sink_->fn = nullptr;
    }
    if (cache_) CFRelease(cache_);
  }

  Result attach(const RenderTarget& target, PresentedFn fn) override {
    NSView* view = (__bridge NSView*)target.native;
    if (!view || ![view.layer isKindOfClass:[CAMetalLayer class]]) return Result::InvalidArgument;
    layer_ = (CAMetalLayer*)view.layer;
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) return Result::InvalidArgument;
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = YES;
    layer_.maximumDrawableCount = 3;
    layer_.allowsNextDrawableTimeout = YES;
    queue_ = [device_ newCommandQueue];

    NSError* error = nil;
    id<MTLLibrary> library = [device_ newLibraryWithSource:kShaders options:nil error:&error];
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"vmain"];
    desc.fragmentFunction = [library newFunctionWithName:@"fmain"];
    desc.colorAttachments[0].pixelFormat = layer_.pixelFormat;
    pipeline_ = library ? [device_ newRenderPipelineStateWithDescriptor:desc error:&error] : nil;
    if (!pipeline_ || CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache_) != kCVReturnSuccess) {
      NSLog(@"[mf] Metal setup failed: %@", error);
      return Result::InvalidArgument;
    }
    sink_->fn = std::move(fn);

    if (CVDisplayLinkCreateWithActiveCGDisplays(&link_) != kCVReturnSuccess) return Result::InvalidArgument;
    CVDisplayLinkSetOutputCallback(link_, &MetalDisplay::onVsync, this);
    NSWindow* window = view.window;
    windowChanged(window);
    // Track occlusion (hidden frames are dropped by T3) and the screen the window is on.
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main = [NSOperationQueue mainQueue];
    auto changed = ^(NSNotification* note) { windowChanged(note.object); };
    observers_[0] = [center addObserverForName:NSWindowDidChangeOcclusionStateNotification object:window queue:main usingBlock:changed];
    observers_[1] = [center addObserverForName:NSWindowDidChangeScreenNotification object:window queue:main usingBlock:changed];
    CVDisplayLinkStart(link_);
    return Result::Ok;
  }

  void present(const VideoFrame& frame, int64_t hostTimeNs) override {
    std::optional<Pending> dropped;
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_.push_back({frame, hostTimeNs});
      if (pending_.size() > kMaxPending) {
        dropped = std::move(pending_.front());
        pending_.pop_front();
      }
    }
    if (dropped) sink_->report(dropped->frame.ptsUs, 0);
  }

  bool visible() const override { return visible_; }
  int64_t vsyncPeriodNs() const override { return vsyncNs_; }
  int64_t latencyNs() const override { return latencyNs_; }
  int64_t vsyncGridNs() const override { return gridNs_; }

 private:
  struct Pending {
    VideoFrame frame;
    int64_t atNs;
  };

  // Main thread.
  void windowChanged(NSWindow* window) {
    if (!window) return;
    visible_ = (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    NSNumber* screen = window.screen.deviceDescription[@"NSScreenNumber"];
    if (screen) CVDisplayLinkSetCurrentCGDisplay(link_, screen.unsignedIntValue);
  }

  static CVReturn onVsync(CVDisplayLinkRef, const CVTimeStamp* now, const CVTimeStamp* output, CVOptionFlags,
                          CVOptionFlags*, void* self) {
    static_cast<MetalDisplay*>(self)->vsync(now, output);
    return kCVReturnSuccess;
  }

  // Display-link thread: show the latest frame due by this output vsync; older ones were late.
  void vsync(const CVTimeStamp* now, const CVTimeStamp* output) {
    int64_t outputNs = hostNs(output->hostTime);
    int64_t period = output->videoTimeScale ? int64_t(1e9 * double(output->videoRefreshPeriod) / output->videoTimeScale)
                                            : vsyncNs_.load();
    vsyncNs_ = period;
    gridNs_ = outputNs;
    latencyNs_ = outputNs - hostNs(now->hostTime) + period;  // T3 must hand frames over by the callback before

    std::optional<Pending> show;
    std::vector<int64_t> late;
    {
      std::lock_guard<std::mutex> lock(mu_);
      while (!pending_.empty() && pending_.front().atNs <= outputNs + period / 2) {
        if (show) late.push_back(show->frame.ptsUs);
        show = std::move(pending_.front());
        pending_.pop_front();
      }
    }
    for (int64_t pts : late) sink_->report(pts, 0);
    if (show) draw(*show);
  }

  static int64_t hostNs(uint64_t hostTime) {
    static const mach_timebase_info_data_t tb = [] {
      mach_timebase_info_data_t t;
      mach_timebase_info(&t);
      return t;
    }();
    return static_cast<int64_t>(hostTime * tb.numer / tb.denom);
  }

  void draw(const Pending& p) {
    @autoreleasepool {
      auto pixels = static_cast<CVPixelBufferRef>(p.frame.image.get());
      id<CAMetalDrawable> drawable = pixels ? [layer_ nextDrawable] : nil;
      if (!drawable) {
        sink_->report(p.frame.ptsUs, 0);
        return;
      }
      size_t w = CVPixelBufferGetWidthOfPlane(pixels, 0), h = CVPixelBufferGetHeightOfPlane(pixels, 0);
      CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
      CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatR8Unorm, w, h, 0, &yRef);
      CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatRG8Unorm, w / 2, h / 2, 1, &uvRef);
      if (!yRef || !uvRef) {
        if (yRef) CFRelease(yRef);
        if (uvRef) CFRelease(uvRef);
        sink_->report(p.frame.ptsUs, 0);
        return;
      }

      double dw = drawable.texture.width, dh = drawable.texture.height;
      double fit = std::min(dw / double(w), dh / double(h));
      float scale[2] = {float(w * fit / dw), float(h * fit / dh)};

      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = drawable.texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;

      id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
      id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
      [enc setRenderPipelineState:pipeline_];
      [enc setVertexBytes:scale length:sizeof(scale) atIndex:0];
      [enc setFragmentTexture:CVMetalTextureGetTexture(yRef) atIndex:0];
      [enc setFragmentTexture:CVMetalTextureGetTexture(uvRef) atIndex:1];
      [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [enc endEncoding];

      int64_t pts = p.frame.ptsUs;
      std::shared_ptr<Sink> sink = sink_;
      [drawable addPresentedHandler:^(id<MTLDrawable> d) {
        sink->report(pts, d.presentedTime > 0 ? static_cast<int64_t>(d.presentedTime * 1e9) : 0);  // 0: dropped
      }];
      [cmd presentDrawable:drawable];
      VideoFrame keep = p.frame;  // release the pixel buffer only once the GPU is done with it
      [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        CFRelease(yRef);
        CFRelease(uvRef);
        (void)keep;
      }];
      [cmd commit];
    }
  }

  CAMetalLayer* layer_ = nil;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> pipeline_ = nil;
  CVMetalTextureCacheRef cache_ = nullptr;
  CVDisplayLinkRef link_ = nullptr;
  id observers_[2] = {nil, nil};
  std::shared_ptr<Sink> sink_ = std::make_shared<Sink>();

  std::atomic<bool> visible_{true};
  std::atomic<int64_t> vsyncNs_{16666667}, latencyNs_{0}, gridNs_{0};

  std::mutex mu_;
  std::deque<Pending> pending_;
};

}  // namespace

std::unique_ptr<IDisplay> createDisplay() { return std::make_unique<MetalDisplay>(); }

}  // namespace mf::macos
