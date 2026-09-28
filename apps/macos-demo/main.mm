// Minimal demo player: open clips, play/pause, seek bar (scrubbing pauses while dragging),
// brightness and contrast sliders. Several clips play back to back with a slide between them.
//
//   mf_demo [options] [clip.mp4 ...]
//   mf_demo --autotest [options] clip.mp4 [clip.mp4 ...] [play-seconds]
//                                  plays, scrubs, seeks, prints metrics, exits
// Options:
//   --text "caption"               caption at the bottom for the whole scene
//   --transition slide-left|slide-right|cut   (default slide-left)
//   --transition-ms N              slide length (default 1000)
//   --scene file.json              play (or export) a scene document instead of clips
//   --driver auto|vsync|leading    what sets the output times while playing (default auto)
//   --export out.mp4               render the scene into a file instead of playing it
//   --size WxH, --fps N            export size (default 1920x1080) and frame rate (default 30)

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <mach/mach.h>

#include <algorithm>
#include <atomic>
#include <random>

#include "mf/exporter.h"
#include "mf/macos.h"
#include "mf/player.h"

@interface VideoView : NSView
@end

@implementation VideoView
- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) self.wantsLayer = YES;
  return self;
}
- (CALayer*)makeBackingLayer {
  return [CAMetalLayer layer];
}
- (void)updateDrawableSize {
  CGFloat scale = self.window ? self.window.backingScaleFactor : 1;
  CAMetalLayer* layer = (CAMetalLayer*)self.layer;
  layer.contentsScale = scale;
  layer.drawableSize = CGSizeMake(self.bounds.size.width * scale, self.bounds.size.height * scale);
}
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  [self updateDrawableSize];
}
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  [self updateDrawableSize];
}
@end

struct Options {
  NSArray<NSString*>* paths = @[];
  NSString* text = nil;
  mf::SceneTransitionKind transition = mf::SceneTransitionKind::Push;  // or Cut
  mf::Direction direction = mf::Direction::Left;
  int64_t transitionUs = 1000000;
  mf::OutputDriver driver = mf::OutputDriver::Auto;
  NSString* exportPath = nil;
  NSString* scenePath = nil;
  mf::ExportSettings exportSettings;
  BOOL autotest = NO;
  double playSeconds = 8;
};

@interface Controller : NSObject <NSApplicationDelegate>
- (void)stateChanged:(mf::State)state;
- (void)failed:(NSString*)reason;
- (void)warned:(NSString*)reason;
- (void)seekCompleted:(int64_t)ptsUs;
- (void)ended;
@end

// Callbacks arrive on internal threads: hop to the main (owner) thread, never wait on it.
class Listener : public mf::PlayerListener {
 public:
  explicit Listener(Controller* c) : controller_(c) {}
  void onStateChanged(mf::State s) override { post(^(Controller* c) { [c stateChanged:s]; }); }
  void onError(mf::Result r, const std::string& reason) override {
    NSString* text = [NSString stringWithFormat:@"%s: %s", mf::toString(r), reason.c_str()];
    post(^(Controller* c) { [c failed:text]; });
  }
  void onWarning(mf::Warning w, const std::string& reason) override {
    NSString* text = [NSString stringWithFormat:@"%s: %s", mf::toString(w), reason.c_str()];
    post(^(Controller* c) { [c warned:text]; });
  }
  void onSeekCompleted(int64_t pts) override { post(^(Controller* c) { [c seekCompleted:pts]; }); }
  void onEnded() override { post(^(Controller* c) { [c ended]; }); }

 private:
  void post(void (^block)(Controller*)) {
    __weak Controller* weak = controller_;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (Controller* c = weak) block(c);
    });
  }
  __weak Controller* controller_;
};

static int64_t nowNs() { return mf::macos::hostNowNs(); }

// The clips back to back on one video track, joined by the chosen transition (at most half the
// shortest clip), with the caption over the whole scene. Each file is opened here to learn its
// length, which places the clips after it. The output takes the first clip's size and rate,
// or the export size and rate.
static mf::Result sceneFromClips(mf::PlatformFactory& platform, const Options& o, NSArray<NSString*>* paths, bool forExport,
                                 mf::Scene* out, std::string* error) {
  mf::SceneTrack video;
  int64_t shortest = INT64_MAX;
  for (NSString* path in paths) {
    mf::SceneItem it;
    it.type = mf::ItemType::Video;
    it.id = path.lastPathComponent.UTF8String;
    it.source = mf::macos::sourceFromPath(path.UTF8String);
    mf::MediaInfo info;
    mf::Result r = platform.createDemuxer()->open(it.source, &info);
    if (r != mf::Result::Ok || info.durationUs <= 0) {
      *error = it.id + ": cannot open media";
      return r != mf::Result::Ok ? r : mf::Result::MalformedMedia;
    }
    it.durationUs = info.durationUs;
    shortest = std::min(shortest, info.durationUs);
    video.items.push_back(it);
  }
  if (video.items.empty()) {
    *error = "no clips";
    return mf::Result::InvalidArgument;
  }
  size_t n = video.items.size();
  int64_t overlap = n < 2 || o.transition == mf::SceneTransitionKind::Cut ? 0 : std::clamp<int64_t>(o.transitionUs, 0, shortest / 2);
  int64_t start = 0;
  for (size_t c = 0; c < n; ++c) {
    video.items[c].startUs = start;
    start += video.items[c].durationUs - overlap;
    if (c + 1 < n && overlap > 0) {
      mf::SceneTransition x;
      x.from = int(c);
      x.kind = o.transition;
      x.direction = o.direction;
      x.durationUs = overlap;
      video.transitions.push_back(x);
    }
  }
  mf::Scene scene;
  scene.output.width = forExport ? o.exportSettings.width : 0;
  scene.output.height = forExport ? o.exportSettings.height : 0;
  scene.output.fpsNum = forExport ? o.exportSettings.fps : 0;
  scene.output.sampleRate = scene.output.channels = 0;  // the first clip's audio
  scene.tracks.push_back(std::move(video));
  if (o.text) {
    mf::SceneItem caption;
    caption.type = mf::ItemType::Text;
    caption.text = o.text.UTF8String;
    caption.durationUs = scene.durationUs();
    caption.style.hasBox = true;
    caption.transform.y = mf::Animatable(0.96);
    caption.transform.anchorY = 1;
    mf::SceneTrack captions;
    captions.items.push_back(caption);
    scene.tracks.push_back(std::move(captions));
  }
  *out = std::move(scene);
  return mf::Result::Ok;
}

@implementation Controller {
  NSWindow* _window;
  VideoView* _video;
  NSButton* _playButton;
  NSSlider* _slider;
  NSTextField* _timeLabel;
  NSTextField* _statusLabel;
  NSSlider* _brightness;
  NSSlider* _contrast;
  NSTimer* _timer;
  std::unique_ptr<mf::PlatformFactory> _platform;
  std::unique_ptr<Listener> _listener;
  std::unique_ptr<mf::Player> _player;
  BOOL _scrubbing, _resumeAfterScrub;
  Options _options;

  // --autotest
  BOOL _autotest;
  double _playSeconds;
  int _phase;  // 0 playing, 1 scrubbing, 2 single seeks, 3 done
  BOOL _started, _scrubIssued;
  int64_t _lastSeekCallNs, _lastSeekTargetUs, _lastCompletionNs, _maxScrubGapNs, _scrubLatencyNs;
}

- (instancetype)initWithOptions:(const Options&)options {
  if ((self = [super init])) {
    _options = options;
    _autotest = options.autotest;
    _playSeconds = options.playSeconds;
    _platform = mf::macos::createPlatform();
    _listener = std::make_unique<Listener>(self);
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  [self buildWindow];
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  if (_options.scenePath) [self openScene:_options.scenePath];
  else if (_options.paths.count) [self openPaths:_options.paths];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  if (_player) _player->shutdown();
}

- (void)buildWindow {
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 760)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  _window.title = @"Media Framework demo";
  NSView* content = _window.contentView;
  NSRect bounds = content.bounds;

  _video = [[VideoView alloc] initWithFrame:NSMakeRect(0, 76, bounds.size.width, bounds.size.height - 76)];
  _video.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [content addSubview:_video];

  NSButton* open = [NSButton buttonWithTitle:@"Open…" target:self action:@selector(openFile:)];
  open.frame = NSMakeRect(8, 8, 80, 28);
  [content addSubview:open];
  _playButton = [NSButton buttonWithTitle:@"Play" target:self action:@selector(togglePlay:)];
  _playButton.frame = NSMakeRect(92, 8, 80, 28);
  [content addSubview:_playButton];

  _slider = [NSSlider sliderWithValue:0 minValue:0 maxValue:1 target:self action:@selector(sliderMoved:)];
  _slider.frame = NSMakeRect(180, 10, bounds.size.width - 180 - 380, 24);
  _slider.autoresizingMask = NSViewWidthSizable;
  _slider.continuous = YES;
  [content addSubview:_slider];

  _timeLabel = [NSTextField labelWithString:@"--"];
  _timeLabel.frame = NSMakeRect(bounds.size.width - 390, 12, 170, 20);
  _timeLabel.autoresizingMask = NSViewMinXMargin;
  _timeLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
  [content addSubview:_timeLabel];
  _statusLabel = [NSTextField labelWithString:@""];
  _statusLabel.frame = NSMakeRect(bounds.size.width - 215, 12, 210, 20);
  _statusLabel.autoresizingMask = NSViewMinXMargin;
  _statusLabel.lineBreakMode = NSLineBreakByTruncatingTail;
  [content addSubview:_statusLabel];

  // Second row: the filter, applied live (a paused frame is redrawn).
  NSTextField* b = [NSTextField labelWithString:@"Brightness"];
  b.frame = NSMakeRect(8, 46, 72, 20);
  [content addSubview:b];
  _brightness = [NSSlider sliderWithValue:0 minValue:-1 maxValue:1 target:self action:@selector(filterChanged:)];
  _brightness.frame = NSMakeRect(84, 44, 200, 24);
  _brightness.continuous = YES;
  [content addSubview:_brightness];
  NSTextField* c = [NSTextField labelWithString:@"Contrast"];
  c.frame = NSMakeRect(300, 46, 60, 20);
  [content addSubview:c];
  _contrast = [NSSlider sliderWithValue:1 minValue:0 maxValue:2 target:self action:@selector(filterChanged:)];
  _contrast.frame = NSMakeRect(364, 44, 200, 24);
  _contrast.continuous = YES;
  [content addSubview:_contrast];
  NSButton* reset = [NSButton buttonWithTitle:@"Reset" target:self action:@selector(resetFilter:)];
  reset.frame = NSMakeRect(572, 42, 70, 28);
  [content addSubview:reset];

  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)openFile:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ UTTypeMovie ];
  panel.allowsMultipleSelection = YES;  // played in the order selected, with a slide between
  if ([panel runModal] != NSModalResponseOK) return;
  NSMutableArray<NSString*>* paths = [NSMutableArray array];
  for (NSURL* url in panel.URLs) [paths addObject:url.path];
  [self openPaths:paths];
}

- (void)openScene:(NSString*)path {
  if (_player) _player->shutdown();
  _player = mf::Player::create(*_platform, _listener.get());
  _statusLabel.stringValue = path.lastPathComponent;
  _statusLabel.textColor = NSColor.labelColor;
  mf::Scene scene;
  std::string error;
  mf::Result r = mf::macos::loadScene(path.UTF8String, &scene, &error);
  if (r == mf::Result::Ok) {
    r = _player->open(scene, mf::macos::targetFromView((__bridge void*)_video), _options.driver, &error);
    _player->setFilter([self currentFilter]);
  }
  if (r != mf::Result::Ok) [self failed:[NSString stringWithFormat:@"%s: %s", mf::toString(r), error.c_str()]];
}

- (void)openPaths:(NSArray<NSString*>*)paths {
  if (_player) _player->shutdown();
  _player = mf::Player::create(*_platform, _listener.get());
  _statusLabel.stringValue = paths.count == 1 ? paths[0].lastPathComponent
                                              : [NSString stringWithFormat:@"%lu clips", (unsigned long)paths.count];
  _statusLabel.textColor = NSColor.labelColor;
  mf::Scene scene;
  std::string error;
  mf::Result r = sceneFromClips(*_platform, _options, paths, NO, &scene, &error);
  if (r == mf::Result::Ok) {
    r = _player->open(scene, mf::macos::targetFromView((__bridge void*)_video), _options.driver, &error);
    _player->setFilter([self currentFilter]);
  }
  if (r != mf::Result::Ok) [self failed:[NSString stringWithFormat:@"%s: %s", mf::toString(r), error.c_str()]];
}

- (mf::VideoFilter)currentFilter {
  return {float(_brightness.doubleValue), float(_contrast.doubleValue)};
}

- (void)filterChanged:(id)sender {
  if (_player) _player->setFilter([self currentFilter]);
}

- (void)resetFilter:(id)sender {
  _brightness.doubleValue = 0;
  _contrast.doubleValue = 1;
  [self filterChanged:sender];
}

- (void)togglePlay:(id)sender {
  if (!_player) return;
  if (_player->state() == mf::State::Play) _player->pause();
  else _player->play();
  [self stateChanged:_player->state()];
}

- (void)sliderMoved:(id)sender {
  if (!_player) return;
  if (!_scrubbing) {  // seek is only allowed in READY (A4): pause while dragging
    _scrubbing = YES;
    _resumeAfterScrub = _player->state() == mf::State::Play;
    if (_resumeAfterScrub) _player->pause();
  }
  _player->seek(static_cast<int64_t>(_slider.doubleValue * _player->durationUs()));
  if (NSApp.currentEvent.type == NSEventTypeLeftMouseUp) {
    _scrubbing = NO;
    if (_resumeAfterScrub) _player->play();
  }
}

- (void)tick {
  if (!_player) return;
  int64_t pos = _player->positionUs(), dur = _player->durationUs();
  _timeLabel.stringValue = [NSString stringWithFormat:@"%6.2f / %6.2f %s", pos / 1e6, dur / 1e6, mf::toString(_player->state())];
  if (!_scrubbing && dur > 0) _slider.doubleValue = double(pos) / double(dur);
}

- (void)stateChanged:(mf::State)state {
  _playButton.title = state == mf::State::Play ? @"Pause" : @"Play";
  if (_autotest) std::printf("state %s at %.3fs\n", mf::toString(state), _player->positionUs() / 1e6);
  if (_autotest && state == mf::State::Ready && _phase == 0 && !_started) {
    _started = YES;
    _player->play();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t((_playSeconds + 30) * 1e9)), dispatch_get_main_queue(), ^{
      std::printf("TIMEOUT in phase %d\n", self->_phase);
      [self finishAutotest];
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(_playSeconds * 1e9)), dispatch_get_main_queue(), ^{
      [self startScrub];
    });
  }
}

- (void)failed:(NSString*)reason {
  _statusLabel.stringValue = reason;
  _statusLabel.textColor = NSColor.systemRedColor;
  if (_autotest) {
    std::printf("ERROR %s\n", reason.UTF8String);
    [self finishAutotest];
  }
}

- (void)warned:(NSString*)reason {
  _statusLabel.stringValue = reason;
  _statusLabel.textColor = NSColor.systemOrangeColor;
  if (_autotest) std::printf("WARNING %s\n", reason.UTF8String);
}

- (void)ended {
  if (_autotest) std::printf("ended\n");
}

// --- autotest ---------------------------------------------------------------------------

- (void)startScrub {
  if (_phase != 0) return;
  _phase = 1;
  if (_player->state() == mf::State::Play) _player->pause();
  _lastCompletionNs = nowNs();
  auto rng = std::make_shared<std::mt19937>(42);
  __block int remaining = 50;  // 50 seeks in 2 s
  [NSTimer scheduledTimerWithTimeInterval:0.04 repeats:YES block:^(NSTimer* timer) {
    if (self->_phase != 1) return [timer invalidate];
    int64_t target = std::uniform_int_distribution<int64_t>(0, self->_player->durationUs())(*rng);
    self->_player->seek(target);
    self->_lastSeekCallNs = nowNs();
    self->_lastSeekTargetUs = target;
    if (--remaining == 0) {
      self->_scrubIssued = YES;
      [timer invalidate];
    }
  }];
}

- (void)seekCompleted:(int64_t)ptsUs {
  if (!_autotest || _phase != 1) return;
  int64_t now = nowNs();
  _maxScrubGapNs = std::max(_maxScrubGapNs, now - _lastCompletionNs);
  _lastCompletionNs = now;
  // The final scrub target is shown: the last frame at or before it (within a frame or two).
  if (_scrubIssued && ptsUs <= _lastSeekTargetUs && _lastSeekTargetUs - ptsUs < 100000) {
    _scrubLatencyNs = now - _lastSeekCallNs;
    _phase = 2;
    [self singleSeeks];
  }
}

- (void)singleSeeks {
  __block int remaining = 10;  // one seek every 300 ms, for seek latency
  [NSTimer scheduledTimerWithTimeInterval:0.3 repeats:YES block:^(NSTimer* timer) {
    if (remaining-- == 0) {
      [timer invalidate];
      return [self finishAutotest];
    }
    self->_player->seek(self->_player->durationUs() * (remaining + 1) / 12);
  }];
}

- (void)finishAutotest {
  if (_phase == 3) return;
  _phase = 3;
  mf::MetricsReport m = _player->metrics();
  task_vm_info_data_t vm;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count);
  std::printf("state=%s\n%s\n", mf::toString(_player->state()), m.toString().c_str());
  std::printf("scrub: last target shown %.0f ms after last call, max gap between frames %.0f ms\n",
              _scrubLatencyNs / 1e6, _maxScrubGapNs / 1e6);
  std::printf("peak memory: %.0f MB\n", vm.ledger_phys_footprint_peak / 1048576.0);
  std::fflush(stdout);
  _player->shutdown();
  [NSApp terminate:nil];
}
@end

// Headless export: prints progress, exits 0 once the file is written.
static int exportScene(const Options& o) {
  struct Waiter : mf::ExportListener {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    std::atomic<bool> ok{false};
    void onWarning(mf::Warning, const std::string& reason) override { std::printf("WARNING %s\n", reason.c_str()); }
    void onCompleted() override {
      ok = true;
      dispatch_semaphore_signal(done);
    }
    void onError(mf::Result r, const std::string& reason) override {
      std::printf("ERROR %s: %s\n", mf::toString(r), reason.c_str());
      dispatch_semaphore_signal(done);
    }
  } waiter;
  auto platform = mf::macos::createPlatform();
  auto exporter = mf::Exporter::create(*platform, &waiter);
  int64_t startNs = mf::macos::hostNowNs();
  mf::ExportTarget target = mf::macos::exportTargetFromPath(o.exportPath.UTF8String);
  mf::Scene scene;
  std::string error;
  mf::Result r = o.scenePath ? mf::macos::loadScene(o.scenePath.UTF8String, &scene, &error)  // its own size and rate
                             : sceneFromClips(*platform, o, o.paths, YES, &scene, &error);
  if (r == mf::Result::Ok) r = exporter->start(scene, target, o.exportSettings, &error);
  if (r != mf::Result::Ok) {
    std::printf("ERROR %s %s\n", mf::toString(r), error.c_str());
    return 1;
  }
  while (dispatch_semaphore_wait(waiter.done, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)) != 0) {
    std::printf("export %3.0f%%\n", exporter->progress() * 100);
  }
  exporter->shutdown();
  std::printf("%s %s in %.1f s\n", waiter.ok ? "wrote" : "failed", o.exportPath.UTF8String,
              (mf::macos::hostNowNs() - startNs) / 1e9);
  return waiter.ok ? 0 : 1;
}

int main(int argc, const char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  @autoreleasepool {
    Options options;
    NSMutableArray<NSString*>* paths = [NSMutableArray array];
    NSCharacterSet* numeric = [NSCharacterSet characterSetWithCharactersInString:@"0123456789."];
    for (int i = 1; i < argc; ++i) {
      NSString* arg = [NSString stringWithUTF8String:argv[i]];
      NSString* value = i + 1 < argc ? [NSString stringWithUTF8String:argv[i + 1]] : nil;
      if ([arg isEqualToString:@"--autotest"]) {
        options.autotest = YES;
      } else if ([arg isEqualToString:@"--text"] && value) {
        options.text = value;
        ++i;
      } else if ([arg isEqualToString:@"--transition"] && value) {
        options.transition = [value isEqualToString:@"cut"] ? mf::SceneTransitionKind::Cut : mf::SceneTransitionKind::Push;
        options.direction = [value isEqualToString:@"slide-right"] ? mf::Direction::Right : mf::Direction::Left;
        ++i;
      } else if ([arg isEqualToString:@"--driver"] && value) {
        options.driver = [value isEqualToString:@"vsync"]     ? mf::OutputDriver::Vsync
                         : [value isEqualToString:@"leading"] ? mf::OutputDriver::LeadingClip
                                                              : mf::OutputDriver::Auto;
        ++i;
      } else if ([arg isEqualToString:@"--scene"] && value) {
        options.scenePath = value;
        ++i;
      } else if ([arg isEqualToString:@"--export"] && value) {
        options.exportPath = value;
        ++i;
      } else if ([arg isEqualToString:@"--size"] && value) {
        NSArray<NSString*>* wh = [value componentsSeparatedByString:@"x"];
        if (wh.count == 2) options.exportSettings = {wh[0].intValue, wh[1].intValue, options.exportSettings.fps};
        ++i;
      } else if ([arg isEqualToString:@"--fps"] && value) {
        options.exportSettings.fps = value.intValue;
        ++i;
      } else if ([arg isEqualToString:@"--transition-ms"] && value) {
        options.transitionUs = static_cast<int64_t>(value.doubleValue * 1000);
        ++i;
      } else if ([[arg stringByTrimmingCharactersInSet:numeric] length] == 0 &&
                 ![[NSFileManager defaultManager] fileExistsAtPath:arg]) {
        options.playSeconds = arg.doubleValue;  // the trailing play-seconds of --autotest
      } else {
        [paths addObject:arg];
      }
    }
    options.paths = paths;
    if (options.exportPath) return exportScene(options);
    NSApplication* app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    Controller* controller = [[Controller alloc] initWithOptions:options];
    app.delegate = controller;
    [app run];
  }
  return 0;
}
