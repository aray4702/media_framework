#pragma once

#include <optional>

#include "composition.h"

namespace mf {

// One output frame per frame of the leading clip (the highest frame rate among the active
// clips), in timeline order; T3 paces them with AvSync. A frame goes next only once the other
// lane can't still produce an earlier one, so every other layer is exact at that time.
class LeadingClipDriver : public CompositionDriver {
 public:
  Progress step(FrameSampler&, CompositionOutput&) override;
};

// One output frame per display refresh, composed half a frame before it must be handed to the
// display, at the clock time it will be seen. Never waits for a layer: one whose decode is
// behind keeps its last frame (a late layer). Refreshes where nothing visible changes produce
// no frame. Composes only while output runs.
class VsyncDriver : public CompositionDriver {
 public:
  explicit VsyncDriver(Context& ctx) : ctx_(ctx) {}
  void restart() override;
  void seekShown(const ComposedFrame& f) override { last_ = f; }
  Progress step(FrameSampler&, CompositionOutput&) override;

 private:
  bool unchanged(const ComposedFrame&) const;

  Context& ctx_;
  int64_t nextSlotNs_ = 0, gridNs_ = 0;
  std::optional<ComposedFrame> last_;  // the last output, to skip unchanged refreshes
};

// Output frames on the fixed grid n / fps, each composed once every layer has its exact frame.
// Starts at 0 without a seek frame.
class ExportDriver : public CompositionDriver {
 public:
  explicit ExportDriver(int fps) : fps_(fps) {}
  bool startsWithSeek() const override { return false; }
  void restart() override { index_ = 0; }
  Progress step(FrameSampler&, CompositionOutput&) override;

 private:
  int fps_;
  int64_t index_ = 0;
};

}  // namespace mf
