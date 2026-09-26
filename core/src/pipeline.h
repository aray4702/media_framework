#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include "bounded_queue.h"
#include "master_clock.h"
#include "metrics.h"
#include "mf/adapters.h"
#include "mf/audio_ring.h"

namespace mf {

// What the stages report to the Player. Called on internal threads.
class PipelineEvents {
 public:
  virtual ~PipelineEvents() = default;
  virtual void onFatal(Result, const std::string& reason) = 0;
  virtual void onWarning(Warning, const std::string& reason) = 0;
  // hadFrame is false when the seek reached end of stream without a frame to show.
  virtual void onSeekDone(uint32_t serial, int64_t shownPtsUs, bool hadFrame) = 0;
  virtual void onEnd() = 0;
};

struct PendingSeek {
  int64_t targetUs;
  int64_t requestedNs;
};

// State shared by the Player and the four stages.
struct Context {
  Context(PlatformFactory& factory, PipelineEvents& events);

  void wake(StageId id) { scheduler->wake(id); }
  void wakeAll();
  void fatal(Result r, const std::string& reason) { events.onFatal(r, reason); }

  // Seeks (single slot, latest wins).
  void requestSeek(int64_t targetUs);
  std::optional<PendingSeek> takeSeek();
  bool hasPendingSeek() const { return hasPending_.load(); }
  void setSeekTarget(const PendingSeek& s);
  PendingSeek seekTarget() const;

  IClock& hostClock;
  PipelineEvents& events;
  std::unique_ptr<IDemuxer> demuxer;
  std::unique_ptr<IVideoDecoder> videoDecoder;
  std::unique_ptr<IAudioDecoder> audioDecoder;
  std::unique_ptr<ISpeaker> speaker;
  std::unique_ptr<IDisplay> display;
  std::unique_ptr<IScheduler> scheduler;

  // Written by T1 while probing, before any packet exists; read-only afterwards.
  MediaSource source;
  MediaInfo info;
  std::atomic<bool> openRequested{false};
  std::atomic<bool> hasAudio{false};
  std::atomic<int64_t> durationUs{0};

  // Buffers, capped per §2.1.
  BoundedQueue<Packet> videoPackets{{60, 32u << 20, 2000000}};
  BoundedQueue<Packet> audioPackets{{120, 1u << 20, 2000000}};
  BoundedQueue<VideoFrame> frames{{4}};
  AudioRing ring;

  MasterClock master{ring};
  Metrics metrics;

  std::atomic<uint32_t> serial{0};       // latest seek started by T1
  std::atomic<uint32_t> shownSerial{0};  // latest seek completed by T3
  std::atomic<int64_t> shownPtsUs{0};
  std::atomic<bool> halted{false};       // fatal error or shutdown: stages go idle

  // Playback. `playing` is written under playMu; `outputRunning` is only touched under playMu.
  std::mutex playMu;
  std::atomic<bool> playing{false};
  bool outputRunning = false;  // speaker started and master clock running

 private:
  mutable std::mutex seekMu_;
  std::optional<PendingSeek> pending_;
  PendingSeek target_{0, 0};
  std::atomic<bool> hasPending_{false};
};

std::array<std::unique_ptr<Stage>, kStageCount> makeStages(Context&);

}  // namespace mf
