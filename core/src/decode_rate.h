#pragma once

#include <cstdint>
#include <vector>

namespace mf {

// Decode rate control for live playback (rate_mismatch_buffering.md §9): when decoding falls
// behind for a sustained time, the lanes move down a ladder of cheaper decodes, and back up once
// they keep up again. Modeled on SegmentRecorder::adaptRateLocked, with the signals inverted: a
// lead of decoded frames means the decoder is ahead, and lateness at the compositor means behind.
//
// Steps: 0 decodes every frame, 1 skips disposable frames, 2 decodes keyframes only. Lanes share
// one hardware decoder, so steps are spread evenly: a step down lowers the lanes at the lowest
// step, a step up raises the lanes at the highest, and lanes are never more than one step apart.
// All times are timeline times, so a burst or a single stall doesn't look like a slow decoder.
class DecodeRate {
 public:
  static constexpr int kMaxStep = 2;
  static constexpr int64_t kBehindForUs = 500000;         // behind this long: a step down
  static constexpr int64_t kStepGapUs = 1000000;          // between steps down, for the last to take effect
  static constexpr int64_t kStepUpAfterUs = 3000000;      // ahead this long: a step up
  static constexpr int64_t kMaxStepUpAfterUs = 30000000;  // a step up that doesn't hold doubles that, up to this
  static constexpr int64_t kMomentumUs = 5000000;         // time constant of the recent level

  struct Lane {
    bool active = false;  // decoding a video item (from its preroll to its end of stream), or showing one
    bool behind = false;  // a visible item of the lane isn't exact at t
    bool ahead = false;   // decoded well past t (or to its item's end): the decoder isn't what limits it
    int step = 0;         // the lane's step; tick() updates it
  };
  enum class Change { None, Down, Up };
  struct Tick {
    Change change = Change::None;
    int64_t reducedUs = 0;  // timeline time since the last tick with some lane above step 0
  };

  // One composition tick at timeline time t. `holdExpired`: the compositor stopped holding the
  // picture for a late layer, an underflow that steps down at once instead of after kBehindForUs.
  Tick tick(int64_t t, std::vector<Lane>& lanes, bool holdExpired);
  // The step a lane's next item starts at: the recent level, within one step of the active lanes.
  int startStep(const std::vector<Lane>& lanes) const;
  // A seek: the timers start over. The momentum and the step-up backoff are kept.
  void restart();

  double momentum() const { return momentum_; }
  int64_t stepUpAfterUs() const { return stepUpAfterUs_; }

 private:
  int64_t lastTickUs_ = -1;
  int64_t behindSinceUs_ = -1, aheadSinceUs_ = -1;
  int64_t lastStepUs_ = -1, lastStepUpUs_ = -1;
  int64_t stepUpAfterUs_ = kStepUpAfterUs;
  double momentum_ = 0;  // exponential moving average of the active lanes' mean step
};

}  // namespace mf
