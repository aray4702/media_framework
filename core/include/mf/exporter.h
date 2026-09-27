#pragma once

#include <memory>
#include <string>

#include "mf/player.h"

namespace mf {

// Callbacks run on internal threads, with the same rules as PlayerListener.
class ExportListener {
 public:
  virtual ~ExportListener() = default;
  virtual void onWarning(Warning, const std::string& /*reason*/) {}
  virtual void onCompleted() {}                                   // the file is written
  virtual void onError(Result, const std::string& /*reason*/) {}  // fatal; nothing more is written
};

// Renders a timeline into a file as fast as the decoders and encoder allow (§2.5): frames on a
// fixed n / fps grid, each composed once every layer has its exact frame, plus the mixed audio.
// Every method must be called on the thread that called create().
class Exporter {
 public:
  static std::unique_ptr<Exporter> create(PlatformFactory&, ExportListener*);
  ~Exporter();

  // Returns at once; probing, decoding and encoding run on the pipeline threads.
  Result start(const Timeline&, const ExportTarget&, const ExportSettings&);
  Result shutdown();  // cancels an export in progress and joins the threads; idempotent

  double progress() const;  // 0 to 1, by video frames written
  bool done() const;        // completed or failed

  struct Impl;

 private:
  explicit Exporter(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};

}  // namespace mf
