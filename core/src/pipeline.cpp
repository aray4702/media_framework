#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <string>

#include "av_sync.h"

namespace mf {
namespace {

constexpr int64_t kMs = 1000000;              // ns
constexpr size_t kMaxSampleBytes = 16u << 20;  // A8
constexpr int kMaxDimension = 8192;            // A8
constexpr int kNoClip = std::numeric_limits<int>::max();
constexpr int64_t kNever = std::numeric_limits<int64_t>::max();

std::string clipName(int clip) { return "clip " + std::to_string(clip + 1) + ": "; }

// ---------------------------------------------------------------------------------------
// T1: probe every clip on open, start seeks, demux. Each lane reads its clip; the lane
// whose clip ends moves on to clip + 2. Reads whichever track has room and the lowest
// timeline decode time, so a full queue never stops the other lane or track (§2.3).
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
    if (advanceLanes()) return Progress::did();
    return readOne();
  }

 private:
  struct LaneState {
    int clip = kNoClip;
    bool eos[2] = {true, true};
  };

  int clips() const { return static_cast<int>(ctx_.sources.size()); }

  void probe() {
    int n = clips();
    ctx_.infos.assign(n, MediaInfo{});
    ctx_.clipAudio.assign(n, 0);
    std::vector<int64_t> durations;
    std::optional<TrackInfo> output;  // the audio format the ring and speaker run at
    for (int c = 0; c < n; ++c) {
      ctx_.demuxers.push_back(ctx_.factory.createDemuxer());
      MediaInfo& info = ctx_.infos[c];
      Result r = ctx_.demuxers[c]->open(ctx_.sources[c], &info);
      if (r != Result::Ok) return ctx_.fatal(r, clipName(c) + "cannot open media");
      const TrackInfo& v = info.video;
      if (!v.supported) return ctx_.fatal(Result::NoDecoder, clipName(c) + "video codec is not H.264");
      if (v.width <= 0 || v.height <= 0 || v.width > kMaxDimension || v.height > kMaxDimension || v.frameDurationUs <= 0 ||
          info.durationUs <= 0) {
        return ctx_.fatal(Result::MalformedMedia, clipName(c) + "bad video dimensions, frame rate or duration");
      }
      if (v.rotated) ctx_.events.onWarning(Warning::RotationIgnored, clipName(c) + "track matrix is not identity");

      // Configure once here, so a stream the decoder rejects fails open() and not playback.
      Lane& lane = ctx_.lanes[Context::laneOf(c)];
      r = lane.videoDecoder->configure(v, [] {});
      if (r != Result::Ok) return ctx_.fatal(r, clipName(c) + "video decoder configuration failed");

      if (info.audio) {
        const TrackInfo& a = *info.audio;
        if (!a.supported || a.sampleRate <= 0 || a.channels <= 0 || lane.audioDecoder->configure(a) != Result::Ok) {
          ctx_.events.onWarning(Warning::AudioUnsupported, clipName(c) + "audio is not AAC-LC; the clip plays silent");
        } else if (output && (a.sampleRate != output->sampleRate || a.channels != output->channels)) {
          ctx_.events.onWarning(Warning::AudioUnsupported,
                                clipName(c) + "audio format differs from the first clip with audio; the clip plays silent");
        } else {
          if (!output) output = a;
          ctx_.clipAudio[c] = 1;
        }
      }
      durations.push_back(info.durationUs);
    }
    if (output) {
      ctx_.ring.open(output->sampleRate, output->channels, output->sampleRate / 5);  // 200 ms
      if (ctx_.speaker->open(output->sampleRate, output->channels, &ctx_.ring) != Result::Ok) {
        return ctx_.fatal(Result::AudioDeviceFailed, "audio output failed to open");
      }
    }
    ctx_.layout.build(durations, ctx_.transition);
    ctx_.hasAudio = output.has_value();
    ctx_.master.setAudio(ctx_.hasAudio);
    ctx_.durationUs = ctx_.layout.durationUs();
    ctx_.probed = true;
    ctx_.requestSeek(0);  // preroll the first frame (A16)
  }

  // Both clips around the target are read: the one playing and the next one, which is
  // either already in its transition or starts after this one ends.
  void startSeek(PendingSeek seek) {
    for (Lane& lane : ctx_.lanes) {
      lane.videoPackets.flush();
      lane.audioPackets.flush();
    }
    int first = ctx_.layout.firstActive(seek.targetUs);
    for (int c : {first, first + 1}) {
      int li = Context::laneOf(c);
      LaneState& lane = lanes_[li];
      seek.laneClip[li] = c;
      lane.clip = c;
      lane.eos[kVideo] = lane.eos[kAudio] = true;
      if (c >= clips()) continue;
      Result r = ctx_.demuxers[c]->seekTo(std::max<int64_t>(0, seek.targetUs - ctx_.layout.startUs(c)));
      if (r != Result::Ok) return ctx_.fatal(r, clipName(c) + "seek failed");
      lane.eos[kVideo] = lane.eos[kAudio] = false;
    }
    ctx_.setSeekTarget(seek);
    serial_ = ctx_.serial + 1;
    ctx_.serial = serial_;  // after the target is set: downstream reads it on seeing the new serial
    ctx_.wakeAll();
  }

  // A lane whose clip is fully read moves on to its next clip.
  bool advanceLanes() {
    for (LaneState& lane : lanes_) {
      if (lane.clip >= clips() || !lane.eos[kVideo] || !lane.eos[kAudio]) continue;
      lane.clip += kLanes;
      if (lane.clip >= clips()) return true;
      Result r = ctx_.demuxers[lane.clip]->seekTo(0);
      if (r != Result::Ok) {
        ctx_.fatal(r, clipName(lane.clip) + "seek failed");
        return false;
      }
      lane.eos[kVideo] = lane.eos[kAudio] = false;
      return true;
    }
    return false;
  }

  // Audio packets are read (to keep the demuxer's tracks balanced) but dropped when audio is off.
  BoundedQueue<Packet>* queueFor(int li, int track) {
    if (track == kVideo) return &ctx_.lanes[li].videoPackets;
    return ctx_.hasAudio ? &ctx_.lanes[li].audioPackets : nullptr;
  }

  Progress readOne() {
    int bestLane = -1, bestTrack = kVideo;
    int64_t bestDts = 0;
    for (int li = 0; li < kLanes; ++li) {
      const LaneState& lane = lanes_[li];
      if (lane.clip >= clips()) continue;
      for (int track : {kVideo, kAudio}) {
        if (lane.eos[track]) continue;
        BoundedQueue<Packet>* q = queueFor(li, track);
        if (q && q->full()) continue;
        if (track == kAudio && !ctx_.infos[lane.clip].audio) {
          pushEos(li, track);
          return Progress::did();
        }
        int64_t dts = 0;
        Result r = ctx_.demuxers[lane.clip]->peekDtsUs(track, &dts);
        if (r == Result::Eos) {
          pushEos(li, track);
          return Progress::did();
        }
        if (r != Result::Ok) {
          ctx_.fatal(r, clipName(lane.clip) + "demux failed");
          return Progress::idle();
        }
        dts += ctx_.layout.startUs(lane.clip);
        if (bestLane < 0 || dts < bestDts) {
          bestLane = li;
          bestTrack = track;
          bestDts = dts;
        }
      }
    }
    if (bestLane < 0) return Progress::idle();  // queues full or every lane ended

    int clip = lanes_[bestLane].clip;
    Packet p;
    Result r = ctx_.demuxers[clip]->read(bestTrack, &p);
    if (r == Result::Eos) {
      pushEos(bestLane, bestTrack);
      return Progress::did();
    }
    if (r != Result::Ok || p.data.size() > kMaxSampleBytes) {
      ctx_.fatal(Result::MalformedMedia, clipName(clip) + "bad sample");
      return Progress::idle();
    }
    p.serial = serial_;
    p.clip = clip;
    bool keep = bestTrack == kVideo || ctx_.clipAudio[clip];
    if (BoundedQueue<Packet>* q = queueFor(bestLane, bestTrack); q && keep) q->tryPush(p);  // has room: checked above
    return Progress::did();
  }

  // Also marks the end of a clip whose audio is dropped, so T4 knows the lane moved on.
  void pushEos(int li, int track) {
    LaneState& lane = lanes_[li];
    lane.eos[track] = true;
    if (BoundedQueue<Packet>* q = queueFor(li, track)) {
      Packet eos;
      eos.track = track;
      eos.eos = true;
      eos.serial = serial_;
      eos.clip = lane.clip;
      q->tryPush(eos);
    }
  }

  Context& ctx_;
  bool opened_ = false;
  LaneState lanes_[kLanes];
  uint32_t serial_ = 0;
};

// ---------------------------------------------------------------------------------------
// T2: per lane, packets into the decoder, frames (PTS order) into the lane's frame queue.
// A lane's next clip reconfigures the decoder once the previous clip has drained.
class VideoDecodeStage : public Stage {
 public:
  explicit VideoDecodeStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.probed) return Progress::idle();
    bool did = false;
    for (int li = 0; li < kLanes && !ctx_.halted; ++li) did |= pumpLane(li);
    return did && !ctx_.halted ? Progress::did() : Progress::idle();
  }

 private:
  struct LaneState {
    uint32_t serial = 0;
    int clip = -1;         // the clip the decoder is configured for
    bool drained = true;   // nothing left in the decoder, so it can be reconfigured
    std::optional<Packet> input;
    std::optional<VideoFrame> output;
    bool skipToKey = false, eosSent = false;
  };

  bool pumpLane(int li) {
    LaneState& st = lanes_[li];
    Lane& lane = ctx_.lanes[li];
    uint32_t serial = ctx_.serial;
    if (serial != st.serial) {
      st.serial = serial;
      lane.videoDecoder->flush();
      st.input.reset();
      st.output.reset();
      st.skipToKey = st.eosSent = false;
      st.drained = true;
    }
    bool did = drainOutput(st, lane);
    did |= feedInput(st, lane);
    return did;
  }

  bool drainOutput(LaneState& st, Lane& lane) {
    bool did = false;
    if (!st.output) {
      VideoFrame f;
      switch (lane.videoDecoder->dequeue(&f)) {
        case Result::Ok:
          if (f.serial == st.serial) {
            f.clip = st.clip;
            st.output = std::move(f);
          }
          did = true;
          break;
        case Result::Eos:
          if (!st.eosSent) {
            st.eosSent = st.drained = true;
            st.output = VideoFrame{};
            st.output->eos = true;
            st.output->serial = st.serial;
            st.output->clip = st.clip;
          }
          break;
        case Result::CorruptFrame:  // hold the last good frame; skip to the next keyframe (A14)
          ctx_.metrics.countCorrupt();
          st.skipToKey = true;
          did = true;
          break;
        case Result::Again:
          break;
        default:
          ctx_.fatal(Result::DecoderFailed, clipName(st.clip) + "video decoder failed");
          return false;
      }
    }
    if (st.output && lane.frames.tryPush(*st.output)) {
      st.output.reset();
      did = true;
    }
    return did;
  }

  bool feedInput(LaneState& st, Lane& lane) {
    bool did = false;
    if (!st.input) {
      Packet p;
      if (!lane.videoPackets.tryPop(&p)) return false;
      did = true;
      if (p.serial != st.serial) return true;
      st.input = std::move(p);
    }
    if (st.input->clip != st.clip) {
      if (!st.drained) return did;  // the previous clip's last frames are still coming out
      Result r = lane.videoDecoder->configure(ctx_.infos[st.input->clip].video, [this] { ctx_.wake(StageId::VideoDecode); });
      if (r != Result::Ok) {
        ctx_.fatal(r, clipName(st.input->clip) + "video decoder configuration failed");
        return false;
      }
      st.clip = st.input->clip;
      st.skipToKey = st.eosSent = false;
    }
    if (st.input->eos) {
      lane.videoDecoder->signalEos();
      st.input.reset();
      return true;
    }
    if (st.skipToKey && !st.input->key) {
      ctx_.metrics.countCorrupt();
      st.input.reset();
      return true;
    }
    switch (lane.videoDecoder->queue(*st.input)) {
      case Result::Ok:
        st.skipToKey = st.drained = false;
        st.input.reset();
        return true;
      case Result::Again:  // decoder full; its output callback wakes us
        return did;
      case Result::CorruptFrame:
        ctx_.metrics.countCorrupt();
        st.skipToKey = true;
        st.drained = false;
        st.input.reset();
        return true;
      default:
        ctx_.fatal(Result::DecoderFailed, clipName(st.clip) + "video decoder failed");
        return false;
    }
  }

  Context& ctx_;
  LaneState lanes_[kLanes];
};

// ---------------------------------------------------------------------------------------
// TC: composition (§2.4). Merges the two lanes' frames into timeline order and describes each
// output frame for the display: the leading clip's frame, the outgoing clip's latest frame
// during a transition (each with its slide offset), the caption and the filter.
// It also completes the frame choice for exact seeks, which T3 then shows.
class CompositionStage : public Stage {
 public:
  explicit CompositionStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.probed) return Progress::idle();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) restart(serial);
    if (serial_ == 0) return Progress::idle();  // the preroll seek hasn't started yet
    while (!out_.empty()) {
      if (!ctx_.composed.tryPush(out_.front())) return Progress::idle();  // T3 frees a slot and wakes us
      out_.pop_front();
    }
    if (ended_) return Progress::idle();
    if (seeking_ && candidate_ && ctx_.hasPendingSeek()) {  // scrubbing: show what we have now
      emitCandidate();
      return Progress::did();
    }

    for (int li = 0; li < kLanes; ++li) {
      LaneView& lane = lanes_[li];
      if (lane.head || lane.done) continue;
      VideoFrame f;
      if (!ctx_.lanes[li].frames.tryPop(&f)) continue;
      if (f.serial != serial_) return Progress::did();
      if (f.eos) {  // the lane's clip has no more frames; its next clip is two ahead
        lane.clip = f.clip + kLanes;
        lane.done = lane.clip >= ctx_.layout.clips();
        return Progress::did();
      }
      lane.head = std::move(f);
    }

    // The earliest frame goes next, once the other lane can't still produce an earlier one.
    int next = -1;
    for (int li = 0; li < kLanes; ++li) {
      if (lanes_[li].head && (next < 0 || timeOf(*lanes_[li].head) < timeOf(*lanes_[next].head))) next = li;
    }
    if (next < 0) {
      if (!lanes_[0].done || !lanes_[1].done) return Progress::idle();
      finish();
      return Progress::did();
    }
    int64_t t = timeOf(*lanes_[next].head);
    const LaneView& other = lanes_[1 - next];
    if (!other.head && floorOf(other) <= t) return Progress::idle();  // its frame queue wakes us
    VideoFrame f = std::move(*lanes_[next].head);
    lanes_[next].head.reset();
    compose(next, std::move(f), t);
    return Progress::did();
  }

 private:
  struct LaneView {
    int clip = kNoClip;  // the clip the lane is delivering
    bool done = true;    // no clips left on this lane
    std::optional<VideoFrame> head;  // next frame, not yet composed
    std::optional<VideoFrame> held;  // latest composed frame: shown while its clip slides out
  };

  int64_t timeOf(const VideoFrame& f) const { return ctx_.layout.startUs(f.clip) + f.ptsUs; }

  // Earliest time the lane's next frame can have: frames come in PTS order per clip.
  int64_t floorOf(const LaneView& lane) const {
    if (lane.done) return kNever;
    int64_t floor = ctx_.layout.startUs(lane.clip);
    if (lane.held && lane.held->clip == lane.clip) floor = std::max(floor, timeOf(*lane.held) + 1);
    return floor;
  }

  void restart(uint32_t serial) {
    serial_ = serial;
    PendingSeek target = ctx_.seekTarget();
    targetUs_ = target.targetUs;
    for (int li = 0; li < kLanes; ++li) {
      lanes_[li] = LaneView{};
      lanes_[li].clip = target.laneClip[li];
      lanes_[li].done = lanes_[li].clip >= ctx_.layout.clips();
    }
    out_.clear();
    candidate_.reset();
    seeking_ = true;
    ended_ = false;
  }

  void compose(int li, VideoFrame f, int64_t t) {
    const TimelineLayout& layout = ctx_.layout;
    int c = f.clip;
    if (!layout.active(c, t)) return;  // past the clip's end: cut by the next clip
    lanes_[li].held = f;
    if (c != layout.lastActive(t)) return;  // outgoing: drawn under the leading clip's next frame

    ComposedFrame out;
    out.ptsUs = t;
    out.serial = serial_;
    out.frameDurationUs = ctx_.infos[c].video.frameDurationUs;
    if (c > 0 && layout.active(c - 1, t)) {
      const std::optional<VideoFrame>& prev = lanes_[Context::laneOf(c - 1)].held;
      if (prev && prev->clip == c - 1) out.layers[out.layerCount++] = {*prev, layout.offsetX(c - 1, t)};
    }
    out.layers[out.layerCount++] = {std::move(f), layout.offsetX(c, t)};
    out.text = ctx_.captionAt(t);
    out.filter = ctx_.filter();
    emit(std::move(out));
  }

  // Exact seek (§4): decode from the keyframe, show the last frame with pts <= target.
  // While a newer seek is pending (scrubbing), show whatever is ready instead.
  void emit(ComposedFrame out) {
    if (seeking_) {
      if (out.ptsUs <= targetUs_) {
        if (candidate_) ctx_.metrics.countDecodeOnly();
        candidate_ = std::move(out);
        if (ctx_.hasPendingSeek()) emitCandidate();
        return;
      }
      emitCandidate();  // past the target: the last frame before it (if any) first, then play on
    }
    out_.push_back(std::move(out));
  }

  void emitCandidate() {
    seeking_ = false;
    if (candidate_) out_.push_back(std::move(*candidate_));
    candidate_.reset();
  }

  void finish() {
    emitCandidate();
    ComposedFrame eos;
    eos.eos = true;
    eos.serial = serial_;
    out_.push_back(std::move(eos));
    ended_ = true;
  }

  Context& ctx_;
  uint32_t serial_ = 0;
  int64_t targetUs_ = 0;
  LaneView lanes_[kLanes];
  std::deque<ComposedFrame> out_;  // composed, waiting for room in the queue to T3
  std::optional<ComposedFrame> candidate_;
  bool seeking_ = false, ended_ = false;
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
      videoEnded_ = false;
    }
    bool seeking = ctx_.shownSerial != serial_;
    updateOutput(!seeking && !ctx_.hasPendingSeek());
    if (!seeking) redrawForFilter();

    if (!current_) {
      ComposedFrame f;
      if (!ctx_.composed.tryPop(&f)) return videoEnded_ ? checkEnd() : Progress::idle();
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

  // While paused nothing new is presented, so a filter change redraws the frame on screen.
  void redrawForFilter() {
    uint32_t version = ctx_.filterVersion;
    if (version == filterVersion_) return;
    filterVersion_ = version;
    if (lastShown_ && !outputRunning_) present(*lastShown_, ctx_.hostClock.nowNs());
  }

  // The latest filter is applied here rather than when composed, so changes show at once.
  void present(ComposedFrame f, int64_t atNs) {
    f.filter = ctx_.filter();
    ctx_.display->present(f, atNs);
    lastShown_ = std::move(f);
  }

  // The composition stage has already chosen the frame for the target: show it.
  Progress seekStep() {
    ComposedFrame f = std::move(*current_);
    current_.reset();
    if (f.eos) {
      videoEnded_ = true;
      complete(nullptr);
    } else {
      complete(&f);
    }
    return Progress::did();
  }

  void complete(const ComposedFrame* frame) {
    PendingSeek target = ctx_.seekTarget();
    int64_t now = ctx_.hostClock.nowNs();
    int64_t shown = ctx_.shownPtsUs;
    if (frame) {
      present(*frame, now);
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
    SyncDecision d = sync_.decide(current_->ptsUs, clockUs, now + lead, vsync, current_->frameDurationUs,
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
        ctx_.shownPtsUs = current_->ptsUs;
        present(std::move(*current_), d.atNs);
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
  std::optional<ComposedFrame> current_, lastShown_;
  uint32_t filterVersion_ = 0;
  bool videoEnded_ = false;
  bool outputRunning_ = false;
};

// ---------------------------------------------------------------------------------------
// T4: decode each lane's audio and mix it on the timeline into the ring the speaker pulls
// from. During a transition the outgoing clip fades out while the incoming one fades in.
// Clips without usable audio, and gaps in a track, are silence.
class AudioStage : public Stage {
 public:
  explicit AudioStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.hasAudio) return Progress::idle();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) restart(serial);
    if (serial_ == 0) return Progress::idle();  // the preroll seek hasn't started yet
    if (!chunk_.empty()) {
      int channels = ctx_.ring.channels();
      int total = static_cast<int>(chunk_.size()) / channels;
      offset_ += ctx_.ring.write(chunk_.data() + size_t(offset_) * channels, total - offset_);
      if (offset_ < total) {
        // Ring full. The real-time consumer can't signal us, so poll while playing.
        return ctx_.playing ? Progress::waitUntil(ctx_.hostClock.nowNs() + 5 * kMs) : Progress::idle();
      }
      chunk_.clear();
    }
    if (eosMarked_) return Progress::idle();
    if (cursor_ >= endSample_) {
      ctx_.ring.markEos();
      eosMarked_ = true;
      return Progress::did();
    }

    // Every clip with audio in the next chunk must be decoded that far, or have ended.
    int64_t chunkEnd = std::min(cursor_ + kChunkFrames, endSample_);
    for (int c = 0; c < ctx_.layout.clips() && startSample(c) < chunkEnd; ++c) {
      if (!ctx_.clipAudio[c] || endSample(c) <= cursor_) continue;
      int li = Context::laneOf(c);
      const LaneMix& lane = lanes_[li];
      if (lane.clip > c || (lane.clip == c && lane.coveredTo >= std::min(chunkEnd, endSample(c)))) continue;
      return pull(li);
    }
    mix(chunkEnd);
    return Progress::did();
  }

 private:
  static constexpr int64_t kChunkFrames = 1024;

  struct Segment {
    int clip;
    int64_t start;  // timeline sample of the first frame
    std::vector<int16_t> pcm;
  };
  struct LaneMix {
    int clip = kNoClip;     // the clip the lane is delivering; earlier ones are finished
    int64_t coveredTo = 0;  // timeline sample up to which that clip's audio is decoded
    int64_t nextStart = -1;  // where the clip's next packet continues, once one is decoded
    std::deque<Segment> segments;
  };

  int64_t toSample(int64_t us) const { return us * ctx_.ring.sampleRate() / 1000000; }
  int64_t toUs(int64_t sample) const { return sample * 1000000 / ctx_.ring.sampleRate(); }
  int64_t startSample(int c) const { return toSample(ctx_.layout.startUs(c)); }
  int64_t endSample(int c) const { return toSample(ctx_.layout.endUs(c)); }
  int64_t frames(const Segment& s) const { return static_cast<int64_t>(s.pcm.size()) / ctx_.ring.channels(); }

  void restart(uint32_t serial) {
    serial_ = serial;
    PendingSeek target = ctx_.seekTarget();
    for (int li = 0; li < kLanes; ++li) {
      ctx_.lanes[li].audioDecoder->flush();
      lanes_[li] = LaneMix{};
      lanes_[li].clip = target.laneClip[li];
      if (lanes_[li].clip < ctx_.layout.clips()) lanes_[li].coveredTo = startSample(lanes_[li].clip);
    }
    cursor_ = toSample(target.targetUs);  // exact seek: nothing before the target is mixed
    endSample_ = toSample(ctx_.layout.durationUs());
    ctx_.ring.flush(target.targetUs, serial_);
    chunk_.clear();
    eosMarked_ = false;
  }

  Progress pull(int li) {
    LaneMix& lane = lanes_[li];
    Packet p;
    if (!ctx_.lanes[li].audioPackets.tryPop(&p)) return Progress::idle();
    if (p.serial != serial_) return Progress::did();
    if (p.eos) {
      lane.clip = p.clip + kLanes;
      lane.nextStart = -1;
      if (lane.clip < ctx_.layout.clips()) lane.coveredTo = startSample(lane.clip);
      return Progress::did();
    }
    if (p.clip != lane.clip) {
      lane.clip = p.clip;
      lane.nextStart = -1;
      lane.coveredTo = startSample(p.clip);
    }
    IAudioDecoder& decoder = *ctx_.lanes[li].audioDecoder;
    if (decoderClip_[li] != p.clip) {
      if (decoder.configure(*ctx_.infos[p.clip].audio) != Result::Ok) {
        ctx_.fatal(Result::DecoderFailed, clipName(p.clip) + "audio decoder configuration failed");
        return Progress::idle();
      }
      decoderClip_[li] = p.clip;
    }
    PcmBuffer pcm;
    Result r = decoder.decode(p, &pcm);  // on CorruptFrame, pcm is silence (A14)
    if (r == Result::CorruptFrame) {
      ctx_.metrics.countCorrupt();
    } else if (r != Result::Ok) {
      ctx_.fatal(Result::DecoderFailed, clipName(p.clip) + "audio decoder failed");
      return Progress::idle();
    }
    Segment s{p.clip, startSample(p.clip) + std::llround(double(pcm.ptsUs) * ctx_.ring.sampleRate() / 1e6),
              std::move(pcm.samples)};
    // Timestamps are rounded (to 1 us here, often coarser in the file): packets within 1 ms
    // of each other are one continuous stream, so rounding never doubles or drops a sample.
    if (lane.nextStart >= 0 && std::llabs(s.start - lane.nextStart) <= ctx_.ring.sampleRate() / 1000) s.start = lane.nextStart;
    int64_t end = s.start + frames(s);
    lane.nextStart = end;
    lane.coveredTo = std::max(lane.coveredTo, end);
    if (end > cursor_ && s.start < endSample(s.clip)) lane.segments.push_back(std::move(s));
    return Progress::did();
  }

  // Sums the clips over [cursor_, chunkEnd), each cut to its time on the timeline and
  // scaled by its transition gain.
  void mix(int64_t chunkEnd) {
    int channels = ctx_.ring.channels();
    std::vector<float> sum(size_t(chunkEnd - cursor_) * channels, 0.0f);
    for (LaneMix& lane : lanes_) {
      for (const Segment& s : lane.segments) {
        int64_t from = std::max({s.start, cursor_, startSample(s.clip)});
        int64_t to = std::min({s.start + frames(s), chunkEnd, endSample(s.clip)});
        for (int64_t i = from; i < to; ++i) {
          float gain = ctx_.layout.gain(s.clip, toUs(i));
          const int16_t* in = &s.pcm[size_t(i - s.start) * channels];
          float* out = &sum[size_t(i - cursor_) * channels];
          for (int ch = 0; ch < channels; ++ch) out[ch] += gain * float(in[ch]);
        }
      }
      while (!lane.segments.empty() && lane.segments.front().start + frames(lane.segments.front()) <= chunkEnd) {
        lane.segments.pop_front();
      }
    }
    chunk_.resize(sum.size());
    for (size_t i = 0; i < sum.size(); ++i) chunk_[i] = static_cast<int16_t>(std::clamp(std::lround(sum[i]), -32768L, 32767L));
    offset_ = 0;
    cursor_ = chunkEnd;
  }

  Context& ctx_;
  uint32_t serial_ = 0;
  LaneMix lanes_[kLanes];
  int decoderClip_[kLanes] = {-1, -1};
  int64_t cursor_ = 0, endSample_ = 0;  // timeline samples
  std::vector<int16_t> chunk_;          // mixed, not yet fully written to the ring
  int offset_ = 0;
  bool eosMarked_ = false;
};

}  // namespace

Context::Context(PlatformFactory& f, PipelineEvents& ev)
    : factory(f),
      hostClock(f.clock()),
      events(ev),
      speaker(f.createSpeaker()),
      display(f.createDisplay()),
      scheduler(f.createScheduler()) {
  for (Lane& lane : lanes) {
    lane.videoDecoder = f.createVideoDecoder();
    lane.audioDecoder = f.createAudioDecoder();
    lane.videoPackets.setHooks([this] { wake(StageId::VideoDecode); }, [this] { wake(StageId::Source); });
    lane.audioPackets.setHooks([this] { wake(StageId::Audio); }, [this] { wake(StageId::Source); });
    lane.frames.setHooks([this] { wake(StageId::Composition); }, [this] { wake(StageId::VideoDecode); });
  }
  composed.setHooks([this] { wake(StageId::VideoRender); }, [this] { wake(StageId::Composition); });
}

void Context::wakeAll() {
  for (int i = 0; i < kStageCount; ++i) wake(static_cast<StageId>(i));
}

std::shared_ptr<const std::string> Context::captionAt(int64_t timelineUs) const {
  for (const Caption& c : captions) {
    if (timelineUs >= c.startUs && timelineUs < c.endUs) return c.text;
  }
  return nullptr;
}

VideoFilter Context::filter() const {
  std::lock_guard<std::mutex> lock(filterMu_);
  return filter_;
}

void Context::setFilter(const VideoFilter& f) {
  {
    std::lock_guard<std::mutex> lock(filterMu_);
    filter_ = f;
  }
  ++filterVersion;
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
          std::make_unique<CompositionStage>(ctx), std::make_unique<VideoRenderStage>(ctx),
          std::make_unique<AudioStage>(ctx)};
}

}  // namespace mf
