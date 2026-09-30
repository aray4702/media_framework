#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mf/adapters.h"
#include "mf/scene.h"

namespace mf::macos {

std::unique_ptr<PlatformFactory> createPlatform();

MediaSource sourceFromPath(const std::string& path);
RenderTarget targetFromView(void* nsView);  // an NSView whose layer is a CAMetalLayer
ExportTarget exportTargetFromPath(const std::string& path);  // an .mp4 file; replaced if it exists

// Reads and validates a scene document (scene_graph_spec.md). Each `src` is a path, relative
// to the document's folder unless absolute.
Result loadScene(const std::string& path, Scene* out, std::string* error);

// Effect plugins (mf/effect_plugin.h). Loads every .dylib in `dir` and registers its effect
// type (mf/effects.h), so scenes can use it. Returns the types loaded. Each file that fails (not
// a plugin, another ABI, a type already registered) adds a line to `errors`. Load plugins
// before parsing or opening scenes that use them. Thread-safe; a file already loaded is skipped.
std::vector<std::string> loadEffectPluginsFrom(const std::string& dir, std::vector<std::string>* errors = nullptr);
// The same for each of these folders that exists, in this order (the first plugin of a type
// wins): those in $MF_EFFECT_PLUGIN_PATH (separated by ':'), "plugins" next to the executable,
// an app bundle's PlugIns, ~/Library/Application Support/Media Framework/Plugins, and the
// build tree's plugins folder.
std::vector<std::string> loadEffectPlugins(std::vector<std::string>* errors = nullptr);

int64_t hostNowNs();  // mach_absolute_time in ns: the base CoreAudio and Core Animation use

// Individual adapters (the platform factory uses these; exposed for adapter tests).
std::unique_ptr<IDemuxer> createDemuxer();
std::unique_ptr<IVideoDecoder> createVideoDecoder();
std::unique_ptr<IAudioDecoder> createAudioDecoder();
std::unique_ptr<ISpeaker> createSpeaker();
std::unique_ptr<IDisplay> createDisplay();
std::unique_ptr<IExportSink> createExportSink();
std::unique_ptr<IImageLoader> createImageLoader();

}  // namespace mf::macos
