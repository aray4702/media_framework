#include "drivers.h"

namespace mf {

// Frames go in timeline order, once the other lane can't still produce an earlier one.
// A frame of the leading clip produces an output; any other frame only updates its lane.
Progress LeadingClipDriver::step(FrameSampler& s, CompositionOutput& out) {
  bool popped = false;
  for (int li = 0; li < kLanes; ++li) popped |= s.pop(li);
  int next = -1;
  for (int li = 0; li < kLanes; ++li) {
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
  int other = 1 - next;
  if (!s.head(other) && s.floorOf(other) <= t) return popped ? Progress::did() : Progress::idle();
  int clip = s.head(next)->clip;
  s.take(next);
  const VideoFrame* taken = s.frameOf(clip);
  if (taken && s.timeOf(*taken) == t && clip == s.layout().leadClip(t)) out.emit(s.composeAt(t));
  return Progress::did();
}

}  // namespace mf
