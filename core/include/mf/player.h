#pragma once

#include <memory>
#include <string>

#include "mf/adapters.h"
#include "mf/scene.h"

namespace mf {

struct MetricsReport {
  int64_t presented = 0, lateDrops = 0, rateCapDrops = 0, hiddenDrops = 0;
  int64_t lateLayers = 0;  // Vsync driver: a layer showed an older frame because its decode was behind
  int64_t decodeOnly = 0, corruptSkips = 0;
  int64_t intervals = 0, janks = 0;
  double droppedRate = 0, jankRate = 0;
  int64_t avSamples = 0;
  double avMeanAbsMs = 0, avP95AbsMs = 0;
  double avMeanMs = 0;  // signed: > 0 means video is shown early relative to audio
  double ttffMs = -1;
  int64_t seeks = 0;
  double seekP50Ms = 0, seekP95Ms = 0;
  std::string toString() const;
};

bool isValid(const VideoFilter&);

// Callbacks run on internal threads. They must return quickly and must never wait on the
// owner thread (e.g. dispatch_sync to it), or shutdown() deadlocks. Post work instead.
class PlayerListener {
 public:
  virtual ~PlayerListener() = default;
  virtual void onStateChanged(State) {}
  virtual void onError(Result, const std::string& /*reason*/) {}
  virtual void onWarning(Warning, const std::string& /*reason*/) {}
  virtual void onFirstFrame() {}
  virtual void onSeekCompleted(int64_t /*shownPtsUs*/) {}
  virtual void onEnded() {}
};

// Every method must be called on the thread that called create().
class Player {
 public:
  static std::unique_ptr<Player> create(PlatformFactory&, PlayerListener*);
  ~Player();

  // Returns at once; probing runs on T1. The scene is validated here (InvalidArgument, with
  // `error` saying why); media problems arrive later through onError. The first frame shown (on
  // Ready) is the one at `startUs`, clamped to the timeline; no frame at 0 is shown before it.
  Result open(const Scene&, const RenderTarget&, OutputDriver = OutputDriver::Auto, std::string* error = nullptr,
              int64_t startUs = 0);
  Result open(const MediaSource&, const RenderTarget&);  // a scene of one video, to the end of the file
  Result play();
  Result pause();
  Result seek(int64_t positionUs);  // completes via onSeekCompleted
  Result shutdown();                // joins the threads; idempotent
  Result setFilter(const VideoFilter&);  // any time before shutdown; a paused frame is redrawn

  State state() const;
  int64_t durationUs() const;  // of the whole timeline
  int64_t positionUs() const;
  MetricsReport metrics() const;

  struct Impl;

 private:
  explicit Player(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};

}  // namespace mf
