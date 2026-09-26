#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "mf/player.h"

namespace mf {

// Fixed-size millisecond histogram, so metrics memory stays bounded.
class Histogram {
 public:
  void add(double ms);
  double percentile(double p) const;
  double mean() const { return count_ ? sum_ / double(count_) : 0; }
  int64_t count() const { return count_; }

 private:
  std::vector<uint32_t> bins_ = std::vector<uint32_t>(2001, 0);  // 1 ms bins; last is overflow
  int64_t count_ = 0;
  double sum_ = 0;
};

class Metrics {
 public:
  void startTtff(int64_t nowNs);
  void countLate();
  void countRateCap();
  void countHidden();
  void countDecodeOnly();
  void countCorrupt();
  void seekLatency(int64_t ns);

  // A playback frame was handed to the display with this vsync slot.
  void planned(int64_t ptsUs, int64_t slot, int64_t vsyncNs);
  // The display reported the frame (presentedNs = 0: never shown).
  void presented(int64_t ptsUs, int64_t presentedNs, std::optional<int64_t> avOffsetUs);
  // Next presented interval starts fresh (after play or seek).
  void discontinuity();

  MetricsReport report() const;

 private:
  struct Plan {
    int64_t ptsUs = -1, slot = 0, vsyncNs = 0;
  };

  mutable std::mutex mu_;
  MetricsReport r_;
  std::array<Plan, 16> plans_{};
  size_t nextPlan_ = 0;
  bool havePrev_ = false;
  int64_t prevNs_ = 0, prevSlot_ = 0;
  int64_t ttffStartNs_ = -1;
  Histogram av_, seek_;
  double avSignedSumMs_ = 0;
};

}  // namespace mf
