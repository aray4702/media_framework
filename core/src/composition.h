#pragma once

#include <memory>
#include <optional>

#include "pipeline.h"

namespace mf {

// Per-lane view of the decoded frames for the composition stage (§2.4, §2.5): each lane's
// next frame (`head`) and its latest frame at or before the last output time (`held`).
// Drivers move the lanes forward to the output times they choose and compose from them.
class FrameSampler {
 public:
  explicit FrameSampler(Context& ctx) : ctx_(ctx) {}

  void restart(uint32_t serial, const PendingSeek& target);
  void setSeeking(bool seeking) { seeking_ = seeking; }  // frames replaced while seeking count as decode-only

  Context& context() const { return ctx_; }
  const TimelineLayout& layout() const { return ctx_.layout; }
  int64_t timeOf(const VideoFrame& f) const { return layout().startUs(f.clip) + f.ptsUs; }

  // Takes the lane's next item from its frame queue: a frame into its head, or the end of a clip.
  bool pop(int lane);
  // Moves the lane's head to its held frame, unless the frame falls after its clip's end.
  void take(int lane);
  // Consumes every frame the lane has up to t, so its held frame is the latest at or before t.
  void advance(int lane, int64_t t);
  void advanceAll(int64_t t);

  // Whether clip c's frame for t is known: its next frame is later than t, or it has ended.
  bool exactAt(int clip, int64_t t) const;
  bool allExactAt(int64_t t) const;
  // Whether the lane may still deliver frames of clip c (it hasn't moved past it).
  bool mayDeliver(int clip) const;

  const VideoFrame* frameOf(int clip) const;  // the clip's held frame, if any
  const std::optional<VideoFrame>& head(int lane) const { return lanes_[lane].head; }
  // Earliest time the lane's next frame can have: frames come in PTS order per clip.
  int64_t floorOf(int lane) const;
  bool done() const;  // every lane has delivered all of its clips

  // The output frame at t from each active clip's latest frame, outgoing clip first.
  ComposedFrame composeAt(int64_t t) const;

 private:
  struct LaneView {
    int clip = kNoClip;  // the clip the lane is delivering
    bool done = true;    // no clips left on this lane
    std::optional<VideoFrame> head;
    std::optional<VideoFrame> held;
  };

  Context& ctx_;
  uint32_t serial_ = 0;
  bool seeking_ = false;
  LaneView lanes_[kLanes];
};

// Where a driver's frames go: to T3, in order.
class CompositionOutput {
 public:
  virtual void emit(ComposedFrame) = 0;
  virtual void finish() = 0;  // the end of the timeline: no more frames for this serial

 protected:
  ~CompositionOutput() = default;
};

// Picks the output times and composes the frames for them (§2.5). The composition stage
// runs the exact seek first (unless startsWithSeek() is false), then calls step() until finished.
class CompositionDriver {
 public:
  virtual ~CompositionDriver() = default;
  virtual bool startsWithSeek() const { return true; }
  virtual void restart() {}                       // a new serial: forget per-run state
  virtual void seekShown(const ComposedFrame&) {}  // the frame the seek showed
  virtual Progress step(FrameSampler&, CompositionOutput&) = 0;
};

std::unique_ptr<Stage> makeCompositionStage(Context&);

}  // namespace mf
