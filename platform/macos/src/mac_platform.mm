#import <Foundation/Foundation.h>

#include <pthread.h>
#include <time.h>

#include "mf/macos.h"
#include "mf/thread_scheduler.h"

namespace mf::macos {
namespace {

struct HostClock : IClock {
  int64_t nowNs() const override { return hostNowNs(); }
};

class MacPlatform : public PlatformFactory {
 public:
  std::unique_ptr<IDemuxer> createDemuxer() override { return macos::createDemuxer(); }
  std::unique_ptr<IVideoDecoder> createVideoDecoder() override { return macos::createVideoDecoder(); }
  std::unique_ptr<IAudioDecoder> createAudioDecoder() override { return macos::createAudioDecoder(); }
  std::unique_ptr<ISpeaker> createSpeaker() override { return macos::createSpeaker(); }
  std::unique_ptr<IDisplay> createDisplay() override { return macos::createDisplay(); }
  std::unique_ptr<IExportSink> createExportSink() override { return macos::createExportSink(); }
  std::unique_ptr<IScheduler> createScheduler() override {
    return std::make_unique<ThreadScheduler>(clock_, [](StageId id) {
      static const char* names[] = {"mf.source", "mf.video-decode", "mf.composition", "mf.video-render", "mf.audio"};
      pthread_setname_np(names[static_cast<int>(id)]);
      bool realtime = id == StageId::VideoRender || id == StageId::Audio;
      pthread_set_qos_class_self_np(realtime ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
    });
  }
  IClock& clock() override { return clock_; }

 private:
  HostClock clock_;
};

}  // namespace

int64_t hostNowNs() { return static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)); }

std::unique_ptr<PlatformFactory> createPlatform() { return std::make_unique<MacPlatform>(); }

MediaSource sourceFromPath(const std::string& path) {
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  return MediaSource{std::shared_ptr<void>(const_cast<void*>(CFBridgingRetain(url)), CFRelease)};
}

RenderTarget targetFromView(void* nsView) { return RenderTarget{nsView}; }

ExportTarget exportTargetFromPath(const std::string& path) {
  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
  return ExportTarget{std::shared_ptr<void>(const_cast<void*>(CFBridgingRetain(url)), CFRelease)};
}

}  // namespace mf::macos
