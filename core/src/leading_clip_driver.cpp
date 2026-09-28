#include "drivers.h"

namespace mf {

// Frames go in timeline order, once no other lane can still produce an earlier one.
// A frame of the leading item produces an output; any other frame only updates its item.
Progress LeadingClipDriver::step(FrameSampler& s, CompositionOutput& out) {
  bool popped = false;
  for (int li = 0; li < s.lanes(); ++li) popped |= s.pop(li);
  int next = -1;
  for (int li = 0; li < s.lanes(); ++li) {
    if (s.head(li) && (next < 0 || s.timeOf(*s.head(li)) < s.timeOf(*s.head(next)))) next = li;
  }
  if (next < 0) {
    if (s.done()) {
      out.finish();
      return Progress::did();
    }
    return popped ? Progress::did() : Progress::idle();
  }
  int64_t t = s.timeOf(*s.head(next));
  for (int li = 0; li < s.lanes(); ++li) {
    if (li != next && !s.head(li) && s.floorOf(li) <= t) return popped ? Progress::did() : Progress::idle();
  }
  int item = s.head(next)->item;
  s.take(next);
  const VideoFrame* taken = s.frameOf(item);
  if (taken && s.timeOf(*taken) == t && item == s.leadItem(t)) out.emit(s.composeAt(t));
  return Progress::did();
}

}  // namespace mf
