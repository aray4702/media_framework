#pragma once

#include <optional>
#include <vector>

#include "composition.h"
#include "decode_rate.h"

namespace mf {

// The decode rate control (DecodeRate) as the live drivers run it: reads each lane's signals from
// the pipeline, ticks the controller, and stores the new steps and the start step for T2.
class DecodeControl {
 public:
  void restart() { rate_.restart(); }  // a seek: T2 puts every lane back to step 0
  // One tick at timeline time t. Per lane: `visible`, its visible video item (-1: none); `behind`,
  // whether that item is late. A lane takes part while T2 decodes a video item on it or one of its
  // items is visible. It is ahead once it has decoded two frames past t, or to its item's end.
  void tick(Context& ctx, int64_t t, const std::vector<int>& visible, const std::vector<bool>& behind, bool holdExpired);

 private:
  DecodeRate rate_;  // kept across seeks: its momentum and step-up backoff carry over
  std::vector<DecodeRate::Lane> lanes_;
};

// One output frame per frame of the leading video item (the highest frame rate among the
// visible ones), in timeline order; T3 paces them with AvSync. A frame goes next only once no
// other lane can still produce an earlier one, so every other layer is exact at that time.
// Where no video is visible there are no output times, and the last frame stays on screen.
// A frame of the leading item whose time the master clock has already passed is late (T3 drops
// it), which drives the decode rate control.
class LeadingClipDriver : public CompositionDriver {
 public:
  explicit LeadingClipDriver(Context& ctx) : ctx_(ctx) {}
  void restart() override { control_.restart(); }
  Progress step(FrameSampler&, CompositionOutput&) override;

 private:
  void adaptDecode(FrameSampler& s, int item, int64_t t);

  Context& ctx_;
  DecodeControl control_;
};

// One output frame per display refresh, composed half a frame before it must be handed to the
// display, at the clock time it will be seen. Never waits long for a layer: one whose decode is
// behind keeps its last frame (a late layer). Refreshes where nothing visible changes produce
// no frame. Composes only while output runs. Drives the decode rate control (DecodeRate).
class VsyncDriver : public CompositionDriver {
 public:
  // The longest the picture is held for a late layer (timeline time), before late frames are shown.
  static constexpr int64_t kMaxHoldUs = 100000;

  explicit VsyncDriver(Context& ctx) : ctx_(ctx) {}
  void restart() override;
  void seekShown(const ComposedFrame& f) override { last_ = f; }
  Progress step(FrameSampler&, CompositionOutput&) override;

 private:
  bool unchanged(const ComposedFrame&) const;

  Context& ctx_;
  int64_t nextSlotNs_ = 0, gridNs_ = 0;
  std::optional<ComposedFrame> last_;  // the last output, to skip unchanged refreshes
  // An item that missed its first visible frame must catch up before it replaces the completed
  // composition already on screen.  Otherwise its first delayed frame is displayed and appears
  // frozen while the decoder works through its backlog.  The hold is bounded (kMaxHoldUs): past
  // it the item's late frames are shown, and the decode rate steps down at once.
  enum class Hold : uint8_t { None, Holding, Expired };
  std::vector<Hold> hold_;
  std::vector<int64_t> holdSinceUs_;
  DecodeControl control_;
};

// Output frames on the fixed grid n / fps, each composed once every layer has its exact frame.
// Starts at 0 without a seek frame.
class ExportDriver : public CompositionDriver {
 public:
  ExportDriver(int fpsNum, int fpsDen) : fpsNum_(fpsNum), fpsDen_(fpsDen) {}
  bool startsWithSeek() const override { return false; }
  void restart() override { index_ = 0; }
  Progress step(FrameSampler&, CompositionOutput&) override;

 private:
  int fpsNum_, fpsDen_;
  int64_t index_ = 0;
};

}  // namespace mf
