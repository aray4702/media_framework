#include "mf/player.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "pipeline.h"

namespace mf {

bool isValid(const VideoFilter& f) {
  return std::isfinite(f.brightness) && std::isfinite(f.contrast) && f.brightness >= -1 && f.brightness <= 1 &&
         f.contrast >= 0 && f.contrast <= 2;
}

struct Player::Impl : PipelineEvents {
  Impl(PlatformFactory& factory, PlayerListener* l) : ctx(factory, *this), listener(l), owner(std::this_thread::get_id()) {
    stages = makeStages(ctx);
    std::array<Stage*, kStageCount> raw;
    for (int i = 0; i < kStageCount; ++i) raw[i] = stages[i].get();
    ctx.scheduler->start(raw);
  }
  ~Impl() override { shutdown(); }

  bool onOwner() const { return std::this_thread::get_id() == owner; }

  template <typename F>
  void notify(F&& f) {
    if (listener && callbacksOn) f(*listener);
  }

  // --- API (owner thread) ---

  Result open(const Scene& scene, const RenderTarget& target, OutputDriver driver, std::string* error, int64_t startUs) {
    if (validateScene(scene, error) != Result::Ok) return Result::InvalidArgument;
    {
      std::lock_guard<std::mutex> lock(stateMu);
      if (state != State::Start || openCalled) return Result::InvalidState;
      openCalled = true;
    }
    Result r = ctx.display->attach(target, [this](int64_t pts, int64_t ns) { onPresented(pts, ns); });
    if (r != Result::Ok) return r;
    ctx.driver = driver == OutputDriver::LeadingClip ? Driver::LeadingClip : Driver::Vsync;
    ctx.autoDriver = driver == OutputDriver::Auto;
    ctx.scene = scene;
    ctx.startUs = std::max<int64_t>(0, startUs);
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

  Result setFilter(const VideoFilter& filter) {
    if (!isValid(filter)) return Result::InvalidArgument;
    if (getState() == State::Shutdown) return Result::InvalidState;
    ctx.setFilter(filter);
    ctx.wake(StageId::VideoRender);
    return Result::Ok;
  }

  // Timing, media, and track structure: an appearance edit must leave these alone, because the
  // decoded frames and the layout were built from them.
  static bool sameStructure(const Scene& a, const Scene& b) {
    if (a.tracks.size() != b.tracks.size()) return false;
    const SceneOutput& oa = a.output, &ob = b.output;
    if (oa.width != ob.width || oa.height != ob.height || oa.fpsNum != ob.fpsNum || oa.fpsDen != ob.fpsDen ||
        oa.sampleRate != ob.sampleRate || oa.channels != ob.channels)
      return false;
    for (size_t t = 0; t < a.tracks.size(); ++t) {
      const SceneTrack& ta = a.tracks[t], &tb = b.tracks[t];
      if (ta.video != tb.video || ta.enabled != tb.enabled || ta.gain != tb.gain || ta.items.size() != tb.items.size() ||
          ta.transitions.size() != tb.transitions.size())
        return false;
      for (size_t k = 0; k < ta.items.size(); ++k) {
        const SceneItem& ia = ta.items[k], &ib = tb.items[k];
        if (ia.id != ib.id || ia.type != ib.type || ia.startUs != ib.startUs || ia.durationUs != ib.durationUs ||
            ia.inUs != ib.inUs || ia.speed != ib.speed || ia.src != ib.src || ia.mute != ib.mute || ia.gain.value != ib.gain.value ||
            ia.gain.keys.size() != ib.gain.keys.size() || ia.pan.value != ib.pan.value || ia.pan.keys.size() != ib.pan.keys.size())
          return false;
      }
      for (size_t x = 0; x < ta.transitions.size(); ++x) {
        const SceneTransition& xa = ta.transitions[x], &xb = tb.transitions[x];
        if (xa.from != xb.from || xa.kind != xb.kind || xa.direction != xb.direction || xa.durationUs != xb.durationUs ||
            xa.audio != xb.audio || xa.easing.kind != xb.easing.kind || xa.easing.x1 != xb.easing.x1 ||
            xa.easing.y1 != xb.easing.y1 || xa.easing.x2 != xb.easing.x2 || xa.easing.y2 != xb.easing.y2)
          return false;
      }
    }
    return true;
  }

  static void copyAppearance(Scene* dst, const Scene& src) {
    dst->output.background = src.output.background;
    for (size_t t = 0; t < dst->tracks.size(); ++t) {
      dst->tracks[t].opacity = src.tracks[t].opacity;
      dst->tracks[t].effects = src.tracks[t].effects;
      for (size_t k = 0; k < dst->tracks[t].items.size(); ++k) {
        SceneItem& d = dst->tracks[t].items[k];
        const SceneItem& s = src.tracks[t].items[k];
        d.transform = s.transform;
        d.opacity = s.opacity;
        d.blend = s.blend;
        d.fit = s.fit;
        d.effects = s.effects;
        d.text = s.text;
        d.style = s.style;
        d.color = s.color;
      }
    }
  }

  Result updateAppearance(const Scene& scene) {
    State s = getState();
    if (s != State::Ready && s != State::Play) return Result::InvalidState;
    if (!ctx.probed) return Result::InvalidState;
    if (!sameStructure(ctx.scene, scene)) return Result::InvalidArgument;
    {
      std::lock_guard<std::mutex> lock(ctx.appearanceMu);
      copyAppearance(&ctx.scene, scene);
      for (int i = 0; i < ctx.layout.items(); ++i) {
        const SceneItem& it = ctx.layout.item(i);
        if (it.type != ItemType::Text) continue;
        if (!ctx.items[i].text || *ctx.items[i].text != it.text) ctx.items[i].text = std::make_shared<const std::string>(it.text);
      }
    }
    ++ctx.appearanceVersion;
    ctx.wake(StageId::Composition);
    ctx.wake(StageId::VideoRender);
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
    for (auto& lane : ctx.lanes) {
      lane->videoDecoder.reset();
      lane->audioDecoder.reset();
    }
    for (ItemRuntime& item : ctx.items) item.demuxer.reset();
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

Result Player::open(const Scene& s, const RenderTarget& t, OutputDriver d, std::string* error, int64_t startUs) {
  return impl_->onOwner() ? impl_->open(s, t, d, error, startUs) : Result::WrongThread;
}
Result Player::open(const MediaSource& s, const RenderTarget& t) {
  Scene scene;
  scene.output.width = scene.output.height = 0;  // the file's size, frame rate and audio format
  scene.output.fpsNum = 0;
  scene.output.sampleRate = scene.output.channels = 0;
  SceneTrack track;
  SceneItem item;
  item.type = ItemType::Video;
  item.source = s;  // duration 0: to the end of the file
  track.items.push_back(item);
  scene.tracks.push_back(track);
  return open(scene, t);
}
Result Player::setFilter(const VideoFilter& f) { return impl_->onOwner() ? impl_->setFilter(f) : Result::WrongThread; }
Result Player::updateAppearance(const Scene& s) { return impl_->onOwner() ? impl_->updateAppearance(s) : Result::WrongThread; }
Result Player::play() { return impl_->onOwner() ? impl_->play() : Result::WrongThread; }
Result Player::pause() { return impl_->onOwner() ? impl_->pause() : Result::WrongThread; }
Result Player::seek(int64_t us) { return impl_->onOwner() ? impl_->seek(us) : Result::WrongThread; }
Result Player::shutdown() { return impl_->onOwner() ? impl_->shutdown() : Result::WrongThread; }
State Player::state() const { return impl_->getState(); }
int64_t Player::durationUs() const { return impl_->ctx.durationUs; }
int64_t Player::positionUs() const { return impl_->position(); }
MetricsReport Player::metrics() const { return impl_->ctx.metrics.report(); }

}  // namespace mf
