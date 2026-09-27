// IDisplay on a CAMetalLayer, presented from the display link. Each vsync callback says when
// the frame drawn now will reach the screen; the frame due at that time is drawn and presented
// right away, so frames land on the real vsync grid. nextDrawable can block for up to 1 s, but
// only on the display-link thread, never on T3 (§2.2).
//
// A composed frame is drawn in one pass (§2.4): each layer straight from its decoder surface at
// its slide offset, with the filter applied in the shader, then the caption on top.

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mf/macos.h"

// CVDisplayLink is deprecated in Mac OS 15; its replacement (CADisplayLink) needs Mac OS 14 (A13).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace mf::macos {
namespace {

// A quad covering `scale` (half-size) around `offset`, in normalized device coordinates.
// Video: NV12 (BT.709, video range) to RGB, then brightness and contrast. Text: premultiplied BGRA.
NSString* const kShaders = @R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
struct Quad { float2 scale; float2 offset; };
struct Filter { float brightness; float contrast; };
vertex V vmain(uint id [[vertex_id]], constant Quad& q [[buffer(0)]]) {
  float2 p = float2((id & 1) ? 1.0 : -1.0, (id & 2) ? -1.0 : 1.0);
  V o;
  o.pos = float4(p * q.scale + q.offset, 0.0, 1.0);
  o.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
  return o;
}
fragment float4 fvideo(V in [[stage_in]], texture2d<float> y [[texture(0)]], texture2d<float> uv [[texture(1)]],
                       constant Filter& f [[buffer(0)]]) {
  constexpr sampler s(filter::linear);
  float Y = (y.sample(s, in.uv).r - 16.0 / 255.0) * (255.0 / 219.0);
  float2 C = (uv.sample(s, in.uv).rg - 128.0 / 255.0) * (255.0 / 224.0);
  float3 rgb = float3(Y + 1.5748 * C.y, Y - 0.1873 * C.x - 0.4681 * C.y, Y + 1.8556 * C.x);
  rgb = (saturate(rgb) - 0.5) * f.contrast + 0.5 + f.brightness;
  return float4(saturate(rgb), 1.0);
}
fragment float4 ftext(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {
  constexpr sampler s(filter::linear);
  return t.sample(s, in.uv);
}
)";

struct Quad {
  float scale[2];
  float offset[2];
};

// A caption rendered once into a texture, reused while the text and size stay the same.
struct CaptionTexture {
  std::string text;
  int fontPx = 0;
  id<MTLTexture> texture = nil;
};

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
    desc.fragmentFunction = [library newFunctionWithName:@"fvideo"];
    desc.colorAttachments[0].pixelFormat = layer_.pixelFormat;
    videoPipeline_ = library ? [device_ newRenderPipelineStateWithDescriptor:desc error:&error] : nil;
    desc.fragmentFunction = [library newFunctionWithName:@"ftext"];
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    textPipeline_ = library ? [device_ newRenderPipelineStateWithDescriptor:desc error:&error] : nil;
    if (!videoPipeline_ || !textPipeline_ ||
        CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache_) != kCVReturnSuccess) {
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

  void present(const ComposedFrame& frame, int64_t hostTimeNs) override {
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
    ComposedFrame frame;
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
      const ComposedFrame& c = p.frame;
      bool any = false;
      for (int i = 0; i < c.layerCount; ++i) any |= c.layers[i].frame.image != nullptr;
      id<CAMetalDrawable> drawable = any ? [layer_ nextDrawable] : nil;
      if (!drawable) {
        sink_->report(c.ptsUs, 0);
        return;
      }
      double dw = drawable.texture.width, dh = drawable.texture.height;

      MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = drawable.texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;

      id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
      id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
      [enc setRenderPipelineState:videoPipeline_];
      float filter[2] = {c.filter.brightness, c.filter.contrast};
      [enc setFragmentBytes:filter length:sizeof(filter) atIndex:0];
      auto textures = std::make_shared<std::vector<CVMetalTextureRef>>();
      double videoHeight = 0;  // of the leading (last) layer, in pixels: the caption sits on it
      Quad lead{{1, 1}, {0, 0}};
      for (int i = 0; i < c.layerCount; ++i) {
        auto pixels = static_cast<CVPixelBufferRef>(c.layers[i].frame.image.get());
        if (!pixels) continue;
        size_t w = CVPixelBufferGetWidthOfPlane(pixels, 0), h = CVPixelBufferGetHeightOfPlane(pixels, 0);
        CVMetalTextureRef yRef = nullptr, uvRef = nullptr;
        CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatR8Unorm, w, h, 0, &yRef);
        CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatRG8Unorm, w / 2, h / 2, 1, &uvRef);
        if (yRef) textures->push_back(yRef);
        if (uvRef) textures->push_back(uvRef);
        if (!yRef || !uvRef) continue;

        double fit = std::min(dw / double(w), dh / double(h));
        Quad q{{float(w * fit / dw), float(h * fit / dh)}, {c.layers[i].offsetX * 2.0f, 0}};  // NDC spans 2 widths
        lead = q;
        videoHeight = h * fit;
        [enc setVertexBytes:&q length:sizeof(q) atIndex:0];
        [enc setFragmentTexture:CVMetalTextureGetTexture(yRef) atIndex:0];
        [enc setFragmentTexture:CVMetalTextureGetTexture(uvRef) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      }
      if (c.text && videoHeight > 0) drawCaption(enc, *c.text, lead, videoHeight, dw, dh);
      [enc endEncoding];

      int64_t pts = c.ptsUs;
      std::shared_ptr<Sink> sink = sink_;
      [drawable addPresentedHandler:^(id<MTLDrawable> d) {
        sink->report(pts, d.presentedTime > 0 ? static_cast<int64_t>(d.presentedTime * 1e9) : 0);  // 0: dropped
      }];
      [cmd presentDrawable:drawable];
      ComposedFrame keep = c;  // release the pixel buffers only once the GPU is done with them
      [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        for (CVMetalTextureRef t : *textures) CFRelease(t);
        (void)keep;
      }];
      [cmd commit];
    }
  }

  // Centered at the bottom of the leading layer's picture, not sliding with it: 5% of the
  // picture's height, narrowed to fit 90% of the view.
  void drawCaption(id<MTLRenderCommandEncoder> enc, const std::string& text, const Quad& lead, double videoHeight,
                   double dw, double dh) {
    int fontPx = std::max(12, static_cast<int>(std::lround(videoHeight * 0.05)));
    id<MTLTexture> texture = captionTexture(text, fontPx, dw * 0.9);
    if (!texture) return;
    double margin = videoHeight * 0.04;
    float halfH = float(texture.height / dh);
    Quad q{{float(texture.width / dw), halfH}, {0, -lead.scale[1] + float(2 * margin / dh) + halfH}};
    [enc setRenderPipelineState:textPipeline_];
    [enc setVertexBytes:&q length:sizeof(q) atIndex:0];
    [enc setFragmentTexture:texture atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }

  // White text on a translucent dark box, rasterized with Core Text (thread-safe) on the
  // display-link thread when the caption or its size changes.
  id<MTLTexture> captionTexture(const std::string& text, int fontPx, double maxWidth) {
    if (caption_.texture && caption_.text == text && caption_.fontPx == fontPx) return caption_.texture;
    caption_ = CaptionTexture{text, fontPx, nil};
    NSString* string = [NSString stringWithUTF8String:text.c_str()];
    if (string.length == 0) return nil;

    CGColorRef white = CGColorCreateSRGB(1, 1, 1, 1);
    auto makeLine = [&](CGFloat px) {
      CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, px, nullptr);
      NSDictionary* attrs = @{(id)kCTFontAttributeName : (__bridge id)font, (id)kCTForegroundColorAttributeName : (__bridge id)white};
      NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:string attributes:attrs];
      CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
      CFRelease(font);
      return line;
    };
    CGFloat px = fontPx;
    CTLineRef line = makeLine(px);
    CGFloat padX = px * 0.5, padY = px * 0.3;
    double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    if (width + 2 * padX > maxWidth) {  // too wide: shrink once to fit
      px = std::max<CGFloat>(8, px * (maxWidth - 2 * padX) / width);
      CFRelease(line);
      line = makeLine(px);
      padX = px * 0.5;
      padY = px * 0.3;
    }
    CGFloat ascent = 0, descent = 0, leading = 0;
    width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    size_t w = static_cast<size_t>(std::ceil(width + 2 * padX)), h = static_cast<size_t>(std::ceil(ascent + descent + 2 * padY));

    std::vector<uint8_t> pixels(w * h * 4, 0);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, space,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(space);
    if (ctx) {
      CGContextSetRGBFillColor(ctx, 0, 0, 0, 0.55);
      CGPathRef box = CGPathCreateWithRoundedRect(CGRectMake(0, 0, w, h), padY, padY, nullptr);
      CGContextAddPath(ctx, box);
      CGContextFillPath(ctx);
      CGPathRelease(box);
      CGContextSetTextPosition(ctx, padX, padY + descent);
      CTLineDraw(line, ctx);
      CGContextRelease(ctx);
    }
    CFRelease(line);
    CGColorRelease(white);
    if (!ctx) return nil;

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                    width:w
                                                                                   height:h
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device_ newTextureWithDescriptor:desc];
    [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:w * 4];
    caption_.texture = texture;
    return texture;
  }

  CAMetalLayer* layer_ = nil;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLRenderPipelineState> videoPipeline_ = nil, textPipeline_ = nil;
  CaptionTexture caption_;  // display-link thread only
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
