#pragma once

#include <cstdint>

namespace mf {

struct SyncDecision {
  enum class Kind { Present, LateDrop, RateCapDrop, Wait };
  Kind kind;
  int64_t atNs = 0;  // Present: the slot's host time. Wait: wake-up time.
  int64_t slot = 0;  // Present: vsync slot, for jank accounting
};

// Per-frame present/drop decision (§4, steps 1-5).
class AvSync {
 public:
  void reset() { anchored_ = false; }
  // gridNs: host time of a real vsync, or 0 if unknown.
  SyncDecision decide(int64_t ptsUs, int64_t clockUs, int64_t nowNs, int64_t vsyncNs, int64_t frameDurUs,
                      int64_t gridNs = 0);

 private:
  bool anchored_ = false;
  int64_t anchorNs_ = 0, presentAnchorNs_ = 0, lastSlot_ = 0, vsyncNs_ = 0;
};

}  // namespace mf
