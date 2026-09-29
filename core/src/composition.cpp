#include "composition.h"

#include <algorithm>
#include <deque>

#include "drivers.h"

namespace mf {

// --- FrameSampler ---------------------------------------------------------------------------

void FrameSampler::setup() {
  lanes_.resize(layout().lanes());
  seqIndex_.assign(layout().items(), -1);
  for (int li = 0; li < lanes(); ++li) {
    const std::vector<int>& list = layout().laneItems(li);
    for (int k = 0; k < int(list.size()); ++k) {
      if (layout().item(list[k]).type != ItemType::Video) continue;
      seqIndex_[list[k]] = int(lanes_[li].seq.size());
      lanes_[li].seq.push_back(list[k]);
      lanes_[li].seqPos.push_back(k);
    }
  }
}

void FrameSampler::restart(uint32_t serial, const PendingSeek& target) {
  if (lanes_.empty()) setup();
  serial_ = serial;
  for (int li = 0; li < lanes(); ++li) {
    LaneView& lane = lanes_[li];
    lane.head.reset();
    lane.latest.clear();
    lane.pos = 0;
    while (lane.pos < int(lane.seq.size()) && lane.seqPos[lane.pos] < target.lanePos[li]) ++lane.pos;
    lane.done = lane.pos >= int(lane.seq.size());
  }
}

bool FrameSampler::pop(int li) {
  LaneView& lane = lanes_[li];
  if (lane.head || lane.done) return false;
  VideoFrame f;
  if (!ctx_.lanes[li]->frames.tryPop(&f)) return false;
  if (f.serial != serial_) return true;
  if (f.eos) {  // the item has no more frames: the lane moves on to its next video item
    lane.pos = seqIndex_[f.item] + 1;
    lane.done = lane.pos >= int(lane.seq.size());
    return true;
  }
  lane.head = std::move(f);
  return true;
}

void FrameSampler::take(int li) {
  LaneView& lane = lanes_[li];
  VideoFrame f = std::move(*lane.head);
  lane.head.reset();
  int64_t t = timeOf(f);
  if (t >= layout().item(f.item).endUs()) return;  // past the item's end: never shown
  // Forget items this lane has finished showing.
  lane.latest.erase(std::remove_if(lane.latest.begin(), lane.latest.end(),
                                   [&](const VideoFrame& v) { return v.item != f.item && layout().item(v.item).endUs() <= t; }),
                    lane.latest.end());
  for (VideoFrame& v : lane.latest) {
    if (v.item == f.item) {
      if (seeking_) ctx_.metrics.countDecodeOnly();
      v = std::move(f);
      return;
    }
  }
  lane.latest.push_back(std::move(f));
}

void FrameSampler::advance(int li, int64_t t) {
  LaneView& lane = lanes_[li];
  for (;;) {
    if (!lane.head && !pop(li)) return;
    if (!lane.head) continue;
    int64_t time = timeOf(*lane.head);
    if (time > t && time < layout().item(lane.head->item).endUs()) return;
    take(li);
  }
}

void FrameSampler::advanceAll(int64_t t) {
  for (int li = 0; li < lanes(); ++li) advance(li, t);
}

bool FrameSampler::exactAt(int item, int64_t t) const {
  const LaneView& lane = lanes_[layout().laneOf(item)];
  int k = seqIndex_[item];
  if (lane.done || lane.pos > k) return true;
  return lane.pos == k && lane.head && timeOf(*lane.head) > t;
}

bool FrameSampler::allExactAt(int64_t t) const {
  for (int i : videoAt(t)) {
    if (!exactAt(i, t)) return false;
  }
  return true;
}

bool FrameSampler::mayDeliver(int item) const {
  const LaneView& lane = lanes_[layout().laneOf(item)];
  return !lane.done && lane.pos <= seqIndex_[item];
}

const VideoFrame* FrameSampler::frameOf(int item) const {
  if (layout().laneOf(item) < 0) return nullptr;
  for (const VideoFrame& v : lanes_[layout().laneOf(item)].latest) {
    if (v.item == item) return &v;
  }
  return nullptr;
}

int64_t FrameSampler::floorOf(int li) const {
  const LaneView& lane = lanes_[li];
  if (lane.done) return kNever;
  int item = lane.seq[lane.pos];
  int64_t floor = layout().timelineUs(item, 0);  // media time 0: the earliest a frame can be
  if (const VideoFrame* f = frameOf(item)) floor = std::max(floor, timeOf(*f) + 1);
  return floor;
}

bool FrameSampler::done() const {
  for (const LaneView& lane : lanes_) {
    if (!lane.done || lane.head) return false;
  }
  return true;
}

std::vector<int> FrameSampler::videoAt(int64_t t) const {
  std::vector<SceneLayout::Visible> visible;
  layout().visibleAt(t, &visible);
  std::vector<int> out;
  for (const SceneLayout::Visible& v : visible) {
    if (layout().item(v.item).type == ItemType::Video) out.push_back(v.item);
  }
  return out;
}

int FrameSampler::leadItem(int64_t t) const {
  int lead = -1;
  for (int i : videoAt(t)) {
    if (lead < 0 || ctx_.items[i].info.video.frameDurationUs <= ctx_.items[lead].info.video.frameDurationUs) lead = i;
  }
  return lead;
}

// Effects evaluated at `at`: item-local time for an item's, scene time for a track's.
static ComposedEffects evaluate(const SceneEffects& e, int64_t at) {
  ComposedEffects out;
  if (e.crop) {
    out.crop[0] = float(e.cropLeft.at(at));
    out.crop[1] = float(e.cropTop.at(at));
    out.crop[2] = float(e.cropRight.at(at));
    out.crop[3] = float(e.cropBottom.at(at));
  }
  if (e.colorAdjust) {
    out.brightness = float(e.brightness.at(at));
    out.contrast = float(e.contrast.at(at));
    out.saturation = float(e.saturation.at(at));
  }
  if (e.blur) out.blur = float(e.blurRadius.at(at));
  if (e.chromaKey) {
    out.chromaKey = true;
    out.keyColor = e.keyColor;
    out.keyTolerance = e.keyTolerance;
    out.keySoftness = e.keySoftness;
  }
  return out;
}

ComposedFrame FrameSampler::composeAt(int64_t t) const {
  ComposedFrame out;
  out.ptsUs = t;
  out.serial = serial_;
  out.width = ctx_.width;
  out.height = ctx_.height;
  out.background = ctx_.scene.output.background;
  out.filter = ctx_.filter();
  int lead = leadItem(t);
  out.frameDurationUs = lead >= 0 ? ctx_.items[lead].info.video.frameDurationUs : int64_t(ctx_.fpsDen) * 1000000 / ctx_.fpsNum;

  std::vector<SceneLayout::Visible> visible;
  layout().visibleAt(t, &visible);
  // A track is drawn on its own first (a group) while two of its items are visible, or when it
  // has effects (§5.1). A lone item without track effects is drawn onto the canvas directly.
  const std::vector<SceneTrack>& tracks = ctx_.scene.tracks;
  std::vector<int> shown(tracks.size(), 0), groupOf(tracks.size(), -1);
  for (const SceneLayout::Visible& v : visible) ++shown[v.track];
  for (const SceneLayout::Visible& v : visible) {
    const SceneItem& it = layout().item(v.item);
    const ItemRuntime& rt = ctx_.items[v.item];
    int64_t local = t - it.startUs;
    ComposedLayer l;
    l.item = v.item;
    switch (it.type) {
      case ItemType::Video: {
        const VideoFrame* f = frameOf(v.item);
        if (!f) continue;  // not decoded yet: nothing to draw
        l.kind = ComposedLayer::Kind::Video;
        l.frame = *f;
        break;
      }
      case ItemType::Image:
        l.kind = ComposedLayer::Kind::Image;
        l.frame = rt.image;
        break;
      case ItemType::Text:
        l.kind = ComposedLayer::Kind::Text;
        l.text = rt.text;
        l.style = it.style;
        break;
      case ItemType::Color:
        l.kind = ComposedLayer::Kind::Color;
        l.color = it.color;
        break;
      case ItemType::Audio:
        continue;
    }
    const SceneTransform& tr = it.transform;
    l.fit = it.type == ItemType::Text ? Fit::None : it.fit;
    l.x = float(tr.x.at(local));
    l.y = float(tr.y.at(local));
    l.anchorX = tr.anchorX;
    l.anchorY = tr.anchorY;
    l.scale = float(tr.scale.at(local));
    l.rotation = float(tr.rotation.at(local));
    l.offsetX = v.offsetX;
    l.offsetY = v.offsetY;
    std::copy(v.clip, v.clip + 4, l.clip);
    l.effects = evaluate(it.effects, local);
    const SceneTrack& track = tracks[v.track];
    float opacity = float(it.opacity.at(local)) * v.fade;
    if (shown[v.track] > 1 || track.effects.any()) {
      int& g = groupOf[v.track];
      if (g < 0) {
        g = int(out.groups.size());
        ComposedGroup group;
        group.track = v.track;
        group.opacity = track.opacity;
        group.effects = evaluate(track.effects, t);
        out.groups.push_back(group);
      }
      out.groups[g].blend = it.blend;  // the top item's: the one drawn last
      l.group = g;
      l.opacity = opacity;
      l.blend = v.mix ? Blend::Add : Blend::Normal;  // combined over a transparent image
    } else {
      l.opacity = opacity * track.opacity;
      l.blend = it.blend;
    }
    out.layers.push_back(std::move(l));
  }
  return out;
}

namespace {

// ---------------------------------------------------------------------------------------
// TC: composition (§2.4, §2.5). Hands composed frames to T3 in order. After each seek it shows
// the exact seek frame, then lets the driver pick the output times.
class CompositionStage : public Stage, private CompositionOutput {
 public:
  explicit CompositionStage(Context& c) : ctx_(c), sampler_(c) {}

  Progress pump() override {
    if (ctx_.halted || !ctx_.probed) return Progress::idle();
    if (!driver_) driver_ = makeDriver();
    uint32_t serial = ctx_.serial;
    if (serial != serial_) restart(serial);
    if (serial_ == 0) return Progress::idle();  // the preroll seek hasn't started yet
    while (!out_.empty()) {
      if (!ctx_.composed.tryPush(out_.front())) return Progress::idle();  // T3 frees a slot and wakes us
      out_.pop_front();
    }
    if (ended_) return Progress::idle();
    if (seeking_) return seekStep();
    return driver_->step(sampler_, *this);
  }

 private:
  std::unique_ptr<CompositionDriver> makeDriver() {
    switch (ctx_.driver) {
      case Driver::LeadingClip: return std::make_unique<LeadingClipDriver>();
      case Driver::Vsync: return std::make_unique<VsyncDriver>(ctx_);
      case Driver::Export: return std::make_unique<ExportDriver>(ctx_.fpsNum, ctx_.fpsDen);
    }
    return nullptr;
  }

  void restart(uint32_t serial) {
    serial_ = serial;
    PendingSeek target = ctx_.seekTarget();
    targetUs_ = std::clamp<int64_t>(target.targetUs, 0, ctx_.layout.durationUs() - 1);
    sampler_.restart(serial, target);
    driver_->restart();
    out_.clear();
    ended_ = false;
    setSeeking(driver_->startsWithSeek());
  }

  void setSeeking(bool seeking) {
    seeking_ = seeking;
    sampler_.setSeeking(seeking);
  }

  void emit(ComposedFrame f) override { out_.push_back(std::move(f)); }

  void finish() override {
    ComposedFrame eos;
    eos.eos = true;
    eos.serial = serial_;
    out_.push_back(std::move(eos));
    ended_ = true;
  }

  // Exact seek (§4), for every driver that starts with one. The picture is the leading item's
  // last frame at or before the target (its first frame, if none is). The frame is composed at
  // the target, so a still that starts there is on that picture. A frame that falls after the
  // target is composed at its own time. While a newer seek is pending (scrubbing), shows
  // whatever is decoded, at that frame's time.
  Progress seekStep() {
    FrameSampler& s = sampler_;
    bool scrub = ctx_.hasPendingSeek();
    s.advanceAll(targetUs_);
    if (!s.allExactAt(targetUs_) && !scrub) return Progress::idle();  // the lanes' frame queues wake us

    int lead = s.leadItem(targetUs_);
    std::optional<int64_t> t;
    if (lead >= 0) {
      const std::optional<VideoFrame>& head = s.head(ctx_.layout.laneOf(lead));
      if (const VideoFrame* f = s.frameOf(lead)) {
        t = s.timeOf(*f);
      } else if (head && head->item == lead) {
        t = s.timeOf(*head);  // nothing at or before the target: the first frame after it
      } else if (s.mayDeliver(lead) && !scrub) {
        return Progress::idle();
      }
    }
    if (t && *t > targetUs_) {
      s.advanceAll(*t);
      if (!s.allExactAt(*t) && !scrub) return Progress::idle();
    }
    int64_t at = targetUs_;
    if (t && (*t > targetUs_ || scrub)) at = *t;  // after the target, or a scrub keyframe
    ComposedFrame out = s.composeAt(at);
    bool missingVideo = false;
    for (int i : s.videoAt(at)) missingVideo |= !s.frameOf(i);
    if (missingVideo && out.layers.empty()) {
      if (!s.done()) return Progress::idle();  // nothing decoded yet, even when scrubbing
      setSeeking(false);
      finish();  // nothing to show: the seek completes without a frame (A5)
      return Progress::did();
    }
    setSeeking(false);
    driver_->seekShown(out);
    emit(std::move(out));
    return Progress::did();
  }

  Context& ctx_;
  FrameSampler sampler_;
  std::unique_ptr<CompositionDriver> driver_;  // made once probing is done: the driver is set by then
  uint32_t serial_ = 0;
  int64_t targetUs_ = 0;
  std::deque<ComposedFrame> out_;  // composed, waiting for room in the queue to T3
  bool seeking_ = false, ended_ = false;
};

}  // namespace

std::unique_ptr<Stage> makeCompositionStage(Context& ctx) { return std::make_unique<CompositionStage>(ctx); }

}  // namespace mf
