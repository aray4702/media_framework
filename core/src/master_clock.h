#pragma once

#include <cstdint>
#include <mutex>

#include "mf/audio_ring.h"

namespace mf {

// Audio clock while an audio track is playing, steady clock otherwise (§4).
// When the audio track ends, it continues on the steady clock from the last audio value.
class MasterClock {
 public:
  explicit MasterClock(const AudioRing& ring) : ring_(ring) {}

  void setAudio(bool on);
  void reset(int64_t baseUs, uint32_t serial);  // after a seek
  void start(int64_t nowNs);
  void stop(int64_t nowNs);
  int64_t nowUs(int64_t nowNs);
  bool running() const;
  bool usingAudio() const;

 private:
  int64_t valueLocked(int64_t nowNs);

  const AudioRing& ring_;
  mutable std::mutex mu_;
  bool hasAudio_ = false, useAudio_ = false, running_ = false;
  int64_t baseUs_ = 0, baseNs_ = 0;
  uint32_t serial_ = 0;
};

}  // namespace mf
