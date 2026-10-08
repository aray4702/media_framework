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
  if (taken && s.timeOf(*taken) == t && item == s.leadItem(t)) {
    adaptDecode(s, item, t);
    out.emit(s.composeAt(t));
  }
  return Progress::did();
}

// One tick of the decode rate control per output frame, at the master clock's time: the leading
// item's lane is behind when the frame's time has already passed. Frames composed ahead of the
// clock (while paused, or with a lead in the queues) are on time.
void LeadingClipDriver::adaptDecode(FrameSampler& s, int item, int64_t t) {
  int64_t playheadUs = ctx_.master.nowUs(ctx_.hostClock.nowNs());
  std::vector<int> visible(size_t(s.lanes()), -1);
  std::vector<bool> behind(size_t(s.lanes()), false);
  for (int i : s.videoAt(t)) visible[size_t(ctx_.layout.laneOf(i))] = i;
  behind[size_t(ctx_.layout.laneOf(item))] = t < playheadUs;
  control_.tick(ctx_, playheadUs, visible, behind, false);
}

}  // namespace mf
