#pragma once

// Fake adapters, a fake clock and a single-threaded manual scheduler, so the Player can be
// tested deterministically on the host.

#include <algorithm>
#include <atomic>
#include <climits>
#include <deque>
#include <set>
#include <vector>

#include "mf/audio_ring.h"
#include "mf/exporter.h"
#include "mf/player.h"

namespace fake {

using namespace mf;

constexpr int64_t kMs = 1000000;

struct Clock : IClock {
  int64_t now = 1000 * kMs;
  int64_t nowNs() const override { return now; }
};

struct Clip {
  int64_t durationUs = 2000000;
  int fps = 30;
  int gop = 30;
  bool audio = true;
  bool audioSupported = true;
  int sampleRate = 48000;
  int64_t audioDurationUs = -1;  // -1: same as video
  std::set<int> corruptFrames;   // video frame indices that fail to decode
  int64_t failDecodeAtUs = -1;   // decoder fails fatally at this pts
  bool failOpen = false;
};

constexpr int kAudioFrames = 1024;
constexpr int kRate = 48000;
constexpr int64_t kAudioPacketUs = int64_t{kAudioFrames} * 1000000 / kRate;

// A MediaSource naming fake clip `index` (see Platform::clips).
inline MediaSource clipSource(int index) { return MediaSource{std::make_shared<int>(index)}; }

class Demuxer : public IDemuxer {
 public:
  // Plays the clip its source names (clipSource), else `fallback` (the clips in creation order).
  Demuxer(const std::vector<Clip>& clips, const Clip& fallback) : clips_(clips), clip_(fallback) {}
  Result open(const MediaSource& source, MediaInfo* out) override {
    if (source.native) clip_ = clips_[size_t(*static_cast<int*>(source.native.get())) % clips_.size()];
    packetUs_ = int64_t{kAudioFrames} * 1000000 / clip_.sampleRate;
    if (clip_.failOpen) return Result::FileOpenFailed;
    int n = static_cast<int>(clip_.durationUs * clip_.fps / 1000000);
    for (int i = 0; i < n; ++i) {
      Packet p;
      p.track = kVideo;
      p.ptsUs = p.dtsUs = int64_t{i} * 1000000 / clip_.fps;
      p.key = i % clip_.gop == 0;
      p.data = {uint8_t(clip_.corruptFrames.count(i) ? 0xFF : 0x00)};
      packets_[kVideo].push_back(p);
    }
    int64_t audioEnd = clip_.audioDurationUs < 0 ? clip_.durationUs : clip_.audioDurationUs;
    for (int64_t pts = 0; clip_.audio && pts < audioEnd; pts += packetUs_) {
      Packet p;
      p.track = kAudio;
      p.ptsUs = p.dtsUs = pts;
      p.key = true;
      p.data = {0};
      packets_[kAudio].push_back(p);
    }
    out->durationUs = clip_.durationUs;
    out->video.supported = true;
    out->video.width = 640;
    out->video.height = 360;
    out->video.frameDurationUs = 1000000 / clip_.fps;
    if (clip_.audio) {
      TrackInfo a;
      a.supported = clip_.audioSupported;
      a.sampleRate = clip_.sampleRate;
      a.channels = 2;
      out->audio = a;
    }
    return Result::Ok;
  }
  Result peekDtsUs(int track, int64_t* out) override {
    if (next_[track] >= packets_[track].size()) return Result::Eos;
    *out = packets_[track][next_[track]].dtsUs;
    return Result::Ok;
  }
  Result read(int track, Packet* out) override {
    if (next_[track] >= packets_[track].size()) return Result::Eos;
    *out = packets_[track][next_[track]++];
    return Result::Ok;
  }
  Result seekTo(int64_t us) override {
    size_t v = 0;
    for (size_t i = 0; i < packets_[kVideo].size(); ++i) {
      if (packets_[kVideo][i].ptsUs > us) break;
      if (packets_[kVideo][i].key) v = i;
    }
    next_[kVideo] = v;
    int64_t keyPts = packets_[kVideo].empty() ? 0 : packets_[kVideo][v].ptsUs;
    next_[kAudio] = static_cast<size_t>(keyPts / packetUs_);
    return Result::Ok;
  }

 private:
  const std::vector<Clip>& clips_;
  Clip clip_;
  int64_t packetUs_ = kAudioPacketUs;
  std::vector<Packet> packets_[2];
  size_t next_[2] = {0, 0};
};

class VideoDecoder : public IVideoDecoder {
 public:
  explicit VideoDecoder(const Clip& c) : clip_(c) {}
  Result configure(const TrackInfo&, std::function<void()> onOutput) override {
    onOutput_ = std::move(onOutput);
    out_.clear();
    eos_ = false;
    return Result::Ok;
  }
  Result queue(const Packet& p) override {
    if (clip_.failDecodeAtUs >= 0 && p.ptsUs >= clip_.failDecodeAtUs) return Result::DecoderFailed;
    if (out_.size() >= 4) return Result::Again;
    if (p.data[0] == 0xFF) return Result::CorruptFrame;
    VideoFrame f;
    f.ptsUs = p.ptsUs;
    f.serial = p.serial;
    out_.push_back(f);
    onOutput_();
    return Result::Ok;
  }
  void signalEos() override { eos_ = true; }
  Result dequeue(VideoFrame* out) override {
    if (!out_.empty()) {
      *out = out_.front();
      out_.pop_front();
      return Result::Ok;
    }
    return eos_ ? Result::Eos : Result::Again;
  }
  void flush() override {
    out_.clear();
    eos_ = false;
  }

 private:
  Clip clip_;
  std::function<void()> onOutput_;
  std::deque<VideoFrame> out_;
  bool eos_ = false;
};

class AudioDecoder : public IAudioDecoder {
 public:
  Result configure(const TrackInfo&) override { return Result::Ok; }
  Result decode(const Packet& p, PcmBuffer* out) override {
    out->ptsUs = p.ptsUs;
    out->samples.assign(kAudioFrames * 2, 100);
    return Result::Ok;
  }
  void flush() override {}
};

// Presents instantly at the requested time.
class Display : public IDisplay {
 public:
  explicit Display(Clock& c) : clock_(c) {}
  Result attach(const RenderTarget&, PresentedFn fn) override {
    presentedFn_ = std::move(fn);
    return Result::Ok;
  }
  void present(const ComposedFrame& f, int64_t hostTimeNs) override {
    shown.push_back(f.ptsUs);
    composed.push_back(f);
    presentedFn_(f.ptsUs, std::max(hostTimeNs, clock_.now));
  }
  bool visible() const override { return true; }
  int64_t vsyncPeriodNs() const override { return 16666667; }
  std::vector<int64_t> shown;
  std::vector<ComposedFrame> composed;

 private:
  Clock& clock_;
  PresentedFn presentedFn_;
};

// Pulls 10 ms from the ring every 10 ms of fake time, with 20 ms output latency.
class Speaker : public ISpeaker {
 public:
  Result open(int, int channels, AudioRing* ring) override {
    ring_ = ring;
    buf_.resize(size_t(480) * channels);
    return Result::Ok;
  }
  Result start() override {
    running_ = true;
    startRequested_ = true;
    return Result::Ok;
  }
  void pause() override { running_ = false; }
  void tick(int64_t now) {
    if (!running_ || !ring_) return;
    if (startRequested_) {
      nextNs_ = now;
      startRequested_ = false;
    }
    while (nextNs_ <= now) {
      ring_->consume(buf_.data(), 480, nextNs_ + 20 * kMs);
      heard.insert(heard.end(), buf_.begin(), buf_.end());
      nextNs_ += 10 * kMs;
    }
  }
  std::vector<int16_t> heard;  // every sample played, interleaved

 private:
  AudioRing* ring_ = nullptr;
  std::vector<int16_t> buf_;
  bool running_ = false, startRequested_ = false;
  int64_t nextNs_ = 0;
};

// Any source loads as a 400x200 image.
class ImageLoader : public IImageLoader {
 public:
  Result load(const MediaSource&, VideoFrame* out, int* width, int* height) override {
    out->image = std::make_shared<int>(0);
    *width = 400;
    *height = 200;
    return Result::Ok;
  }
};

// Records what an export writes. With busyEvery = n, every n-th write reports Again. With credits
// of 0 or more, each write takes one and reports Again when there are none: an encoder that keeps
// up only as fast as the test grants them (-1: no limit).
class ExportSink : public IExportSink {
 public:
  Result open(const ExportTarget&, const ExportSettings& s, int rate, int ch) override {
    settings = s;
    sampleRate = rate;
    channels = ch;
    return Result::Ok;
  }
  Result writeVideo(const ComposedFrame& f) override {
    if (busyEvery && ++videoCalls % busyEvery == 0) return Result::Again;
    if (!take(videoCredits)) return Result::Again;
    video.push_back(f);
    return Result::Ok;
  }
  Result writeAudio(const int16_t* pcm, int frames, int64_t ptsUs) override {
    if (busyEvery && ++audioCalls % busyEvery == 0) return Result::Again;
    if (!take(audioCredits)) return Result::Again;
    contiguous &= ptsUs == int64_t(audioFrames) * 1000000 / sampleRate;
    audioPts.push_back(ptsUs);
    audioFrames += frames;
    samples.insert(samples.end(), pcm, pcm + size_t(frames) * channels);
    return Result::Ok;
  }
  void finish(std::function<void(Result)> done) override {
    finished = true;
    done(Result::Ok);
  }

  static bool take(std::atomic<int>& credits) {
    int c = credits;
    while (c != 0 && !credits.compare_exchange_weak(c, c < 0 ? c : c - 1)) {
    }
    return c != 0;
  }

  int busyEvery = 0, videoCalls = 0, audioCalls = 0;
  std::atomic<int> videoCredits{-1}, audioCredits{-1};
  ExportSettings settings;
  int sampleRate = 0, channels = 0;
  std::vector<ComposedFrame> video;
  int64_t audioFrames = 0;
  std::vector<int64_t> audioPts;
  std::vector<int16_t> samples;
  bool contiguous = true;
  std::atomic<bool> finished{false};
};

class ManualScheduler : public IScheduler {
 public:
  void start(std::array<Stage*, kStageCount> s) override { stages_ = s; }
  void wake(StageId) override {}
  void stop() override { stopped_ = true; }
  void pumpOnce(StageId id) { stages_[static_cast<int>(id)]->pump(); }
  void runUntilIdle() {
    for (int guard = 0; guard < 100000 && !stopped_; ++guard) {
      bool progress = false;
      for (Stage* s : stages_) progress |= s->pump().kind == Progress::Kind::Did;
      if (!progress) return;
    }
  }

 private:
  std::array<Stage*, kStageCount> stages_{};
  bool stopped_ = false;
};

// Demuxers are made per clip, in timeline order; decoders per lane, with the first clip's faults.
class Platform : public PlatformFactory {
 public:
  explicit Platform(std::vector<Clip> c = {Clip{}}) : clips(std::move(c)) {}
  std::unique_ptr<IDemuxer> createDemuxer() override {
    return std::make_unique<Demuxer>(clips, clips[demuxers++ % clips.size()]);
  }
  std::unique_ptr<IImageLoader> createImageLoader() override { return std::make_unique<ImageLoader>(); }
  std::unique_ptr<IVideoDecoder> createVideoDecoder() override { return std::make_unique<VideoDecoder>(clips[0]); }
  std::unique_ptr<IAudioDecoder> createAudioDecoder() override { return std::make_unique<AudioDecoder>(); }
  std::unique_ptr<ISpeaker> createSpeaker() override {
    auto s = std::make_unique<Speaker>();
    speaker = s.get();
    return s;
  }
  std::unique_ptr<IDisplay> createDisplay() override {
    auto d = std::make_unique<Display>(clock_);
    display = d.get();
    return d;
  }
  std::unique_ptr<IScheduler> createScheduler() override {
    auto s = std::make_unique<ManualScheduler>();
    scheduler = s.get();
    return s;
  }
  std::unique_ptr<IExportSink> createExportSink() override {
    if (!exportSupported) return nullptr;
    auto s = std::make_unique<ExportSink>();
    s->busyEvery = sinkBusyEvery;
    exportSink = s.get();
    return s;
  }
  IClock& clock() override { return clock_; }

  bool exportSupported = true;
  int sinkBusyEvery = 0;
  ExportSink* exportSink = nullptr;
  std::vector<Clip> clips;
  size_t demuxers = 0;
  Clock& clockRef() { return clock_; }
  Display* display = nullptr;
  Speaker* speaker = nullptr;
  ManualScheduler* scheduler = nullptr;

 private:
  Clock clock_;
};

struct Listener : PlayerListener {
  void onStateChanged(State s) override { states.push_back(s); }
  void onError(Result r, const std::string&) override { errors.push_back(r); }
  void onWarning(Warning w, const std::string&) override { warnings.push_back(w); }
  void onFirstFrame() override { ++firstFrames; }
  void onSeekCompleted(int64_t pts) override { seeks.push_back(pts); }
  void onEnded() override { ++ended; }
  std::vector<State> states;
  std::vector<Result> errors;
  std::vector<Warning> warnings;
  std::vector<int64_t> seeks;
  int firstFrames = 0, ended = 0;
};

// A player on fake adapters, driven 1 ms at a time.
struct Harness {
  explicit Harness(Clip c = {}) : Harness(std::vector<Clip>{c}) {}
  explicit Harness(std::vector<Clip> clips) : platform(std::move(clips)) { player = Player::create(platform, &listener); }
  ~Harness() {
    if (player) player->shutdown();
  }
  void step() {
    platform.scheduler->runUntilIdle();
    platform.clockRef().now += kMs;
    if (platform.speaker) platform.speaker->tick(platform.clockRef().now);
  }
  void run(int ms) {
    for (int i = 0; i < ms; ++i) step();
  }
  Result open() { return player->open(MediaSource{}, RenderTarget{}); }
  const ComposedFrame& lastComposed() const { return platform.display->composed.back(); }
  Result openScene(const Scene& scene, OutputDriver driver = OutputDriver::Auto, std::string* error = nullptr) {
    return player->open(scene, RenderTarget{}, driver, error);
  }
  int64_t lastShown() const { return platform.display->shown.empty() ? -1 : platform.display->shown.back(); }

  Platform platform;
  Listener listener;
  std::unique_ptr<Player> player;
};

struct ExportRecorder : ExportListener {
  void onCompleted() override { ++completed; }
  void onError(Result r, const std::string&) override { errors.push_back(r); }
  int completed = 0;
  std::vector<Result> errors;
};

// An exporter on fake adapters, driven 1 ms at a time.
struct ExportHarness {
  explicit ExportHarness(std::vector<Clip> clips, bool supported = true, int busyEvery = 0) : platform(std::move(clips)) {
    platform.exportSupported = supported;
    platform.sinkBusyEvery = busyEvery;
    exporter = Exporter::create(platform, &listener);
  }
  ~ExportHarness() { exporter->shutdown(); }
  Result start(const Scene& scene, ExportSettings s = {}) { return exporter->start(scene, ExportTarget{}, s); }
  // Runs until the export completes or fails, or `ms` of fake time pass.
  void run(int ms) {
    for (int i = 0; i < ms && !exporter->done(); ++i) {
      platform.scheduler->runUntilIdle();
      platform.clockRef().now += kMs;
    }
  }

  Platform platform;
  ExportRecorder listener;
  std::unique_ptr<Exporter> exporter;
};

}  // namespace fake
