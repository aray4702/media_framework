#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <string>

#include "av_sync.h"
#include "composition.h"

namespace mf {
namespace {

constexpr int64_t kMs = 1000000;              // ns
constexpr size_t kMaxSampleBytes = 16u << 20;  // A8
constexpr int kMaxDimension = 8192;            // A8
constexpr int64_t kReadPastEndUs = 1000000;    // read a little past an item's end, for B-frame reordering

// A Timeline (the simple API) as a scene: its clips back to back on one video track, joined by
// a push (or a cut), and its captions on a second track. Clip durations come from probing.
Scene sceneFromTimeline(const Timeline& tl, const std::vector<MediaInfo>& infos, const ExportSettings* exportSettings) {
  Scene s;
  s.output.width = s.output.height = 0;  // the first clip's size
  s.output.fpsNum = 0;                   // the first clip's rate
  s.output.sampleRate = s.output.channels = 0;  // the first clip's audio
  if (exportSettings) {
    s.output.width = exportSettings->width;
    s.output.height = exportSettings->height;
    s.output.fpsNum = exportSettings->fps;
  }
  int n = int(tl.clips.size());
  int64_t shortest = kNever;
  for (const MediaInfo& info : infos) shortest = std::min(shortest, info.durationUs);
  int64_t overlap = n < 2 || tl.transition.kind == TransitionKind::Cut ? 0 : std::clamp<int64_t>(tl.transition.durationUs, 0, shortest / 2);

  SceneTrack video;
  video.id = "clips";
  int64_t start = 0;
  for (int c = 0; c < n; ++c) {
    SceneItem item;
    item.id = "clip" + std::to_string(c + 1);
    item.type = ItemType::Video;
    item.source = tl.clips[c];
    item.startUs = start;
    item.durationUs = infos[c].durationUs;
    video.items.push_back(item);
    if (c + 1 < n && overlap > 0) {
      SceneTransition x;
      x.from = c;
      x.kind = SceneTransitionKind::Push;
      x.direction = tl.transition.kind == TransitionKind::SlideLeft ? Direction::Left : Direction::Right;
      x.durationUs = overlap;
      video.transitions.push_back(x);
    }
    start += infos[c].durationUs - overlap;
  }
  s.tracks.push_back(std::move(video));

  // Captions: bottom center on a dark box. A caption ends where the next one starts.
  int64_t duration = s.durationUs();
  std::vector<TextOverlay> texts = tl.texts;
  std::stable_sort(texts.begin(), texts.end(), [](const TextOverlay& a, const TextOverlay& b) { return a.startUs < b.startUs; });
  SceneTrack captions;
  captions.id = "captions";
  for (size_t k = 0; k < texts.size(); ++k) {
    int64_t from = texts[k].startUs, to = std::min(texts[k].endUs, duration);
    if (k + 1 < texts.size()) to = std::min(to, texts[k + 1].startUs);
    if (to <= from) continue;
    SceneItem item;
    item.type = ItemType::Text;
    item.text = texts[k].text;
    item.startUs = from;
    item.durationUs = to - from;
    item.style.hasBox = true;
    item.transform.y = Animatable(0.96);
    item.transform.anchorY = 1;
    captions.items.push_back(item);
  }
  if (!captions.items.empty()) s.tracks.push_back(std::move(captions));
  return s;
}

// ---------------------------------------------------------------------------------------
// T1: probe every item on open, start seeks, demux. Each lane reads its items in order; a
// lane whose item is read to the end moves on to its next one. Reads whichever track has room
// and the lowest timeline decode time, so a full queue never stops another lane or track (§2.3).
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
  struct LaneRead {
    int pos = kNoItem;  // position in layout.laneItems(lane)
    bool eos[2] = {true, true};
  };

  const SceneLayout& layout() const { return ctx_.layout; }
  int itemAt(int li) const {
    const std::vector<int>& list = layout().laneItems(li);
    return reads_[li].pos < int(list.size()) ? list[reads_[li].pos] : -1;
  }

  // open(Timeline): the clips' durations place them, so they are probed before the scene exists.
  bool probeTimeline() {
    const Timeline& tl = *ctx_.timeline;
    std::vector<MediaInfo> infos(tl.clips.size());
    for (size_t c = 0; c < tl.clips.size(); ++c) {
      preopened_.push_back(ctx_.factory.createDemuxer());
      Result r = preopened_[c]->open(tl.clips[c], &infos[c]);
      if (r != Result::Ok) {
        ctx_.fatal(r, "clip" + std::to_string(c + 1) + ": cannot open media");
        return false;
      }
      if (infos[c].durationUs <= 0) {
        ctx_.fatal(Result::MalformedMedia, "clip" + std::to_string(c + 1) + ": bad duration");
        return false;
      }
      preinfos_.push_back(infos[c]);
    }
    ctx_.scene = sceneFromTimeline(tl, infos, ctx_.driver == Driver::Export ? &ctx_.exportSettings : nullptr);
    return true;
  }

  void probe() {
    if (ctx_.timeline && !probeTimeline()) return;
    std::string error;
    if (validateScene(ctx_.scene, &error) != Result::Ok) return ctx_.fatal(Result::InvalidArgument, error);
    if (!ctx_.layout.build(ctx_.scene, &error)) return ctx_.fatal(Result::Unsupported, error);
    int n = layout().items();
    ctx_.items.resize(n);
    for (size_t c = 0; c < preopened_.size(); ++c) {  // the Timeline's clips are items 0..n-1
      ctx_.items[c].demuxer = std::move(preopened_[c]);
      ctx_.items[c].info = preinfos_[c];
    }
    for (int l = 0; l < layout().lanes(); ++l) {
      auto lane = std::make_unique<Lane>();
      lane->videoDecoder = ctx_.factory.createVideoDecoder();
      lane->audioDecoder = ctx_.factory.createAudioDecoder();
      lane->videoPackets.setHooks([this] { ctx_.wake(StageId::VideoDecode); }, [this] { ctx_.wake(StageId::Source); });
      lane->audioPackets.setHooks([this] { ctx_.wake(StageId::Audio); }, [this] { ctx_.wake(StageId::Source); });
      lane->frames.setHooks([this] { ctx_.wake(StageId::Composition); }, [this] { ctx_.wake(StageId::VideoDecode); });
      ctx_.lanes.push_back(std::move(lane));
    }
    reads_.assign(layout().lanes(), LaneRead{});

    std::optional<std::pair<int, int>> firstAudio;  // rate, channels
    const TrackInfo* firstVideo = nullptr;
    const ItemRuntime* firstImage = nullptr;
    std::unique_ptr<IImageLoader> images;
    for (int i = 0; i < n; ++i) {
      const SceneItem& it = layout().item(i);
      ItemRuntime& rt = ctx_.items[i];
      std::string name = ctx_.itemName(i) + ": ";
      if (it.type == ItemType::Text) rt.text = std::make_shared<const std::string>(it.text);
      if (it.type == ItemType::Image) {
        if (!images) images = ctx_.factory.createImageLoader();
        if (!images) return ctx_.fatal(Result::Unsupported, name + "images are not supported on this platform");
        Result r = images->load(it.source, &rt.image, &rt.imageWidth, &rt.imageHeight);
        if (r != Result::Ok) return ctx_.fatal(r, name + "cannot load the image");
        rt.image.item = i;
        if (!firstImage) firstImage = &rt;
      }
      if (!layout().decodable(i)) continue;
      if (!rt.demuxer) {
        rt.demuxer = ctx_.factory.createDemuxer();
        Result r = rt.demuxer->open(it.source, &rt.info);
        if (r != Result::Ok) return ctx_.fatal(r, name + "cannot open media");
      }
      Lane& lane = *ctx_.lanes[layout().laneOf(i)];
      if (it.inUs >= rt.info.durationUs) return ctx_.fatal(Result::MalformedMedia, name + "`in` is past the end of the file (R10)");
      if (it.type == ItemType::Video) {
        const TrackInfo& v = rt.info.video;
        if (!v.supported) return ctx_.fatal(Result::NoDecoder, name + "video codec is not H.264");
        if (v.width <= 0 || v.height <= 0 || v.width > kMaxDimension || v.height > kMaxDimension || v.frameDurationUs <= 0) {
          return ctx_.fatal(Result::MalformedMedia, name + "bad video dimensions or frame rate");
        }
        if (v.rotated) ctx_.events.onWarning(Warning::RotationIgnored, name + "track matrix is not identity");
        // Configure once here, so a stream the decoder rejects fails open() and not playback.
        Result r = lane.videoDecoder->configure(v, [] {});
        if (r != Result::Ok) return ctx_.fatal(r, name + "video decoder configuration failed");
        if (!firstVideo) firstVideo = &v;
      }
      bool usable = false;
      if (rt.info.audio) {
        const TrackInfo& a = *rt.info.audio;
        usable = a.supported && a.sampleRate > 0 && a.channels > 0 && lane.audioDecoder->configure(a) == Result::Ok;
      }
      if (it.type == ItemType::Audio && !usable) return ctx_.fatal(Result::NoDecoder, name + "has no AAC-LC audio track (R9)");
      if (it.type == ItemType::Video && rt.info.audio && !usable && !it.mute) {
        ctx_.events.onWarning(Warning::AudioUnsupported, name + "audio is not AAC-LC; the item plays silent");
      }
      rt.mixAudio = usable && !it.mute;
      if (rt.mixAudio && !firstAudio) firstAudio = std::make_pair(rt.info.audio->sampleRate, rt.info.audio->channels);
    }

    const SceneOutput& o = ctx_.scene.output;
    ctx_.width = o.width > 0 ? o.width : firstVideo ? firstVideo->width : firstImage ? firstImage->imageWidth : 1280;
    ctx_.height = o.height > 0 ? o.height : firstVideo ? firstVideo->height : firstImage ? firstImage->imageHeight : 720;
    if (o.fpsNum > 0) {
      ctx_.fpsNum = o.fpsNum;
      ctx_.fpsDen = o.fpsDen;
    } else if (firstVideo) {
      ctx_.fpsNum = 1000000;
      ctx_.fpsDen = int(firstVideo->frameDurationUs);
    }
    bool audio = false;
    for (const ItemRuntime& rt : ctx_.items) audio |= rt.mixAudio;
    int rate = o.sampleRate > 0 ? o.sampleRate : firstAudio ? firstAudio->first : 48000;
    int channels = o.channels > 0 ? o.channels : firstAudio ? std::min(2, firstAudio->second) : 2;
    if (audio) ctx_.ring.open(rate, channels, rate / 5);  // 200 ms
    if (ctx_.driver == Driver::Export) {
      ExportSettings s = ctx_.exportSettings;
      s.width = ctx_.width;
      s.height = ctx_.height;
      s.fps = int(std::lround(double(ctx_.fpsNum) / ctx_.fpsDen));
      ctx_.exportSettings = s;
      Result r = ctx_.exportSink->open(ctx_.exportTarget, s, audio ? rate : 0, audio ? channels : 0);
      if (r != Result::Ok) return ctx_.fatal(r, "cannot create the output file");
    } else if (audio && ctx_.speaker->open(rate, channels, &ctx_.ring) != Result::Ok) {
      return ctx_.fatal(Result::AudioDeviceFailed, "audio output failed to open");
    }
    if (ctx_.autoDriver) {  // one video and nothing else visual: pace by its frames
      int visual = 0, video = 0;
      for (int i = 0; i < n; ++i) {
        visual += layout().item(i).type != ItemType::Audio;
        video += layout().item(i).type == ItemType::Video;
      }
      ctx_.driver = visual == 1 && video == 1 ? Driver::LeadingClip : Driver::Vsync;
    }
    ctx_.hasAudio = audio;
    ctx_.master.setAudio(audio);
    ctx_.durationUs = layout().durationUs();
    ctx_.probed = true;
    ctx_.requestSeek(0);  // preroll the first frame (A16)
  }

  // Each lane starts at its first item that hasn't ended by the target: the one playing, or
  // the next one, which is read ahead.
  void startSeek(PendingSeek seek) {
    for (auto& lane : ctx_.lanes) {
      lane->videoPackets.flush();
      lane->audioPackets.flush();
    }
    seek.lanePos.assign(layout().lanes(), 0);
    for (int li = 0; li < layout().lanes(); ++li) {
      const std::vector<int>& list = layout().laneItems(li);
      int pos = 0;
      while (pos < int(list.size()) && layout().item(list[pos]).endUs() <= seek.targetUs) ++pos;
      reads_[li] = LaneRead{};
      reads_[li].pos = seek.lanePos[li] = pos;
      if (pos < int(list.size()) && !begin(li, seek.targetUs)) return;
    }
    ctx_.setSeekTarget(seek);
    serial_ = ctx_.serial + 1;
    ctx_.serial = serial_;  // after the target is set: downstream reads it on seeing the new serial
    ctx_.wakeAll();
  }

  // Positions the lane's item at timeline time t (its start, or later when seeking into it).
  bool begin(int li, int64_t t) {
    int i = itemAt(li);
    ItemRuntime& rt = ctx_.items[i];
    Result r = rt.demuxer->seekTo(layout().mediaUs(i, t));
    if (r != Result::Ok) {
      ctx_.fatal(r, ctx_.itemName(i) + ": seek failed");
      return false;
    }
    reads_[li].eos[kVideo] = layout().item(i).type != ItemType::Video;
    reads_[li].eos[kAudio] = !rt.info.audio;  // read (and maybe drop) audio when the file has it
    return true;
  }

  // A lane whose item is fully read moves on to its next item.
  bool advanceLanes() {
    for (int li = 0; li < layout().lanes(); ++li) {
      LaneRead& lane = reads_[li];
      if (itemAt(li) < 0 || !lane.eos[kVideo] || !lane.eos[kAudio]) continue;
      ++lane.pos;
      int i = itemAt(li);
      if (i >= 0) begin(li, layout().item(i).startUs);
      return true;
    }
    return false;
  }

  // Audio packets are read (to keep the demuxer's tracks balanced) but dropped when not mixed.
  BoundedQueue<Packet>* queueFor(int li, int track) {
    if (track == kVideo) return &ctx_.lanes[li]->videoPackets;
    return ctx_.hasAudio ? &ctx_.lanes[li]->audioPackets : nullptr;
  }

  Progress readOne() {
    int bestLane = -1, bestTrack = kVideo;
    int64_t bestDts = 0;
    for (int li = 0; li < layout().lanes(); ++li) {
      int i = itemAt(li);
      if (i < 0) continue;
      for (int track : {kVideo, kAudio}) {
        if (reads_[li].eos[track]) continue;
        BoundedQueue<Packet>* q = queueFor(li, track);
        if (q && q->full()) continue;
        int64_t dts = 0;
        Result r = ctx_.items[i].demuxer->peekDtsUs(track, &dts);
        if (r == Result::Ok && layout().timelineUs(i, dts) > layout().item(i).endUs() + kReadPastEndUs) r = Result::Eos;
        if (r == Result::Eos) {
          pushEos(li, track);
          return Progress::did();
        }
        if (r != Result::Ok) {
          ctx_.fatal(r, ctx_.itemName(i) + ": demux failed");
          return Progress::idle();
        }
        dts = layout().timelineUs(i, dts);
        if (bestLane < 0 || dts < bestDts) {
          bestLane = li;
          bestTrack = track;
          bestDts = dts;
        }
      }
    }
    if (bestLane < 0) return Progress::idle();  // queues full or every lane ended

    int i = itemAt(bestLane);
    Packet p;
    Result r = ctx_.items[i].demuxer->read(bestTrack, &p);
    if (r == Result::Eos) {
      pushEos(bestLane, bestTrack);
      return Progress::did();
    }
    if (r != Result::Ok || p.data.size() > kMaxSampleBytes) {
      ctx_.fatal(Result::MalformedMedia, ctx_.itemName(i) + ": bad sample");
      return Progress::idle();
    }
    p.serial = serial_;
    p.item = i;
    bool keep = bestTrack == kVideo || ctx_.items[i].mixAudio;
    if (BoundedQueue<Packet>* q = queueFor(bestLane, bestTrack); q && keep) q->tryPush(p);  // has room: checked above
    return Progress::did();
  }

  // The end of an item's track, so T2 (video) or T4 (mixed audio) knows the lane moved on.
  void pushEos(int li, int track) {
    reads_[li].eos[track] = true;
    int i = itemAt(li);
    bool marked = track == kVideo ? layout().item(i).type == ItemType::Video : ctx_.items[i].mixAudio;
    BoundedQueue<Packet>* q = queueFor(li, track);
    if (!marked || !q) return;
    Packet eos;
    eos.track = track;
    eos.eos = true;
    eos.serial = serial_;
    eos.item = i;
    q->tryPush(eos);
  }

  Context& ctx_;
  bool opened_ = false;
  std::vector<LaneRead> reads_;
  std::vector<std::unique_ptr<IDemuxer>> preopened_;  // open(Timeline): the clips, probed first
  std::vector<MediaInfo> preinfos_;
  uint32_t serial_ = 0;
};

// ---------------------------------------------------------------------------------------
// T2: per lane, packets into the decoder, frames (PTS order) into the lane's frame queue.
// A lane's next item reconfigures the decoder once the previous item has drained.
class VideoDecodeStage : public Stage {
 public:
  explicit VideoDecodeStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.probed) return Progress::idle();
    if (lanes_.size() != ctx_.lanes.size()) lanes_.resize(ctx_.lanes.size());
    bool did = false;
    for (size_t li = 0; li < lanes_.size() && !ctx_.halted; ++li) did |= pumpLane(lanes_[li], *ctx_.lanes[li]);
    return did && !ctx_.halted ? Progress::did() : Progress::idle();
  }

 private:
  struct LaneState {
    uint32_t serial = 0;
    int item = -1;         // the item the decoder is configured for
    bool drained = true;   // nothing left in the decoder, so it can be reconfigured
    std::optional<Packet> input;
    std::optional<VideoFrame> output;
    bool skipToKey = false, eosSent = false;
  };

  bool pumpLane(LaneState& st, Lane& lane) {
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
            f.item = st.item;
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
            st.output->item = st.item;
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
          ctx_.fatal(Result::DecoderFailed, ctx_.itemName(st.item) + ": video decoder failed");
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
    if (st.input->item != st.item) {
      if (!st.drained) return did;  // the previous item's last frames are still coming out
      Result r = lane.videoDecoder->configure(ctx_.items[st.input->item].info.video, [this] { ctx_.wake(StageId::VideoDecode); });
      if (r != Result::Ok) {
        ctx_.fatal(r, ctx_.itemName(st.input->item) + ": video decoder configuration failed");
        return false;
      }
      st.item = st.input->item;
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
        ctx_.fatal(Result::DecoderFailed, ctx_.itemName(st.item) + ": video decoder failed");
        return false;
    }
  }

  Context& ctx_;
  std::vector<LaneState> lanes_;
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
// T4: decode each lane's audio and mix every sounding item on the timeline (spec §5.2) into
// the ring the speaker pulls from (or the export sink). Each item is read from its decoded
// samples at its own media time, `in + (t − start) × speed`, with linear interpolation, so
// sample-rate conversion, speed and trims fall out of one rule. Gain, pan and transition
// fades are evaluated at each chunk's ends and ramped across it.
class AudioStage : public Stage {
 public:
  explicit AudioStage(Context& c) : ctx_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.hasAudio) return Progress::idle();
    if (lanes_.empty()) setup();
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
        // A writer that interleaves tracks can hold the video until it has audio past it;
        // saying the audio is complete lets the last video through.
        ctx_.exportSink->endAudio();
        ctx_.audioWritten = true;
        ctx_.wake(StageId::VideoRender);
      }
      return Progress::did();
    }

    // Every item sounding in the next chunk must be decoded that far, or have ended.
    int64_t chunkEnd = std::min(cursor_ + kChunkFrames, endSample_);
    double t0 = double(cursor_) / rate_, t1 = double(chunkEnd) / rate_;
    for (int i : sounding(t0, t1)) {
      if (!ready(i, t1)) return pull(ctx_.layout.laneOf(i));
    }
    mix(chunkEnd);
    return Progress::did();
  }

 private:
  static constexpr int64_t kChunkFrames = 1024;

  // Decoded audio of the lane's current item, in the item's media samples at its own rate.
  struct LaneMix {
    std::vector<int> seq;     // the lane's mixed items, in order
    std::vector<int> seqPos;  // their positions in layout.laneItems(lane)
    int pos = 0;              // index into seq of the item being delivered
    bool ended = false;       // its audio has all been decoded
    int decoderItem = -1;
    int rate = 0, channels = 0;
    int64_t bufStart = 0;     // media sample index of buf's first frame
    std::vector<float> buf;   // interleaved
    int64_t nextStart = -1;   // where the next packet continues, once one is decoded
    int item() const { return pos < int(seq.size()) ? seq[pos] : -1; }
    int64_t bufEnd() const { return channels ? bufStart + int64_t(buf.size()) / channels : bufStart; }
  };

  const SceneLayout& layout() const { return ctx_.layout; }
  int64_t toUs(int64_t sample) const { return sample * 1000000 / rate_; }

  void setup() {
    rate_ = ctx_.ring.sampleRate();
    outChannels_ = ctx_.ring.channels();
    lanes_.resize(layout().lanes());
    seqIndex_.assign(layout().items(), -1);
    for (int li = 0; li < layout().lanes(); ++li) {
      const std::vector<int>& list = layout().laneItems(li);
      for (int k = 0; k < int(list.size()); ++k) {
        if (!ctx_.items[list[k]].mixAudio) continue;
        seqIndex_[list[k]] = int(lanes_[li].seq.size());
        lanes_[li].seq.push_back(list[k]);
        lanes_[li].seqPos.push_back(k);
      }
    }
  }

  void restart(uint32_t serial) {
    serial_ = serial;
    PendingSeek target = ctx_.seekTarget();
    for (int li = 0; li < int(lanes_.size()); ++li) {
      ctx_.lanes[li]->audioDecoder->flush();
      LaneMix& lane = lanes_[li];
      lane.pos = 0;
      while (lane.pos < int(lane.seq.size()) && lane.seqPos[lane.pos] < target.lanePos[li]) ++lane.pos;
      resetBuffer(lane);
    }
    cursor_ = std::llround(double(target.targetUs) * rate_ / 1e6);  // exact seek: nothing before the target is mixed
    endSample_ = std::llround(double(layout().durationUs()) * rate_ / 1e6);
    ctx_.ring.flush(target.targetUs, serial_);
    chunk_.clear();
    eosMarked_ = false;
  }

  static void resetBuffer(LaneMix& lane) {
    lane.ended = false;
    lane.buf.clear();
    lane.bufStart = 0;
    lane.nextStart = -1;
  }

  // Mixed items sounding somewhere in [t0, t1) seconds.
  std::vector<int> sounding(double t0, double t1) const {
    std::vector<int> out;
    for (int i = 0; i < layout().items(); ++i) {
      const SceneItem& it = layout().item(i);
      if (ctx_.items[i].mixAudio && it.startUs / 1e6 < t1 && it.endUs() / 1e6 > t0) out.push_back(i);
    }
    return out;
  }

  // Media sample position of item i at timeline time t (seconds), at the item's source rate.
  double sourcePos(int i, double t, int rate) const {
    const SceneItem& it = layout().item(i);
    return (it.inUs / 1e6 + (t - it.startUs / 1e6) * it.speed) * rate;
  }

  bool ready(int i, double t1) {
    LaneMix& lane = lanes_[layout().laneOf(i)];
    int k = seqIndex_[i];
    while (k > lane.pos && lane.ended) {  // the lane's earlier item is done: move on
      ++lane.pos;
      resetBuffer(lane);
    }
    if (k < lane.pos) return true;  // already finished
    if (k > lane.pos) return false;
    if (lane.ended) return true;
    if (!lane.rate) return false;
    double end = std::min(t1, layout().item(i).endUs() / 1e6);
    return lane.bufEnd() >= int64_t(std::ceil(sourcePos(i, end, lane.rate))) + 2;
  }

  Progress pull(int li) {
    LaneMix& lane = lanes_[li];
    Packet p;
    if (!ctx_.lanes[li]->audioPackets.tryPop(&p)) return Progress::idle();
    if (p.serial != serial_) return Progress::did();
    int k = seqIndex_[p.item];
    if (k < lane.pos) return Progress::did();  // an item the mix is already past
    while (k > lane.pos) {  // the previous item ended (its end marker came first)
      ++lane.pos;
      resetBuffer(lane);
    }
    if (p.eos) {
      lane.ended = true;
      return Progress::did();
    }
    const TrackInfo& format = *ctx_.items[p.item].info.audio;
    IAudioDecoder& decoder = *ctx_.lanes[li]->audioDecoder;
    if (lane.decoderItem != p.item) {
      if (decoder.configure(format) != Result::Ok) {
        ctx_.fatal(Result::DecoderFailed, ctx_.itemName(p.item) + ": audio decoder configuration failed");
        return Progress::idle();
      }
      lane.decoderItem = p.item;
    }
    PcmBuffer pcm;
    Result r = decoder.decode(p, &pcm);  // on CorruptFrame, pcm is silence (A14)
    if (r == Result::CorruptFrame) {
      ctx_.metrics.countCorrupt();
    } else if (r != Result::Ok) {
      ctx_.fatal(Result::DecoderFailed, ctx_.itemName(p.item) + ": audio decoder failed");
      return Progress::idle();
    }
    lane.rate = format.sampleRate;
    lane.channels = format.channels;
    append(lane, pcm);
    return Progress::did();
  }

  // Adds a decoded packet to the lane's buffer. Timestamps are rounded (to 1 us here, often
  // coarser in the file): packets within 1 ms of each other are one continuous stream, so
  // rounding never doubles or drops a sample.
  static void append(LaneMix& lane, const PcmBuffer& pcm) {
    int ch = lane.channels;
    int64_t frames = int64_t(pcm.samples.size()) / ch;
    int64_t start = std::llround(double(pcm.ptsUs) * lane.rate / 1e6);
    if (lane.nextStart >= 0 && std::llabs(start - lane.nextStart) <= lane.rate / 1000) start = lane.nextStart;
    lane.nextStart = start + frames;
    if (lane.buf.empty()) lane.bufStart = start;
    int64_t skip = std::max<int64_t>(0, lane.bufEnd() - start);  // overlaps what's there: keep the first
    if (start > lane.bufEnd()) lane.buf.resize(size_t(start - lane.bufStart) * ch, 0.0f);  // a gap: silence
    for (int64_t f = skip; f < frames; ++f) {
      for (int c = 0; c < ch; ++c) lane.buf.push_back(float(pcm.samples[size_t(f) * ch + c]));
    }
  }

  // Sums the sounding items over [cursor_, chunkEnd), each cut to its time on the timeline.
  void mix(int64_t chunkEnd) {
    int n = int(chunkEnd - cursor_);
    std::vector<float> sum(size_t(n) * outChannels_, 0.0f);
    double t0 = double(cursor_) / rate_, t1 = double(chunkEnd) / rate_;
    for (int i : sounding(t0, t1)) {
      LaneMix& lane = lanes_[layout().laneOf(i)];
      if (lane.item() != i || lane.buf.empty()) continue;
      const SceneItem& it = layout().item(i);
      const SceneTrack& track = layout().trackOf(i);
      auto gainAt = [&](double t) {
        int64_t us = std::clamp<int64_t>(std::llround(t * 1e6), it.startUs, it.endUs());
        return track.gain * it.gain.at(us - it.startUs) * layout().transitionGain(i, us);
      };
      double g0 = gainAt(t0), g1 = gainAt(t1);
      double p0 = it.pan.at(std::llround(t0 * 1e6) - it.startUs), p1 = it.pan.at(std::llround(t1 * 1e6) - it.startUs);
      int ch = lane.channels;
      int64_t frames = int64_t(lane.buf.size()) / ch;
      for (int j = 0; j < n; ++j) {
        double t = double(cursor_ + j) / rate_;
        if (t * 1e6 < it.startUs || t * 1e6 >= it.endUs()) continue;
        double x = sourcePos(i, t, lane.rate) - double(lane.bufStart);
        int64_t k = int64_t(std::floor(x));
        if (k < 0 || k >= frames) continue;
        double frac = x - double(k);
        int64_t k1 = std::min(k + 1, frames - 1);
        const float* a = &lane.buf[size_t(k) * ch];
        const float* b = &lane.buf[size_t(k1) * ch];
        double left = a[0] + (b[0] - a[0]) * frac;
        double right = ch > 1 ? a[1] + (b[1] - a[1]) * frac : left;
        double w = double(j) / n;
        double g = g0 + (g1 - g0) * w, pan = p0 + (p1 - p0) * w;
        float* out = &sum[size_t(j) * outChannels_];
        if (outChannels_ == 1) {
          out[0] += float(g * (left + right) / 2);
        } else {
          out[0] += float(g * std::min(1.0, 1 - pan) * left);
          out[1] += float(g * std::min(1.0, 1 + pan) * right);
        }
      }
      // Drop what the mix has passed (keep one frame for interpolation).
      int64_t keep = std::max<int64_t>(0, int64_t(std::floor(sourcePos(i, t1, lane.rate))) - 1 - lane.bufStart);
      keep = std::min(keep, frames);
      lane.buf.erase(lane.buf.begin(), lane.buf.begin() + size_t(keep) * ch);
      lane.bufStart += keep;
    }
    chunk_.resize(sum.size());
    for (size_t k = 0; k < sum.size(); ++k) chunk_[k] = static_cast<int16_t>(std::clamp(std::lround(sum[k]), -32768L, 32767L));
    offset_ = 0;
    chunkStart_ = cursor_;
    cursor_ = chunkEnd;
  }

  Context& ctx_;
  uint32_t serial_ = 0;
  int rate_ = 48000, outChannels_ = 2;
  std::vector<LaneMix> lanes_;
  std::vector<int> seqIndex_;           // per item: its index in its lane's seq, or -1
  int64_t cursor_ = 0, endSample_ = 0;  // timeline samples at the output rate
  int64_t chunkStart_ = 0;              // timeline sample of chunk_'s first frame
  std::vector<int16_t> chunk_;          // mixed, not yet fully written
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
  composed.setHooks([this] { wake(StageId::VideoRender); }, [this] { wake(StageId::Composition); });
}

void Context::wakeAll() {
  for (int i = 0; i < kStageCount; ++i) wake(static_cast<StageId>(i));
}

std::string Context::itemName(int item) const {
  if (item < 0 || item >= layout.items()) return "item";
  const std::string& id = layout.item(item).id;
  return id.empty() ? "item " + std::to_string(item + 1) : id;
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
  pending_ = PendingSeek{targetUs, hostClock.nowNs(), {}};
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
          makeCompositionStage(ctx), std::make_unique<VideoRenderStage>(ctx),
          std::make_unique<AudioStage>(ctx)};
}

}  // namespace mf
