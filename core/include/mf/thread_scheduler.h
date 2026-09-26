#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "mf/adapters.h"

namespace mf {

// Native scheduler (§2.3): one thread per stage, each waiting on its own CV between pumps.
class ThreadScheduler : public IScheduler {
 public:
  explicit ThreadScheduler(IClock& clock, std::function<void(StageId)> onThreadStart = nullptr);
  ~ThreadScheduler() override;

  void start(std::array<Stage*, kStageCount> stages) override;
  void wake(StageId) override;
  void stop() override;

 private:
  struct Worker {
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    bool woken = false;
  };
  void run(int index, Stage* stage);

  IClock& clock_;
  std::function<void(StageId)> onThreadStart_;
  std::array<Worker, kStageCount> workers_;
  std::atomic<bool> running_{false};
};

}  // namespace mf
