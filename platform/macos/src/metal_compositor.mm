#import "metal_compositor.h"

#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

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

}  // namespace

MetalCompositor::~MetalCompositor() {
  if (cache_) CFRelease(cache_);
}

bool MetalCompositor::init(id<MTLDevice> device, MTLPixelFormat format) {
  device_ = device;
  NSError* error = nil;
  id<MTLLibrary> library = [device_ newLibraryWithSource:kShaders options:nil error:&error];
  MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
  desc.vertexFunction = [library newFunctionWithName:@"vmain"];
  desc.fragmentFunction = [library newFunctionWithName:@"fvideo"];
  desc.colorAttachments[0].pixelFormat = format;
  videoPipeline_ = library ? [device_ newRenderPipelineStateWithDescriptor:desc error:&error] : nil;
  desc.fragmentFunction = [library newFunctionWithName:@"ftext"];
  desc.colorAttachments[0].blendingEnabled = YES;
  desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
  desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  textPipeline_ = library ? [device_ newRenderPipelineStateWithDescriptor:desc error:&error] : nil;
  if (!videoPipeline_ || !textPipeline_ || CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache_) != kCVReturnSuccess) {
    NSLog(@"[mf] Metal setup failed: %@", error);
    return false;
  }
  return true;
}

void MetalCompositor::encode(const ComposedFrame& c, id<MTLTexture> target, id<MTLCommandBuffer> cmd) {
  double dw = target.width, dh = target.height;
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;

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

  ComposedFrame keep = c;  // release the pixel buffers only once the GPU is done with them
  [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
    for (CVMetalTextureRef t : *textures) CFRelease(t);
    (void)keep;
  }];
}

// Centered at the bottom of the leading layer's picture, not sliding with it: 5% of the
// picture's height, narrowed to fit 90% of the view.
void MetalCompositor::drawCaption(id<MTLRenderCommandEncoder> enc, const std::string& text, const Quad& lead, double videoHeight,
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
id<MTLTexture> MetalCompositor::captionTexture(const std::string& text, int fontPx, double maxWidth) {
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

}  // namespace mf::macos
