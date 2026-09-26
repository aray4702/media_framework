#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>

namespace mf {

// Non-blocking queue capped by count, bytes and duration, whichever is reached first (A23).
// An empty queue always accepts one item, so a single oversized item can't wedge it.
// T needs `bytes()` and `ptsUs`.
template <typename T>
class BoundedQueue {
 public:
  struct Caps {
    size_t count;
    size_t bytes = std::numeric_limits<size_t>::max();
    int64_t durationUs = std::numeric_limits<int64_t>::max();
  };

  explicit BoundedQueue(Caps caps) : caps_(caps) {}

  // onItem wakes the consumer, onSpace wakes the producer. Called without the lock held.
  void setHooks(std::function<void()> onItem, std::function<void()> onSpace) {
    onItem_ = std::move(onItem);
    onSpace_ = std::move(onSpace);
  }

  bool full() const {
    std::lock_guard<std::mutex> lock(mu_);
    return fullLocked();
  }

  // Moves from `item` only on success.
  bool tryPush(T& item) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (fullLocked()) return false;
      bytes_ += item.bytes();
      items_.push_back(std::move(item));
    }
    if (onItem_) onItem_();
    return true;
  }

  bool tryPop(T* out) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (items_.empty()) return false;
      *out = std::move(items_.front());
      items_.pop_front();
      bytes_ -= out->bytes();
    }
    if (onSpace_) onSpace_();
    return true;
  }

  void flush() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      items_.clear();
      bytes_ = 0;
    }
    if (onSpace_) onSpace_();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return items_.size();
  }

 private:
  bool fullLocked() const {
    if (items_.empty()) return false;
    return items_.size() >= caps_.count || bytes_ >= caps_.bytes ||
           items_.back().ptsUs - items_.front().ptsUs >= caps_.durationUs;
  }

  const Caps caps_;
  mutable std::mutex mu_;
  std::deque<T> items_;
  size_t bytes_ = 0;
  std::function<void()> onItem_, onSpace_;
};

}  // namespace mf
