#include "mf/player.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#include "pipeline.h"

namespace mf {

struct Player::Impl : PipelineEvents {
  Impl(PlatformFactory& factory, PlayerListener* l) : ctx(factory, *this), listener(l), owner(std::this_thread::get_id()) {
    stages = makeStages(ctx);
    ctx.scheduler->start({stages[0].get(), stages[1].get(), stages[2].get(), stages[3].get()});
  }
  ~Impl() override { shutdown(); }

  bool onOwner() const { return std::this_thread::get_id() == owner; }

  template <typename F>
  void notify(F&& f) {
    if (listener && callbacksOn) f(*listener);
  }

  // --- API (owner thread) ---

  Result open(const MediaSource& source, const RenderTarget& target) {
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state != State::Start || openCalled) return Result::InvalidState;
      openCalled = true;
    }
    Result r = ctx.display->attach(target, [this](int64_t pts, int64_t ns) { onPresented(pts, ns); });
    if (r != Result::Ok) return r;
    ctx.source = source;
    ctx.metrics.startTtff(ctx.hostClock.nowNs());
    ctx.openRequested = true;
    ctx.wake(StageId::Source);
    return Result::Ok;
  }

  Result play() {
    bool restart;
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state != State::Ready) return Result::InvalidState;
      state = State::Play;
      restart = ended;
      ended = false;
    }
    if (restart) ctx.requestSeek(0);  // A5: play after end of stream restarts at 0
    {
      std::lock_guard<std::mutex> lock(ctx.playMu);
      ctx.playing = true;
    }
    ctx.wakeAll();
    return Result::Ok;
  }

  Result pause() {
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state != State::Play) return Result::InvalidState;
      state = State::Ready;
    }
    stopPlaying();
    return Result::Ok;
  }

  Result seek(int64_t positionUs) {
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state != State::Ready) return Result::InvalidState;
      ended = false;
    }
    ctx.requestSeek(std::clamp<int64_t>(positionUs, 0, ctx.durationUs));
    ctx.wake(StageId::Source);
    return Result::Ok;
  }

  Result shutdown() {
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state == State::Shutdown) return Result::Ok;
      state = State::Shutdown;
    }
    callbacksOn = false;
    ctx.halted = true;
    ctx.scheduler->stop();
    {
      std::lock_guard<std::mutex> lock(ctx.playMu);
      if (ctx.outputRunning && ctx.hasAudio) ctx.speaker->pause();
      ctx.outputRunning = false;
    }
    // Release adapters before the buffers they call back into (ring, metrics, scheduler).
    ctx.speaker.reset();
    ctx.display.reset();
    ctx.videoDecoder.reset();
    ctx.audioDecoder.reset();
    ctx.demuxer.reset();
    std::fprintf(stderr, "[mf] metrics: %s\n", ctx.metrics.report().toString().c_str());
    return Result::Ok;
  }

  State getState() const {
    std::lock_guard<std::mutex> lock(stateMu);
    return state;
  }

  int64_t position() {
    if (getState() == State::Play) {
      return std::clamp<int64_t>(ctx.master.nowUs(ctx.hostClock.nowNs()), 0, ctx.durationUs);
    }
    return ctx.shownPtsUs;
  }

  // --- Events (internal threads) ---

  void onFatal(Result r, const std::string& reason) override {
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state == State::Error || state == State::Shutdown) return;
      state = State::Error;
    }
    ctx.halted = true;
    {
      std::lock_guard<std::mutex> lock(ctx.playMu);
      ctx.playing = false;
      if (ctx.outputRunning && ctx.hasAudio) ctx.speaker->pause();
      ctx.outputRunning = false;
      ctx.master.stop(ctx.hostClock.nowNs());
    }
    std::fprintf(stderr, "[mf] fatal %s: %s\n", toString(r), reason.c_str());
    notify([&](PlayerListener& l) { l.onStateChanged(State::Error); });
    notify([&](PlayerListener& l) { l.onError(r, reason); });
  }

  void onWarning(Warning w, const std::string& reason) override {
    std::fprintf(stderr, "[mf] warning %s: %s\n", toString(w), reason.c_str());
    notify([&](PlayerListener& l) { l.onWarning(w, reason); });
  }

  void onSeekDone(uint32_t serial, int64_t shownPtsUs, bool hadFrame) override {
    if (serial == 1) {  // the preroll started by open()
      if (!hadFrame) return onFatal(Result::MalformedMedia, "no decodable video frame");
      {
        std::lock_guard<std::mutex> lock(stateMu);
        if (state != State::Start) return;
        state = State::Ready;
      }
      notify([](PlayerListener& l) { l.onFirstFrame(); });
      notify([](PlayerListener& l) { l.onStateChanged(State::Ready); });
      return;
    }
    notify([&](PlayerListener& l) { l.onSeekCompleted(shownPtsUs); });
  }

  void onEnd() override {
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state != State::Play) return;
      state = State::Ready;
      ended = true;
    }
    stopPlaying();
    ctx.shownPtsUs = ctx.durationUs.load();
    notify([](PlayerListener& l) { l.onEnded(); });
    notify([](PlayerListener& l) { l.onStateChanged(State::Ready); });
  }

  // Display thread: actual present time of a frame, for jank and A/V offset.
  void onPresented(int64_t ptsUs, int64_t presentedNs) {
    std::optional<int64_t> av;
    if (presentedNs > 0 && ctx.hasAudio && ctx.master.running() && ctx.master.usingAudio()) {
      int64_t now = ctx.hostClock.nowNs();
      av = ptsUs - (ctx.master.nowUs(now) - (now - presentedNs) / 1000);
    }
    ctx.metrics.presented(ptsUs, presentedNs, av);
  }

  // Freeze the clock now; T3 stops the speaker on its next pump.
  void stopPlaying() {
    {
      std::lock_guard<std::mutex> lock(ctx.playMu);
      ctx.playing = false;
      ctx.master.stop(ctx.hostClock.nowNs());
    }
    ctx.wake(StageId::VideoRender);
  }

  Context ctx;
  std::array<std::unique_ptr<Stage>, kStageCount> stages;
  PlayerListener* listener;
  const std::thread::id owner;
  mutable std::mutex stateMu;
  State state = State::Start;
  bool ended = false, openCalled = false;
  std::atomic<bool> callbacksOn{true};
};

std::unique_ptr<Player> Player::create(PlatformFactory& factory, PlayerListener* listener) {
  return std::unique_ptr<Player>(new Player(std::make_unique<Impl>(factory, listener)));
}

Player::Player(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Player::~Player() = default;

Result Player::open(const MediaSource& s, const RenderTarget& t) {
  return impl_->onOwner() ? impl_->open(s, t) : Result::WrongThread;
}
Result Player::play() { return impl_->onOwner() ? impl_->play() : Result::WrongThread; }
Result Player::pause() { return impl_->onOwner() ? impl_->pause() : Result::WrongThread; }
Result Player::seek(int64_t us) { return impl_->onOwner() ? impl_->seek(us) : Result::WrongThread; }
Result Player::shutdown() { return impl_->onOwner() ? impl_->shutdown() : Result::WrongThread; }
State Player::state() const { return impl_->getState(); }
int64_t Player::durationUs() const { return impl_->ctx.durationUs; }
int64_t Player::positionUs() const { return impl_->position(); }
MetricsReport Player::metrics() const { return impl_->ctx.metrics.report(); }

}  // namespace mf
