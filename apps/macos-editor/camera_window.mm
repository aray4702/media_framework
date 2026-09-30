#import "camera_window.h"

#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#import "inspector_view.h"
#import "preview_overlay.h"
#import "preview_view.h"
#import "sidebar_view.h"
#include "mf/live_preview.h"
#include "mf/macos.h"
#include "mf/segment_recorder.h"

namespace {
constexpr CGFloat kPaneWidth = 380, kBarHeight = 112;  // same width as the editor sidebar (main.mm kSidebarWidth)
constexpr int kFps = 30;
constexpr int kMicRate = 48000, kMicChannels = 1;  // what AvCamera delivers
const char* const kCameraItem = "camera";

NSString* seconds(int64_t us) {
  double s = us / 1e6;
  return s >= 60 ? [NSString stringWithFormat:@"%d:%04.1f", int(s) / 60, std::fmod(s, 60.0)] : [NSString stringWithFormat:@"%.1f s", s];
}

NSString* effectList(const mf::SceneEffects& e) {
  NSMutableArray* names = [NSMutableArray array];
  if (e.crop) [names addObject:@"Crop"];
  if (e.chromaKey) [names addObject:@"Chroma Key"];
  if (e.colorAdjust) [names addObject:@"Color Adjust"];
  for (const mf::ScenePluginEffect& p : e.plugins) {
    NSString* t = [NSString stringWithUTF8String:p.type.c_str()];
    [names addObject:t.length ? [[t substringToIndex:1].uppercaseString stringByAppendingString:[t substringFromIndex:1]] : t];
  }
  if (e.blur) [names addObject:@"Blur"];
  return [names componentsJoinedByString:@", "];
}
}  // namespace

// Delete removes the selected overlay (a text being edited takes its keys first).
@interface CameraWindow : NSWindow
@property(nonatomic, copy) void (^deleteKey)(void);
@end
@implementation CameraWindow
- (void)keyDown:(NSEvent*)event {
  if ((event.keyCode == 51 || event.keyCode == 117) && self.deleteKey) return self.deleteKey();
  [super keyDown:event];
}
@end

// Holds the preview at the project's aspect ratio; `resized` lays it out again.
@interface CameraStage : NSView
@property(nonatomic, copy) void (^resized)(void);
@end
@implementation CameraStage
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (self.resized) self.resized();
}
- (void)drawRect:(NSRect)dirty {
  [NSColor.blackColor setFill];
  NSRectFill(dirty);
}
@end

// The segments recorded, drawn against the maximum length (or, unlimited, a scale that grows),
// the one being recorded in red. A segment with effects has a star; hovering lists them.
@interface SegmentBar : NSView
@property(nonatomic) const editor::CaptureSession* session;
@property(nonatomic) int64_t recordingUs;  // of the segment being recorded; 0 when not recording
@end
@implementation SegmentBar
- (int64_t)scaleUs {
  if (_session->maxDurationUs() != editor::CaptureSession::kUnlimited) return _session->maxDurationUs();
  return std::max<int64_t>(15000000, (_session->totalUs() + _recordingUs) * 5 / 4);
}
- (void)drawRect:(NSRect)dirty {
  NSRect track = NSInsetRect(self.bounds, 0, 4);
  [[NSColor.labelColor colorWithAlphaComponent:0.1] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:track xRadius:4 yRadius:4] fill];
  if (!_session) return;
  double k = track.size.width / double([self scaleUs]);
  [self removeAllToolTips];
  CGFloat x = track.origin.x;
  const auto& segments = _session->segments();
  for (size_t i = 0; i < segments.size(); ++i) {
    CGFloat w = CGFloat(segments[i].durationUs * k);
    NSRect r = NSMakeRect(x, track.origin.y, std::max<CGFloat>(w - 2, 1), track.size.height);  // 2 pt apart
    [NSColor.controlAccentColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:r xRadius:3 yRadius:3] fill];
    if (segments[i].effects.any() && w > 14) {
      NSImage* star = [NSImage imageWithSystemSymbolName:@"wand.and.stars" accessibilityDescription:@"Effects"];
      [star drawInRect:NSMakeRect(NSMinX(r) + 3, NSMidY(r) - 5, 10, 10)];
    }
    [self addToolTipRect:r owner:self userData:(void*)i];
    x += w;
  }
  if (_recordingUs > 0) {
    NSRect r = NSMakeRect(x, track.origin.y, std::max<CGFloat>(CGFloat(_recordingUs * k), 1), track.size.height);
    [NSColor.systemRedColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:r xRadius:3 yRadius:3] fill];
  }
}
- (NSString*)view:(NSView*)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point userData:(void*)data {
  size_t i = size_t(data);
  if (!_session || i >= _session->segments().size()) return nil;
  const auto& s = _session->segments()[i];
  NSString* fx = effectList(s.effects);
  return [NSString stringWithFormat:@"Segment %zu: %@%@%@", i + 1, seconds(s.durationUs), fx.length ? @" — " : @"", fx];
}
@end

@interface CameraWindowController () <SidebarDelegate, PreviewOverlayDelegate, InspectorDelegate>
@end

@implementation CameraWindowController {
  mf::PlatformFactory* _platform;
  std::unique_ptr<editor::CaptureSession> _session;
  editor::Selection _sel;        // the overlay selected in the preview
  editor::Selection _cameraSel;  // the camera item: what the Effects tab edits
  std::unique_ptr<mf::ICamera> _camera;
  std::unique_ptr<mf::LivePreview> _preview;
  std::unique_ptr<mf::SegmentRecorder> _recorder;
  std::atomic<int> _frameWidth, _frameHeight;
  std::atomic<bool> _firstFrame;
  std::vector<mf::CameraDevice> _devices;
  NSString* _folder;
  NSString* _recordingFile;  // the segment being recorded (or saved)
  int _nextSegment;
  BOOL _saving, _finished, _cameraRunning, _previewAttached, _updatingZOrder;
  AVAudioPlayer* _music;
  NSTimer* _timer;

  SidebarView* _pane;
  NSBox* _sideLine;
  InspectorView* _effects;
  CameraStage* _stage;
  PreviewView* _video;
  PreviewOverlay* _overlay;
  NSTextField* _message;
  NSButton* _settings;
  SegmentBar* _bar;
  NSTextField* _time;
  NSPopUpButton* _cameraMenu;
  NSButton *_mirror, *_mic, *_musicRemove, *_record, *_deleteLast, *_cancel, *_done;
  NSTextField* _musicLabel;
}

+ (NSString*)journalName {
  return @"session.json";
}

- (instancetype)initWithPlatform:(mf::PlatformFactory*)platform
                          output:(const mf::SceneOutput&)output
                     maxDuration:(int64_t)maxDurationUs
                          folder:(NSString*)folder
                         journal:(const mf::Scene*)journal
                      audioFiles:(NSArray<NSString*>*)audioFiles {
  CameraWindow* window = [[CameraWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1280, 820)
                                                         styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                           backing:NSBackingStoreBuffered
                                                             defer:NO];
  if (!(self = [super initWithWindow:window])) return nil;
  _platform = platform;
  _folder = folder;
  _camera = platform->createCamera();
  _preview = std::make_unique<mf::LivePreview>(*platform);
  _recorder = std::make_unique<mf::SegmentRecorder>(*platform);
  _frameWidth = _frameHeight = 0;
  _firstFrame = false;
  _devices = _camera ? _camera->devices() : std::vector<mf::CameraDevice>{};
  bool front = !_devices.empty() && _devices[0].front;
  _session = std::make_unique<editor::CaptureSession>(output, maxDurationUs, front);
  if (journal && _session->restore(*journal)) {
    _session->setMaxDurationUs(maxDurationUs);
    [self resolveSources];
  }
  _nextSegment = 1;  // past the numbers of the segments recovered
  for (const auto& seg : _session->segments()) {
    NSString* name = [NSString stringWithUTF8String:seg.file.c_str()].lastPathComponent.stringByDeletingPathExtension;
    if ([name hasPrefix:@"Segment "]) _nextSegment = std::max(_nextSegment, [name substringFromIndex:8].intValue + 1);
  }
  _cameraSel = {editor::CaptureSession::kCameraTrack, 0};

  window.title = @"Camera";
  window.releasedWhenClosed = NO;  // the controller (ARC) owns it
  window.delegate = self;
  window.minSize = NSMakeSize(1080, 600);
  __weak CameraWindowController* weak = self;
  window.deleteKey = ^{
    [weak deleteSelectedOverlay];
  };
  [self buildWithAudioFiles:audioFiles];
  if (_session->hasMusic()) [self loadMusicPlayer:[NSString stringWithUTF8String:_session->musicSrc().c_str()]];
  [self sceneChanged];
  [self updateControls];
  [window center];
  _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  dispatch_async(dispatch_get_main_queue(), ^{
    [weak startCamera];
  });
  return self;
}

// --- Layout -----------------------------------------------------------------------------------

- (void)buildWithAudioFiles:(NSArray<NSString*>*)audioFiles {
  NSView* content = self.window.contentView;
  NSSize size = content.bounds.size;
  __weak CameraWindowController* weak = self;

  _pane = [[SidebarView alloc] initWithFrame:NSMakeRect(0, 0, kPaneWidth, size.height)
                                        tabs:SidebarTabStickers | SidebarTabEmojis | SidebarTabText | SidebarTabEffects | SidebarTabAudio];
  _pane.autoresizingMask = NSViewHeightSizable;
  _pane.clipsToBounds = YES;
  _pane.delegate = self;
  _pane.musicMode = YES;
  for (NSString* path in audioFiles) [_pane rememberFile:path];
  [content addSubview:_pane];
  _effects = [[InspectorView alloc] initWithFrame:NSMakeRect(0, 0, kPaneWidth, 500) document:&_session->doc selection:&_cameraSel];
  _effects.delegate = self;
  _effects.effectsTitle = @"Camera";
  _effects.readOnlyNote = @"Stop recording to change effects.";
  _effects.effectsPage = YES;
  [_pane setEffectsView:_effects];
  _sideLine = [[NSBox alloc] initWithFrame:NSMakeRect(kPaneWidth, 0, 1, size.height)];
  _sideLine.boxType = NSBoxSeparator;
  _sideLine.autoresizingMask = NSViewHeightSizable;
  [content addSubview:_sideLine];

  CGFloat x = kPaneWidth + 1, w = size.width - x;
  _stage = [[CameraStage alloc] initWithFrame:NSMakeRect(x, kBarHeight, w, size.height - kBarHeight)];
  _stage.clipsToBounds = YES;
  _stage.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  _stage.resized = ^{
    [weak layoutStage];
  };
  [content addSubview:_stage];
  _video = [[PreviewView alloc] initWithFrame:_stage.bounds];
  [_stage addSubview:_video];
  _overlay = [[PreviewOverlay alloc] initWithFrame:_stage.bounds document:&_session->doc selection:&_sel];
  _overlay.delegate = self;
  _overlay.firstSelectableTrack = editor::CaptureSession::kCameraTrack + 1;  // the camera stays put
  NSMutableDictionary<NSString*, NSValue*>* sizes = [NSMutableDictionary dictionary];
  _overlay.naturalSize = ^NSSize(const mf::SceneItem& it) {  // a sticker's pixel size, read once per file
    if (it.type != mf::ItemType::Image) return NSZeroSize;
    NSString* path = [NSString stringWithUTF8String:it.src.c_str()];
    if (NSValue* known = sizes[path]) return known.sizeValue;
    NSSize size = NSZeroSize;
    if (CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr)) {
      NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
      CFRelease(source);
      size = NSMakeSize([props[(id)kCGImagePropertyPixelWidth] doubleValue], [props[(id)kCGImagePropertyPixelHeight] doubleValue]);
    }
    sizes[path] = [NSValue valueWithSize:size];
    return size;
  };
  [_overlay registerForDraggedTypes:@[ SidebarDragType ]];
  [_stage addSubview:_overlay];
  _message = [NSTextField wrappingLabelWithString:@"Starting the camera…"];
  _message.alignment = NSTextAlignmentCenter;
  _message.textColor = NSColor.whiteColor;
  _message.font = [NSFont systemFontOfSize:15];
  [_stage addSubview:_message];
  _settings = [NSButton buttonWithTitle:@"Open Privacy Settings" target:self action:@selector(openPrivacySettings:)];
  _settings.hidden = YES;
  [_stage addSubview:_settings];

  // The bar: the segments, music and time above; the controls below (laid out in layoutBar).
  _bar = [[SegmentBar alloc] initWithFrame:NSZeroRect];
  _bar.session = _session.get();
  [content addSubview:_bar];
  _time = [NSTextField labelWithString:@""];
  _time.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
  [content addSubview:_time];
  _musicLabel = [NSTextField labelWithString:@""];
  _musicLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
  [content addSubview:_musicLabel];
  _musicRemove = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"xmark.circle.fill" accessibilityDescription:@"Remove music"]
                                    target:self
                                    action:@selector(removeMusic:)];
  _musicRemove.bordered = NO;
  _musicRemove.toolTip = @"No music";
  [content addSubview:_musicRemove];

  _cameraMenu = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  for (const auto& d : _devices) [_cameraMenu addItemWithTitle:[NSString stringWithUTF8String:d.name.c_str()]];
  if (_devices.empty()) [_cameraMenu addItemWithTitle:@"No camera"];
  _cameraMenu.target = self;
  _cameraMenu.action = @selector(cameraChosen:);
  [content addSubview:_cameraMenu];
  _mirror = [NSButton checkboxWithTitle:@"Mirror" target:self action:@selector(mirrorChanged:)];
  _mirror.toolTip = @"Mirror the camera, in the preview and the recording";
  [content addSubview:_mirror];
  _mic = [NSButton checkboxWithTitle:@"Mic" target:self action:@selector(micChanged:)];
  _mic.toolTip = @"Record the microphone";
  [content addSubview:_mic];
  _record = [NSButton buttonWithTitle:@"Record" target:self action:@selector(toggleRecord:)];
  _record.bezelColor = NSColor.systemRedColor;
  _record.keyEquivalent = @" ";
  _record.toolTip = @"Start or stop a segment (Space)";
  _record.wantsLayer = YES;
  [content addSubview:_record];
  _deleteLast = [NSButton buttonWithTitle:@"Delete Last" target:self action:@selector(deleteLast:)];
  _deleteLast.image = [NSImage imageWithSystemSymbolName:@"delete.backward" accessibilityDescription:nil];
  _deleteLast.imagePosition = NSImageLeading;
  _deleteLast.toolTip = @"Removes the most recent segment";
  [content addSubview:_deleteLast];
  _cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
  [content addSubview:_cancel];
  _done = [NSButton buttonWithTitle:@"Done" target:self action:@selector(done:)];
  _done.keyEquivalent = @"\r";
  [content addSubview:_done];
  [self layoutBar];
  [self layoutStage];
  [self attachPreview];
  [self updateContentZOrder];
}

// The camera preview uses a Metal layer; keep the left pane above it so the Effects inspector stays visible.
- (void)updateContentZOrder {
  if (_updatingZOrder) return;
  NSView* content = self.window.contentView;
  if (!content || !_pane) return;
  _updatingZOrder = YES;
  for (NSView* v in @[
         _stage, _bar, _time, _musicLabel, _musicRemove, _cameraMenu, _mirror, _mic, _record, _deleteLast, _cancel, _done
       ]) {
    if (v.superview == content) [content addSubview:v positioned:NSWindowBelow relativeTo:_pane];
  }
  if (_sideLine.superview == content && _stage) {
    [content addSubview:_sideLine positioned:NSWindowAbove relativeTo:_stage];
  }
  if (content.subviews.lastObject != _pane) [content addSubview:_pane positioned:NSWindowAbove relativeTo:nil];
  _updatingZOrder = NO;
}

// Below the stage, from the pane's edge: the segments, music and time; then the camera's settings
// on the left, Delete Last, Cancel and Done on the right, and Record in the middle of the space left.
- (void)layoutBar {
  CGFloat x = _pane.frame.size.width + 1, W = self.window.contentView.bounds.size.width;
  _bar.frame = NSMakeRect(x + 16, 80, std::max<CGFloat>(40, W - x - 16 - 390), 20);
  _musicLabel.frame = NSMakeRect(W - 360, 81, 140, 18);
  _musicRemove.frame = NSMakeRect(W - 216, 80, 20, 20);
  _time.frame = NSMakeRect(W - 186, 81, 176, 18);
  _cameraMenu.frame = NSMakeRect(x + 16, 22, 150, 26);
  _mirror.frame = NSMakeRect(x + 174, 24, 70, 22);
  _mic.frame = NSMakeRect(x + 246, 24, 60, 22);
  _done.frame = NSMakeRect(W - 96, 18, 84, 32);
  _cancel.frame = NSMakeRect(W - 186, 18, 84, 32);
  _deleteLast.frame = NSMakeRect(W - 306, 18, 114, 32);
  CGFloat left = x + 316, right = W - 316;
  _record.frame = NSMakeRect(std::round((left + right) / 2 - 65), 14, 130, 40);
}

- (void)windowDidResize:(NSNotification*)note {
  [self layoutBar];
  [self layoutStage];
}

// The preview at the project's aspect ratio, as large as fits in the stage, centered.
- (void)layoutStage {
  NSRect b = _stage.bounds;
  const mf::SceneOutput& o = _session->doc.scene.output;
  double k = std::min(b.size.width / o.width, b.size.height / o.height);
  NSRect r = NSMakeRect(std::round((b.size.width - o.width * k) / 2), std::round((b.size.height - o.height * k) / 2),
                        std::round(o.width * k), std::round(o.height * k));
  _video.frame = r;
  _overlay.frame = r;
  _message.frame = NSMakeRect(NSMidX(b) - 220, NSMidY(b) - 10, 440, 60);
  _settings.frame = NSMakeRect(NSMidX(b) - 90, NSMidY(b) - 50, 180, 30);
}

// --- The camera -------------------------------------------------------------------------------

// Connects LivePreview to the Metal layer in _video (once the view is in the window).
- (void)attachPreview {
  if (_previewAttached) return;
  mf::Result r = _preview->attach(mf::macos::targetFromView((__bridge void*)_video));
  if (r == mf::Result::Ok || r == mf::Result::InvalidState) {
    _previewAttached = YES;
    [self sceneChanged];
    return;
  }
  [self showMessage:[NSString stringWithFormat:@"The preview couldn't start (%s).", mf::toString(r)] settings:NO];
}

- (void)windowDidBecomeKey:(NSNotification*)note {
  [self attachPreview];
  [self updateContentZOrder];
}

- (NSInteger)deviceIndex {
  return std::clamp<NSInteger>(_cameraMenu.indexOfSelectedItem, 0, std::max<NSInteger>(0, NSInteger(_devices.size()) - 1));
}

- (void)showMessage:(NSString*)text settings:(BOOL)settings {
  _message.stringValue = text ?: @"";
  _message.hidden = !text;
  _settings.hidden = !settings;
}

// Starts (or restarts) the camera, with the microphone when it's on: asks for access first.
- (void)startCamera {
  if (_finished) return;
  if (!_camera || _devices.empty()) return [self showMessage:@"No camera is connected." settings:NO];
  __weak CameraWindowController* weak = self;
  bool mic = _session->recordMicrophone();
  if (mf::macos::cameraAccess(false) == mf::macos::CameraAccess::NotAsked ||
      (mic && mf::macos::cameraAccess(true) == mf::macos::CameraAccess::NotAsked)) {
    [self showMessage:@"Waiting for access to the camera…" settings:NO];
    return mf::macos::requestCameraAccess(mic, [weak](bool) {
      dispatch_async(dispatch_get_main_queue(), ^{
        [weak startCameraAsked];
      });
    });
  }
  [self startCameraAsked];
}

- (void)startCameraAsked {
  if (_finished) return;
  if (mf::macos::cameraAccess(false) != mf::macos::CameraAccess::Granted) {
    return [self showMessage:@"Media Editor isn't allowed to use the camera. Allow it in System Settings > Privacy & Security > Camera "
                             @"(for the app it runs from, such as Terminal)."
                    settings:YES];
  }
  if (_session->recordMicrophone() && mf::macos::cameraAccess(true) != mf::macos::CameraAccess::Granted) {
    _session->setRecordMicrophone(false);  // the camera still works: record without the microphone
    [self updateControls];
  }
  mf::LivePreview* preview = _preview.get();
  mf::SegmentRecorder* recorder = _recorder.get();
  std::atomic<int>* width = &_frameWidth;
  std::atomic<int>* height = &_frameHeight;
  std::atomic<bool>* first = &_firstFrame;
  __weak CameraWindowController* weak = self;
  mf::ICamera::AudioFn audio;
  if (_session->recordMicrophone()) {
    audio = [recorder](const int16_t* pcm, int frames, int rate, int channels, int64_t hostNs) {
      if (rate == kMicRate && channels == kMicChannels) recorder->audio(pcm, frames, hostNs);
    };
  }
  [self showMessage:@"Starting the camera…" settings:NO];
  mf::Result r = _camera->start(_devices[size_t([self deviceIndex])].id,
                                [=](const mf::VideoFrame& f) {
                                  int w = 0, h = 0;
                                  if (mf::macos::frameSize(f, &w, &h)) {
                                    *width = w;
                                    *height = h;
                                  }
                                  preview->present(f);
                                  recorder->video(f);
                                  if (!first->exchange(true)) {
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                      [weak showMessage:nil settings:NO];
                                    });
                                  }
                                },
                                audio);
  _cameraRunning = r == mf::Result::Ok;
  if (r == mf::Result::PermissionDenied) return [self startCameraAsked];  // access changed meanwhile
  if (r != mf::Result::Ok) [self showMessage:[NSString stringWithFormat:@"The camera couldn't start (%s).", mf::toString(r)] settings:NO];
}

- (void)restartCamera {
  _firstFrame = false;
  if (_camera) _camera->stop();
  [self startCamera];
}

- (void)openPrivacySettings:(id)sender {
  [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_Camera"]];
}

- (void)cameraChosen:(id)sender {
  if (!_devices.empty()) _session->setMirror(_devices[size_t([self deviceIndex])].front);
  [self sceneChanged];
  [self updateControls];
  [self restartCamera];
}

- (void)mirrorChanged:(id)sender {
  _session->setMirror(_mirror.state == NSControlStateValueOn);
  [self sceneChanged];
}

- (void)micChanged:(id)sender {
  _session->setRecordMicrophone(_mic.state == NSControlStateValueOn);
  [self restartCamera];
}

// --- The scene: overlays, effects ---------------------------------------------------------------

// Stickers are image files: the preview loads them by source.
- (void)resolveSources {
  for (int t = 0; t < _session->doc.tracks(); ++t) {
    for (mf::SceneItem& it : _session->doc.track(t).items) {
      if (it.type == mf::ItemType::Image && !it.src.empty()) it.source = mf::macos::sourceFromPath(it.src);
    }
  }
}

- (void)sceneChanged {
  std::string error;
  if (_preview->setScene(_session->doc.scene, kCameraItem, &error) == mf::Result::InvalidArgument && !error.empty()) {
    NSLog(@"[camera] preview: %s", error.c_str());
  }
  _overlay.needsDisplay = YES;
}

- (void)addOverlay:(mf::SceneItem)item {
  if (!item.src.empty()) item.source = mf::macos::sourceFromPath(item.src);
  int t = _session->addOverlay(std::move(item));
  if (t < 0) return NSBeep();
  _sel = {t, 0};
  [self sceneChanged];
}

- (void)deleteSelectedOverlay {
  if (_sel.track <= editor::CaptureSession::kCameraTrack || _sel.track >= _session->doc.tracks()) return;
  [_overlay stopEditing];
  _session->doc.removeTrack(_sel.track);  // an overlay has a track of its own
  _sel = {};
  [self sceneChanged];
}

- (void)sidebarAddItem:(const mf::SceneItem&)item {
  [self addOverlay:item];
}

- (void)sidebarAddFile:(NSString*)path {  // the Music tab: a click picks the music
  [self setMusicFile:path];
}

- (void)sidebarAddFiles:(NSArray<NSString*>*)paths {  // imported under Music
  for (NSString* p in paths) [_pane rememberFile:p];
  if (paths.count) [self setMusicFile:paths.firstObject];
}

- (void)sidebarCollapsedChanged {
  NSView* content = self.window.contentView;
  CGFloat side = _pane.collapsed ? SidebarView.collapsedWidth : kPaneWidth;
  NSRect f = _pane.frame;
  f.size.width = side;
  _pane.frame = f;
  _sideLine.frame = NSMakeRect(side, 0, 1, content.bounds.size.height);
  _stage.frame = NSMakeRect(side + 1, kBarHeight, content.bounds.size.width - side - 1, content.bounds.size.height - kBarHeight);
  [self layoutBar];
  [self layoutStage];
  [self updateContentZOrder];
}

- (void)sidebarDidSelectTab:(NSInteger)tab {
  [self updateContentZOrder];
}

- (void)sidebarRecordVoiceOver {
}

- (void)overlaySelectionChanged {
}

- (void)overlayEdited {
  [self sceneChanged];
}

// Dragged from the pane onto the preview: a sticker, emoji or text, where it's dropped.
- (BOOL)overlayDrop:(id<NSDraggingInfo>)info at:(NSPoint)position {
  SidebarPayload p = _pane.draggedPayload;
  if (!p.hasItem) {
    if (p.file) [self setMusicFile:p.file];
    return p.file != nil;
  }
  p.item.transform.x = mf::Animatable(position.x);
  p.item.transform.y = mf::Animatable(position.y);
  [self addOverlay:p.item];
  return YES;
}

- (void)inspectorEdited {  // the Effects tab
  [self sceneChanged];
}

- (void)inspectorSelectionChanged {
}

- (void)inspectorDeleteSelection {
}

- (void)inspectorShowEffects {
  [_pane showEffectsTab];
  [self updateContentZOrder];
}

// --- Music ------------------------------------------------------------------------------------

- (void)setMusicFile:(NSString*)path {
  if (_session->recording()) return NSBeep();
  mf::MediaInfo info;
  auto demuxer = _platform->createDemuxer();
  if (demuxer->open(mf::macos::sourceFromPath(path.UTF8String), &info) != mf::Result::Ok || !info.audio || !info.audio->supported ||
      info.durationUs <= 0 || ![self loadMusicPlayer:path]) {
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"This file can't be used as music.";
    alert.informativeText = @"Music must be an AAC or MP3 file (such as .m4a or .mp3).";
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
    return;
  }
  bool mic = _session->recordMicrophone();
  _session->setMusic(path.UTF8String, info.durationUs);
  [self updateControls];
  if (mic != _session->recordMicrophone()) [self restartCamera];  // music: the microphone goes off
}

- (BOOL)loadMusicPlayer:(NSString*)path {
  _music = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:path] error:nil];
  [_music prepareToPlay];
  return _music != nil;
}

- (void)removeMusic:(id)sender {
  if (_session->recording()) return;
  bool mic = _session->recordMicrophone();
  _session->clearMusic();
  [_music stop];
  _music = nil;
  [self updateControls];
  if (mic != _session->recordMicrophone()) [self restartCamera];
}

// --- Recording --------------------------------------------------------------------------------

- (void)toggleRecord:(id)sender {
  if (_session->recording()) [self stopRecording];
  else [self startRecording];
}

- (void)startRecording {
  if (!_session->canRecord() || _saving) return NSBeep();
  int w = _frameWidth, h = _frameHeight;
  if (!_cameraRunning || w <= 0) return NSBeep();  // no picture yet
  [NSFileManager.defaultManager createDirectoryAtPath:_folder withIntermediateDirectories:YES attributes:nil error:nil];
  NSString* file = [_folder stringByAppendingPathComponent:[NSString stringWithFormat:@"Segment %03d.mp4", _nextSegment]];
  bool mic = _session->recordMicrophone();
  _session->startSegment();  // takes the effects and mirror now
  mf::Result r = _recorder->start(mf::macos::exportTargetFromPath(file.UTF8String), w, h, kFps, mic ? kMicRate : 0, mic ? kMicChannels : 0);
  if (r != mf::Result::Ok) {
    _session->cancelSegment();
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Recording couldn't start.";
    alert.informativeText = [NSString stringWithFormat:@"%s", mf::toString(r)];
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
    return;
  }
  ++_nextSegment;
  _recordingFile = file;
  if (_music) {
    _music.currentTime = _session->musicPositionUs() / 1e6;
    [_music play];
  }
  [self updateControls];
}

- (void)stopRecording {
  if (!_session->recording() || _saving) return;
  [_music pause];
  _saving = YES;
  NSString* file = _recordingFile;
  __weak CameraWindowController* weak = self;
  _recorder->stop([weak, file](mf::Result r, int64_t durationUs) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [weak segment:file savedWith:r duration:durationUs];
    });
  });
  [self updateControls];
}

- (void)segment:(NSString*)file savedWith:(mf::Result)r duration:(int64_t)durationUs {
  _saving = NO;
  if (r != mf::Result::Ok) {
    _session->cancelSegment();
    [NSFileManager.defaultManager removeItemAtPath:file error:nil];
    if (r != mf::Result::WriteFailed || durationUs > 0) NSLog(@"[camera] segment failed: %s", mf::toString(r));
  } else if (!_session->stopSegment(file.UTF8String, durationUs)) {
    [NSFileManager.defaultManager removeItemAtPath:file error:nil];  // too short to keep
  }
  [self saveJournal];
  [self updateControls];
}

- (void)deleteLast:(id)sender {
  auto last = _session->removeLast();
  if (!last) return;
  [NSFileManager.defaultManager removeItemAtPath:[NSString stringWithUTF8String:last->file.c_str()] error:nil];
  [self saveJournal];
  [self updateControls];
}

// After each segment: what's needed to recover the recording (removed once there's nothing to recover).
- (void)saveJournal {
  NSString* path = [_folder stringByAppendingPathComponent:CameraWindowController.journalName];
  if (_session->segments().empty()) {
    [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    return;
  }
  std::string text = mf::serializeScene(_session->journal());
  [[NSString stringWithUTF8String:text.c_str()] writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

- (void)tick {
  int64_t recording = _session->recording() ? std::max<int64_t>(_recorder->durationUs(), 1) : 0;
  if (_session->recording() && !_saving && _session->maxDurationUs() != editor::CaptureSession::kUnlimited &&
      _session->totalUs() + recording >= _session->maxDurationUs()) {
    [self stopRecording];  // full: stops by itself
  }
  _bar.recordingUs = _saving ? 0 : recording;
  _bar.needsDisplay = YES;
  int64_t total = _session->totalUs() + (_saving ? 0 : recording);
  _time.stringValue = _session->maxDurationUs() == editor::CaptureSession::kUnlimited
                          ? seconds(total)
                          : [NSString stringWithFormat:@"%@ / %@", seconds(total), seconds(_session->maxDurationUs())];
  if (_session->recording() && !_saving) {  // pulses while recording
    _record.alphaValue = 0.65 + 0.35 * std::cos(CACurrentMediaTime() * 4);
  } else {
    _record.alphaValue = 1;
  }
}

- (void)updateControls {
  bool recording = _session->recording() || _saving;
  _record.title = _session->recording() ? @"Stop" : @"Record";
  _record.enabled = _saving ? NO : (_session->recording() || _session->canRecord());
  _deleteLast.enabled = !recording && !_session->segments().empty();
  _done.enabled = !recording && !_session->segments().empty();
  _cancel.enabled = !recording;
  _cameraMenu.enabled = !recording && _devices.size() > 1;
  _mirror.enabled = !recording;
  _mic.enabled = !recording;
  _mirror.state = _session->mirror() ? NSControlStateValueOn : NSControlStateValueOff;
  _mic.state = _session->recordMicrophone() ? NSControlStateValueOn : NSControlStateValueOff;
  _musicLabel.stringValue = _session->hasMusic()
                                ? [@"♪ " stringByAppendingString:[NSString stringWithUTF8String:_session->musicSrc().c_str()].lastPathComponent]
                                : @"No music (see Music)";
  _musicLabel.textColor = _session->hasMusic() ? NSColor.labelColor : NSColor.secondaryLabelColor;
  _musicRemove.hidden = !_session->hasMusic();
  _musicRemove.enabled = !recording;
  _effects.readOnly = recording;  // effects apply from the next segment: not while one records
}

- (void)setMaxDuration:(int64_t)maxDurationUs {
  _session->setMaxDurationUs(maxDurationUs);
  [self updateControls];
}

// --- Finishing --------------------------------------------------------------------------------

- (void)done:(id)sender {
  if (_session->recording() || _saving || _session->segments().empty()) return;
  if (![self.delegate cameraWindowAdd:*_session]) return;
  _finished = YES;
  [NSFileManager.defaultManager removeItemAtPath:[_folder stringByAppendingPathComponent:CameraWindowController.journalName] error:nil];
  [self.window close];
}

- (void)cancel:(id)sender {
  [self.window performClose:sender];
}

- (void)discard {
  for (const auto& s : _session->segments()) {
    [NSFileManager.defaultManager removeItemAtPath:[NSString stringWithUTF8String:s.file.c_str()] error:nil];
  }
  [NSFileManager.defaultManager removeItemAtPath:[_folder stringByAppendingPathComponent:CameraWindowController.journalName] error:nil];
  NSArray* left = [NSFileManager.defaultManager contentsOfDirectoryAtPath:_folder error:nil];
  if (left && left.count == 0) [NSFileManager.defaultManager removeItemAtPath:_folder error:nil];
}

- (BOOL)windowShouldClose:(NSWindow*)window {
  if (_finished) return YES;
  if (_session->recording() || _saving) {
    [self stopRecording];  // stops first; closing again asks about what was recorded
    return NO;
  }
  if (_session->segments().empty()) {
    [self discard];
    return YES;
  }
  NSAlert* alert = [NSAlert new];
  alert.messageText = [NSString stringWithFormat:@"Add the %zu recorded segment%s to the project?", _session->segments().size(),
                                                 _session->segments().size() == 1 ? "" : "s"];
  alert.informativeText = @"Discarding deletes them.";
  [alert addButtonWithTitle:@"Add to Project"];
  [alert addButtonWithTitle:@"Discard"];
  [alert addButtonWithTitle:@"Keep Recording"];
  __weak CameraWindowController* weak = self;
  [alert beginSheetModalForWindow:window
                completionHandler:^(NSModalResponse response) {
                  CameraWindowController* s = weak;
                  if (!s) return;
                  if (response == NSAlertFirstButtonReturn) {
                    [s done:nil];
                  } else if (response == NSAlertSecondButtonReturn) {
                    [s discard];
                    s->_finished = YES;
                    [s.window close];
                  }
                }];
  return NO;
}

- (void)windowWillClose:(NSNotification*)note {
  _finished = YES;
  [_timer invalidate];
  _timer = nil;
  if (_camera) _camera->stop();  // no frame reaches the preview or the recorder after this
  [_music stop];
  [self.delegate cameraWindowClosed];
}

- (void)dealloc {
  if (_camera) _camera->stop();
}

@end
