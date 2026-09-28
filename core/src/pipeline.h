#pragma once

#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "bounded_queue.h"
#include "layout.h"
#include "master_clock.h"
#include "metrics.h"
#include "mf/adapters.h"
#include "mf/audio_ring.h"
#include "mf/player.h"
#include "mf/scene.h"

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

constexpr int kNoItem = std::numeric_limits<int>::max();          // a lane with no item left
constexpr int64_t kNever = std::numeric_limits<int64_t>::max();  // a time that never comes

struct PendingSeek {
  int64_t targetUs = 0;
  int64_t requestedNs = 0;
  // Set by T1 when the seek starts: per lane, the position in layout.laneItems() it reads first.
  std::vector<int> lanePos;
};

// Decoders and queues shared by the items of one lane (SceneLayout), which never overlap.
struct Lane {
  std::unique_ptr<IVideoDecoder> videoDecoder;
  std::unique_ptr<IAudioDecoder> audioDecoder;
  BoundedQueue<Packet> videoPackets{{60, 32u << 20, 2000000}};
  BoundedQueue<Packet> audioPackets{{120, 1u << 20, 2000000}};
  BoundedQueue<VideoFrame> frames{{4}};
};

// What an item needs while playing, found by T1 while probing.
struct ItemRuntime {
  std::unique_ptr<IDemuxer> demuxer;  // video and audio items
  MediaInfo info;
  bool mixAudio = false;  // its audio is decoded and mixed (usable, not muted)
  VideoFrame image;       // image items: decoded once
  int imageWidth = 0, imageHeight = 0;
  std::shared_ptr<const std::string> text;  // text items
};

// What sets the output times inside the pipeline (§2.5).
enum class Driver { LeadingClip, Vsync, Export };

// State shared by the Player (or Exporter) and the five stages.
struct Context {
  // An export context makes an export sink instead of a display and a speaker.
  Context(PlatformFactory& factory, PipelineEvents& events, bool forExport = false);

  void wake(StageId id) { scheduler->wake(id); }
  void wakeAll();
  void fatal(Result r, const std::string& reason) { events.onFatal(r, reason); }
  std::string itemName(int item) const;  // for messages: its id, or its position

  // Set on the owner thread; T3 applies the latest value to every frame it presents.
  VideoFilter filter() const;
  void setFilter(const VideoFilter&);

  // Seeks (single slot, latest wins).
  void requestSeek(int64_t targetUs);
  std::optional<PendingSeek> takeSeek();
  bool hasPendingSeek() const { return hasPending_.load(); }
  void setSeekTarget(const PendingSeek& s);
  PendingSeek seekTarget() const;

  PlatformFactory& factory;
  IClock& hostClock;
  PipelineEvents& events;
  std::unique_ptr<ISpeaker> speaker;        // playback only
  std::unique_ptr<IDisplay> display;        // playback only
  std::unique_ptr<IExportSink> exportSink;  // export only
  std::unique_ptr<IScheduler> scheduler;

  // Set by open() (or Exporter::start) before T1 is woken.
  Driver driver = Driver::LeadingClip;
  bool autoDriver = false;  // pick LeadingClip for a single video item, else Vsync
  ExportTarget exportTarget;
  ExportSettings exportSettings;
  Scene scene;
  std::optional<Timeline> timeline;  // open(Timeline): turned into `scene` once its clips are probed
  std::atomic<bool> openRequested{false};

  // Written by T1 while probing, before any packet exists; read-only once `probed` is set.
  SceneLayout layout;
  std::vector<ItemRuntime> items;
  std::vector<std::unique_ptr<Lane>> lanes;
  int width = 0, height = 0;  // the canvas
  int fpsNum = 30, fpsDen = 1;
  std::atomic<bool> probed{false};
  std::atomic<bool> hasAudio{false};  // some item has mixed audio: the ring and speaker run
  std::atomic<int64_t> durationUs{0};

  // Buffers, capped per §2.1. The lanes hold the rest.
  BoundedQueue<ComposedFrame> composed{{4}};
  AudioRing ring;

  MasterClock master{ring};
  Metrics metrics;

  std::atomic<uint32_t> serial{0};       // latest seek started by T1
  std::atomic<uint32_t> shownSerial{0};  // latest seek completed by T3
  std::atomic<int64_t> shownPtsUs{0};
  std::atomic<bool> halted{false};       // fatal error or shutdown: stages go idle
  std::atomic<uint32_t> filterVersion{0};  // bumped by setFilter, so T3 can redraw a paused frame

  // Export progress.
  std::atomic<bool> audioWritten{false};  // T4 has written the last audio
  std::atomic<int64_t> writtenUs{0};      // timeline time of the last video frame written

  // Playback. `playing` is written under playMu; `outputRunning` is only touched under playMu.
  std::mutex playMu;
  std::atomic<bool> playing{false};
  bool outputRunning = false;  // speaker started and master clock running

 private:
  mutable std::mutex filterMu_;
  VideoFilter filter_;
  mutable std::mutex seekMu_;
  std::optional<PendingSeek> pending_;
  PendingSeek target_;
  std::atomic<bool> hasPending_{false};
};

std::array<std::unique_ptr<Stage>, kStageCount> makeStages(Context&);

}  // namespace mf
