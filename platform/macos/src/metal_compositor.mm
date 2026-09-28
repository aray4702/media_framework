#import "metal_compositor.h"

#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace mf::macos {
namespace {

// Vertices come as the four corners (TL, TR, BL, BR) in normalized device coordinates with the
// texture rect. Fragment shaders output premultiplied alpha. `shade` applies, in order: chroma
// key, color adjust (brightness, contrast, saturation), the global filter, then opacity.
NSString* const kShaders = @R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
struct Vert { float2 pos[4]; float4 uv; };
vertex V vmain(uint id [[vertex_id]], constant Vert& v [[buffer(0)]]) {
  V o;
  o.pos = float4(v.pos[id], 0.0, 1.0);
  float2 c = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
  o.uv = mix(v.uv.xy, v.uv.zw, c);
  return o;
}
struct Fx {
  float4 color;
  float brightness, contrast, saturation, opacity, gBrightness, gContrast;
  float keyOn, tolerance, softness, keyCb, keyCr, pad;
};
constant float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float4 shade(float3 rgb, float a, constant Fx& f) {
  if (f.keyOn > 0.5) {
    float Y = dot(rgb, kLuma);
    float2 cc = float2((rgb.b - Y) / 1.8556, (rgb.r - Y) / 1.5748);
    a *= smoothstep(f.tolerance, f.tolerance + max(f.softness, 0.0001), distance(cc, float2(f.keyCb, f.keyCr)));
  }
  float3 c = (rgb - 0.5) * f.contrast + 0.5 + f.brightness;
  float L = dot(c, kLuma);
  c = L + (c - L) * f.saturation;
  c = (saturate(c) - 0.5) * f.gContrast + 0.5 + f.gBrightness;
  a *= f.opacity;
  return float4(saturate(c) * a, a);
}
fragment float4 fnv12(V in [[stage_in]], texture2d<float> y [[texture(0)]], texture2d<float> uv [[texture(1)]],
                      constant Fx& f [[buffer(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float Y = (y.sample(s, in.uv).r - 16.0 / 255.0) * (255.0 / 219.0);
  float2 C = (uv.sample(s, in.uv).rg - 128.0 / 255.0) * (255.0 / 224.0);
  float3 rgb = float3(Y + 1.5748 * C.y, Y - 0.1873 * C.x - 0.4681 * C.y, Y + 1.8556 * C.x);
  return shade(saturate(rgb), 1.0, f);
}
fragment float4 frgba(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant Fx& f [[buffer(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 p = t.sample(s, in.uv);
  return shade(p.a > 0.0 ? p.rgb / p.a : float3(0.0), p.a, f);
}
fragment float4 fcolor(V in [[stage_in]], constant Fx& f [[buffer(0)]]) { return shade(f.color.rgb, f.color.a, f); }
struct Blur { float2 step; float sigma; int radius; };
fragment float4 fblur(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant Blur& b [[buffer(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 sum = 0.0;
  float total = 0.0;
  for (int i = -b.radius; i <= b.radius; ++i) {
    float x = float(i), w = exp(-x * x / (2.0 * b.sigma * b.sigma));
    sum += w * t.sample(s, in.uv + b.step * x);
    total += w;
  }
  return sum / total;
}
)";

struct Vert {
  float pos[8];  // TL, TR, BL, BR
  float uv[4];   // u0, v0, u1, v1
};
struct Fx {
  float color[4];
  float brightness, contrast, saturation, opacity, gBrightness, gContrast;
  float keyOn, tolerance, softness, keyCb, keyCr, pad;
};
struct BlurParams {
  float step[2];
  float sigma;
  int radius;
};
constexpr MTLPixelFormat kOffscreenFormat = MTLPixelFormatRGBA16Float;
constexpr size_t kMaxTexts = 16;
const Vert kFullQuad = {{-1, 1, 1, 1, -1, -1, 1, -1}, {0, 0, 1, 1}};

// The layer's own effects as shader parameters: chroma key and color adjust.
Fx effectsOf(const ComposedLayer& l, float opacity) {
  Fx fx{{l.color.r, l.color.g, l.color.b, l.color.a}, l.brightness, l.contrast, l.saturation, opacity, 0, 1, 0, 0, 0, 0, 0, 0};
  if (l.chromaKey) {
    float Y = 0.2126f * l.keyColor.r + 0.7152f * l.keyColor.g + 0.0722f * l.keyColor.b;
    fx.keyOn = 1;
    fx.tolerance = l.keyTolerance;
    fx.softness = l.keySoftness;
    fx.keyCb = (l.keyColor.b - Y) / 1.8556f;
    fx.keyCr = (l.keyColor.r - Y) / 1.5748f;
  }
  return fx;
}

}  // namespace

struct MetalCompositor::Prepared {
  Source source = kColor;
  id<MTLTexture> tex0 = nil, tex1 = nil;
  Vert vert = kFullQuad;
  double boxW = 0, boxH = 0;  // the fitted box, in canvas pixels
  bool effectsDone = false;   // drawn through a blur texture: only opacity and the filter are left
};

MetalCompositor::~MetalCompositor() {
  if (cache_) CFRelease(cache_);
}

bool MetalCompositor::init(id<MTLDevice> device, MTLPixelFormat format) {
  device_ = device;
  format_ = format;
  NSError* error = nil;
  id<MTLLibrary> library = [device_ newLibraryWithSource:kShaders options:nil error:&error];
  if (!library) {
    NSLog(@"[mf] Metal shaders failed: %@", error);
    return false;
  }
  id<MTLFunction> vertex = [library newFunctionWithName:@"vmain"];
  NSString* fragments[kSources] = {@"fnv12", @"frgba", @"fcolor"};
  auto make = [&](NSString* fragment, MTLPixelFormat pf, int blend) -> id<MTLRenderPipelineState> {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vertex;
    d.fragmentFunction = [library newFunctionWithName:fragment];
    MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[0];
    c.pixelFormat = pf;
    if (blend >= 0) {  // premultiplied source over an opaque canvas
      c.blendingEnabled = YES;
      c.sourceAlphaBlendFactor = MTLBlendFactorOne;
      c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      switch (static_cast<Blend>(blend)) {
        case Blend::Normal:  // S + D·(1 − αS)
          c.sourceRGBBlendFactor = MTLBlendFactorOne;
          c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
          break;
        case Blend::Add:  // S + D
          c.sourceRGBBlendFactor = MTLBlendFactorOne;
          c.destinationRGBBlendFactor = MTLBlendFactorOne;
          break;
        case Blend::Multiply:  // S·D + D·(1 − αS), the destination being opaque
          c.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
          c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
          break;
        case Blend::Screen:  // S + D − S·D
          c.sourceRGBBlendFactor = MTLBlendFactorOne;
          c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceColor;
          break;
      }
    }
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state) NSLog(@"[mf] Metal pipeline failed: %@", error);
    return state;
  };
  for (int s = 0; s < kSources; ++s) {
    for (int b = 0; b < 4; ++b) {
      if (!(pipelines_[s][b] = make(fragments[s], format, b))) return false;
    }
    if (!(offscreen_[s] = make(fragments[s], kOffscreenFormat, -1))) return false;
  }
  if (!(blur_ = make(@"fblur", kOffscreenFormat, -1))) return false;
  return CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache_) == kCVReturnSuccess;
}

// Resolves the layer's textures, then its geometry (§4.1): crop the natural size, fit it to the
// canvas, then scale and rotate about the anchor placed at (x, y) plus the transition offset.
bool MetalCompositor::prepare(const ComposedLayer& l, double W, double H, double scale, Prepared* p,
                              std::vector<CVMetalTextureRef>* keep) {
  double nw = W, nh = H;  // natural size, in canvas pixels
  switch (l.kind) {
    case ComposedLayer::Kind::Video:
    case ComposedLayer::Kind::Image: {
      auto pixels = static_cast<CVPixelBufferRef>(l.frame.image.get());
      if (!pixels) return false;
      bool bgra = CVPixelBufferGetPixelFormatType(pixels) == kCVPixelFormatType_32BGRA;
      size_t w = bgra ? CVPixelBufferGetWidth(pixels) : CVPixelBufferGetWidthOfPlane(pixels, 0);
      size_t h = bgra ? CVPixelBufferGetHeight(pixels) : CVPixelBufferGetHeightOfPlane(pixels, 0);
      CVMetalTextureRef a = nullptr, b = nullptr;
      if (bgra) {
        CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatBGRA8Unorm, w, h, 0, &a);
      } else {
        CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatR8Unorm, w, h, 0, &a);
        CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatRG8Unorm, w / 2, h / 2, 1, &b);
      }
      if (a) keep->push_back(a);
      if (b) keep->push_back(b);
      if (!a || (!bgra && !b)) return false;
      p->source = bgra ? kRgba : kNv12;
      p->tex0 = CVMetalTextureGetTexture(a);
      p->tex1 = b ? CVMetalTextureGetTexture(b) : nil;
      nw = double(w);
      nh = double(h);
      break;
    }
    case ComposedLayer::Kind::Text: {
      if (!l.text) return false;
      int lineHeight = std::max(4, int(std::lround(l.style.size * H * scale)));
      int maxWidth = std::max(lineHeight, int(std::lround(l.style.maxWidth * W * scale)));
      p->tex0 = textTexture(*l.text, l.style, maxWidth, lineHeight);
      if (!p->tex0) return false;
      p->source = kRgba;
      nw = p->tex0.width / scale;  // rasterized at the target's resolution
      nh = p->tex0.height / scale;
      break;
    }
    case ComposedLayer::Kind::Color:
      p->source = kColor;
      break;
  }

  float u0 = l.crop[0], v0 = l.crop[1], u1 = 1 - l.crop[2], v1 = 1 - l.crop[3];
  if (u1 <= u0 || v1 <= v0) return false;
  double cw = nw * (u1 - u0), ch = nh * (v1 - v0);
  double bw = cw, bh = ch;
  if (l.fit == Fit::Contain || l.fit == Fit::Cover) {
    double k = l.fit == Fit::Contain ? std::min(W / cw, H / ch) : std::max(W / cw, H / ch);
    bw = cw * k;
    bh = ch * k;
  } else if (l.fit == Fit::Fill) {
    bw = W;
    bh = H;
  }
  p->boxW = bw * l.scale * scale;  // target pixels, for a blur texture
  p->boxH = bh * l.scale * scale;
  double px = (l.x + l.offsetX) * W, py = (l.y + l.offsetY) * H;
  double a = l.rotation * M_PI / 180, cs = std::cos(a), sn = std::sin(a);
  for (int k = 0; k < 4; ++k) {
    double u = k & 1 ? 1 : 0, v = k & 2 ? 1 : 0;
    double dx = (u - l.anchorX) * bw * l.scale, dy = (v - l.anchorY) * bh * l.scale;
    double x = px + dx * cs - dy * sn, y = py + dx * sn + dy * cs;  // clockwise on screen (y down)
    p->vert.pos[2 * k] = float(2 * x / W - 1);
    p->vert.pos[2 * k + 1] = float(1 - 2 * y / H);
  }
  p->vert.uv[0] = u0;
  p->vert.uv[1] = v0;
  p->vert.uv[2] = u1;
  p->vert.uv[3] = v1;
  return true;
}

// Draws the layer's source with its effects into a texture of its size on screen, then blurs
// it in two passes (horizontal, vertical). The layer then draws that texture.
void MetalCompositor::blurInto(Prepared* p, const ComposedLayer& l, id<MTLCommandBuffer> cmd, double sigmaPx) {
  NSUInteger w = NSUInteger(std::clamp(std::lround(p->boxW), 1L, 4096L)), h = NSUInteger(std::clamp(std::lround(p->boxH), 1L, 4096L));
  MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kOffscreenFormat width:w height:h mipmapped:NO];
  d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  d.storageMode = MTLStorageModePrivate;
  id<MTLTexture> a = [device_ newTextureWithDescriptor:d], b = [device_ newTextureWithDescriptor:d];
  auto pass = [&](id<MTLTexture> target) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    return [cmd renderCommandEncoderWithDescriptor:rp];
  };
  // The source with its own effects (not yet opacity or the global filter), filling the texture.
  id<MTLRenderCommandEncoder> enc = pass(a);
  Vert v = kFullQuad;
  std::copy(p->vert.uv, p->vert.uv + 4, v.uv);
  Fx fx = effectsOf(l, 1);
  [enc setRenderPipelineState:offscreen_[p->source]];
  [enc setVertexBytes:&v length:sizeof(v) atIndex:0];
  [enc setFragmentBytes:&fx length:sizeof(fx) atIndex:0];
  if (p->tex0) [enc setFragmentTexture:p->tex0 atIndex:0];
  if (p->tex1) [enc setFragmentTexture:p->tex1 atIndex:1];
  [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  [enc endEncoding];

  // Wide blurs sample every few pixels, so a pass takes at most 64 taps each side.
  double reach = 3 * sigmaPx, stride = std::max(1.0, reach / 64);
  BlurParams bp{{0, 0}, float(std::max(0.5, sigmaPx / stride)), std::max(1, int(std::ceil(reach / stride)))};
  for (int axis = 0; axis < 2; ++axis) {
    bp.step[0] = axis == 0 ? float(stride / w) : 0;
    bp.step[1] = axis == 1 ? float(stride / h) : 0;
    enc = pass(axis == 0 ? b : a);
    [enc setRenderPipelineState:blur_];
    [enc setVertexBytes:&kFullQuad length:sizeof(kFullQuad) atIndex:0];
    [enc setFragmentBytes:&bp length:sizeof(bp) atIndex:0];
    [enc setFragmentTexture:axis == 0 ? a : b atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
  }
  p->source = kRgba;
  p->tex0 = a;
  p->tex1 = nil;
  p->vert.uv[0] = p->vert.uv[1] = 0;
  p->vert.uv[2] = p->vert.uv[3] = 1;
  p->effectsDone = true;
}

void MetalCompositor::draw(id<MTLRenderCommandEncoder> enc, const Prepared& p, const ComposedLayer& l, const VideoFilter& filter,
                           bool effects) {
  Fx fx = effectsOf(l, l.opacity);
  if (!effects) {
    fx.brightness = 0;
    fx.contrast = fx.saturation = 1;
    fx.keyOn = 0;
  }
  if (l.kind == ComposedLayer::Kind::Video || l.kind == ComposedLayer::Kind::Image) {  // the global filter
    fx.gBrightness = filter.brightness;
    fx.gContrast = filter.contrast;
  }
  [enc setRenderPipelineState:pipelines_[p.source][int(l.blend)]];
  [enc setVertexBytes:&p.vert length:sizeof(p.vert) atIndex:0];
  [enc setFragmentBytes:&fx length:sizeof(fx) atIndex:0];
  if (p.tex0) [enc setFragmentTexture:p.tex0 atIndex:0];
  if (p.tex1) [enc setFragmentTexture:p.tex1 atIndex:1];
  [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void MetalCompositor::encode(const ComposedFrame& c, id<MTLTexture> target, id<MTLCommandBuffer> cmd) {
  double tw = target.width, th = target.height;
  double W = c.width > 0 ? c.width : tw, H = c.height > 0 ? c.height : th;
  double scale = std::min(tw / W, th / H);  // letterbox the canvas into the target
  MTLViewport viewport{(tw - W * scale) / 2, (th - H * scale) / 2, W * scale, H * scale, 0, 1};

  auto textures = std::make_shared<std::vector<CVMetalTextureRef>>();
  std::vector<Prepared> prepared(c.layers.size());
  std::vector<bool> ok(c.layers.size());
  for (size_t i = 0; i < c.layers.size(); ++i) {  // blurred layers render first, into their own textures
    const ComposedLayer& l = c.layers[i];
    ok[i] = l.opacity > 0 && prepare(l, W, H, scale, &prepared[i], textures.get());
    if (ok[i] && l.blur > 0) blurInto(&prepared[i], l, cmd, l.blur * H * scale);
  }

  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);  // letterbox bars
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
  [enc setViewport:viewport];

  ComposedLayer background;
  background.kind = ComposedLayer::Kind::Color;
  background.color = c.background;
  draw(enc, Prepared{}, background, c.filter, false);

  MTLScissorRect full{0, 0, NSUInteger(tw), NSUInteger(th)};
  for (size_t i = 0; i < c.layers.size(); ++i) {
    if (!ok[i]) continue;
    const ComposedLayer& l = c.layers[i];
    bool clipped = l.clip[0] > 0 || l.clip[1] > 0 || l.clip[2] < 1 || l.clip[3] < 1;
    if (clipped) {  // wipe: only this part of the canvas
      double x0 = viewport.originX + l.clip[0] * viewport.width, x1 = viewport.originX + l.clip[2] * viewport.width;
      double y0 = viewport.originY + l.clip[1] * viewport.height, y1 = viewport.originY + l.clip[3] * viewport.height;
      NSUInteger sx = NSUInteger(std::clamp(std::floor(x0), 0.0, tw)), sy = NSUInteger(std::clamp(std::floor(y0), 0.0, th));
      NSUInteger ex = NSUInteger(std::clamp(std::ceil(x1), 0.0, tw)), ey = NSUInteger(std::clamp(std::ceil(y1), 0.0, th));
      if (ex <= sx || ey <= sy) continue;
      [enc setScissorRect:{sx, sy, ex - sx, ey - sy}];
    }
    draw(enc, prepared[i], l, c.filter, !prepared[i].effectsDone);
    if (clipped) [enc setScissorRect:full];
  }
  [enc endEncoding];

  ComposedFrame keep = c;  // release the surfaces only once the GPU is done with them
  [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
    for (CVMetalTextureRef t : *textures) CFRelease(t);
    (void)keep;
  }];
}

// Text (§4.5) with Core Text (thread-safe): wrapped at maxWidthPx, aligned, in the style's
// color, on its box when it has one. Rasterized at the target's resolution.
id<MTLTexture> MetalCompositor::textTexture(const std::string& text, const TextStyle& style, int maxWidthPx, int lineHeightPx) {
  for (auto it = texts_.begin(); it != texts_.end(); ++it) {
    if (it->text == text && it->style == style && it->widthPx == maxWidthPx && it->lineHeightPx == lineHeightPx) {
      texts_.splice(texts_.begin(), texts_, it);
      return it->texture;
    }
  }
  NSString* string = [NSString stringWithUTF8String:text.c_str()];
  if (string.length == 0) return nil;

  CGFloat points = lineHeightPx / 1.2;  // a line is about 1.2 × the font size
  CTFontRef font;
  if (style.font == "system" || style.font == "system-bold") {
    font = CTFontCreateUIFontForLanguage(style.font == "system" ? kCTFontUIFontSystem : kCTFontUIFontEmphasizedSystem, points, nullptr);
  } else {
    CFStringRef name = CFStringCreateWithCString(nullptr, style.font.c_str(), kCFStringEncodingUTF8);
    font = CTFontCreateWithName(name, points, nullptr);
    CFRelease(name);
  }
  CGColorRef color = CGColorCreateSRGB(style.color.r, style.color.g, style.color.b, style.color.a);
  CTTextAlignment align = style.align == TextAlign::Left    ? kCTTextAlignmentLeft
                          : style.align == TextAlign::Right ? kCTTextAlignmentRight
                                                            : kCTTextAlignmentCenter;
  CTParagraphStyleSetting setting{kCTParagraphStyleSpecifierAlignment, sizeof(align), &align};
  CTParagraphStyleRef paragraph = CTParagraphStyleCreate(&setting, 1);
  NSDictionary* attrs = @{(id)kCTFontAttributeName : (__bridge id)font, (id)kCTForegroundColorAttributeName : (__bridge id)color,
                          (id)kCTParagraphStyleAttributeName : (__bridge id)paragraph};
  NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:string attributes:attrs];
  CTFramesetterRef setter = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
  CFRelease(font);
  CFRelease(paragraph);
  CGColorRelease(color);

  CGFloat pad = style.hasBox ? lineHeightPx * 0.3 : 2;
  CGSize fit = CTFramesetterSuggestFrameSizeWithConstraints(setter, CFRangeMake(0, 0), nullptr,
                                                            CGSizeMake(std::max<CGFloat>(1, maxWidthPx - 2 * pad), CGFLOAT_MAX), nullptr);
  size_t w = size_t(std::ceil(fit.width + 2 * pad)), h = size_t(std::ceil(fit.height + 2 * pad));
  std::vector<uint8_t> pixels(w * h * 4, 0);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGColorSpaceRelease(space);
  if (ctx) {
    if (style.hasBox) {
      CGContextSetRGBFillColor(ctx, style.box.r, style.box.g, style.box.b, style.box.a);
      CGPathRef box = CGPathCreateWithRoundedRect(CGRectMake(0, 0, w, h), pad, pad, nullptr);
      CGContextAddPath(ctx, box);
      CGContextFillPath(ctx);
      CGPathRelease(box);
    }
    CGPathRef path = CGPathCreateWithRect(CGRectMake(pad, pad, std::ceil(fit.width), std::ceil(fit.height)), nullptr);
    CTFrameRef frame = CTFramesetterCreateFrame(setter, CFRangeMake(0, 0), path, nullptr);
    CTFrameDraw(frame, ctx);
    CFRelease(frame);
    CGPathRelease(path);
    CGContextRelease(ctx);
  }
  CFRelease(setter);
  if (!ctx) return nil;

  MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
  d.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device_ newTextureWithDescriptor:d];
  [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:w * 4];
  texts_.push_front({text, style, maxWidthPx, lineHeightPx, texture});
  if (texts_.size() > kMaxTexts) texts_.pop_back();
  return texture;
}

}  // namespace mf::macos
