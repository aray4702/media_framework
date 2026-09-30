#pragma once

// A live camera with a scene drawn over it (the camera window). Each camera frame, as it
// arrives, becomes the picture of the scene's camera item, and the frame is composed with every
// other item and effect (as playback composes, §5.1) and handed to the display at once. There is
// no clock, seeking or decoding: frames come from the camera, and the display shows the latest.
// The scene's items are drawn as at time 0 (the camera window's items don't move by themselves).
//
// The owner feeds it frames (from an ICamera callback, on the camera's thread) and changes the
// scene (on its own thread): both are thread-safe. A scene change redraws the last frame.

#include <cstdint>
#include <memory>
#include <string>

#include "mf/adapters.h"
#include "mf/scene.h"

namespace mf {

class LivePreview {
 public:
  explicit LivePreview(PlatformFactory& platform);
  ~LivePreview();
  LivePreview(const LivePreview&) = delete;
  LivePreview& operator=(const LivePreview&) = delete;

  // Makes the display and attaches it to the target. Once, before frames.
  Result attach(const RenderTarget&);
  // The scene (validated) and the id of its video item that shows the camera. Loads the images
  // it hasn't loaded yet (by src). InvalidArgument, with `error`, for an invalid scene or no such
  // item.
  Result setScene(const Scene&, const std::string& cameraItemId, std::string* error = nullptr);
  // A camera frame: composed with the scene and presented. Any thread.
  void present(const VideoFrame& camera);
  int64_t presented() const;  // frames handed to the display

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mf
