#include "pipeline.h"

#include <algorithm>

#include "av_sync.h"

namespace mf {
namespace {

constexpr int64_t kMs = 1000000;              // ns
constexpr size_t kMaxSampleBytes = 16u << 20;  // A8
constexpr int kMaxDimension = 8192;            // A8

// ---------------------------------------------------------------------------------------
// T1: probe on open, start seeks, demux. Reads whichever track has room and the lowest
// decode time, so a full video queue never stops audio from being read (§2.3).
class SourceStage : public Stage {
 public:
  explicit SourceStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted) return Progress::idle();
    if (!opened_) {
      if (!ctx_.openRequested) return Progress::idle();
      opened_ = true;
      probe();
      return Progress::did();
    }
    // Start the latest pending seek once the previous one has been shown.
    if (ctx_.shownSerial == ctx_.serial) {
      if (auto seek = ctx_.takeSeek()) {
        startSeek(*seek);
        return Progress::did();
      }
    }
    return readOne();
  }

 private:
  void probe() {
    MediaInfo info;
    Result r = ctx_.demuxer->open(ctx_.source, &info);
    if (r != Result::Ok) return ctx_.fatal(r, "cannot open media");
    const TrackInfo& v = info.video;
    if (!v.supported) return ctx_.fatal(Result::NoDecoder, "video codec is not H.264");
    if (v.width <= 0 || v.height <= 0 || v.width > kMaxDimension || v.height > kMaxDimension || v.frameDurationUs <= 0) {
      return ctx_.fatal(Result::MalformedMedia, "bad video dimensions or frame rate");
    }
    if (v.rotated) ctx_.events.onWarning(Warning::RotationIgnored, "track matrix is not identity");

    r = ctx_.videoDecoder->configure(v, [this] { ctx_.wake(StageId::VideoDecode); });
    if (r != Result::Ok) return ctx_.fatal(r, "video decoder configuration failed");

    bool audio = false;
    if (info.audio) {
      const TrackInfo& a = *info.audio;
      if (!a.supported || a.sampleRate <= 0 || a.channels <= 0 || ctx_.audioDecoder->configure(a) != Result::Ok) {
        ctx_.events.onWarning(Warning::AudioUnsupported, "audio is not AAC-LC; playing video only");
      } else {
        ctx_.ring.open(a.sampleRate, a.channels, a.sampleRate / 5);  // 200 ms
        if (ctx_.speaker->open(a.sampleRate, a.channels, &ctx_.ring) != Result::Ok) {
          return ctx_.fatal(Result::AudioDeviceFailed, "audio output failed to open");
        }
        audio = true;
      }
    }
    ctx_.info = info;
    ctx_.hasAudio = audio;
    ctx_.master.setAudio(audio);
    ctx_.durationUs = info.durationUs;
    ctx_.requestSeek(0);  // preroll the first frame (A16)
  }

  void startSeek(const PendingSeek& seek) {
    ctx_.setSeekTarget(seek);
    ctx_.videoPackets.flush();
    ctx_.audioPackets.flush();
    Result r = ctx_.demuxer->seekTo(seek.targetUs);
    if (r != Result::Ok) return ctx_.fatal(r, "seek failed");
    eos_[kVideo] = eos_[kAudio] = false;
    serial_ = ctx_.serial + 1;
    ctx_.serial = serial_;  // after the target is set: downstream reads it on seeing the new serial
    ctx_.wakeAll();
  }

  // Audio packets are read (to keep the demuxer's tracks balanced) but dropped when audio is off.
  BoundedQueue<Packet>* queueFor(int track) {
    if (track == kVideo) return &ctx_.videoPackets;
    return ctx_.hasAudio ? &ctx_.audioPackets : nullptr;
  }

  Progress readOne() {
    int best = -1;
    int64_t bestDts = 0;
    for (int track : {kVideo, kAudio}) {
      if (eos_[track] || (track == kAudio && !ctx_.info.audio)) continue;
      BoundedQueue<Packet>* q = queueFor(track);
      if (q && q->full()) continue;
      int64_t dts = 0;
      Result r = ctx_.demuxer->peekDtsUs(track, &dts);
      if (r == Result::Eos) {
        pushEos(track);
        return Progress::did();
      }
      if (r != Result::Ok) {
        ctx_.fatal(r, "demux failed");
        return Progress::idle();
      }
      if (best < 0 || dts < bestDts) {
        best = track;
        bestDts = dts;
      }
    }
    if (best < 0) return Progress::idle();  // queues full or both tracks ended

    Packet p;
    Result r = ctx_.demuxer->read(best, &p);
    if (r == Result::Eos) {
      pushEos(best);
      return Progress::did();
    }
    if (r != Result::Ok || p.data.size() > kMaxSampleBytes) {
      ctx_.fatal(Result::MalformedMedia, "bad sample");
      return Progress::idle();
    }
    p.serial = serial_;
    if (BoundedQueue<Packet>* q = queueFor(best)) q->tryPush(p);  // has room: checked above
    return Progress::did();
  }

  void pushEos(int track) {
    eos_[track] = true;
    if (BoundedQueue<Packet>* q = queueFor(track)) {
      Packet eos;
      eos.track = track;
      eos.eos = true;
      eos.serial = serial_;
      q->tryPush(eos);
    }
  }

  Context& ctx_;
  bool opened_ = false;
  bool eos_[2] = {false, false};
  uint32_t serial_ = 0;
};

// ---------------------------------------------------------------------------------------
// T2: packets into the decoder, frames (PTS order) into the frame queue.
class VideoDecodeStage : public Stage {
 public:
  explicit VideoDecodeStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted) return Progress::idle();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) {
      serial_ = serial;
      ctx_.videoDecoder->flush();
      input_.reset();
      output_.reset();
      skipToKey_ = eosSent_ = false;
    }
    bool did = drainOutput();
    did |= feedInput();
    return did ? Progress::did() : Progress::idle();
  }

 private:
  bool drainOutput() {
    bool did = false;
    if (!output_) {
      VideoFrame f;
      switch (ctx_.videoDecoder->dequeue(&f)) {
        case Result::Ok:
          if (f.serial == serial_) output_ = std::move(f);
          did = true;
          break;
        case Result::Eos:
          if (!eosSent_) {
            eosSent_ = true;
            output_ = VideoFrame{};
            output_->eos = true;
            output_->serial = serial_;
          }
          break;
        case Result::CorruptFrame:  // hold the last good frame; skip to the next keyframe (A14)
          ctx_.metrics.countCorrupt();
          skipToKey_ = true;
          did = true;
          break;
        case Result::Again:
          break;
        default:
          ctx_.fatal(Result::DecoderFailed, "video decoder failed");
          return false;
      }
    }
    if (output_ && ctx_.frames.tryPush(*output_)) {
      output_.reset();
      did = true;
    }
    return did;
  }

  bool feedInput() {
    bool did = false;
    if (!input_) {
      Packet p;
      if (!ctx_.videoPackets.tryPop(&p)) return false;
      did = true;
      if (p.serial != serial_) return true;
      input_ = std::move(p);
    }
    if (input_->eos) {
      ctx_.videoDecoder->signalEos();
      input_.reset();
      return true;
    }
    if (skipToKey_ && !input_->key) {
      ctx_.metrics.countCorrupt();
      input_.reset();
      return true;
    }
    switch (ctx_.videoDecoder->queue(*input_)) {
      case Result::Ok:
        skipToKey_ = false;
        input_.reset();
        return true;
      case Result::Again:  // decoder full; its output callback wakes us
        return did;
      case Result::CorruptFrame:
        ctx_.metrics.countCorrupt();
        skipToKey_ = true;
        input_.reset();
        return true;
      default:
        ctx_.fatal(Result::DecoderFailed, "video decoder failed");
        return false;
    }
  }

  Context& ctx_;
  uint32_t serial_ = 0;
  std::optional<Packet> input_;
  std::optional<VideoFrame> output_;
  bool skipToKey_ = false, eosSent_ = false;
};

// ---------------------------------------------------------------------------------------
// T3: completes seeks, runs AvSync, presents or drops. Also starts and stops the output
// (speaker + master clock), so play/pause never race a seek that is still in flight.
class VideoRenderStage : public Stage {
 public:
  explicit VideoRenderStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted) return Progress::idle();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) {
      serial_ = serial;
      current_.reset();
      candidate_.reset();
      videoEnded_ = false;
    }
    bool seeking = ctx_.shownSerial != serial_;
    updateOutput(!seeking && !ctx_.hasPendingSeek());

    if (!current_) {
      VideoFrame f;
      if (!ctx_.frames.tryPop(&f)) return videoEnded_ ? checkEnd() : Progress::idle();
      if (f.serial != serial_) return Progress::did();
      current_ = std::move(f);
    }
    if (seeking) return seekStep();
    if (current_->eos) {
      videoEnded_ = true;
      current_.reset();
      return checkEnd();
    }
    if (!outputRunning_) return Progress::idle();
    return renderStep();
  }

 private:
  void updateOutput(bool seekDone) {
    int64_t now = ctx_.hostClock.nowNs();
    bool failed = false;
    {
      std::lock_guard<std::mutex> lock(ctx_.playMu);
      bool want = ctx_.playing && seekDone;
      if (want && !ctx_.outputRunning) {
        if (ctx_.hasAudio && ctx_.speaker->start() != Result::Ok) {
          failed = true;
        } else {
          ctx_.master.start(now);
          ctx_.outputRunning = true;
          sync_.reset();
          ctx_.metrics.discontinuity();
        }
      } else if (!want && ctx_.outputRunning) {
        if (ctx_.hasAudio) ctx_.speaker->pause();
        ctx_.master.stop(now);
        ctx_.outputRunning = false;
      }
      outputRunning_ = ctx_.outputRunning;
    }
    if (failed) ctx_.fatal(Result::AudioDeviceFailed, "audio output failed to start");
  }

  // Exact seek (§4): decode from the keyframe, show the last frame with pts <= target.
  // While a newer seek is pending (scrubbing), show whatever is decodable now instead.
  Progress seekStep() {
    PendingSeek target = ctx_.seekTarget();
    if (current_->eos) {
      std::optional<VideoFrame> show = std::move(candidate_);
      candidate_.reset();
      current_.reset();
      videoEnded_ = true;
      complete(show ? &*show : nullptr, target);
      return Progress::did();
    }
    if (ctx_.hasPendingSeek()) {
      VideoFrame show = candidate_ ? std::move(*candidate_) : std::move(*current_);
      candidate_.reset();
      current_.reset();
      complete(&show, target);
      return Progress::did();
    }
    if (current_->ptsUs <= target.targetUs) {
      if (candidate_) ctx_.metrics.countDecodeOnly();
      candidate_ = std::move(current_);
      current_.reset();
      return Progress::did();
    }
    if (candidate_) {  // current_ is past the target: keep it for playback
      VideoFrame show = std::move(*candidate_);
      candidate_.reset();
      complete(&show, target);
    } else {
      VideoFrame show = std::move(*current_);
      current_.reset();
      complete(&show, target);
    }
    return Progress::did();
  }

  void complete(const VideoFrame* frame, const PendingSeek& target) {
    int64_t now = ctx_.hostClock.nowNs();
    int64_t shown = ctx_.shownPtsUs;
    if (frame) {
      ctx_.display->present(*frame, now);
      shown = frame->ptsUs;
    }
    ctx_.shownPtsUs = shown;
    ctx_.master.reset(shown, serial_);
    sync_.reset();
    if (serial_ > 1) ctx_.metrics.seekLatency(now - target.requestedNs);  // serial 1 is the preroll (TTFF)
    ctx_.shownSerial = serial_;
    ctx_.events.onSeekDone(serial_, shown, frame != nullptr);
    ctx_.wake(StageId::Source);
  }

  Progress renderStep() {
    int64_t now = ctx_.hostClock.nowNs();
    if (!ctx_.display->visible()) {
      ctx_.metrics.countHidden();
      current_.reset();
      return Progress::did();
    }
    int64_t vsync = ctx_.display->vsyncPeriodNs();
    // Decide as of the time the frame would actually reach the screen if handed over now.
    int64_t lead = ctx_.display->latencyNs();
    int64_t clockUs = ctx_.master.nowUs(now) + lead / 1000;
    SyncDecision d = sync_.decide(current_->ptsUs, clockUs, now + lead, vsync, ctx_.info.video.frameDurationUs,
                                  ctx_.display->vsyncGridNs());
    switch (d.kind) {
      case SyncDecision::Kind::LateDrop:
        ctx_.metrics.countLate();
        break;
      case SyncDecision::Kind::RateCapDrop:
        ctx_.metrics.countRateCap();
        break;
      case SyncDecision::Kind::Wait:
        // Capped so a clock that is holding (audio not started, underrun) is re-checked.
        return Progress::waitUntil(std::min(d.atNs - lead, now + 50 * kMs));
      case SyncDecision::Kind::Present:
        ctx_.metrics.planned(current_->ptsUs, d.slot, vsync);
        ctx_.display->present(*current_, d.atNs);
        ctx_.shownPtsUs = current_->ptsUs;
        break;
    }
    current_.reset();
    return Progress::did();
  }

  // End of stream once both tracks have finished (A5).
  Progress checkEnd() {
    if (!ctx_.playing || !outputRunning_) return Progress::idle();
    int64_t now = ctx_.hostClock.nowNs();
    if (ctx_.hasAudio && !ctx_.ring.drained(now)) return Progress::waitUntil(now + 10 * kMs);
    ctx_.events.onEnd();
    return Progress::idle();
  }

  Context& ctx_;
  AvSync sync_;
  uint32_t serial_ = 0;
  std::optional<VideoFrame> current_, candidate_;
  bool videoEnded_ = false;
  bool outputRunning_ = false;
};

// ---------------------------------------------------------------------------------------
// T4: decode audio and write it into the ring the speaker pulls from.
class AudioStage : public Stage {
 public:
  explicit AudioStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.hasAudio) return Progress::idle();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) {
      serial_ = serial;
      ctx_.audioDecoder->flush();
      pcm_.reset();
      trimUs_ = ctx_.seekTarget().targetUs;
      ctx_.ring.flush(trimUs_, serial_);
    }
    if (pcm_) {
      int channels = ctx_.ring.channels();
      int total = static_cast<int>(pcm_->samples.size()) / channels;
      offset_ += ctx_.ring.write(pcm_->samples.data() + size_t(offset_) * channels, total - offset_);
      if (offset_ < total) {
        // Ring full. The real-time consumer can't signal us, so poll while playing.
        return ctx_.playing ? Progress::waitUntil(ctx_.hostClock.nowNs() + 5 * kMs) : Progress::idle();
      }
      pcm_.reset();
    }

    Packet p;
    if (!ctx_.audioPackets.tryPop(&p)) return Progress::idle();
    if (p.serial != serial_) return Progress::did();
    if (p.eos) {
      ctx_.ring.markEos();
      return Progress::did();
    }
    PcmBuffer pcm;
    Result r = ctx_.audioDecoder->decode(p, &pcm);  // on CorruptFrame, pcm is silence (A14)
    if (r == Result::CorruptFrame) {
      ctx_.metrics.countCorrupt();
    } else if (r != Result::Ok) {
      ctx_.fatal(Result::DecoderFailed, "audio decoder failed");
      return Progress::idle();
    }
    // Exact seek: drop samples before the target.
    int channels = ctx_.ring.channels();
    int frames = static_cast<int>(pcm.samples.size()) / channels;
    int skip = 0;
    if (pcm.ptsUs < trimUs_) {
      skip = static_cast<int>(std::min<int64_t>(frames, (trimUs_ - pcm.ptsUs) * ctx_.ring.sampleRate() / 1000000));
    }
    if (skip < frames) {
      pcm_ = std::move(pcm);
      offset_ = skip;
    }
    return Progress::did();
  }

 private:
  Context& ctx_;
  uint32_t serial_ = 0;
  std::optional<PcmBuffer> pcm_;
  int offset_ = 0;
  int64_t trimUs_ = 0;
};

}  // namespace

Context::Context(PlatformFactory& factory, PipelineEvents& ev)
    : hostClock(factory.clock()),
      events(ev),
      demuxer(factory.createDemuxer()),
      videoDecoder(factory.createVideoDecoder()),
      audioDecoder(factory.createAudioDecoder()),
      speaker(factory.createSpeaker()),
      display(factory.createDisplay()),
      scheduler(factory.createScheduler()) {
  videoPackets.setHooks([this] { wake(StageId::VideoDecode); }, [this] { wake(StageId::Source); });
  audioPackets.setHooks([this] { wake(StageId::Audio); }, [this] { wake(StageId::Source); });
  frames.setHooks([this] { wake(StageId::VideoRender); }, [this] { wake(StageId::VideoDecode); });
}

void Context::wakeAll() {
  for (int i = 0; i < kStageCount; ++i) wake(static_cast<StageId>(i));
}

void Context::requestSeek(int64_t targetUs) {
  std::lock_guard<std::mutex> lock(seekMu_);
  pending_ = PendingSeek{targetUs, hostClock.nowNs()};
  hasPending_ = true;
}

std::optional<PendingSeek> Context::takeSeek() {
  std::lock_guard<std::mutex> lock(seekMu_);
  std::optional<PendingSeek> s = pending_;
  pending_.reset();
  hasPending_ = false;
  return s;
}

void Context::setSeekTarget(const PendingSeek& s) {
  std::lock_guard<std::mutex> lock(seekMu_);
  target_ = s;
}

PendingSeek Context::seekTarget() const {
  std::lock_guard<std::mutex> lock(seekMu_);
  return target_;
}

std::array<std::unique_ptr<Stage>, kStageCount> makeStages(Context& ctx) {
  return {std::make_unique<SourceStage>(ctx), std::make_unique<VideoDecodeStage>(ctx),
          std::make_unique<VideoRenderStage>(ctx), std::make_unique<AudioStage>(ctx)};
}

}  // namespace mf
