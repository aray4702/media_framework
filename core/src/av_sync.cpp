#include "av_sync.h"

#include <cmath>

namespace mf {

SyncDecision AvSync::decide(int64_t ptsUs, int64_t clockUs, int64_t nowNs, int64_t vsyncNs, int64_t frameDurUs,
                            int64_t gridNs) {
  using K = SyncDecision::Kind;
  if (vsyncNs != vsyncNs_) {  // display changed: re-anchor the slots
    vsyncNs_ = vsyncNs;
    anchored_ = false;
  }
  int64_t deltaNs = (ptsUs - clockUs) * 1000;
  if (deltaNs < -frameDurUs * 1000) return {K::LateDrop};

  int64_t target = nowNs + deltaNs;
  int64_t slot = anchored_ ? std::llround(double(target - anchorNs_) / double(vsyncNs)) : 0;
  if (anchored_ && slot <= lastSlot_) return {K::RateCapDrop};
  if (deltaNs > vsyncNs) return {K::Wait, target - vsyncNs};

  // Commit the slot only when presenting, so a frame that waited isn't capped against itself.
  if (!anchored_) {
    anchored_ = true;
    anchorNs_ = target;
    // Present times sit on the real vsync grid, aligned once here: at most half a vsync of A/V
    // offset, and no rounding at each frame for clock jitter to flip.
    presentAnchorNs_ = gridNs ? gridNs + std::llround(double(target - gridNs) / double(vsyncNs)) * vsyncNs : target;
    slot = 0;
  }
  lastSlot_ = slot;
  return {K::Present, presentAnchorNs_ + slot * vsyncNs, slot};
}

}  // namespace mf
