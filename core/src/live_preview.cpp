#include "mf/live_preview.h"

#include <map>
#include <mutex>
#include <optional>

#include "composition.h"
#include "layout.h"

namespace mf {

struct LivePreview::Impl {
  explicit Impl(PlatformFactory& p) : platform(p), images(p.createImageLoader()) {}

  // Composes the last camera frame with the scene and hands it to the display. Holds mu.
  void draw() {
    if (!display || !last || camera < 0) return;
    ComposedFrame out;
    out.ptsUs = last->ptsUs;
    out.width = scene.output.width;
    out.height = scene.output.height;
    out.background = scene.output.background;
    out.frameDurationUs = scene.output.fpsNum > 0 ? int64_t(scene.output.fpsDen) * 1000000 / scene.output.fpsNum : 0;
    composeLayers(scene, layout, items, 0, [&](int item) { return item == camera ? &*last : nullptr; }, &out);
    display->present(out, platform.clock().nowNs());
    ++presented;
  }

  PlatformFactory& platform;
  std::unique_ptr<IImageLoader> images;
  std::unique_ptr<IDisplay> display;
  std::mutex mu;  // guards everything below: the owner changes the scene while the camera's thread draws
  Scene scene;
  SceneLayout layout;
  std::vector<ItemRuntime> items;
  int camera = -1;  // the camera item's index in layout
  std::map<std::string, VideoFrame> loaded;  // images, by src
  std::optional<VideoFrame> last;            // the latest camera frame
  int64_t presented = 0;
};

LivePreview::LivePreview(PlatformFactory& platform) : impl_(std::make_unique<Impl>(platform)) {}

LivePreview::~LivePreview() = default;

Result LivePreview::attach(const RenderTarget& target) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  if (impl_->display) return Result::InvalidState;
  std::unique_ptr<IDisplay> display = impl_->platform.createDisplay();
  if (!display) return Result::Unsupported;
  Result r = display->attach(target, [](int64_t, int64_t) {});
  if (r != Result::Ok) return r;
  impl_->display = std::move(display);
  return Result::Ok;
}

Result LivePreview::setScene(const Scene& scene, const std::string& cameraItemId, std::string* error) {
  if (validateScene(scene, error) != Result::Ok) return Result::InvalidArgument;
  Impl& m = *impl_;
  std::lock_guard<std::mutex> lock(m.mu);
  m.scene = scene;
  if (!m.layout.build(m.scene, error)) return Result::InvalidArgument;
  m.camera = -1;
  m.items.clear();
  m.items.resize(size_t(m.layout.items()));
  for (int i = 0; i < m.layout.items(); ++i) {
    const SceneItem& it = m.layout.item(i);
    if (it.id == cameraItemId && it.type == ItemType::Video) m.camera = i;
    if (it.type == ItemType::Text) m.items[i].text = std::make_shared<const std::string>(it.text);
    if (it.type == ItemType::Image && m.images) {
      auto found = m.loaded.find(it.src);
      if (found == m.loaded.end()) {  // a failed load stays empty: the item isn't drawn
        VideoFrame image;
        int w = 0, h = 0;
        if (m.images->load(it.source, &image, &w, &h) != Result::Ok) image = {};
        found = m.loaded.emplace(it.src, std::move(image)).first;
      }
      m.items[i].image = found->second;
    }
  }
  if (m.camera < 0) {
    if (error) *error = "no video item '" + cameraItemId + "' to show the camera";
    return Result::InvalidArgument;
  }
  m.draw();
  return Result::Ok;
}

void LivePreview::present(const VideoFrame& camera) {
  std::lock_guard<std::mutex> lock(impl_->mu);
  impl_->last = camera;
  impl_->draw();
}

int64_t LivePreview::presented() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->presented;
}

}  // namespace mf
