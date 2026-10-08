#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "mf/adapters.h"

namespace mf {

// Runs every stage on one thread, for hosts with an event loop and no blocking threads (a
// browser: WebCodecs delivers its output only when the event loop runs, which a thread blocked
// on a condition variable never lets happen). Stages never block, so they share the thread: run()
// pumps them until each is idle or waiting for a time, or a slice of time is used up, and then
// asks the host to call it again, as soon as possible or at the earliest deadline.
//
// Single-threaded: start(), wake(), stop() and run() are all called on the host's thread.
class CooperativeScheduler : public IScheduler {
 public:
  // requestRun(delayNs): the host calls run() once that much time has passed (0: as soon as it
  // can, after other events). Extra calls to run() are harmless.
  CooperativeScheduler(IClock& clock, std::function<void(int64_t delayNs)> requestRun, int64_t sliceNs = 4000000);

  void start(std::array<Stage*, kStageCount> stages) override;
  void wake(StageId) override;
  void stop() override;

  void run();

 private:
  void request(int64_t atNs);

  IClock& clock_;
  std::function<void(int64_t)> requestRun_;
  int64_t sliceNs_;
  std::array<Stage*, kStageCount> stages_{};
  std::array<bool, kStageCount> ready_{};     // pump it on the next pass
  std::array<int64_t, kStageCount> waitNs_{};  // pump it once this time has come
  bool running_ = false, inRun_ = false;
  int64_t requestedNs_;  // the earliest run already asked for
};

}  // namespace mf
