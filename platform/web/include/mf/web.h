#pragma once

// The web platform (Emscripten): WebCodecs decoders, a WebGPU display, an AudioWorklet speaker,
// the portable MP4 demuxer and a cooperative scheduler on the page's thread.

#include <memory>

#include "mf/adapters.h"

namespace mf::web {

// RenderTarget::native: the CSS selector of the <canvas> the display draws into, e.g. "#view".
// Its WebGPU device must be set up first (mf.initGpu() in library_mf.js).
std::unique_ptr<PlatformFactory> createPlatform();

}  // namespace mf::web
