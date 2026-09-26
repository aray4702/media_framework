#include "master_clock.h"

namespace mf {

void MasterClock::setAudio(bool on) {
  std::lock_guard<std::mutex> lock(mu_);
  hasAudio_ = useAudio_ = on;
}

void MasterClock::reset(int64_t baseUs, uint32_t serial) {
  std::lock_guard<std::mutex> lock(mu_);
  baseUs_ = baseUs;
  serial_ = serial;
  useAudio_ = hasAudio_;
}

void MasterClock::start(int64_t nowNs) {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_) return;
  running_ = true;
  baseNs_ = nowNs;
}

void MasterClock::stop(int64_t nowNs) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_) return;
  baseUs_ = valueLocked(nowNs);
  running_ = false;
}

int64_t MasterClock::nowUs(int64_t nowNs) {
  std::lock_guard<std::mutex> lock(mu_);
  return running_ ? valueLocked(nowNs) : baseUs_;
}

bool MasterClock::running() const {
  std::lock_guard<std::mutex> lock(mu_);
  return running_;
}

bool MasterClock::usingAudio() const {
  std::lock_guard<std::mutex> lock(mu_);
  return useAudio_;
}

int64_t MasterClock::valueLocked(int64_t nowNs) {
  if (useAudio_) {
    int64_t v;
    uint32_t tag;
    // Hold until the audio for this seek is actually heard.
    if (!ring_.clockUs(nowNs, &v, &tag) || tag != serial_) return baseUs_;
    if (ring_.drained(nowNs)) {  // audio ended: hand off to the steady clock
      useAudio_ = false;
      baseUs_ = v;
      baseNs_ = nowNs;
    }
    return v;
  }
  return baseUs_ + (nowNs - baseNs_) / 1000;
}

}  // namespace mf
