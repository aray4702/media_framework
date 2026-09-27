#include "drivers.h"

namespace mf {

Progress ExportDriver::step(FrameSampler& s, CompositionOutput& out) {
  int64_t t = index_ * 1000000 / fps_;
  if (t >= s.layout().durationUs()) {
    out.finish();
    return Progress::did();
  }
  s.advanceAll(t);
  if (!s.allExactAt(t)) return Progress::idle();  // the lanes' frame queues wake us
  ComposedFrame frame = s.composeAt(t);  // no layer yet (a clip's first frame is later): background
  frame.frameDurationUs = 1000000 / fps_;
  out.emit(std::move(frame));
  ++index_;
  return Progress::did();
}

}  // namespace mf
