// The web platform's adapters. The decoders, display and timers are in library_mf.js; this side
// keeps the core's contracts (ordering, flush, Eos) and the frames' lifetimes.

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "mf/audio_ring.h"
#include "mf/cooperative_scheduler.h"
#include "mf/web.h"
#include "mp4_demuxer.h"
#include "mp4_muxer.h"

extern "C" {
// library_mf.js
void mf_js_request_run(void* scheduler, double delayMs);
void mf_js_scheduler_gone(void* scheduler);
int mf_js_vdec_create(void* onOutput);
int mf_js_vdec_configure(int id, const char* codec, const uint8_t* description, int length);
int mf_js_vdec_decode(int id, const uint8_t* data, int length, double ptsUs, int key);
void mf_js_vdec_flush_eos(int id);
void mf_js_vdec_reset(int id);
int mf_js_vdec_dequeue(int id, double* ptsUs);
void mf_js_vdec_destroy(int id);
void mf_js_frame_close(int handle);
int mf_js_adec_create(void* onOutput);
int mf_js_adec_configure(int id, const char* codec, const uint8_t* description, int length, int sampleRate, int channels);
int mf_js_adec_decode(int id, const uint8_t* data, int length, double ptsUs);
void mf_js_adec_flush_eos(int id);
void mf_js_adec_reset(int id);
int mf_js_adec_dequeue(int id, double* ptsUs, int* channels);
void mf_js_adec_take(int id, int16_t* out);
void mf_js_adec_destroy(int id);
int mf_js_display_attach(const char* selector, void* display);
void mf_js_display_detach(void* display);
int mf_js_display_visible();
void mf_js_display_draw(const char* frameJson);
void mf_js_speaker_track(int context, void* speaker);
void mf_js_speaker_untrack(void* speaker);
void mf_js_speaker_suspend(int context);
int mf_js_export_open(void* sink, int width, int height, int fps, int videoBitrate, int sampleRate, int channels, int audioBitrate);
int mf_js_export_video(const char* frameJson, double ptsUs, double durationUs, int key);
int mf_js_export_audio(const int16_t* pcm, int frames, double ptsUs);
void mf_js_export_finish(void* sink);
void mf_js_export_close();
}

namespace mf::web {
namespace {

constexpr int64_t kMs = 1000000;  // ns

// Absolute time (performance.timeOrigin + performance.now()), as emscripten_get_now() is with
// threads: the player's thread and the page's have different time origins, so library_mf.js
// makes rAF timestamps and the audio clock mapping absolute too.
class WebClock : public IClock {
 public:
  int64_t nowNs() const override { return int64_t(emscripten_get_now() * 1e6); }
};

// A CooperativeScheduler whose runs are timers on the page's event loop.
class WebScheduler : public CooperativeScheduler {
 public:
  explicit WebScheduler(IClock& clock) : CooperativeScheduler(clock, [this](int64_t delayNs) { mf_js_request_run(this, double(delayNs) / 1e6); }) {}
  ~WebScheduler() override { mf_js_scheduler_gone(this); }  // its pending timers find it gone
};

// --- Composed frames as JSON, for the compositor (library_mf_compositor.js) ---

void num(std::string& s, double v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.6g", std::isfinite(v) ? v : 0.0);
  s += b;
}
void key(std::string& s, const char* k) {
  if (s.back() != '{') s += ',';
  s += '"';
  s += k;
  s += "\":";
}
void field(std::string& s, const char* k, double v) {
  key(s, k);
  num(s, v);
}
void color(std::string& s, const char* k, const Color& c) {
  key(s, k);
  s += '[';
  for (float v : {c.r, c.g, c.b, c.a}) num(s, v), s += ',';
  s.back() = ']';
}
void string(std::string& s, const char* k, const std::string& v) {
  key(s, k);
  s += '"';
  for (unsigned char c : v) {
    if (c == '"' || c == '\\') {
      s += '\\';
      s += char(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      s += b;
    } else {
      s += char(c);
    }
  }
  s += '"';
}
void effects(std::string& s, const ComposedEffects& e) {
  key(s, "fx");
  s += "{\"crop\":[";
  for (float v : e.crop) num(s, v), s += ',';
  s.back() = ']';
  field(s, "brightness", e.brightness);
  field(s, "contrast", e.contrast);
  field(s, "saturation", e.saturation);
  field(s, "blur", e.blur);
  if (e.chromaKey) {
    key(s, "key");
    s += '{';
    color(s, "color", e.keyColor);
    field(s, "tolerance", e.keyTolerance);
    field(s, "softness", e.keySoftness);
    s += '}';
  }
  s += '}';
}

std::string frameJson(const ComposedFrame& f) {
  std::string s = "{";
  field(s, "width", f.width);
  field(s, "height", f.height);
  color(s, "background", f.background);
  key(s, "filter");
  s += '[';
  num(s, f.filter.brightness);
  s += ',';
  num(s, f.filter.contrast);
  s += ']';
  key(s, "layers");
  s += '[';
  for (const ComposedLayer& l : f.layers) {
    s += '{';
    field(s, "kind", int(l.kind));
    if (l.frame.image) field(s, "h", *static_cast<int*>(l.frame.image.get()));
    field(s, "group", l.group);
    field(s, "fit", int(l.fit));
    field(s, "x", l.x);
    field(s, "y", l.y);
    field(s, "anchorX", l.anchorX);
    field(s, "anchorY", l.anchorY);
    field(s, "scale", l.scale);
    field(s, "rotation", l.rotation);
    field(s, "flipX", l.flipX);
    field(s, "offsetX", l.offsetX);
    field(s, "offsetY", l.offsetY);
    key(s, "clip");
    s += '[';
    for (float v : l.clip) num(s, v), s += ',';
    s.back() = ']';
    field(s, "opacity", l.opacity);
    field(s, "blend", int(l.blend));
    color(s, "color", l.color);
    effects(s, l.effects);
    if (l.kind == ComposedLayer::Kind::Text && l.text) {
      string(s, "text", *l.text);
      key(s, "style");
      s += '{';
      string(s, "font", l.style.font);
      field(s, "size", l.style.size);
      color(s, "color", l.style.color);
      field(s, "align", int(l.style.align));
      field(s, "hasBox", l.style.hasBox);
      color(s, "box", l.style.box);
      field(s, "maxWidth", l.style.maxWidth);
      s += '}';
    }
    s += "},";
  }
  if (s.back() == ',') s.back() = ']';
  else s += ']';
  key(s, "groups");
  s += '[';
  for (const ComposedGroup& g : f.groups) {
    s += '{';
    field(s, "opacity", g.opacity);
    field(s, "blend", int(g.blend));
    effects(s, g.effects);
    s += "},";
  }
  if (s.back() == ',') s.back() = ']';
  else s += ']';
  return s + "}";
}

// A frame the display can draw: the handle of a WebCodecs VideoFrame, closed with the last reference.
std::shared_ptr<void> frameHandle(int handle) {
  return std::shared_ptr<void>(new int(handle), [](void* p) {
    mf_js_frame_close(*static_cast<int*>(p));
    delete static_cast<int*>(p);
  });
}

// IVideoDecoder on WebCodecs. Frames come out in presentation order (WebCodecs reorders), so the
// only state kept here is the serial: reset() drops every earlier output, so what comes after a
// flush() belongs to the packets queued since.
class WebVideoDecoder : public IVideoDecoder {
 public:
  WebVideoDecoder() : id_(mf_js_vdec_create(&onOutput_)) {}
  ~WebVideoDecoder() override { mf_js_vdec_destroy(id_); }

  Result configure(const TrackInfo& track, std::function<void()> onOutput) override {
    onOutput_ = std::move(onOutput);
    auto config = std::static_pointer_cast<CodecConfig>(track.format);
    if (!config) return Result::NoDecoder;
    int ok = mf_js_vdec_configure(id_, config->codec.c_str(), config->description.data(), int(config->description.size()));
    return ok ? Result::Ok : Result::NoDecoder;
  }

  Result queue(const Packet& p) override {
    switch (mf_js_vdec_decode(id_, p.data.data(), int(p.data.size()), double(p.ptsUs), p.key)) {
      case 0:
        serial_ = p.serial;
        return Result::Ok;
      case 1:
        return Result::Again;
      default:
        return Result::DecoderFailed;
    }
  }

  void signalEos() override { mf_js_vdec_flush_eos(id_); }

  Result dequeue(VideoFrame* out) override {
    double pts = 0;
    int h = mf_js_vdec_dequeue(id_, &pts);
    if (h > 0) {
      out->ptsUs = std::llround(pts);
      out->serial = serial_;
      out->image = frameHandle(h);
      return Result::Ok;
    }
    if (h == -3) return Result::CorruptFrame;  // a decode error: the core skips to the next keyframe
    return h == 0 ? Result::Again : h == -1 ? Result::Eos : Result::DecoderFailed;
  }

  void flush() override { mf_js_vdec_reset(id_); }

 private:
  std::function<void()> onOutput_;  // before id_: library_mf.js holds a pointer to it
  int id_;
  uint32_t serial_ = 0;
};

// IAudioDecoder on WebCodecs: PCM comes out as S16 interleaved, in packet order.
class WebAudioDecoder : public IAudioDecoder {
 public:
  WebAudioDecoder() : id_(mf_js_adec_create(&onOutput_)) {}
  ~WebAudioDecoder() override { mf_js_adec_destroy(id_); }

  Result configure(const TrackInfo& track, std::function<void()> onOutput) override {
    onOutput_ = std::move(onOutput);
    auto config = std::static_pointer_cast<CodecConfig>(track.format);
    if (!config) return Result::NoDecoder;
    int ok = mf_js_adec_configure(id_, config->codec.c_str(), config->description.data(), int(config->description.size()),
                                  track.sampleRate, track.channels);
    return ok ? Result::Ok : Result::NoDecoder;
  }

  Result queue(const Packet& p) override {
    int r = mf_js_adec_decode(id_, p.data.data(), int(p.data.size()), double(p.ptsUs));
    return r == 0 ? Result::Ok : r == 1 ? Result::Again : Result::DecoderFailed;
  }

  void signalEos() override { mf_js_adec_flush_eos(id_); }

  Result dequeue(PcmBuffer* out) override {
    double pts = 0;
    int channels = 0;
    int frames = mf_js_adec_dequeue(id_, &pts, &channels);
    if (frames > 0) {
      out->ptsUs = std::llround(pts);
      out->samples.resize(size_t(frames) * size_t(channels));
      mf_js_adec_take(id_, out->samples.data());
      return Result::Ok;
    }
    if (frames == -3) {  // audio lost to a decode error: no samples; the mixer fills the gap with silence
      out->samples.clear();
      return Result::CorruptFrame;
    }
    return frames == 0 ? Result::Again : frames == -1 ? Result::Eos : Result::DecoderFailed;
  }

  void flush() override { mf_js_adec_reset(id_); }

 private:
  std::function<void()> onOutput_;
  int id_;
};

// IDisplay on a WebGPU canvas. A handed-over frame waits for the refresh it was composed for.
// What is drawn in a rAF callback reaches the screen about a refresh later, so each callback
// draws the latest frame due by the refresh after it, and reports earlier ones as never shown;
// that next refresh is the drawn frame's present time, and a refresh is the display's latency.
class WebDisplay : public IDisplay {
 public:
  ~WebDisplay() override { mf_js_display_detach(this); }  // its animation frames stop calling tick()

  Result attach(const RenderTarget& target, PresentedFn fn) override {
    presented_ = std::move(fn);
    // No such canvas, or no WebGPU device: Unsupported.
    return mf_js_display_attach(static_cast<const char*>(target.native), this) ? Result::Ok : Result::Unsupported;
  }

  void present(const ComposedFrame& f, int64_t hostTimeNs) override {
    pending_.push_back({f, hostTimeNs});
    while (pending_.size() > 8) {  // the display isn't refreshing (hidden): don't hold frames forever
      presented_(pending_.front().frame.ptsUs, 0);
      pending_.pop_front();
    }
  }

  bool visible() const override { return mf_js_display_visible(); }
  int64_t vsyncPeriodNs() const override { return periodNs_; }
  int64_t latencyNs() const override { return periodNs_; }
  int64_t vsyncGridNs() const override { return lastRafNs_; }

  // A requestAnimationFrame callback at `rafNs`.
  void tick(int64_t rafNs) {
    // The refresh period: the median of the last intervals, which a stall doesn't move. The one
    // reported changes only when the refresh rate does (more than 5%): A/V sync re-anchors its
    // slots on every change.
    if (lastRafNs_ > 0) {
      intervals_[nextInterval_++ % intervals_.size()] = rafNs - lastRafNs_;
      size_t n = std::min(nextInterval_, intervals_.size());
      std::array<int64_t, 15> sorted = intervals_;
      std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.begin() + n);
      int64_t median = sorted[n / 2];
      if (median > 4 * kMs && std::llabs(median - periodNs_) * 20 > periodNs_) periodNs_ = median;
    }
    lastRafNs_ = rafNs;
    int due = -1;
    int64_t shownNs = rafNs + periodNs_;  // when what is drawn now reaches the screen
    for (int i = 0; i < int(pending_.size()); ++i) {
      if (pending_[size_t(i)].atNs <= shownNs + periodNs_ / 2) due = i;
    }
    if (due < 0) return;
    for (int i = 0; i < due; ++i) presented_(pending_[size_t(i)].frame.ptsUs, 0);  // superseded before its refresh
    Pending p = std::move(pending_[size_t(due)]);
    pending_.erase(pending_.begin(), pending_.begin() + due + 1);
    draw(p.frame);
    presented_(p.frame.ptsUs, shownNs);
  }

 private:
  struct Pending {
    ComposedFrame frame;
    int64_t atNs;
  };

  void draw(const ComposedFrame& f) { mf_js_display_draw(frameJson(f).c_str()); }

  PresentedFn presented_;
  std::deque<Pending> pending_;
  int64_t periodNs_ = 16666667, lastRafNs_ = 0;
  std::array<int64_t, 15> intervals_{};
  size_t nextInterval_ = 0;
};

// ISpeaker on an AudioWorklet that runs C++ on the audio thread: it pulls from the AudioRing in
// the shared memory, as the Core Audio render callback does on macOS. Browsers allow an
// AudioContext only on the page's thread, so the speaker's calls are carried out there; the
// player's thread only writes the ring. The AudioContext and the worklet start asynchronously;
// until then the master clock holds, as before audio is first heard.
class WebSpeaker : public ISpeaker {
 public:
  ~WebSpeaker() override { onPage(&WebSpeaker::destroyOnPage, true); }

  Result open(int sampleRate, int channels, AudioRing* ring) override {
    ring_ = ring;
    channels_ = channels;
    sampleRate_ = sampleRate;
    scratch_.assign(size_t(kMaxQuantum) * size_t(channels), 0);
    onPage(&WebSpeaker::openOnPage);
    return Result::Ok;  // a failure to start shows as audio never heard: the clock falls back
  }

  Result start() override {
    wantRunning_ = true;
    onPage(&WebSpeaker::resumeOnPage);
    return Result::Ok;
  }

  void pause() override {
    wantRunning_ = false;
    onPage(&WebSpeaker::suspendOnPage);
  }

  // From library_mf.js on the page's thread: audio rendered at context time `contextSec` is heard
  // at `performanceMs` (AudioContext.getOutputTimestamp, or now + its latency before that).
  void setClock(double contextSec, double performanceMs) {
    seq_.fetch_add(1, std::memory_order_acq_rel);
    contextNs_.store(int64_t(contextSec * 1e9), std::memory_order_relaxed);
    perfNs_.store(int64_t(performanceMs * 1e6), std::memory_order_relaxed);
    seq_.fetch_add(1, std::memory_order_acq_rel);
  }

 private:
  static constexpr int kMaxQuantum = 4096;

  // Runs fn on the page's thread: at once when already there; `wait`: before returning.
  void onPage(void (WebSpeaker::*fn)(), bool wait = false) {
    if (emscripten_is_main_runtime_thread()) return (this->*fn)();
    struct Call {
      WebSpeaker* self;
      void (WebSpeaker::*fn)();
    };
    auto run = [](void* p) {
      auto* c = static_cast<Call*>(p);
      (c->self->*c->fn)();
      delete c;
    };
    pthread_t page = emscripten_main_runtime_thread_id();
    if (wait) {
      emscripten_proxy_sync(emscripten_proxy_get_system_queue(), page, run, new Call{this, fn});
    } else {
      emscripten_proxy_async(emscripten_proxy_get_system_queue(), page, run, new Call{this, fn});
    }
  }

  void openOnPage() {
    EmscriptenWebAudioCreateAttributes attrs{"interactive", uint32_t(sampleRate_), AUDIO_CONTEXT_RENDER_SIZE_DEFAULT};
    context_ = emscripten_create_audio_context(&attrs);
    if (context_) emscripten_start_wasm_audio_worklet_thread_async(context_, stack_, sizeof(stack_), &WebSpeaker::onThread, this);
  }
  void resumeOnPage() {
    if (node_) emscripten_resume_audio_context_sync(context_);
  }
  void suspendOnPage() {
    if (context_) mf_js_speaker_suspend(context_);
  }
  void destroyOnPage() {
    mf_js_speaker_untrack(this);
    if (context_) emscripten_destroy_audio_context(context_);
  }

  static void onThread(EMSCRIPTEN_WEBAUDIO_T context, bool ok, void* self) {
    if (!ok) return;
    WebAudioWorkletProcessorCreateOptions opts{"mf-speaker", 0, nullptr};
    emscripten_create_wasm_audio_worklet_processor_async(context, &opts, &WebSpeaker::onProcessor, self);
  }

  static void onProcessor(EMSCRIPTEN_WEBAUDIO_T context, bool ok, void* p) {
    if (!ok) return;
    auto* self = static_cast<WebSpeaker*>(p);
    int counts[1] = {self->channels_};
    EmscriptenAudioWorkletNodeCreateOptions opts{0, 1, counts, 0, WEBAUDIO_CHANNEL_COUNT_MODE_MAX, WEBAUDIO_CHANNEL_INTERPRETATION_SPEAKERS};
    self->node_ = emscripten_create_wasm_audio_worklet_node(context, "mf-speaker", &opts, &WebSpeaker::process, self);
    emscripten_audio_node_connect(self->node_, context, 0, 0);
    mf_js_speaker_track(context, self);
    if (self->wantRunning_) emscripten_resume_audio_context_sync(context);
  }

  // The audio thread: no locks, no allocation.
  static bool process(int, const AudioSampleFrame*, int numOutputs, AudioSampleFrame* outputs, int, const AudioParamFrame*, void* p) {
    auto* self = static_cast<WebSpeaker*>(p);
    if (numOutputs < 1) return true;
    AudioSampleFrame& out = outputs[0];
    int frames = std::min(out.samplesPerChannel, kMaxQuantum);
    int channels = std::min(out.numberOfChannels, self->channels_);
    double contextSec = EM_ASM_DOUBLE({ return currentTime; });
    self->ring_->consume(self->scratch_.data(), frames, self->audibleNs(contextSec));
    for (int c = 0; c < out.numberOfChannels; ++c) {
      float* dst = out.data + size_t(c) * size_t(out.samplesPerChannel);
      int src = std::min(c, channels - 1);
      for (int i = 0; i < frames; ++i) dst[i] = float(self->scratch_[size_t(i * self->channels_ + src)]) / 32768.0f;
    }
    return true;
  }

  int64_t audibleNs(double contextSec) const {
    int64_t ctx, perf;
    uint32_t before, after;
    do {
      before = seq_.load(std::memory_order_acquire);
      ctx = contextNs_.load(std::memory_order_relaxed);
      perf = perfNs_.load(std::memory_order_relaxed);
      after = seq_.load(std::memory_order_acquire);
    } while (before != after || (before & 1));
    return perf + (int64_t(contextSec * 1e9) - ctx);
  }

  AudioRing* ring_ = nullptr;
  int channels_ = 2, sampleRate_ = 48000;
  EMSCRIPTEN_WEBAUDIO_T context_ = 0, node_ = 0;  // on the page's thread
  std::atomic<bool> wantRunning_{false};
  std::vector<int16_t> scratch_;
  std::atomic<uint32_t> seq_{0};
  std::atomic<int64_t> contextNs_{0}, perfNs_{0};
  alignas(16) uint8_t stack_[16384];
};

// IExportSink on WebCodecs encoders and the portable MP4 muxer. A frame is drawn by the compositor
// into an export-sized canvas and encoded; audio is encoded from the mixed PCM. Encoded chunks come
// back (mf_web_export_chunk) to be muxed; the file goes to the ExportTarget, a MemoryWriter. Writes
// return Again while an encoder's queue is full: the core retries.
class WebExportSink : public IExportSink {
 public:
  ~WebExportSink() override { mf_js_export_close(); }

  Result open(const ExportTarget& target, const ExportSettings& s, int sampleRate, int channels) override {
    writer_ = static_cast<MemoryWriter*>(target.native.get());
    if (!writer_) return Result::WriteFailed;
    muxer_ = std::make_unique<Mp4Muxer>(*writer_);
    fps_ = std::max(1, s.fps);
    frameUs_ = 1000000 / fps_;
    width_ = s.width;
    height_ = s.height;
    sampleRate_ = sampleRate;
    channels_ = channels;
    return mf_js_export_open(this, s.width, s.height, fps_, s.videoBitrate, sampleRate, channels, s.audioBitrate) ? Result::Ok : Result::Unsupported;
  }

  Result writeVideo(const ComposedFrame& f) override {
    if (failed_) return Result::WriteFailed;
    int r = mf_js_export_video(frameJson(f).c_str(), double(f.ptsUs), double(frameUs_), frames_ % (2 * fps_) == 0);
    if (r == 1) return Result::Again;
    if (r != 0) return Result::WriteFailed;
    ++frames_;
    return Result::Ok;
  }

  Result writeAudio(const int16_t* pcm, int frames, int64_t ptsUs) override {
    if (failed_) return Result::WriteFailed;
    int r = mf_js_export_audio(pcm, frames, double(ptsUs));
    return r == 0 ? Result::Ok : r == 1 ? Result::Again : Result::WriteFailed;
  }

  void finish(std::function<void(Result)> done) override {
    done_ = std::move(done);
    mf_js_export_finish(this);  // flushes the encoders, then mf_web_export_finished()
  }

  // From library_mf.js: an encoded chunk (track 0 video, 1 audio), with the codec's description
  // (avcC, AudioSpecificConfig) on a track's first chunk.
  void chunk(int track, const uint8_t* data, int size, double ptsUs, double durationUs, int key, const uint8_t* config, int configSize) {
    if (ids_[track] < 0) {
      Mp4Muxer::Track t;
      t.video = track == 0;
      t.width = width_;
      t.height = height_;
      t.sampleRate = sampleRate_;
      t.channels = channels_;
      t.config.assign(config, config + configSize);
      ids_[track] = muxer_->addTrack(t);
    }
    if (!muxer_->write(ids_[track], data, size_t(size), std::llround(ptsUs), std::llround(durationUs), key)) failed_ = true;
  }

  void finished(bool ok) {
    ok = ok && !failed_ && muxer_->finish();
    if (done_) done_(ok ? Result::Ok : Result::WriteFailed);
  }

 private:
  MemoryWriter* writer_ = nullptr;
  std::unique_ptr<Mp4Muxer> muxer_;
  std::function<void(Result)> done_;
  int ids_[2] = {-1, -1};
  int fps_ = 30, width_ = 0, height_ = 0, sampleRate_ = 0, channels_ = 0;
  int64_t frameUs_ = 33333, frames_ = 0;
  bool failed_ = false;
};

// Images are decoded by the page before the scene opens (the core loads them synchronously, the
// browser decodes asynchronously): an image source names the decoded ImageBitmap.
class WebImageLoader : public IImageLoader {
 public:
  Result load(const MediaSource& source, VideoFrame* out, int* width, int* height) override {
    auto image = std::dynamic_pointer_cast<ImageSource>(std::static_pointer_cast<ByteSource>(source.native));
    if (!image) return Result::UnsupportedFormat;
    out->image = std::make_shared<int>(image->handle);  // stays decoded for the session
    *width = image->width;
    *height = image->height;
    return Result::Ok;
  }
};

class WebPlatform : public PlatformFactory {
 public:
  std::unique_ptr<IImageLoader> createImageLoader() override { return std::make_unique<WebImageLoader>(); }
  std::unique_ptr<IExportSink> createExportSink() override { return std::make_unique<WebExportSink>(); }
  std::unique_ptr<IDemuxer> createDemuxer() override { return createMp4Demuxer(); }
  std::unique_ptr<IVideoDecoder> createVideoDecoder() override { return std::make_unique<WebVideoDecoder>(); }
  std::unique_ptr<IAudioDecoder> createAudioDecoder() override { return std::make_unique<WebAudioDecoder>(); }
  std::unique_ptr<ISpeaker> createSpeaker() override { return std::make_unique<WebSpeaker>(); }
  std::unique_ptr<IDisplay> createDisplay() override { return std::make_unique<WebDisplay>(); }
  std::unique_ptr<IScheduler> createScheduler() override { return std::make_unique<WebScheduler>(clock_); }
  IClock& clock() override { return clock_; }

 private:
  WebClock clock_;
};

}  // namespace

std::unique_ptr<PlatformFactory> createPlatform() { return std::make_unique<WebPlatform>(); }

}  // namespace mf::web

// Called from library_mf.js.
extern "C" {
EMSCRIPTEN_KEEPALIVE void mf_web_run(void* scheduler) { static_cast<mf::CooperativeScheduler*>(scheduler)->run(); }
EMSCRIPTEN_KEEPALIVE void mf_web_output(void* onOutput) {
  auto* fn = static_cast<std::function<void()>*>(onOutput);
  if (*fn) (*fn)();
}
EMSCRIPTEN_KEEPALIVE void mf_web_display_tick(void* display, double rafMs) {
  static_cast<mf::web::WebDisplay*>(display)->tick(int64_t(rafMs * 1e6));
}
EMSCRIPTEN_KEEPALIVE void mf_web_export_chunk(void* sink, int track, const uint8_t* data, int size, double ptsUs, double durationUs,
                                              int key, const uint8_t* config, int configSize) {
  static_cast<mf::web::WebExportSink*>(sink)->chunk(track, data, size, ptsUs, durationUs, key, config, configSize);
}
EMSCRIPTEN_KEEPALIVE void mf_web_export_finished(void* sink, int ok) { static_cast<mf::web::WebExportSink*>(sink)->finished(ok); }
EMSCRIPTEN_KEEPALIVE void mf_web_speaker_clock(void* speaker, double contextSec, double performanceMs) {
  static_cast<mf::web::WebSpeaker*>(speaker)->setClock(contextSec, performanceMs);
}
}
