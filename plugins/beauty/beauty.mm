// The "beauty" effect plugin (mf/effect_plugin.h): smooths skin, brightens it, and sharpens the
// rest, like a beauty camera. Two passes over the item's image:
//   1. an edge-preserving (bilateral) blur: fine texture such as pores averages out, while edges
//      (eyes, lips, hair) weigh little because their colors differ. It's one 2D pass on a sparse
//      9 × 9 grid: a separable one (horizontal, then vertical) leaves streaks, because its first
//      pass sees the noisy image and rejects more neighbors than the second;
//   2. the mix: where the skin mask says skin, the smoothed image by `smooth`, then a log curve
//      that lifts it by `whiten`; elsewhere, an unsharp mask by `sharpen`.
// The skin mask is a feathered box in the CbCr plane, so it needs no face detection. It can take
// in warm backgrounds (wood, sand), where smoothing shows little anyway.
//
// Parameters (0 to 1): smooth (0.5), whiten (0.2), sharpen (0.2).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "mf/effect_plugin.h"

namespace {

// Vertices: a quad covering the target, corners TL, TR, BL, BR. Colors are premultiplied.
NSString* const kShaders = @R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V vquad(uint id [[vertex_id]]) {
  float2 c = float2(id & 1, id >> 1);
  V o;
  o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
  o.uv = c;
  return o;
}
constant float3 kLuma = float3(0.2126, 0.7152, 0.0722);
float3 straight(float4 p) { return p.a > 0.0 ? p.rgb / p.a : float3(0.0); }

// A bilateral filter on a (2·radius + 1)² grid of taps `step` apart (in uv), weighted by their
// distance (sigma, in taps) and by how far their color is from the center's (sigmaColor).
struct Bilateral { float2 step; float sigma; float sigmaColor; int radius; int pad; };
fragment float4 fbilateral(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant Bilateral& b [[buffer(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float3 c0 = straight(t.sample(s, in.uv));
  float4 sum = 0.0;
  float total = 0.0;
  for (int j = -b.radius; j <= b.radius; ++j) {
    for (int i = -b.radius; i <= b.radius; ++i) {
      float2 o = float2(i, j);
      float4 p = t.sample(s, in.uv + b.step * o);
      float3 d = straight(p) - c0;
      float w = exp(-dot(o, o) / (2.0 * b.sigma * b.sigma) - dot(d, d) / (2.0 * b.sigmaColor * b.sigmaColor));
      sum += w * p;
      total += w;
    }
  }
  return sum / total;
}

// How much a color looks like skin: Cb in about [0.30, 0.52], Cr in about [0.53, 0.70], feathered.
float skin(float3 rgb) {
  float Y = dot(rgb, kLuma);
  float cb = (rgb.b - Y) / 1.8556 + 0.5, cr = (rgb.r - Y) / 1.5748 + 0.5;
  return smoothstep(0.28, 0.32, cb) * (1.0 - smoothstep(0.50, 0.54, cb)) *
         smoothstep(0.51, 0.55, cr) * (1.0 - smoothstep(0.68, 0.72, cr));
}

struct Beauty { float smooth, whiten, sharpen, pad; };
fragment float4 fbeauty(V in [[stage_in]], texture2d<float> src [[texture(0)]], texture2d<float> smoothed [[texture(1)]],
                        constant Beauty& b [[buffer(0)]]) {
  constexpr sampler s(filter::linear, address::clamp_to_edge);
  float4 o = src.sample(s, in.uv);
  if (o.a <= 0.0) return o;
  float3 c = o.rgb / o.a, blurred = straight(smoothed.sample(s, in.uv));
  float k = skin(blurred);  // from the smoothed image: steadier than per pixel
  float3 r = mix(c, blurred, b.smooth * k);
  if (b.whiten > 0.0) {
    float lift = 1.0 + 4.0 * b.whiten;
    r = mix(r, log(r * (lift - 1.0) + 1.0) / log(lift), k);
  }
  r += b.sharpen * (1.0 - k) * (c - blurred);
  return float4(saturate(r) * o.a, o.a);
}
)";

struct BilateralParams {
  float step[2];
  float sigma, sigmaColor;
  int radius, pad;
};
struct BeautyParams {
  float smooth, whiten, sharpen, pad;
};

const MfEffectParam kParams[] = {{"smooth", 0.5f, 0, 1}, {"whiten", 0.2f, 0, 1}, {"sharpen", 0.2f, 0, 1}};

class Beauty {
 public:
  bool init(id<MTLDevice> device) {
    device_ = device;
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:kShaders options:nil error:&error];
    if (!library) {
      NSLog(@"[beauty] shaders failed: %@", error);
      return false;
    }
    bilateral_ = pipeline(library, @"fbilateral");
    combine_ = pipeline(library, @"fbeauty");
    return bilateral_ && combine_;
  }

  int encode(id<MTLCommandBuffer> cmd, id<MTLTexture> in, id<MTLTexture> out, const float* p, const MfEffectContext* context) {
    float smooth = std::clamp(p[0], 0.0f, 1.0f), whiten = std::clamp(p[1], 0.0f, 1.0f), sharpen = std::clamp(p[2], 0.0f, 1.0f);
    bool knowsScale = context && context->size >= offsetof(MfEffectContext, pixelsPerUnit) + sizeof(float);
    float pixelsPerUnit = knowsScale && context->pixelsPerUnit > 0 ? context->pixelsPerUnit : 1080;
    NSUInteger w = in.width, h = in.height;
    id<MTLTexture> smoothed = in;
    if (smooth > 0 || whiten > 0 || sharpen > 0) {
      texture(smoothed_, w, h);
      // Wider and more forgiving of color differences as `smooth` grows: about 2 to 6 px at 1080p.
      double sigmaPx = std::max(0.5, (0.002 + 0.004 * smooth) * pixelsPerUnit);
      double reach = 2.5 * sigmaPx, stride = std::max(1.0, reach / 4);  // at most 4 taps each side
      BilateralParams bp{{float(stride / w), float(stride / h)}, float(std::max(0.5, sigmaPx / stride)), 0.06f + 0.10f * smooth,
                         std::clamp(int(std::ceil(reach / stride)), 1, 4), 0};
      id<MTLRenderCommandEncoder> enc = pass(cmd, smoothed_);
      [enc setRenderPipelineState:bilateral_];
      [enc setFragmentBytes:&bp length:sizeof(bp) atIndex:0];
      [enc setFragmentTexture:in atIndex:0];
      [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      [enc endEncoding];
      smoothed = smoothed_;
    }
    BeautyParams params{smooth, whiten, sharpen, 0};
    id<MTLRenderCommandEncoder> enc = pass(cmd, out);
    [enc setRenderPipelineState:combine_];
    [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [enc setFragmentTexture:in atIndex:0];
    [enc setFragmentTexture:smoothed atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];
    return 0;
  }

 private:
  id<MTLRenderPipelineState> pipeline(id<MTLLibrary> library, NSString* fragment) {
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"vquad"];
    d.fragmentFunction = [library newFunctionWithName:fragment];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state) NSLog(@"[beauty] pipeline %@ failed: %@", fragment, error);
    return state;
  }

  // Kept while the size stays the same. Frames (and layers) are encoded in order on one queue,
  // and Metal orders the passes that write and read a texture.
  void texture(__strong id<MTLTexture>& t, NSUInteger w, NSUInteger h) {
    if (t && t.width == w && t.height == h) return;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    t = [device_ newTextureWithDescriptor:d];
  }

  static id<MTLRenderCommandEncoder> pass(id<MTLCommandBuffer> cmd, id<MTLTexture> target) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;  // every pixel is drawn
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    return [cmd renderCommandEncoderWithDescriptor:rp];
  }

  id<MTLDevice> device_ = nil;
  id<MTLRenderPipelineState> bilateral_ = nil, combine_ = nil;
  id<MTLTexture> smoothed_ = nil;
};

void* create(void* device) {
  auto* b = new Beauty;
  if (!b->init((__bridge id<MTLDevice>)device)) {
    delete b;
    return nullptr;
  }
  return b;
}

void destroy(void* instance) { delete static_cast<Beauty*>(instance); }

int encode(void* instance, void* cmd, void* in, void* out, const float* params, const MfEffectContext* context) {
  return static_cast<Beauty*>(instance)->encode((__bridge id<MTLCommandBuffer>)cmd, (__bridge id<MTLTexture>)in,
                                                (__bridge id<MTLTexture>)out, params, context);
}

const MfEffectPlugin kPlugin = {MF_EFFECT_PLUGIN_ABI, "beauty", "Beauty", kParams, sizeof(kParams) / sizeof(kParams[0]), create, destroy, encode};

}  // namespace

MF_EFFECT_PLUGIN_EXPORT const MfEffectPlugin* mf_effect_plugin(void) { return &kPlugin; }
