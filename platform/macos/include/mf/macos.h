#pragma once

#include <memory>
#include <string>

#include "mf/adapters.h"

namespace mf::macos {

std::unique_ptr<PlatformFactory> createPlatform();

MediaSource sourceFromPath(const std::string& path);
RenderTarget targetFromView(void* nsView);  // an NSView whose layer is a CAMetalLayer

int64_t hostNowNs();  // mach_absolute_time in ns: the base CoreAudio and Core Animation use

// Individual adapters (the platform factory uses these; exposed for adapter tests).
std::unique_ptr<IDemuxer> createDemuxer();
std::unique_ptr<IVideoDecoder> createVideoDecoder();
std::unique_ptr<IAudioDecoder> createAudioDecoder();
std::unique_ptr<ISpeaker> createSpeaker();
std::unique_ptr<IDisplay> createDisplay();

}  // namespace mf::macos
