#pragma once

// Draws a composed frame (scene_graph_spec.md §5.1): the scene's canvas, letterboxed into the
// target, cleared to its background, then every layer bottom to top. A layer is fitted,
// transformed (anchor, scale, rotation, transition offset), clipped (wipe) and blended, with
// its effects applied in the shader: chroma key, color adjust, then the global filter. A layer
// with plugin effects or a blur is first drawn into a texture, then each plugin effect runs on
// it (mf/effect_plugin.h), then the two Gaussian blur passes. A group (a
// track in a transition, or with effects) is first combined into its own texture, then drawn
// onto the canvas as one layer with the track's effects, opacity and blend. Video layers
// are sampled straight from their decoder surfaces, so compositing adds no copy. The display
// draws into drawables with it, and the export sink into encoder buffers.

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include "mf/effect_plugin.h"
#include "mf/types.h"

namespace mf::macos {

class MetalCompositor {
 public:
  ~MetalCompositor();
  bool init(id<MTLDevice> device, MTLPixelFormat format);
  // How the canvas goes into a target of another aspect ratio. By default it's letterboxed with
  // black edges (the display); an export can fill its bands with the scene's background, or fill
  // the target and crop the canvas, keeping the part at cropX / cropY (0 left or top, 1 right or
  // bottom).
  struct Framing {
    bool fill = false;
    float cropX = 0.5f, cropY = 0.5f;
    bool backgroundBands = false;
  };
  // Encodes the frame into `target`. The surfaces it reads stay alive until `cmd` completes.
  // Call from one thread at a time.
  void encode(const ComposedFrame&, id<MTLTexture> target, id<MTLCommandBuffer> cmd, const Framing& framing);
  void encode(const ComposedFrame& c, id<MTLTexture> target, id<MTLCommandBuffer> cmd) { encode(c, target, cmd, Framing{}); }

 private:
  enum Source { kNv12, kRgba, kColor, kSources };

  // A layer resolved for drawing: textures, uv rect and the corners on the canvas.
  struct Prepared;

  bool prepare(const ComposedLayer&, double canvasW, double canvasH, double scale, Prepared*, std::vector<CVMetalTextureRef>*);
  // Draws the layer with its shader effects into a texture, then runs its plugin effects and
  // its blur on it. `pixelsPerUnit`: the output height in target pixels.
  void effectsInto(Prepared*, const ComposedLayer&, id<MTLCommandBuffer>, double pixelsPerUnit, int64_t timeUs);
  // A texture for effects, from a pool reused frame to frame (reset at each encode).
  id<MTLTexture> scratch(NSUInteger width, NSUInteger height);
  // This compositor's instance of the plugin with that type; null if none is loaded or it failed.
  struct PluginInstance {
    const MfEffectPlugin* plugin = nullptr;
    void* instance = nullptr;
  };
  const PluginInstance* plugin(const std::string& type);
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
  id<MTLRenderPipelineState> offscreen_[kSources] = {};     // effects into an effects texture, no blending
  id<MTLRenderPipelineState> group_[kSources][2] = {};      // into a track's image: over, and plus (crossfade)
  id<MTLRenderPipelineState> blur_ = nil;                    // one Gaussian pass
  std::vector<id<MTLTexture>> groupTextures_;  // one per group of a frame, reused
  std::vector<id<MTLTexture>> scratch_;        // effect textures, reused in order
  size_t scratchUsed_ = 0;
  std::unordered_map<std::string, PluginInstance> plugins_;  // by type, made on first use
  CVMetalTextureCacheRef cache_ = nullptr;
  std::list<TextTexture> texts_;  // most recent first
};

}  // namespace mf::macos
