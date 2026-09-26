// Minimal demo player: open a file, play/pause, seek bar (scrubbing pauses while dragging).
//
//   mf_demo [clip.mp4]
//   mf_demo --autotest clip.mp4 [play-seconds]   plays, scrubs, seeks, prints metrics, exits

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <mach/mach.h>

#include <random>

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

@implementation Controller {
  NSWindow* _window;
  VideoView* _video;
  NSButton* _playButton;
  NSSlider* _slider;
  NSTextField* _timeLabel;
  NSTextField* _statusLabel;
  NSTimer* _timer;
  std::unique_ptr<mf::PlatformFactory> _platform;
  std::unique_ptr<Listener> _listener;
  std::unique_ptr<mf::Player> _player;
  BOOL _scrubbing, _resumeAfterScrub;
  NSString* _initialPath;

  // --autotest
  BOOL _autotest;
  double _playSeconds;
  int _phase;  // 0 playing, 1 scrubbing, 2 single seeks, 3 done
  BOOL _started, _scrubIssued;
  int64_t _lastSeekCallNs, _lastSeekTargetUs, _lastCompletionNs, _maxScrubGapNs, _scrubLatencyNs;
}

- (instancetype)initWithPath:(NSString*)path autotest:(BOOL)autotest playSeconds:(double)seconds {
  if ((self = [super init])) {
    _initialPath = path;
    _autotest = autotest;
    _playSeconds = seconds;
    _platform = mf::macos::createPlatform();
    _listener = std::make_unique<Listener>(self);
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  [self buildWindow];
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  if (_initialPath) [self openPath:_initialPath];
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

  _video = [[VideoView alloc] initWithFrame:NSMakeRect(0, 44, bounds.size.width, bounds.size.height - 44)];
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

  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)openFile:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ UTTypeMovie ];
  if ([panel runModal] == NSModalResponseOK) [self openPath:panel.URL.path];
}

- (void)openPath:(NSString*)path {
  if (_player) _player->shutdown();
  _player = mf::Player::create(*_platform, _listener.get());
  _statusLabel.stringValue = path.lastPathComponent;
  _statusLabel.textColor = NSColor.labelColor;
  mf::Result r = _player->open(mf::macos::sourceFromPath(path.UTF8String), mf::macos::targetFromView((__bridge void*)_video));
  if (r != mf::Result::Ok) [self failed:[NSString stringWithUTF8String:mf::toString(r)]];
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

int main(int argc, const char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  @autoreleasepool {
    NSString* path = nil;
    BOOL autotest = NO;
    double seconds = 8;
    for (int i = 1; i < argc; ++i) {
      NSString* arg = [NSString stringWithUTF8String:argv[i]];
      if ([arg isEqualToString:@"--autotest"]) autotest = YES;
      else if (!path) path = arg;
      else seconds = arg.doubleValue;
    }
    NSApplication* app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    Controller* controller = [[Controller alloc] initWithPath:path autotest:autotest playSeconds:seconds];
    app.delegate = controller;
    [app run];
  }
  return 0;
}
