#include "mf/exporter.h"

#include <cstdio>
#include <thread>

#include "pipeline.h"

namespace mf {

// The playback pipeline with the export driver: T1 and T2 as for playback, TC composes on
// the n / fps grid, T3 and T4 write to the export sink instead of the display and speaker.
struct Exporter::Impl : PipelineEvents {
  Impl(PlatformFactory& factory, ExportListener* l) : ctx(factory, *this, true), listener(l), owner(std::this_thread::get_id()) {
    stages = makeStages(ctx);
    std::array<Stage*, kStageCount> raw;
    for (int i = 0; i < kStageCount; ++i) raw[i] = stages[i].get();
    ctx.scheduler->start(raw);
  }
  ~Impl() override { shutdown(); }

  bool onOwner() const { return std::this_thread::get_id() == owner; }

  static bool valid(const ExportSettings& s) {
    return s.width > 0 && s.height > 0 && s.width % 2 == 0 && s.height % 2 == 0 && s.width <= 8192 && s.height <= 8192 &&
           s.fps >= 1 && s.fps <= 240 && s.videoBitrate > 0 && s.audioBitrate > 0;
  }

  Result start(const Timeline& timeline, const ExportTarget& target, const ExportSettings& settings) {
    if (!isValid(timeline) || !valid(settings)) return Result::InvalidArgument;
    if (started || stopped) return Result::InvalidState;
    if (!ctx.exportSink) return Result::Unsupported;
    started = true;
    ctx.driver = Driver::Export;
    ctx.exportTarget = target;
    ctx.exportSettings = settings;
    ctx.sources = timeline.clips;
    ctx.transition = timeline.transition;
    for (const TextOverlay& o : timeline.texts) {
      ctx.captions.push_back({o.startUs, o.endUs, std::make_shared<const std::string>(o.text)});
    }
    ctx.setFilter(timeline.filter);
    ctx.openRequested = true;
    ctx.wake(StageId::Source);
    return Result::Ok;
  }

  Result shutdown() {
    if (stopped) return Result::Ok;
    stopped = true;
    callbacksOn = false;
    ctx.halted = true;
    ctx.scheduler->stop();
    ctx.exportSink.reset();  // cancels an unfinished file; no callback after this returns
    for (Lane& lane : ctx.lanes) {
      lane.videoDecoder.reset();
      lane.audioDecoder.reset();
    }
    ctx.demuxers.clear();
    return Result::Ok;
  }

  double progress() const {
    if (completed) return 1.0;
    int64_t duration = ctx.durationUs;
    return duration > 0 ? std::min(1.0, double(ctx.writtenUs) / double(duration)) : 0.0;
  }

  // --- Events (internal threads) ---

  void onFatal(Result r, const std::string& reason) override {
    if (finished.exchange(true)) return;
    ctx.halted = true;
    std::fprintf(stderr, "[mf] export failed %s: %s\n", toString(r), reason.c_str());
    if (listener && callbacksOn) listener->onError(r, reason);
  }

  void onWarning(Warning w, const std::string& reason) override {
    if (listener && callbacksOn) listener->onWarning(w, reason);
  }

  void onSeekDone(uint32_t, int64_t, bool) override {}

  void onEnd() override {  // the sink has finished the file
    if (finished.exchange(true)) return;
    completed = true;
    if (listener && callbacksOn) listener->onCompleted();
  }

  Context ctx;
  std::array<std::unique_ptr<Stage>, kStageCount> stages;
  ExportListener* listener;
  const std::thread::id owner;
  bool started = false, stopped = false;
  std::atomic<bool> finished{false}, completed{false}, callbacksOn{true};
};

std::unique_ptr<Exporter> Exporter::create(PlatformFactory& factory, ExportListener* listener) {
  return std::unique_ptr<Exporter>(new Exporter(std::make_unique<Impl>(factory, listener)));
}

Exporter::Exporter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Exporter::~Exporter() = default;

Result Exporter::start(const Timeline& t, const ExportTarget& target, const ExportSettings& s) {
  return impl_->onOwner() ? impl_->start(t, target, s) : Result::WrongThread;
}
Result Exporter::shutdown() { return impl_->onOwner() ? impl_->shutdown() : Result::WrongThread; }
double Exporter::progress() const { return impl_->progress(); }
bool Exporter::done() const { return impl_->finished; }

}  // namespace mf
