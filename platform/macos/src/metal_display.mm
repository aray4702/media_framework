// IDisplay on a CAMetalLayer, presented from the display link. Each vsync callback says when
// the frame drawn now will reach the screen; the frame due at that time is drawn and presented
// right away, so frames land on the real vsync grid. nextDrawable can block for up to 1 s, but
// only on the display-link thread, never on T3 (§2.2).
// Composed frames are drawn by MetalCompositor (§2.4).

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "metal_compositor.h"
#include "mf/macos.h"

// CVDisplayLink is deprecated in Mac OS 15; its replacement (CADisplayLink) needs Mac OS 14 (A13).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace mf::macos {
namespace {

constexpr size_t kMaxPending = 4;

// Outlives the display: GPU callbacks may fire after it is destroyed.
struct Sink {
  std::mutex mu;
  IDisplay::PresentedFn fn;
  void report(int64_t pts, int64_t presentedNs) {
    std::lock_guard<std::mutex> lock(mu);
    if (fn) fn(pts, presentedNs);
  }
};

class MetalDisplay : public IDisplay {
 public:
  ~MetalDisplay() override {
    for (id observer : observers_) [[NSNotificationCenter defaultCenter] removeObserver:observer];
    if (link_) {
      CVDisplayLinkStop(link_);  // waits for a running callback
      CVDisplayLinkRelease(link_);
    }
    {
      std::lock_guard<std::mutex> lock(sink_->mu);
      sink_->fn = nullptr;
    }
  }

  Result attach(const RenderTarget& target, PresentedFn fn) override {
    NSView* view = (__bridge NSView*)target.native;
    if (!view || ![view.layer isKindOfClass:[CAMetalLayer class]]) return Result::InvalidArgument;
    layer_ = (CAMetalLayer*)view.layer;
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) return Result::InvalidArgument;
    // Set only what differs: a layer reused by a new display (a player reopened on the same
    // view) keeps its drawables.
    if (layer_.device != device_) layer_.device = device_;
    if (layer_.pixelFormat != MTLPixelFormatBGRA8Unorm) layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (!layer_.framebufferOnly) layer_.framebufferOnly = YES;
    if (layer_.maximumDrawableCount != 3) layer_.maximumDrawableCount = 3;
    if (!layer_.allowsNextDrawableTimeout) layer_.allowsNextDrawableTimeout = YES;
    queue_ = [device_ newCommandQueue];

    if (!compositor_.init(device_, layer_.pixelFormat)) return Result::InvalidArgument;
    sink_->fn = std::move(fn);

    if (CVDisplayLinkCreateWithActiveCGDisplays(&link_) != kCVReturnSuccess) return Result::InvalidArgument;
    CVDisplayLinkSetOutputCallback(link_, &MetalDisplay::onVsync, this);
    NSWindow* window = view.window;
    windowChanged(window);
    // Track occlusion (hidden frames are dropped by T3) and the screen the window is on.
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    NSOperationQueue* main = [NSOperationQueue mainQueue];
    auto changed = ^(NSNotification* note) { windowChanged(note.object); };
    observers_[0] = [center addObserverForName:NSWindowDidChangeOcclusionStateNotification object:window queue:main usingBlock:changed];
    observers_[1] = [center addObserverForName:NSWindowDidChangeScreenNotification object:window queue:main usingBlock:changed];
    CVDisplayLinkStart(link_);
    return Result::Ok;
  }

  void present(const ComposedFrame& frame, int64_t hostTimeNs) override {
    std::optional<Pending> dropped;
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_.push_back({frame, hostTimeNs});
      if (pending_.size() > kMaxPending) {
        dropped = std::move(pending_.front());
        pending_.pop_front();
      }
    }
    if (dropped) sink_->report(dropped->frame.ptsUs, 0);
  }

  bool visible() const override { return visible_; }
  int64_t vsyncPeriodNs() const override { return vsyncNs_; }
  int64_t latencyNs() const override { return latencyNs_; }
  int64_t vsyncGridNs() const override { return gridNs_; }

 private:
  struct Pending {
    ComposedFrame frame;
    int64_t atNs;
  };

  // Main thread.
  void windowChanged(NSWindow* window) {
    if (!window) return;
    visible_ = (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    NSNumber* screen = window.screen.deviceDescription[@"NSScreenNumber"];
    if (screen) CVDisplayLinkSetCurrentCGDisplay(link_, screen.unsignedIntValue);
  }

  static CVReturn onVsync(CVDisplayLinkRef, const CVTimeStamp* now, const CVTimeStamp* output, CVOptionFlags,
                          CVOptionFlags*, void* self) {
    static_cast<MetalDisplay*>(self)->vsync(now, output);
    return kCVReturnSuccess;
  }

  // Display-link thread: show the latest frame due by this output vsync; older ones were late.
  void vsync(const CVTimeStamp* now, const CVTimeStamp* output) {
    int64_t outputNs = hostNs(output->hostTime);
    int64_t period = output->videoTimeScale ? int64_t(1e9 * double(output->videoRefreshPeriod) / output->videoTimeScale)
                                            : vsyncNs_.load();
    vsyncNs_ = period;
    gridNs_ = outputNs;
    latencyNs_ = outputNs - hostNs(now->hostTime) + period;  // T3 must hand frames over by the callback before

    std::optional<Pending> show;
    std::vector<int64_t> late;
    {
      std::lock_guard<std::mutex> lock(mu_);
      while (!pending_.empty() && pending_.front().atNs <= outputNs + period / 2) {
        if (show) late.push_back(show->frame.ptsUs);
        show = std::move(pending_.front());
        pending_.pop_front();
      }
    }
    for (int64_t pts : late) sink_->report(pts, 0);
    if (show) {
      draw(*show);
      // The first frame is drawn again on the next vsync, unless a newer one comes: the first
      // present on a layer just taken over can be lost, and paused (a player opened at a time)
      // it may be the only frame, which would leave the last display's frame on screen.
      if (!drewFirst_) {
        drewFirst_ = true;
        again_ = std::move(show);
      } else {
        again_.reset();
      }
    } else if (again_) {
      draw(*again_, false);
      again_.reset();
    }
  }

  static int64_t hostNs(uint64_t hostTime) {
    static const mach_timebase_info_data_t tb = [] {
      mach_timebase_info_data_t t;
      mach_timebase_info(&t);
      return t;
    }();
    return static_cast<int64_t>(hostTime * tb.numer / tb.denom);
  }

  void draw(const Pending& p, bool report = true) {
    @autoreleasepool {
      const ComposedFrame& c = p.frame;
      id<CAMetalDrawable> drawable = [layer_ nextDrawable];
      if (!drawable) {
        if (report) sink_->report(c.ptsUs, 0);
        return;
      }
      id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
      compositor_.encode(c, drawable.texture, cmd);

      int64_t pts = c.ptsUs;
      std::shared_ptr<Sink> sink = sink_;
      if (report) [drawable addPresentedHandler:^(id<MTLDrawable> d) {
        sink->report(pts, d.presentedTime > 0 ? static_cast<int64_t>(d.presentedTime * 1e9) : 0);  // 0: dropped
      }];
      [cmd presentDrawable:drawable];
      [cmd commit];
    }
  }

  CAMetalLayer* layer_ = nil;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  MetalCompositor compositor_;  // display-link thread only
  CVDisplayLinkRef link_ = nullptr;
  id observers_[2] = {nil, nil};
  std::shared_ptr<Sink> sink_ = std::make_shared<Sink>();

  std::atomic<bool> visible_{true};
  std::atomic<int64_t> vsyncNs_{16666667}, latencyNs_{0}, gridNs_{0};

  std::mutex mu_;
  std::deque<Pending> pending_;

  // Display-link thread.
  bool drewFirst_ = false;
  std::optional<Pending> again_;  // the first frame, drawn once more
};

}  // namespace

std::unique_ptr<IDisplay> createDisplay() { return std::make_unique<MetalDisplay>(); }

}  // namespace mf::macos
