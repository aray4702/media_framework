#pragma once

// The effect plugin interface on macOS (scene_graph_spec.md §4.4). A plugin is a .dylib that
// exports mf_effect_plugin(), returning a description of one effect type: its name, its
// parameters, and how to draw it with Metal. mf::macos::loadEffectPlugins loads plugins and
// registers their types, so scenes can use them like the built-in effects.
//
// The interface is plain C, and Metal objects pass as void* (an `id` bridged with __bridge), so
// a plugin needs nothing else from the framework: not its headers, libraries or C++ runtime.
//
// Drawing. The compositor draws the item (or the track's combined image) with its crop, chroma
// key and color adjust into a texture the size it will have on screen, then calls each plugin
// effect's encode() in document order, then blurs. encode() reads `input` and writes all of
// `output`:
//   - both are MTLPixelFormatRGBA16Float, the same size, premultiplied alpha, private storage,
//     with usage RenderTarget | ShaderRead | ShaderWrite;
//   - it only encodes into `commandBuffer` (render or compute passes): it never commits, waits
//     or reads results back on the CPU, and leaves no encoder open;
//   - it returns 0 on success. Anything else, and the effect is skipped: `input` is used as is.
// Each compositor (the display, each export) calls create() once for the effects it draws, and
// uses that instance from one thread at a time. Different instances may run at the same time
// on different threads, so an instance keeps its state (pipelines, textures) to itself.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MF_EFFECT_PLUGIN_ABI 1
#define MF_EFFECT_PLUGIN_ENTRY "mf_effect_plugin"

typedef struct MfEffectParam {
  const char* name;  // the parameter's key in a document: letters, digits, _ . -
  float defaultValue, min, max;
} MfEffectParam;

typedef struct MfEffectContext {
  uint32_t size;        // sizeof(MfEffectContext): later versions only add fields at the end
  float pixelsPerUnit;  // the output height, in pixels of the textures: sizes relative to the output height times this
  int64_t timeUs;       // the frame's scene time (parameters are already evaluated at it)
} MfEffectContext;

typedef struct MfEffectPlugin {
  uint32_t abi;             // MF_EFFECT_PLUGIN_ABI
  const char* type;         // the effect's "type" in a document; not a built-in effect's
  const char* displayName;  // for editors
  const MfEffectParam* params;
  uint32_t paramCount;
  // `device` is the compositor's id<MTLDevice>. NULL: the effect can't run (it's skipped).
  void* (*create)(void* device);
  void (*destroy)(void* instance);
  // `params` holds paramCount values, in the order of `params` above, evaluated at this frame.
  int (*encode)(void* instance, void* commandBuffer, void* input, void* output, const float* params,
                const MfEffectContext* context);
} MfEffectPlugin;

// The one symbol a plugin exports. The result stays valid while the plugin is loaded (plugins
// are never unloaded).
typedef const MfEffectPlugin* (*MfEffectPluginEntry)(void);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
#define MF_EFFECT_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define MF_EFFECT_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
