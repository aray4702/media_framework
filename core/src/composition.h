#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "pipeline.h"

namespace mf {

// Per-lane view of the decoded frames for the composition stage (§2.5): each lane's next
// frame (`head`) and each item's latest frame at or before the last output time. Drivers move
// the lanes forward to the output times they choose, and compose from them.
class FrameSampler {
 public:
  explicit FrameSampler(Context& ctx) : ctx_(ctx) {}

  void restart(uint32_t serial, const PendingSeek& target);
  void setSeeking(bool seeking) { seeking_ = seeking; }  // frames replaced while seeking count as decode-only

  Context& context() const { return ctx_; }
  const SceneLayout& layout() const { return ctx_.layout; }
  int lanes() const { return static_cast<int>(lanes_.size()); }
  int64_t timeOf(const VideoFrame& f) const { return layout().timelineUs(f.item, f.ptsUs); }

  // Takes the lane's next item from its frame queue: a frame into its head, or the end of an item.
  bool pop(int lane);
  // Moves the lane's head to its item's latest frame, unless it falls after the item's end.
  void take(int lane);
  // Consumes every frame the lane has up to t (and any past its item's end).
  void advance(int lane, int64_t t);
  void advanceAll(int64_t t);

  // Whether video item i's frame for t is known: its next frame is later than t, or it has ended.
  bool exactAt(int item, int64_t t) const;
  bool allExactAt(int64_t t) const;  // every video item visible at t
  bool mayDeliver(int item) const;   // its lane hasn't moved past it

  const VideoFrame* frameOf(int item) const;
  const std::optional<VideoFrame>& head(int lane) const { return lanes_[lane].head; }
  int64_t floorOf(int lane) const;  // earliest time the lane's next frame can have
  bool done() const;                // every lane has delivered all of its items

  std::vector<int> videoAt(int64_t t) const;  // video items visible at t, bottom to top
  int leadItem(int64_t t) const;  // the highest frame rate among them (the upper one on a tie); -1: none

  // The output frame at t: every visible item's layer with its values evaluated at t.
  ComposedFrame composeAt(int64_t t) const;

 private:
  struct LaneView {
    std::vector<int> seq;     // the lane's video items, in order
    std::vector<int> seqPos;  // their positions in layout.laneItems(lane)
    int pos = 0;              // index into seq of the item being delivered
    bool done = true;
    std::optional<VideoFrame> head;
    std::vector<VideoFrame> latest;  // per item: its latest taken frame
  };
  void setup();

  Context& ctx_;
  uint32_t serial_ = 0;
  bool seeking_ = false;
  std::vector<LaneView> lanes_;
  std::vector<int> seqIndex_;  // per item: its index in its lane's seq, or -1
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
