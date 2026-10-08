#include "mf/cooperative_scheduler.h"

#include <algorithm>
#include <limits>

namespace mf {
namespace {
constexpr int64_t kNoTime = std::numeric_limits<int64_t>::max();
}  // namespace

CooperativeScheduler::CooperativeScheduler(IClock& clock, std::function<void(int64_t)> requestRun, int64_t sliceNs)
    : clock_(clock), requestRun_(std::move(requestRun)), sliceNs_(sliceNs), requestedNs_(kNoTime) {
  waitNs_.fill(kNoTime);
}

void CooperativeScheduler::start(std::array<Stage*, kStageCount> stages) {
  stages_ = stages;
  running_ = true;
  ready_.fill(true);
  request(clock_.nowNs());
}

void CooperativeScheduler::wake(StageId id) {
  ready_[static_cast<int>(id)] = true;
  if (running_ && !inRun_) request(clock_.nowNs());  // during run(), the next pass picks it up
}

void CooperativeScheduler::stop() { running_ = false; }

void CooperativeScheduler::run() {
  if (!running_ || inRun_) return;
  inRun_ = true;
  requestedNs_ = kNoTime;  // this is the run that was asked for
  int64_t start = clock_.nowNs();
  for (;;) {
    int64_t now = clock_.nowNs();
    bool any = false;
    for (int i = 0; i < kStageCount && running_; ++i) {
      if (!ready_[i] && waitNs_[i] > now) continue;
      ready_[i] = false;
      waitNs_[i] = kNoTime;
      Progress p = stages_[i]->pump();
      if (p.kind == Progress::Kind::Did) {
        ready_[i] = any = true;
      } else if (p.kind == Progress::Kind::WaitUntil) {
        waitNs_[i] = p.deadlineNs;
      }
    }
    if (!running_) break;
    bool woken = std::find(ready_.begin(), ready_.end(), true) != ready_.end();
    if ((!any && !woken) || clock_.nowNs() - start >= sliceNs_) break;
  }
  inRun_ = false;
  if (!running_) return;
  // Yield to the host's other events, then carry on: at once if a stage has more to do, else at
  // the earliest deadline. With neither, a wake() asks again.
  if (std::find(ready_.begin(), ready_.end(), true) != ready_.end()) {
    request(clock_.nowNs());
  } else if (int64_t next = *std::min_element(waitNs_.begin(), waitNs_.end()); next != kNoTime) {
    request(next);
  }
}

void CooperativeScheduler::request(int64_t atNs) {
  if (requestedNs_ <= atNs) return;  // an earlier run is already coming
  requestedNs_ = atNs;
  requestRun_(std::max<int64_t>(0, atNs - clock_.nowNs()));
}

}  // namespace mf
