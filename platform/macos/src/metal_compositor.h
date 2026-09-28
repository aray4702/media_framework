#pragma once

// Draws a composed frame (scene_graph_spec.md §5.1): the scene's canvas, letterboxed into the
// target, cleared to its background, then every layer bottom to top. A layer is fitted,
// transformed (anchor, scale, rotation, transition offset), clipped (wipe) and blended, with
// its effects applied in the shader: chroma key, color adjust, then the global filter. A
// blurred layer is first drawn into a texture and blurred in two Gaussian passes. A group (a
// track in a transition, or with effects) is first combined into its own texture, then drawn
// onto the canvas as one layer with the track's effects, opacity and blend. Video layers
// are sampled straight from their decoder surfaces, so compositing adds no copy. The display
// draws into drawables with it, and the export sink into encoder buffers.

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <list>
#include <string>
#include <vector>

#include "mf/types.h"

namespace mf::macos {

class MetalCompositor {
 public:
  ~MetalCompositor();
  bool init(id<MTLDevice> device, MTLPixelFormat format);
  // Encodes the frame into `target`. The surfaces it reads stay alive until `cmd` completes.
  // Call from one thread at a time.
  void encode(const ComposedFrame&, id<MTLTexture> target, id<MTLCommandBuffer> cmd);

 private:
  enum Source { kNv12, kRgba, kColor, kSources };

  // A layer resolved for drawing: textures, uv rect and the corners on the canvas.
  struct Prepared;

  bool prepare(const ComposedLayer&, double canvasW, double canvasH, double scale, Prepared*, std::vector<CVMetalTextureRef>*);
  void blurInto(Prepared*, const ComposedLayer&, id<MTLCommandBuffer>, double sigmaPx);
  // `filter`: the global filter, for video and image layers; null for a track's combined image.
  void draw(id<MTLRenderCommandEncoder>, id<MTLRenderPipelineState>, const Prepared&, const ComposedLayer&, const VideoFilter* filter,
            bool effects);
  void drawLayer(id<MTLRenderCommandEncoder>, id<MTLRenderPipelineState>, const Prepared&, const ComposedLayer&, const VideoFilter*,
                 const MTLViewport&, double targetW, double targetH);
  bool combine(const ComposedFrame&, int group, const std::vector<Prepared>&, const std::vector<bool>& ok, const MTLViewport&,
               double targetW, double targetH, id<MTLCommandBuffer>, Prepared* out);

  // Text rendered once into a texture, reused while the text, style and size stay the same.
  struct TextTexture {
    std::string text;
    TextStyle style;
    int widthPx = 0, lineHeightPx = 0;
    id<MTLTexture> texture = nil;
  };
  id<MTLTexture> textTexture(const std::string& text, const TextStyle&, int maxWidthPx, int lineHeightPx);

  id<MTLDevice> device_ = nil;
  MTLPixelFormat format_ = MTLPixelFormatBGRA8Unorm;
  id<MTLRenderPipelineState> pipelines_[kSources][4] = {};  // by source, by blend mode
  id<MTLRenderPipelineState> offscreen_[kSources] = {};     // effects into a blur texture, no blending
  id<MTLRenderPipelineState> group_[kSources][2] = {};      // into a track's image: over, and plus (crossfade)
  id<MTLRenderPipelineState> blur_ = nil;                    // one Gaussian pass
  std::vector<id<MTLTexture>> groupTextures_;  // one per group of a frame, reused
  CVMetalTextureCacheRef cache_ = nullptr;
  std::list<TextTexture> texts_;  // most recent first
};

}  // namespace mf::macos
