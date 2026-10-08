// Exports a scene document with the macOS platform (Metal compositor, AVFoundation encoder): the
// reference the web export is compared with (platform/web/tools/compare_exports.py).
// Usage: export_reference scene.json out.mp4

#include <chrono>
#include <cstdio>
#include <thread>

#include "mf/exporter.h"
#include "mf/macos.h"

using namespace mf;

struct Listener : ExportListener {
  void onError(Result r, const std::string& reason) override { std::fprintf(stderr, "export failed: %s: %s\n", toString(r), reason.c_str()); }
};

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s scene.json out.mp4\n", argv[0]);
    return 2;
  }
  Scene scene;
  std::string error;
  if (macos::loadScene(argv[1], &scene, &error) != Result::Ok) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  auto platform = macos::createPlatform();
  Listener listener;
  auto exporter = Exporter::create(*platform, &listener);
  ExportSettings settings;
  settings.videoBitrate = 8000000;
  auto started = std::chrono::steady_clock::now();
  if (exporter->start(scene, macos::exportTargetFromPath(argv[2]), settings, &error) != Result::Ok) {
    std::fprintf(stderr, "start: %s\n", error.c_str());
    return 1;
  }
  while (!exporter->done()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  bool ok = exporter->progress() >= 1;
  exporter->shutdown();
  std::fprintf(stderr, "%s: %s in %.0f ms\n", argv[2], ok ? "exported" : "failed", ms);
  return ok ? 0 : 1;
}
