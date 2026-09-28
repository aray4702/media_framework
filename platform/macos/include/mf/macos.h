#pragma once

#include <memory>
#include <string>

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
