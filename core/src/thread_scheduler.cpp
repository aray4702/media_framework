#include "mf/thread_scheduler.h"

#include <chrono>

namespace mf {

ThreadScheduler::ThreadScheduler(IClock& clock, std::function<void(StageId)> onThreadStart)
    : clock_(clock), onThreadStart_(std::move(onThreadStart)) {}

ThreadScheduler::~ThreadScheduler() { stop(); }

void ThreadScheduler::start(std::array<Stage*, kStageCount> stages) {
  running_ = true;
  for (int i = 0; i < kStageCount; ++i) {
    workers_[i].thread = std::thread([this, i, stage = stages[i]] { run(i, stage); });
  }
}

void ThreadScheduler::wake(StageId id) {
  Worker& w = workers_[static_cast<int>(id)];
  {
    std::lock_guard<std::mutex> lock(w.mu);
    w.woken = true;
  }
  w.cv.notify_one();
}

void ThreadScheduler::stop() {
  running_ = false;
  for (int i = 0; i < kStageCount; ++i) wake(static_cast<StageId>(i));
  for (Worker& w : workers_) {
    if (w.thread.joinable()) w.thread.join();
  }
}

void ThreadScheduler::run(int index, Stage* stage) {
  if (onThreadStart_) onThreadStart_(static_cast<StageId>(index));
  Worker& w = workers_[index];
  while (running_) {
    Progress p = stage->pump();
    if (p.kind == Progress::Kind::Did) continue;
    std::unique_lock<std::mutex> lock(w.mu);
    auto ready = [&] { return w.woken || !running_; };
    if (p.kind == Progress::Kind::Idle) {
      w.cv.wait(lock, ready);
    } else {
      int64_t waitNs = p.deadlineNs - clock_.nowNs();
      if (waitNs > 0) w.cv.wait_for(lock, std::chrono::nanoseconds(waitNs), ready);
    }
    w.woken = false;
  }
}

}  // namespace mf
