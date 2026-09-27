#pragma once

// Draws a composed frame in one pass (§2.4): each layer straight from its decoder surface,
// aspect-fit at its slide offset, with the filter applied in the shader, then the caption on
// top. The display draws into drawables with it, and the export sink into encoder buffers.

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <string>

#include "mf/types.h"

namespace mf::macos {

class MetalCompositor {
 public:
  ~MetalCompositor();
  bool init(id<MTLDevice> device, MTLPixelFormat format);
  // Encodes the frame into `target` (cleared to black). The decoder surfaces it reads stay
  // alive until `cmd` completes. Call from one thread at a time.
  void encode(const ComposedFrame&, id<MTLTexture> target, id<MTLCommandBuffer> cmd);

 private:
  struct Quad {
    float scale[2];   // half-size, in normalized device coordinates
    float offset[2];  // center
  };
  // A caption rendered once into a texture, reused while the text and size stay the same.
  struct CaptionTexture {
    std::string text;
    int fontPx = 0;
    id<MTLTexture> texture = nil;
  };

  void drawCaption(id<MTLRenderCommandEncoder> enc, const std::string& text, const Quad& lead, double videoHeight,
                   double dw, double dh);
  id<MTLTexture> captionTexture(const std::string& text, int fontPx, double maxWidth);

  id<MTLDevice> device_ = nil;
  id<MTLRenderPipelineState> videoPipeline_ = nil, textPipeline_ = nil;
  CVMetalTextureCacheRef cache_ = nullptr;
  CaptionTexture caption_;
};

}  // namespace mf::macos
