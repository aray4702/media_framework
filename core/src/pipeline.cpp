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
    if (output) ctx_.ring.open(output->sampleRate, output->channels, output->sampleRate / 5);  // 200 ms
    if (ctx_.driver == Driver::Export) {
      Result r = ctx_.exportSink->open(ctx_.exportTarget, ctx_.exportSettings, output ? output->sampleRate : 0,
                                       output ? output->channels : 0);
      if (r != Result::Ok) return ctx_.fatal(r, "cannot create the output file");
    } else if (output && ctx_.speaker->open(output->sampleRate, output->channels, &ctx_.ring) != Result::Ok) {
      return ctx_.fatal(Result::AudioDeviceFailed, "audio output failed to open");
    }
    std::vector<int64_t> frameDurations;
    for (const MediaInfo& info : ctx_.infos) frameDurations.push_back(info.video.frameDurationUs);
    ctx_.layout.build(durations, ctx_.transition, std::move(frameDurations));
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
// TC: composition (§2.4, §2.5). Keeps, per lane, the latest frame at or before the output
// time, and composes the output at a time `t` from every active clip's frame: layers with
// their slide offsets, the caption and the filter. The driver decides the times:
//   - LeadingClip: each frame of the highest-fps active clip, in timeline order; AvSync paces them.
//   - Vsync: the clock at each display refresh; a layer whose decode is behind holds its frame.
//   - Export: n / fps; waits until every layer has its exact frame.
// Every driver starts with the same exact seek, which T3 shows.
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
    if (seeking_) return seekStep();
    switch (ctx_.driver) {
      case Driver::LeadingClip: return leadingStep();
      case Driver::Vsync: return vsyncStep();
      case Driver::Export: return exportStep();
    }
    return Progress::idle();
  }

 private:
  struct LaneView {
    int clip = kNoClip;  // the clip the lane is delivering
    bool done = true;    // no clips left on this lane
    std::optional<VideoFrame> head;  // next frame, later than every output so far
    std::optional<VideoFrame> held;  // latest frame at or before the last output time
  };

  const TimelineLayout& layout() const { return ctx_.layout; }
  int64_t timeOf(const VideoFrame& f) const { return layout().startUs(f.clip) + f.ptsUs; }

  void restart(uint32_t serial) {
    serial_ = serial;
    PendingSeek target = ctx_.seekTarget();
    targetUs_ = std::clamp<int64_t>(target.targetUs, 0, layout().durationUs() - 1);
    for (int li = 0; li < kLanes; ++li) {
      lanes_[li] = LaneView{};
      lanes_[li].clip = target.laneClip[li];
      lanes_[li].done = lanes_[li].clip >= layout().clips();
    }
    out_.clear();
    seeking_ = ctx_.driver != Driver::Export;  // export starts at 0 and has no frame to show first
    ended_ = false;
    nextSlotNs_ = gridNs_ = 0;
    last_.reset();
    exportIndex_ = 0;
  }

  // --- Lanes ----------------------------------------------------------------------------

  // Takes the lane's next item from its frame queue: a frame into `head`, or the end of a clip.
  bool pop(int li) {
    LaneView& lane = lanes_[li];
    if (lane.head || lane.done) return false;
    VideoFrame f;
    if (!ctx_.lanes[li].frames.tryPop(&f)) return false;
    if (f.serial != serial_) return true;
    if (f.eos) {  // the lane's clip has no more frames; its next clip is two ahead
      lane.clip = f.clip + kLanes;
      lane.done = lane.clip >= layout().clips();
      return true;
    }
    lane.head = std::move(f);
    return true;
  }

  // Moves the lane's head to `held`, unless the frame falls after its clip's end (cut there).
  void take(int li) {
    LaneView& lane = lanes_[li];
    VideoFrame f = std::move(*lane.head);
    lane.head.reset();
    if (!layout().active(f.clip, timeOf(f))) return;
    if (seeking_ && lane.held && lane.held->clip == f.clip) ctx_.metrics.countDecodeOnly();
    lane.held = std::move(f);
  }

  // Consumes every frame the lane has up to t, so `held` is its latest frame at or before t.
  void advance(int li, int64_t t) {
    LaneView& lane = lanes_[li];
    for (;;) {
      if (!lane.head && !pop(li)) return;
      if (!lane.head) continue;
      if (timeOf(*lane.head) > t) return;
      take(li);
    }
  }

  void advanceAll(int64_t t) {
    for (int li = 0; li < kLanes; ++li) advance(li, t);
  }

  // Whether clip c's frame for t is known: its next frame is later than t, or it has ended.
  bool exactAt(int c, int64_t t) const {
    const LaneView& lane = lanes_[Context::laneOf(c)];
    if (lane.done || lane.clip > c) return true;
    return lane.clip == c && lane.head && timeOf(*lane.head) > t;
  }

  bool allExactAt(int64_t t) const {
    for (int c = layout().firstActive(t); c < layout().clips() && layout().active(c, t); ++c) {
      if (!exactAt(c, t)) return false;
    }
    return true;
  }

  const VideoFrame* frameOf(int c) const {
    const std::optional<VideoFrame>& held = lanes_[Context::laneOf(c)].held;
    return held && held->clip == c ? &*held : nullptr;
  }

  bool lanesDone() const { return lanes_[0].done && lanes_[1].done && !lanes_[0].head && !lanes_[1].head; }

  // --- Composing --------------------------------------------------------------------------

  // The output frame at t from each active clip's latest frame, outgoing clip first.
  ComposedFrame composeAt(int64_t t) const {
    ComposedFrame out;
    out.ptsUs = t;
    out.serial = serial_;
    out.frameDurationUs = ctx_.infos[layout().leadClip(t)].video.frameDurationUs;
    for (int c = layout().firstActive(t); c < layout().clips() && layout().active(c, t); ++c) {
      if (const VideoFrame* f = frameOf(c)) out.layers[out.layerCount++] = {*f, layout().offsetX(c, t)};
    }
    out.text = ctx_.captionAt(t);
    out.filter = ctx_.filter();
    return out;
  }

  void finish() {
    ComposedFrame eos;
    eos.eos = true;
    eos.serial = serial_;
    out_.push_back(std::move(eos));
    ended_ = true;
  }

  // --- Exact seek (§4), for every driver ----------------------------------------------------

  // Shows the leading clip's last frame at or before the target (its first frame, if none is),
  // with every other active clip's latest frame at that time. While a newer seek is pending
  // (scrubbing), shows whatever is decoded instead of waiting for the exact frames.
  Progress seekStep() {
    bool scrub = ctx_.hasPendingSeek();
    advanceAll(targetUs_);
    if (!allExactAt(targetUs_) && !scrub) return Progress::idle();  // the lanes' frame queues wake us

    int lead = layout().leadClip(targetUs_);
    const LaneView& lane = lanes_[Context::laneOf(lead)];
    std::optional<int64_t> t;
    if (const VideoFrame* f = frameOf(lead)) {
      t = timeOf(*f);
    } else if (lane.head && lane.head->clip == lead) {
      t = timeOf(*lane.head);  // nothing at or before the target: the first frame after it
    } else if (lane.clip <= lead && !lane.done && !scrub) {
      return Progress::idle();
    }
    if (t && *t > targetUs_) {
      advanceAll(*t);
      if (!allExactAt(*t) && !scrub) return Progress::idle();
    }
    ComposedFrame out = composeAt(t.value_or(targetUs_));
    if (out.layerCount == 0) {
      if (!lanesDone()) return Progress::idle();  // nothing decoded yet, even when scrubbing
      seeking_ = false;
      finish();  // nothing to show: the seek completes without a frame (A5)
      return Progress::did();
    }
    seeking_ = false;
    last_ = out;
    out_.push_back(std::move(out));
    return Progress::did();
  }

  // --- LeadingClip driver -----------------------------------------------------------------

  // Earliest time the lane's next frame can have: frames come in PTS order per clip.
  int64_t floorOf(const LaneView& lane) const {
    if (lane.done) return kNever;
    int64_t floor = layout().startUs(lane.clip);
    if (lane.held && lane.held->clip == lane.clip) floor = std::max(floor, timeOf(*lane.held) + 1);
    return floor;
  }

  // Frames go in timeline order, once the other lane can't still produce an earlier one.
  // A frame of the leading clip produces an output; any other frame only updates its lane.
  Progress leadingStep() {
    bool popped = false;
    for (int li = 0; li < kLanes; ++li) popped |= pop(li);
    int next = -1;
    for (int li = 0; li < kLanes; ++li) {
      if (lanes_[li].head && (next < 0 || timeOf(*lanes_[li].head) < timeOf(*lanes_[next].head))) next = li;
    }
    if (next < 0) {
      if (lanesDone()) {
        finish();
        return Progress::did();
      }
      return popped ? Progress::did() : Progress::idle();
    }
    int64_t t = timeOf(*lanes_[next].head);
    const LaneView& other = lanes_[1 - next];
    if (!other.head && floorOf(other) <= t) return popped ? Progress::did() : Progress::idle();
    int clip = lanes_[next].head->clip;
    take(next);
    if (frameOf(clip) && timeOf(*frameOf(clip)) == t && clip == layout().leadClip(t)) out_.push_back(composeAt(t));
    return Progress::did();
  }

  // --- Vsync driver -----------------------------------------------------------------------

  // Composes each refresh half a frame before it must be handed to the display, at the clock
  // time it will be seen. Never waits for a layer: one that is behind keeps its last frame.
  Progress vsyncStep() {
    {
      std::lock_guard<std::mutex> lock(ctx_.playMu);
      if (!ctx_.outputRunning) {  // paused: T3 wakes us when output starts
        nextSlotNs_ = gridNs_ = 0;
        return Progress::idle();
      }
    }
    int64_t now = ctx_.hostClock.nowNs();
    int64_t period = std::max<int64_t>(ctx_.display->vsyncPeriodNs(), kMs);
    int64_t lead = ctx_.display->latencyNs();
    int64_t grid = ctx_.display->vsyncGridNs();
    if (grid == 0) grid = gridNs_ ? gridNs_ : (gridNs_ = now);  // no real vsync known: our own grid
    // The first refresh not yet composed that a frame handed over now can still reach.
    int64_t earliest = std::max(now + lead, nextSlotNs_);
    int64_t slot = grid + (earliest - grid + period - 1) / period * period;
    int64_t composeNs = slot - lead - period / 2;
    if (now < composeNs) return Progress::waitUntil(composeNs);
    nextSlotNs_ = slot + period / 2;

    int64_t t = ctx_.master.nowUs(now) + (slot - now) / 1000;
    if (last_ && t <= last_->ptsUs) return Progress::did();  // the clock is holding (audio not heard yet)
    if (t >= layout().durationUs()) {
      finish();
      return Progress::did();
    }
    advanceAll(t);
    for (int c = layout().firstActive(t); c < layout().clips() && layout().active(c, t); ++c) {
      const VideoFrame* f = frameOf(c);
      if (!exactAt(c, t) && f && timeOf(*f) + ctx_.infos[c].video.frameDurationUs <= t) ctx_.metrics.countLateLayer();
    }
    ComposedFrame out = composeAt(t);
    out.presentAtNs = slot;
    if (out.layerCount == 0 || unchanged(out)) return Progress::did();  // the frame on screen stays
    last_ = out;
    out_.push_back(std::move(out));
    return Progress::did();
  }

  // Nothing visible differs from the last output: same frames, offsets, caption and filter.
  bool unchanged(const ComposedFrame& f) const {
    if (!last_ || last_->layerCount != f.layerCount || last_->text != f.text || !(last_->filter == f.filter)) return false;
    for (int i = 0; i < f.layerCount; ++i) {
      const ComposedFrame::Layer &a = last_->layers[i], &b = f.layers[i];
      if (a.frame.clip != b.frame.clip || a.frame.ptsUs != b.frame.ptsUs || a.offsetX != b.offsetX) return false;
    }
    return true;
  }

  // --- Export driver ----------------------------------------------------------------------

  Progress exportStep() {
    int fps = ctx_.exportSettings.fps;
    int64_t t = exportIndex_ * 1000000 / fps;
    if (t >= layout().durationUs()) {
      finish();
      return Progress::did();
    }
    advanceAll(t);
    if (!allExactAt(t)) return Progress::idle();  // the lanes' frame queues wake us
    ComposedFrame out = composeAt(t);  // no layer yet (a clip's first frame is later): background
    out.frameDurationUs = 1000000 / fps;
    out_.push_back(std::move(out));
    ++exportIndex_;
    return Progress::did();
  }

  Context& ctx_;
  uint32_t serial_ = 0;
  int64_t targetUs_ = 0;
  LaneView lanes_[kLanes];
  std::deque<ComposedFrame> out_;  // composed, waiting for room in the queue to T3
  bool seeking_ = false, ended_ = false;
  int64_t nextSlotNs_ = 0, gridNs_ = 0;  // Vsync
  std::optional<ComposedFrame> last_;    // Vsync: the last output, to skip unchanged refreshes
  int64_t exportIndex_ = 0;              // Export
};

// ---------------------------------------------------------------------------------------
// T3: completes seeks, runs AvSync (or presents at the vsync the frame was composed for),
// presents or drops. Also starts and stops the output (speaker + master clock), so
// play/pause never race a seek that is still in flight. On export, hands frames to the sink.
class VideoRenderStage : public Stage {
 public:
  explicit VideoRenderStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted) return Progress::idle();
    if (ctx_.driver == Driver::Export) return exportStep();
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
    return current_->presentAtNs ? scheduledStep() : renderStep();
  }

 private:
  void updateOutput(bool seekDone) {
    int64_t now = ctx_.hostClock.nowNs();
    bool failed = false, started = false;
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
          anchored_ = false;
          started = true;
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
    if (started && ctx_.driver == Driver::Vsync) ctx_.wake(StageId::Composition);  // it composes only while running
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

  // Vsync driver: the frame was composed for a refresh; hand it over for that one.
  Progress scheduledStep() {
    int64_t now = ctx_.hostClock.nowNs();
    int64_t vsync = ctx_.display->vsyncPeriodNs();
    int64_t at = current_->presentAtNs;
    if (!ctx_.display->visible()) {
      ctx_.metrics.countHidden();
    } else if (at + vsync < now) {
      ctx_.metrics.countLate();  // composed before a pause, or T3 fell a refresh behind
    } else {
      if (!anchored_) {
        anchorNs_ = at;
        anchored_ = true;
      }
      ctx_.metrics.planned(current_->ptsUs, std::llround(double(at - anchorNs_) / double(vsync)), vsync);
      ctx_.shownPtsUs = current_->ptsUs;
      present(std::move(*current_), at);
    }
    current_.reset();
    return Progress::did();
  }

  // Export: frames to the sink in order; the file is finished once the audio is written too.
  Progress exportStep() {
    if (finished_) return Progress::idle();
    int64_t now = ctx_.hostClock.nowNs();
    if (!current_ && !videoEnded_) {
      ComposedFrame f;
      if (!ctx_.composed.tryPop(&f)) return Progress::idle();
      if (f.serial != ctx_.serial) return Progress::did();
      current_ = std::move(f);
    }
    if (current_ && current_->eos) {
      videoEnded_ = true;
      current_.reset();
    }
    if (current_) {
      Result r = ctx_.exportSink->writeVideo(*current_);
      if (r == Result::Again) return Progress::waitUntil(now + 2 * kMs);  // the encoder can't signal us
      if (r != Result::Ok) {
        ctx_.fatal(r, "writing video failed");
        return Progress::idle();
      }
      ctx_.writtenUs = current_->ptsUs;
      current_.reset();
      return Progress::did();
    }
    if (ctx_.hasAudio && !ctx_.audioWritten) return Progress::idle();  // T4 wakes us when it's done
    finished_ = true;
    PipelineEvents& events = ctx_.events;
    ctx_.exportSink->finish([&events](Result r) {
      if (r == Result::Ok) events.onEnd();
      else events.onFatal(r, "finishing the file failed");
    });
    return Progress::idle();
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
  bool anchored_ = false;  // Vsync driver: slots count from the first frame after play or seek
  int64_t anchorNs_ = 0;
  bool finished_ = false;  // Export
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
      if (ctx_.driver == Driver::Export) {
        Result r = ctx_.exportSink->writeAudio(chunk_.data(), total, toUs(chunkStart_));
        if (r == Result::Again) return Progress::waitUntil(ctx_.hostClock.nowNs() + 2 * kMs);  // the encoder can't signal us
        if (r != Result::Ok) {
          ctx_.fatal(r, "writing audio failed");
          return Progress::idle();
        }
        chunk_.clear();
        return Progress::did();
      }
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
      if (ctx_.driver == Driver::Export) {
        ctx_.audioWritten = true;
        ctx_.wake(StageId::VideoRender);
      }
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
    chunkStart_ = cursor_;
    cursor_ = chunkEnd;
  }

  Context& ctx_;
  uint32_t serial_ = 0;
  LaneMix lanes_[kLanes];
  int decoderClip_[kLanes] = {-1, -1};
  int64_t cursor_ = 0, endSample_ = 0;  // timeline samples
  int64_t chunkStart_ = 0;               // timeline sample of chunk_'s first frame
  std::vector<int16_t> chunk_;          // mixed, not yet fully written to the ring
  int offset_ = 0;
  bool eosMarked_ = false;
};

}  // namespace

Context::Context(PlatformFactory& f, PipelineEvents& ev, bool forExport)
    : factory(f),
      hostClock(f.clock()),
      events(ev),
      speaker(forExport ? nullptr : f.createSpeaker()),
      display(forExport ? nullptr : f.createDisplay()),
      exportSink(forExport ? f.createExportSink() : nullptr),
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
