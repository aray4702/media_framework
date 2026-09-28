#include "drivers.h"

namespace mf {

Progress ExportDriver::step(FrameSampler& s, CompositionOutput& out) {
  int64_t t = index_ * fpsDen_ * 1000000 / fpsNum_;
  if (t >= s.layout().durationUs()) {
    out.finish();
    return Progress::did();
  }
  s.advanceAll(t);
  if (!s.allExactAt(t)) return Progress::idle();  // the lanes' frame queues wake us
  ComposedFrame frame = s.composeAt(t);  // a video item without a frame yet (its first is later) is left out
  frame.frameDurationUs = int64_t(fpsDen_) * 1000000 / fpsNum_;
  out.emit(std::move(frame));
  ++index_;
  return Progress::did();
}

}  // namespace mf
