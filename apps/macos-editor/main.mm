// A small video editor on the media framework. Left, the whole height: tabs of things to add
// (video, images, stickers, emojis, text, audio). Right of it: the preview of the scene with
// play/pause and the time, then the timeline (tracks and their items). The properties of the
// selection open in a floating window from its "…" button. The document is an mf::Scene; after
// each edit the player reopens it at the same time (the player has no live scene update), a
// moment after the last change. A voice-over records the microphone while the video plays.
//
//   mf_editor [file ...]   adds the files (video, image, audio) to the timeline; a .json opens

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "document.h"
#include "export_view.h"
#include "inspector_view.h"
#include "mf/macos.h"
#include "mf/player.h"
#include "preview_overlay.h"
#include "sidebar_view.h"
#include "timeline_view.h"
#include "voice_recorder.h"
#include "waveform.h"

// The player draws into its CAMetalLayer. Resized, the last frame keeps its aspect ratio (never
// stretched) until `resized` has it drawn again at the new size.
@interface PreviewView : NSView
@property(nonatomic, copy) void (^resized)(void);
@end

@implementation PreviewView
- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.wantsLayer = YES;
    self.layerContentsPlacement = NSViewLayerContentsPlacementScaleProportionallyToFit;  // AppKit sets the layer's gravity from it
  }
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
  if (self.resized) self.resized();
}
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  [self updateDrawableSize];
}
@end

// A strip over the left pane's right edge: dragging it resizes the pane.
@interface PaneHandle : NSView
@property(nonatomic, copy) void (^dragged)(CGFloat x);  // in the superview's coordinates
@end

@implementation PaneHandle
- (void)resetCursorRects {
  [self addCursorRect:self.bounds cursor:NSCursor.resizeLeftRightCursor];
}
- (void)drawRect:(NSRect)dirty {
  NSRect grip = NSMakeRect(NSMidX(self.bounds) - 2, NSMidY(self.bounds) - 20, 4, 40);
  [NSColor.tertiaryLabelColor setFill];
  [[NSBezierPath bezierPathWithRoundedRect:grip xRadius:2 yRadius:2] fill];
}
- (void)mouseDown:(NSEvent*)event {
}
- (void)mouseDragged:(NSEvent*)event {
  self.dragged([self.superview convertPoint:event.locationInWindow fromView:nil].x);
}
@end

@interface Editor : NSObject <NSApplicationDelegate, TimelineDelegate, InspectorDelegate, SidebarDelegate,
                                PreviewOverlayDelegate>
- (void)player:(int)generation state:(mf::State)state;
- (void)player:(int)generation failed:(NSString*)reason;
- (void)player:(int)generation seekCompleted:(int64_t)ptsUs;
@end

// Callbacks arrive on internal threads: hop to the main thread, never wait on it. Each player
// has its own listener, so a late callback from a player already replaced is ignored.
class Listener : public mf::PlayerListener {
 public:
  Listener(Editor* e, int generation) : editor_(e), generation_(generation) {}
  void onStateChanged(mf::State s) override { post(^(Editor* e, int g) { [e player:g state:s]; }); }
  void onError(mf::Result r, const std::string& reason) override {
    NSString* text = [NSString stringWithFormat:@"%s: %s", mf::toString(r), reason.c_str()];
    post(^(Editor* e, int g) { [e player:g failed:text]; });
  }
  void onWarning(mf::Warning w, const std::string& reason) override {
    NSString* text = [NSString stringWithFormat:@"%s: %s", mf::toString(w), reason.c_str()];
    post(^(Editor* e, int g) { [e player:g failed:text]; });
  }
  void onSeekCompleted(int64_t pts) override { post(^(Editor* e, int g) { [e player:g seekCompleted:pts]; }); }

 private:
  void post(void (^block)(Editor*, int)) {
    __weak Editor* weak = editor_;
    int g = generation_;
    dispatch_async(dispatch_get_main_queue(), ^{
      if (Editor* e = weak) block(e, g);
    });
  }
  __weak Editor* editor_;
  int generation_;
};

// The left pane, open: its width at start (the properties view fits in it), and the range its
// handle drags it in. Dragged narrower than kSidebarShut, it collapses to its tabs.
static constexpr CGFloat kSidebarWidth = 380, kSidebarMin = 280, kSidebarMax = 640, kSidebarShut = 180;

// The editor's part of a document's metadata: {"editor": {"playhead": seconds}}.
static int64_t savedPlayheadUs(const std::string& metadata) {
  NSData* data = [NSData dataWithBytes:metadata.data() length:metadata.size()];
  id doc = metadata.empty() ? nil : [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingFragmentsAllowed error:nil];
  id editor = [doc isKindOfClass:NSDictionary.class] ? doc[@"editor"] : nil;
  id playhead = [editor isKindOfClass:NSDictionary.class] ? editor[@"playhead"] : nil;
  if (![playhead isKindOfClass:NSNumber.class] || !std::isfinite([playhead doubleValue])) return -1;
  return std::llround([playhead doubleValue] * 1e6);
}

// The metadata with the editor's playhead set, keeping everything else in it.
static std::string withPlayhead(const std::string& metadata, int64_t us) {
  NSData* data = [NSData dataWithBytes:metadata.data() length:metadata.size()];
  id doc = metadata.empty() ? nil : [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers | NSJSONReadingFragmentsAllowed error:nil];
  NSMutableDictionary* all = [doc isKindOfClass:NSMutableDictionary.class] ? doc : [NSMutableDictionary dictionary];
  NSMutableDictionary* editor = [all[@"editor"] isKindOfClass:NSMutableDictionary.class] ? all[@"editor"] : [NSMutableDictionary dictionary];
  editor[@"playhead"] = @(us / 1e6);
  all[@"editor"] = editor;
  NSData* out = [NSJSONSerialization dataWithJSONObject:all options:NSJSONWritingSortedKeys error:nil];
  return out ? std::string((const char*)out.bytes, out.length) : metadata;
}

static NSString* timeString(int64_t us) {
  int64_t tenths = us / 100000;
  return [NSString stringWithFormat:@"%lld:%02lld.%lld", tenths / 600, tenths / 10 % 60, tenths % 10];
}

@implementation Editor {
  NSArray<NSString*>* _initialFiles;
  editor::Document _doc;
  editor::Selection _sel;

  NSWindow* _window;
  SidebarView* _sidebar;
  PreviewView* _preview;
  PreviewOverlay* _overlay;  // over the preview: the selection, and editing it there
  std::map<std::string, NSSize> _naturalSizes;  // by src: a video's or image's pixel size
  std::map<std::string, std::vector<float>> _peaks;  // by src: an audio file's waveform
  std::set<std::string> _peaksLoading;
  std::map<std::string, std::string> _audioProblems;  // by src: why a video's audio can't play, or ""
  NSTextField* _placeholder;
  NSButton* _playButton;
  NSTextField* _timeLabel;
  NSTextField* _status;
  TimelineView* _timeline;
  InspectorView* _inspector;
  NSPanel* _properties;  // floating; holds _inspector
  InspectorView* _projectInspector;  // in the left pane's Project tab
  InspectorView* _transitionInspector;  // in the left pane while a join is selected
  ExportView* _exportView;              // the Export tab
  editor::Selection _projectSel;     // always the project, except just after it adds a track
  NSBox* _sideLine;
  PaneHandle* _sideHandle;
  CGFloat _sidebarWidth;  // when open
  NSSplitView* _split;

  std::unique_ptr<mf::PlatformFactory> _platform;
  std::unique_ptr<Listener> _listener;
  std::unique_ptr<mf::Player> _player;
  int _generation;
  BOOL _reloadPending;
  int _reloadToken;  // the reload waiting, if any: a newer token cancels it
  NSURL* _fileURL;  // the scene document this was opened from or saved to; nil: never saved
  BOOL _redrawPending;
  int64_t _playheadUs;
  int64_t _seekOnReadyUs;  // -1: none; else where the playhead is while the player opens
  int64_t _openedAtUs;     // the frame the player was opened at (its first frame)
  BOOL _playAfterSeek;

  VoiceRecorder* _recorder;  // a voice-over
  NSURL* _voiceURL;          // the file it's recorded into
  int64_t _voiceStartUs;     // the playhead when it started: where it goes on the timeline
  NSTimer* _voiceTimer;      // shows the recording as it goes, every 1/10 s
  std::vector<float> _voiceLevels;  // the input level of each 1/10 s
}

- (instancetype)initWithFiles:(NSArray<NSString*>*)files {
  if ((self = [super init])) {
    _initialFiles = files;
    _platform = mf::macos::createPlatform();
    _seekOnReadyUs = -1;
  }
  return self;
}

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  [self buildMenu];
  [self buildWindow];
  // A document opens as saved; loose media makes a new, unsaved project, one file after another.
  NSMutableArray<NSString*>* media = [NSMutableArray array];
  for (NSString* path in _initialFiles) {
    if ([path.pathExtension caseInsensitiveCompare:@"json"] == NSOrderedSame) [self openURL:[NSURL fileURLWithPath:path]];
    else [media addObject:path];
  }
  [self addFiles:media];
  _sel = {};
  [self structureChanged];
  _window.documentEdited = media.count > 0;
  [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick) userInfo:nil repeats:YES];
}

// Unsaved changes are offered a save before quitting.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
  return [self keepChanges] ? NSTerminateNow : NSTerminateCancel;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)app {
  return YES;
}

- (void)applicationWillTerminate:(NSNotification*)note {
  [_exportView cancel];
  if (_player) _player->shutdown();
}

// --- Window ---------------------------------------------------------------------------------

- (void)buildMenu {
  NSMenu* bar = [NSMenu new];
  NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
  appItem.submenu = [NSMenu new];
  [appItem.submenu addItemWithTitle:@"Quit Media Editor" action:@selector(terminate:) keyEquivalent:@"q"];
  NSMenuItem* fileItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
  fileItem.submenu = [[NSMenu alloc] initWithTitle:@"File"];
  [fileItem.submenu addItemWithTitle:@"Open…" action:@selector(openDocument:) keyEquivalent:@"o"];
  [fileItem.submenu addItemWithTitle:@"Close" action:@selector(closeDocument:) keyEquivalent:@"w"];
  [fileItem.submenu addItemWithTitle:@"Save" action:@selector(saveDocument:) keyEquivalent:@"s"];
  [fileItem.submenu addItemWithTitle:@"Save As…" action:@selector(saveDocumentAs:) keyEquivalent:@"S"];  // ⇧⌘S
  [fileItem.submenu addItem:[NSMenuItem separatorItem]];
  [fileItem.submenu addItemWithTitle:@"Add Media…" action:@selector(addMedia:) keyEquivalent:@"i"];
  // Standard editing commands, so that text fields take copy and paste.
  NSMenuItem* editItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
  editItem.submenu = [[NSMenu alloc] initWithTitle:@"Edit"];
  [editItem.submenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [editItem.submenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [editItem.submenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [editItem.submenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
  NSApp.mainMenu = bar;
}

- (void)buildWindow {
  _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1320, 880)
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                  NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
  [self showFileName];
  _window.minSize = NSMakeSize(900, 600);
  NSView* content = _window.contentView;
  NSSize size = content.bounds.size;

  // Left, the whole height: the tabs of things to add, and the project's settings.
  const CGFloat side = kSidebarWidth;
  _sidebar = [[SidebarView alloc] initWithFrame:NSMakeRect(0, 0, side, size.height)];
  _sidebar.autoresizingMask = NSViewHeightSizable;
  _sidebar.delegate = self;
  [content addSubview:_sidebar];
  _projectInspector = [[InspectorView alloc] initWithFrame:NSMakeRect(0, 0, 300, 500) document:&_doc selection:&_projectSel];
  _projectInspector.delegate = self;
  [_sidebar setProjectView:_projectInspector];
  _exportView = [[ExportView alloc] initWithFrame:NSMakeRect(0, 0, 300, 500) document:&_doc platform:_platform.get()];
  [_sidebar setExportView:_exportView];
  _transitionInspector = [[InspectorView alloc] initWithFrame:NSMakeRect(0, 0, 300, 500) document:&_doc selection:&_sel];
  _transitionInspector.delegate = self;
  _sideLine = [[NSBox alloc] initWithFrame:NSMakeRect(side, 0, 1, size.height)];
  _sideLine.boxType = NSBoxSeparator;
  _sideLine.autoresizingMask = NSViewHeightSizable;
  [content addSubview:_sideLine];
  _sidebarWidth = side;

  // Right of it: the preview above the timeline, with a draggable divider.
  const CGFloat width = size.width - side - 1;
  NSSplitView* split = _split = [[NSSplitView alloc] initWithFrame:NSMakeRect(side + 1, 0, width, size.height)];
  split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  split.dividerStyle = NSSplitViewDividerStyleThin;
  [content addSubview:split];

  // Top: the preview, then play/pause and the time.
  NSView* top = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 500)];
  const CGFloat mid = width / 2;
  _status = [NSTextField labelWithString:@""];
  _status.frame = NSMakeRect(12, 500 - 26, width - 24, 18);
  _status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
  _status.alignment = NSTextAlignmentRight;
  _status.lineBreakMode = NSLineBreakByTruncatingTail;
  [top addSubview:_status];

  _preview = [[PreviewView alloc] initWithFrame:NSMakeRect(0, 44, width, 500 - 44 - 32)];
  _preview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [top addSubview:_preview];
  _overlay = [[PreviewOverlay alloc] initWithFrame:_preview.frame document:&_doc selection:&_sel];
  _overlay.autoresizingMask = _preview.autoresizingMask;
  _overlay.delegate = self;
  __weak Editor* weakSelf = self;
  _overlay.naturalSize = ^NSSize(const mf::SceneItem& it) {
    return weakSelf ? [weakSelf naturalSizeOf:it] : NSZeroSize;
  };
  [top addSubview:_overlay];
  _preview.resized = ^{
    [weakSelf redrawPreview];
  };
  // What can be dropped on the preview and the timeline: the left pane's things, and files.
  [_overlay registerForDraggedTypes:@[ SidebarDragType, NSPasteboardTypeFileURL ]];
  _placeholder = [NSTextField labelWithString:@"Add video, images, text or audio to start"];
  _placeholder.textColor = NSColor.secondaryLabelColor;
  _placeholder.alignment = NSTextAlignmentCenter;
  _placeholder.frame = NSMakeRect(0, 240, width, 20);
  _placeholder.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin | NSViewMaxYMargin;
  [top addSubview:_placeholder];

  _playButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"play.fill" accessibilityDescription:@"Play"]
                                   target:self
                                   action:@selector(togglePlay:)];
  _playButton.bezelStyle = NSBezelStyleCircular;
  // Play/pause and the time move together, centered under the preview.
  NSView* transport = [[NSView alloc] initWithFrame:NSMakeRect(mid - 18, 4, 36 + 10 + 200, 36)];
  transport.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin;
  [top addSubview:transport];
  _playButton.frame = NSMakeRect(0, 0, 36, 36);
  [transport addSubview:_playButton];
  _timeLabel = [NSTextField labelWithString:@"0:00.0 / 0:00.0"];
  _timeLabel.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightMedium];
  _timeLabel.frame = NSMakeRect(46, 9, 200, 18);
  [transport addSubview:_timeLabel];

  // Bottom: the timeline, with the zoom above it.
  NSView* bottom = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 380)];
  NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, width, 380 - 30)];
  scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  scroll.hasHorizontalScroller = YES;
  scroll.hasVerticalScroller = YES;
  _timeline = [[TimelineView alloc] initWithDocument:&_doc selection:&_sel];
  _timeline.delegate = self;
  [_timeline registerForDraggedTypes:@[ SidebarDragType, NSPasteboardTypeFileURL ]];
  __weak Editor* weakEditor = self;
  _timeline.peaks = ^const std::vector<float>*(const mf::SceneItem& it) {
    return weakEditor ? [weakEditor peaksOf:it] : nullptr;
  };
  scroll.documentView = _timeline;
  [bottom addSubview:scroll];
  NSImageView* zoomIcon = [NSImageView imageViewWithImage:[NSImage imageWithSystemSymbolName:@"plus.magnifyingglass"
                                                                    accessibilityDescription:@"Zoom"]];
  zoomIcon.frame = NSMakeRect(width - 190, 380 - 25, 20, 20);
  zoomIcon.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
  [bottom addSubview:zoomIcon];
  NSSlider* zoom = [NSSlider sliderWithValue:std::log(40.0) minValue:std::log(5.0) maxValue:std::log(400.0) target:self
                                      action:@selector(zoomChanged:)];
  zoom.controlSize = NSControlSizeSmall;
  zoom.frame = NSMakeRect(width - 165, 380 - 25, 150, 20);
  zoom.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
  [bottom addSubview:zoom];

  // The properties of the selection, in a floating window shown by the "…" buttons.
  _inspector = [[InspectorView alloc] initWithFrame:NSMakeRect(0, 0, 360, 560) document:&_doc selection:&_sel];
  _inspector.delegate = self;
  __weak Editor* weakInspectorOwner = self;
  _inspector.audioProblem = ^NSString*(const mf::SceneItem& it) {
    return weakInspectorOwner ? [weakInspectorOwner audioProblemOf:it] : nil;
  };
  _properties = [[NSPanel alloc] initWithContentRect:_inspector.frame
                                           styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                                                     NSWindowStyleMaskUtilityWindow
                                             backing:NSBackingStoreBuffered
                                               defer:YES];
  _properties.floatingPanel = YES;
  _properties.releasedWhenClosed = NO;
  _properties.minSize = NSMakeSize(340, 240);
  _properties.contentView = _inspector;

  [split addArrangedSubview:top];
  [split addArrangedSubview:bottom];
  [_window center];
  [_window makeKeyAndOrderFront:nil];
  [split setPosition:500 ofDividerAtIndex:0];

  __weak Editor* weak = self;
  _sideHandle = [[PaneHandle alloc] initWithFrame:NSMakeRect(side - 4, 0, 9, size.height)];
  _sideHandle.autoresizingMask = NSViewHeightSizable;
  _sideHandle.toolTip = @"Drag to resize the panel";
  _sideHandle.dragged = ^(CGFloat x) {
    [weak dragSidebarTo:x];
  };
  [content addSubview:_sideHandle];  // last: over the pane's edge and the preview's
  [NSApp activateIgnoringOtherApps:YES];
}

// --- Properties window ----------------------------------------------------------------------

- (void)updatePropertiesTitle {
  static NSString* const kinds[] = {@"Video", @"Image", @"Text", @"Color", @"Audio"};
  if (_sel.track < 0) _properties.title = @"Project";
  else if (_sel.transition) _properties.title = @"Transition";
  else if (_sel.item < 0) _properties.title = _doc.track(_sel.track).video ? @"Video Track" : @"Audio Track";
  else _properties.title = kinds[int(_doc.item(_sel.track, _sel.item).type)];
}

// Shows the selection's properties next to `anchor` (screen coordinates), inside the screen.
- (void)showPropertiesNear:(NSRect)anchor {
  [_inspector rebuild];
  [self updatePropertiesTitle];
  NSRect frame = _properties.frame;
  NSRect screen = (_window.screen ?: NSScreen.mainScreen).visibleFrame;
  frame.origin.x = NSMaxX(anchor) + 8;
  if (NSMaxX(frame) > NSMaxX(screen)) frame.origin.x = NSMinX(anchor) - 8 - frame.size.width;  // no room at the right
  frame.origin.y = NSMaxY(anchor) - frame.size.height / 2;
  frame.origin.x = std::clamp(frame.origin.x, NSMinX(screen), std::max(NSMinX(screen), NSMaxX(screen) - frame.size.width));
  frame.origin.y = std::clamp(frame.origin.y, NSMinY(screen), std::max(NSMinY(screen), NSMaxY(screen) - frame.size.height));
  [_properties setFrame:frame display:NO];
  [_properties makeKeyAndOrderFront:nil];
  [_properties makeFirstResponder:nil];  // no field starts focused, so no key edits one by accident
}

- (void)timelineShowProperties:(NSRect)button {
  [self showPropertiesNear:[_window convertRectToScreen:[_timeline convertRect:button toView:nil]]];
}

// The left pane opened or collapsed: the preview and timeline take the rest of the width.
- (void)sidebarCollapsedChanged {
  CGFloat side = _sidebar.collapsed ? SidebarView.collapsedWidth : _sidebarWidth;
  NSSize size = _window.contentView.bounds.size;
  _sidebar.frame = NSMakeRect(0, 0, side, size.height);
  _sideLine.frame = NSMakeRect(side, 0, 1, size.height);
  _sideHandle.frame = NSMakeRect(side - 4, 0, 9, size.height);
  _split.frame = NSMakeRect(side + 1, 0, size.width - side - 1, size.height);
  [_window invalidateCursorRectsForView:_sideHandle];
}

// The pane's handle dragged to x: that width, within its range and leaving room for the preview
// and timeline; below kSidebarShut the pane collapses, and dragging out again opens it.
- (void)dragSidebarTo:(CGFloat)x {
  if (x < kSidebarShut) {
    if (!_sidebar.collapsed) _sidebar.collapsed = YES;  // lays out
    return;
  }
  CGFloat widest = std::max(kSidebarMin, std::min(kSidebarMax, _window.contentView.bounds.size.width - 500));
  _sidebarWidth = std::clamp(x, kSidebarMin, widest);
  if (_sidebar.collapsed) _sidebar.collapsed = NO;  // lays out
  else [self sidebarCollapsedChanged];
}

- (void)zoomChanged:(NSSlider*)slider {
  _timeline.pixelsPerSecond = std::exp(slider.doubleValue);
}

- (void)showStatus:(NSString*)text error:(BOOL)error {
  _status.stringValue = text;
  _status.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

// --- Voice-over -----------------------------------------------------------------------------

- (void)sidebarRecordVoiceOver {
  if (_recorder.recording) [self stopVoiceOver];
  else [self startVoiceOver];
}

// Where a recording goes: next to the project when it's saved (so it moves with it), else in
// ~/Movies/Media Editor.
- (NSURL*)newVoiceOverURL {
  NSURL* folder = _fileURL ? _fileURL.URLByDeletingLastPathComponent
                           : [[NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory inDomains:NSUserDomainMask].firstObject
                                 URLByAppendingPathComponent:@"Media Editor"];
  NSDateFormatter* format = [NSDateFormatter new];
  format.dateFormat = @"yyyy-MM-dd 'at' HH.mm.ss";
  return [folder URLByAppendingPathComponent:[NSString stringWithFormat:@"Voice-over %@.m4a", [format stringFromDate:NSDate.date]]];
}

- (void)startVoiceOver {
  if (!_recorder) _recorder = [VoiceRecorder new];
  [_recorder requestAccess:^(BOOL granted) {
    if (granted) return [self beginVoiceOver];
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"The microphone isn't available to Media Editor.";
    alert.informativeText = @"Allow it in System Settings › Privacy & Security › Microphone (for the app the editor runs from, "
                            @"such as Terminal), then record again.";
    [alert runModal];
  }];
}

// Records from the playhead, playing the video along so the voice can follow it.
- (void)beginVoiceOver {
  NSURL* url = [self newVoiceOverURL];
  NSString* error = nil;
  if (![_recorder startInto:url error:&error]) return [self showStatus:[@"Can't record: " stringByAppendingString:error] error:YES];
  _voiceURL = url;
  _voiceStartUs = _playheadUs;
  if (_player && _player->state() != mf::State::Play) [self togglePlay:nil];
  _sidebar.recording = YES;
  _voiceLevels.clear();
  __weak Editor* weak = self;
  _voiceTimer = [NSTimer scheduledTimerWithTimeInterval:0.1
                                                repeats:YES
                                                  block:^(NSTimer*) {
                                                    [weak showRecordingProgress];
                                                  }];
  [self showRecordingProgress];
}

// The recording so far, in the timeline (a growing block with its levels) and the status line.
- (void)showRecordingProgress {
  int64_t us = int64_t(_recorder.seconds * 1e6);
  if (_recorder.recording && _voiceLevels.size() < size_t(us / 100000 + 1)) _voiceLevels.push_back(_recorder.level);
  [_timeline showRecordingFrom:_voiceStartUs length:us levels:_voiceLevels];
  [self showStatus:[NSString stringWithFormat:@"● Recording voice-over  %@", timeString(us)] error:YES];
}

- (void)stopVoiceOver {
  if (!_recorder.recording) return;
  [_voiceTimer invalidate];
  _voiceTimer = nil;
  NSTimeInterval seconds = [_recorder stop];
  if (_player && _player->state() == mf::State::Play) _player->pause();
  _sidebar.recording = NO;
  [_timeline hideRecording];
  [self showStatus:@"" error:NO];
  if (seconds < 0.2) {  // a click, not a recording
    [[NSFileManager defaultManager] removeItemAtURL:_voiceURL error:nil];
    return [self showStatus:@"The voice-over was too short to keep." error:NO];
  }
  [self addVoiceOver:_voiceURL at:_voiceStartUs];
}

// A recording onto the timeline at `startUs`, on the lowest audio track free for it (or a new
// one), selected.
- (void)addVoiceOver:(NSURL*)url at:(int64_t)startUs {
  mf::SceneItem it;
  int64_t length;
  if (![self item:&it fromFile:url.path lengthUs:&length]) return;  // it says why
  int t = _doc.freeTrack(false, startUs, startUs + it.durationUs);
  if (t < 0) t = _doc.addTrack(false);
  if (t < 0) return [self showStatus:@"At most 16 tracks" error:YES];
  int k = _doc.insertItem(t, std::move(it), startUs, length);
  _sel = {t, k};
  [self structureChanged];
  [self showStatus:[NSString stringWithFormat:@"Voice-over added: %@ (%.1f s)", url.lastPathComponent, _doc.item(t, k).durationUs / 1e6]
             error:NO];
}

// --- Saving and opening ---------------------------------------------------------------------

- (void)showFileName {
  _window.title = _fileURL ? _fileURL.lastPathComponent : @"Untitled";
  _window.representedURL = _fileURL;  // the title's proxy icon
}

// Before the scene is replaced or the app quits: with unsaved changes, asks to save them.
// NO: the user cancelled (or the save didn't happen).
- (BOOL)keepChanges {
  [self stopVoiceOver];  // a recording in progress is kept: it goes on the timeline, a change to save
  if (!_window.documentEdited || _doc.empty()) return YES;
  NSAlert* alert = [NSAlert new];
  alert.messageText = [NSString stringWithFormat:@"Save the changes to “%@”?", _fileURL ? _fileURL.lastPathComponent : @"Untitled"];
  alert.informativeText = @"Your changes will be lost if you don't save them.";
  [alert addButtonWithTitle:@"Save"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert addButtonWithTitle:@"Don't Save"];
  switch ([alert runModal]) {
    case NSAlertFirstButtonReturn: return [self save:NO];
    case NSAlertThirdButtonReturn: return YES;
    default: return NO;
  }
}

- (void)saveDocument:(id)sender {
  [self save:NO];
}

- (void)saveDocumentAs:(id)sender {
  [self save:YES];
}

// Writes the scene document (scene_graph_spec.md): media paths inside the document's folder are
// written relative to it, so the folder can move as a whole; others stay absolute.
- (BOOL)save:(BOOL)askWhere {
  NSURL* url = _fileURL;
  if (askWhere || !url) {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.allowedContentTypes = @[ UTTypeJSON ];
    panel.nameFieldStringValue = _fileURL ? _fileURL.lastPathComponent : @"Untitled.json";
    if ([panel runModal] != NSModalResponseOK) return NO;
    url = panel.URL;
  }
  // Compared with symlinks resolved: /tmp and /private/tmp are one folder.
  auto resolved = [](NSString* path) { return std::string([NSURL fileURLWithPath:path].URLByResolvingSymlinksInPath.path.UTF8String); };
  std::string folder = resolved(url.URLByDeletingLastPathComponent.path) + "/";
  _doc.scene.metadata = withPlayhead(_doc.scene.metadata, _playheadUs);
  std::string text = mf::serializeScene(_doc.scene, [&](const std::string& src) {
    std::string full = resolved([NSString stringWithUTF8String:src.c_str()]);
    return full.compare(0, folder.size(), folder) == 0 ? full.substr(folder.size()) : src;
  });
  NSError* error = nil;
  if (![[NSData dataWithBytes:text.data() length:text.size()] writeToURL:url options:NSDataWritingAtomic error:&error]) {
    [[NSAlert alertWithError:error] runModal];
    return NO;
  }
  _fileURL = url;
  _window.documentEdited = NO;
  [self showFileName];
  [self showStatus:[NSString stringWithFormat:@"Saved %@", url.lastPathComponent] error:NO];
  return YES;
}

// Closes the project (offering to save its changes) and starts a new, empty one in its place. The
// files listed in the left pane stay listed.
- (void)closeDocument:(id)sender {
  if (![self keepChanges]) return;
  [_properties orderOut:nil];
  _doc = editor::Document();
  _fileURL = nil;
  _sel = {};
  [self movePlayhead:0];
  [self structureChanged];
  _window.documentEdited = NO;
  [self showFileName];
  [self showStatus:@"" error:NO];
}

- (void)openDocument:(id)sender {
  if (![self keepChanges]) return;
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ UTTypeJSON ];
  if ([panel runModal] == NSModalResponseOK) [self openURL:panel.URL];
}

// Reads a scene document; `src` paths are relative to its folder unless absolute. Each item's
// src becomes the full path, video and audio files are opened for their lengths (and, for an
// item "to the end of the file", its duration), and the media is listed in the left pane.
- (void)openURL:(NSURL*)url {
  mf::Scene scene;
  std::string error;
  if (mf::macos::loadScene(url.path.UTF8String, &scene, &error) != mf::Result::Ok) {
    NSAlert* alert = [NSAlert new];
    alert.messageText = [NSString stringWithFormat:@"“%@” can't be opened.", url.lastPathComponent];
    alert.informativeText = [NSString stringWithUTF8String:error.c_str()];
    [alert runModal];
    return;
  }
  _doc.load(std::move(scene));
  NSMutableArray<NSString*>* problems = [NSMutableArray array];
  for (mf::SceneTrack& track : _doc.scene.tracks) {
    for (mf::SceneItem& it : track.items) {
      if (it.src.empty()) continue;
      NSString* path = ((__bridge NSURL*)it.source.native.get()).path;
      it.src = path.UTF8String;
      if (it.type != mf::ItemType::Video && it.type != mf::ItemType::Audio) {
        [_sidebar rememberFile:path];
        continue;
      }
      mf::MediaInfo info;
      if (_platform->createDemuxer()->open(it.source, &info) != mf::Result::Ok || info.durationUs <= 0) {
        [problems addObject:path.lastPathComponent];
        continue;
      }
      _doc.setLength(it.id, info.durationUs);
      if (it.toEnd()) it.durationUs = std::max<int64_t>(editor::Document::kMinDurationUs, std::llround(double(info.durationUs - it.inUs) / it.speed));
      [_sidebar rememberFile:path];
    }
  }
  _fileURL = url;
  _sel = {};
  if (_player) {  // not seeking this one to the saved playhead: it still shows the last project
    _player->shutdown();
    _player.reset();
  }
  [self movePlayhead:std::max<int64_t>(0, savedPlayheadUs(_doc.scene.metadata))];
  [self structureChanged];
  [self reloadNow];  // opens at the saved playhead
  _window.documentEdited = NO;
  [self showFileName];
  [self showStatus:problems.count ? [NSString stringWithFormat:@"Can't open %@", [problems componentsJoinedByString:@", "]] : @""
             error:problems.count > 0];
}

// --- Adding items ---------------------------------------------------------------------------

// Where a new item goes with its left edge at `at`: on a track free for its whole length there,
// so nothing moves. A picture goes on the highest video track above every track in use then, so
// it shows in front; sound on the lowest free audio track. Else on a new track (-1: 16 already).
- (int)trackForNew:(const mf::SceneItem&)it at:(int64_t)at {
  int64_t end = at + std::max<int64_t>(1, it.durationUs);
  if (it.type == mf::ItemType::Audio) {
    int t = _doc.freeTrack(false, at, end);
    return t >= 0 ? t : _doc.addTrack(false);
  }
  int free = -1;
  for (int t = _doc.tracks() - 1; t >= 0; --t) {
    if (!_doc.track(t).video) continue;
    bool busy = std::any_of(_doc.track(t).items.begin(), _doc.track(t).items.end(),
                            [&](const mf::SceneItem& other) { return other.startUs < end && other.endUs() > at; });
    if (busy) break;
    free = t;
  }
  return free >= 0 ? free : _doc.addTrack(true);
}

// Adds the item with its left edge at `at` (see trackForNew) and selects it: at the playhead, it
// shows in the preview at once. Returns where it ends, or -1 when it can't go in.
- (int64_t)place:(mf::SceneItem)item at:(int64_t)at lengthUs:(int64_t)length {
  int t = [self trackForNew:item at:at];
  if (t < 0) {
    [self showStatus:@"At most 16 tracks" error:YES];
    return -1;
  }
  int k = _doc.insertItem(t, std::move(item), at, length);
  _sel = {t, k};
  [self structureChanged];
  // Shown at once: drawn over the preview until the reopened player's frame has it.
  _overlay.provisional = YES;
  [self reloadNow];
  return _doc.item(t, k).endUs();
}

- (void)sidebarAddFile:(NSString*)path {
  [self addFile:path];
}

- (void)sidebarAddFiles:(NSArray<NSString*>*)paths {
  [self addFiles:paths];
}

- (void)sidebarAddItem:(const mf::SceneItem&)item {
  mf::SceneItem it = item;
  if (!it.src.empty()) it.source = mf::macos::sourceFromPath(it.src);  // stickers: a PNG file
  [self place:std::move(it) at:_playheadUs lengthUs:0];
}

- (void)addMedia:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ UTTypeMovie, UTTypeImage, UTTypeAudio ];
  panel.allowsMultipleSelection = YES;
  if ([panel runModal] != NSModalResponseOK) return;
  NSMutableArray<NSString*>* paths = [NSMutableArray array];
  for (NSURL* url in panel.URLs) [paths addObject:url.path];
  [self addFiles:paths];
}

- (void)addFile:(NSString*)path {
  [self addFiles:@[ path ]];
}

// Files from the playhead: the first starts there, each next one where the one before ends.
- (void)addFiles:(NSArray<NSString*>*)paths {
  int64_t at = _playheadUs;
  for (NSString* path in paths) {
    mf::SceneItem it;
    int64_t length;
    if (![self item:&it fromFile:path lengthUs:&length]) continue;  // it says why
    int64_t end = [self place:std::move(it) at:at lengthUs:length];
    if (end < 0) break;
    at = end;
  }
}

// A file as an item, listed in the left pane: an image, or a video or audio item as long as
// the file (`length`; 0 for images), which is opened here to learn it. NO: it can't be used.
- (BOOL)item:(mf::SceneItem*)it fromFile:(NSString*)path lengthUs:(int64_t*)length {
  UTType* type = [UTType typeWithFilenameExtension:path.pathExtension];
  it->src = path.UTF8String;
  it->source = mf::macos::sourceFromPath(it->src);
  *length = 0;
  if ([type conformsToType:UTTypeImage]) {
    it->type = mf::ItemType::Image;
    it->durationUs = editor::Document::kStillDurationUs;
  } else {
    mf::MediaInfo info;
    mf::Result r = _platform->createDemuxer()->open(it->source, &info);
    if (r != mf::Result::Ok || info.durationUs <= 0) {
      [self showStatus:[NSString stringWithFormat:@"%@: cannot open (%s)", path.lastPathComponent, mf::toString(r)] error:YES];
      return NO;
    }
    bool video = info.video.width > 0;
    if (!video && !info.audio) {
      [self showStatus:[NSString stringWithFormat:@"%@: no video or audio", path.lastPathComponent] error:YES];
      return NO;
    }
    if (!video && !info.audio->supported) {  // the engine plays AAC-LC and MP3 (R9)
      [self showStatus:[NSString stringWithFormat:@"%@: the audio must be AAC or MP3", path.lastPathComponent] error:YES];
      return NO;
    }
    it->type = video ? mf::ItemType::Video : mf::ItemType::Audio;
    it->durationUs = *length = info.durationUs;
  }
  [_sidebar rememberFile:path];
  return YES;
}

// --- Drag and drop --------------------------------------------------------------------------

// What's dropped, as items: the left pane's (one), or files from the Finder. `length`: a
// video's or audio file's.
struct Dropped {
  mf::SceneItem item;
  int64_t length = 0;
};

- (std::vector<Dropped>)droppedItems:(id<NSDraggingInfo>)info {
  std::vector<Dropped> out;
  NSPasteboard* pasteboard = info.draggingPasteboard;
  if ([pasteboard.types containsObject:SidebarDragType]) {
    SidebarPayload p = [_sidebar draggedPayload];
    Dropped d;
    if (p.file && [self item:&d.item fromFile:p.file lengthUs:&d.length]) out.push_back(d);
    if (p.hasItem) {
      d.item = p.item;
      if (!d.item.src.empty()) d.item.source = mf::macos::sourceFromPath(d.item.src);  // stickers: a PNG file
      out.push_back(d);
    }
    return out;
  }
  for (NSURL* url in [pasteboard readObjectsForClasses:@[ NSURL.class ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}]) {
    Dropped d;
    if ([self item:&d.item fromFile:url.path lengthUs:&d.length]) out.push_back(d);
  }
  return out;
}

// On the timeline: at that time on that row, or on a new track when the row doesn't take the
// item (audio on a video track, or the reverse) or there's no row. Several files go one after
// another.
- (BOOL)timelineDrop:(id<NSDraggingInfo>)info atUs:(int64_t)us track:(int)t {
  std::vector<Dropped> items = [self droppedItems:info];
  for (Dropped& d : items) {
    bool video = d.item.type != mf::ItemType::Audio;
    if (t < 0 || _doc.track(t).video != video) t = _doc.addTrack(video);
    if (t < 0) {
      [self showStatus:@"At most 16 tracks" error:YES];
      break;
    }
    int k = _doc.insertItem(t, std::move(d.item), us, d.length);
    _sel = {t, k};
    us = _doc.item(t, k).endUs();
  }
  if (!items.empty()) [self structureChanged];
  return !items.empty();
}

// On the preview: from the playhead, in front (see place), one after another; an image, text or
// sticker centered where it's dropped. A video stays centered (it fills the output), and a color
// fills it anyway.
- (BOOL)overlayDrop:(id<NSDraggingInfo>)info at:(NSPoint)position {
  std::vector<Dropped> items = [self droppedItems:info];
  int64_t at = _playheadUs;
  for (Dropped& d : items) {
    mf::ItemType type = d.item.type;
    if (type == mf::ItemType::Image || type == mf::ItemType::Text) {
      d.item.transform.x = mf::Animatable(position.x);
      d.item.transform.y = mf::Animatable(position.y);
    }
    int64_t end = [self place:std::move(d.item) at:at lengthUs:d.length];
    if (end < 0) break;
    at = end;
  }
  return !items.empty();
}

// The preview changed size: a paused frame is drawn again for it (setFilter redraws it), once
// per burst of resizing. Playing, the next frame is.
- (void)redrawPreview {
  if (_redrawPending) return;
  _redrawPending = YES;
  dispatch_async(dispatch_get_main_queue(), ^{
    self->_redrawPending = NO;
    if (self->_player && self->_seekOnReadyUs < 0 && self->_player->state() == mf::State::Ready) self->_player->setFilter(self->_doc.filter);
  });
}

// --- The preview's overlay ------------------------------------------------------------------

// Selected in the preview: the timeline and properties follow.
- (void)overlaySelectionChanged {
  _overlay.provisional = NO;
  [self syncTransitionPanel];
  _timeline.needsDisplay = YES;
  [_inspector rebuild];
  [self updatePropertiesTitle];
}

- (void)overlayEdited {
  [_inspector refresh];
  [self scheduleReload];
}

// An audio item's waveform, read once per file in the background; null until it's read.
- (const std::vector<float>*)peaksOf:(const mf::SceneItem&)it {
  auto found = _peaks.find(it.src);
  if (found != _peaks.end()) return &found->second;
  if (_peaksLoading.insert(it.src).second) {
    std::string src = it.src;
    __weak Editor* weak = self;
    editor::loadPeaks([NSString stringWithUTF8String:src.c_str()], ^(std::vector<float> peaks) {
      Editor* s = weak;
      if (!s) return;
      s->_peaks[src] = std::move(peaks);
      s->_timeline.needsDisplay = YES;
    });
  }
  return nullptr;
}

// Why a video file's audio can't play, or nil: read once per file.
- (NSString*)audioProblemOf:(const mf::SceneItem&)it {
  auto found = _audioProblems.find(it.src);
  if (found == _audioProblems.end()) {
    mf::MediaInfo info;
    mf::Result r = _platform->createDemuxer()->open(it.source, &info);
    const char* problem = r != mf::Result::Ok ? "Can't read the file" : !info.audio ? "No audio track" : !info.audio->supported ? "Audio isn't AAC or MP3" : "";
    found = _audioProblems.emplace(it.src, problem).first;
  }
  return found->second.empty() ? nil : [NSString stringWithUTF8String:found->second.c_str()];
}

// A video's or image's pixel size (upright, for images), read once per file.
- (NSSize)naturalSizeOf:(const mf::SceneItem&)it {
  auto found = _naturalSizes.find(it.src);
  if (found != _naturalSizes.end()) return found->second;
  NSSize size = NSZeroSize;
  if (it.type == mf::ItemType::Video) {
    mf::MediaInfo info;
    if (_platform->createDemuxer()->open(it.source, &info) == mf::Result::Ok) size = NSMakeSize(info.video.width, info.video.height);
  } else if (CGImageSourceRef source = CGImageSourceCreateWithURL(
                 (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:it.src.c_str()]], nullptr)) {
    NSDictionary* props = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
    CFRelease(source);
    size = NSMakeSize([props[(id)kCGImagePropertyPixelWidth] doubleValue], [props[(id)kCGImagePropertyPixelHeight] doubleValue]);
    if ([props[(id)kCGImagePropertyOrientation] intValue] >= 5) size = NSMakeSize(size.height, size.width);  // turned a quarter
  }
  _naturalSizes[it.src] = size;
  return size;
}

// --- Edits ----------------------------------------------------------------------------------

// Tracks or items were added, removed or moved, or the selection changed.
// A selected join shows its transition in the left pane; anything else selected hides it. A join
// that's no longer one (an item moved or removed) is no longer selected.
- (void)syncTransitionPanel {
  if (_sel.transition && !(_sel.track < _doc.tracks() && _doc.junction(_sel.track, _sel.item))) _sel = {};
  if (!_sel.transition) return [_sidebar hidePanel];
  [_transitionInspector rebuild];
  [_sidebar showPanel:_transitionInspector title:@"Transition"];
}

- (void)structureChanged {
  [_overlay stopEditing];
  [self syncTransitionPanel];
  [_timeline reload];
  _overlay.needsDisplay = YES;
  [_inspector rebuild];
  [self rebuildProjectTab];
  [self updatePropertiesTitle];
  [self scheduleReload];
}

// The Project tab shows the project as it is (e.g. a document's size, just opened). Rebuilt just
// after this event: one of its own buttons (Add Track) may be what changed the structure.
- (void)rebuildProjectTab {
  dispatch_async(dispatch_get_main_queue(), ^{
    [self->_projectInspector rebuild];
  });
}

// An open properties window follows the selection.
- (void)timelineSelectionChanged {
  _overlay.provisional = NO;
  [_overlay stopEditing];
  [self syncTransitionPanel];
  _overlay.needsDisplay = YES;
  [_inspector rebuild];
  [self updatePropertiesTitle];
}

- (void)timelineEdited {
  _overlay.needsDisplay = YES;
  [_inspector refresh];
  [self scheduleReload];
}

- (void)timelineSeek:(int64_t)us {
  [self seekTo:us];
}

- (void)timelineTogglePlay {
  [self togglePlay:nil];
}

- (void)timelineDeleteSelection {
  [self inspectorDeleteSelection];
}

- (void)inspectorEdited {
  _overlay.needsDisplay = YES;
  [_timeline reload];
  [_inspector refresh];  // both show the project when nothing is selected
  [_projectInspector refresh];
  [_transitionInspector refresh];
  if (_sel.transition && !_doc.junction(_sel.track, _sel.item)) {  // e.g. removed between two text items
    [self syncTransitionPanel];
    _timeline.needsDisplay = YES;
  }
  [self scheduleReload];
}

- (void)inspectorSelectionChanged {
  if (_projectSel.track >= 0) {  // the Project tab added a track: select it there, and stay on the project
    _sel = _projectSel;
    _projectSel = {};
    [self rebuildProjectTab];
  }
  [self structureChanged];
}

- (void)inspectorDeleteSelection {
  if (_sel.track < 0) return;
  if (_sel.transition) {  // a join: its transition goes, the join stays selected
    _doc.setTransition(_sel.track, _sel.item, nullptr);
    return [self structureChanged];
  }
  if (_sel.item >= 0) {
    _doc.removeItem(_sel.track, _sel.item);
    _sel.item = -1;
  } else {
    _doc.removeTrack(_sel.track);
    if (_doc.tracks() == 0) _doc.addTrack(true);
    _sel = {};
  }
  [self structureChanged];
}

// --- Preview --------------------------------------------------------------------------------

// Reopens the player a moment after the first of a burst of edits, e.g. while a slider moves.
// Every edit comes through here: the Export tab's summary (size, rate, length) follows too.
- (void)scheduleReload {
  _window.documentEdited = YES;  // something changed since the last save or open
  [_exportView refresh];
  if (_reloadPending) return;
  _reloadPending = YES;
  int token = ++_reloadToken;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    if (token != self->_reloadToken) return;  // reloaded already (reloadNow)
    self->_reloadPending = NO;
    [self reloadPlayer];
  });
}

// Reopens the player at once, dropping a reload still waiting: for a new item, which shouldn't
// wait on a burst of edits that isn't coming.
- (void)reloadNow {
  _window.documentEdited = YES;
  [_exportView refresh];
  _reloadPending = NO;
  ++_reloadToken;
  [self reloadPlayer];
}

- (void)reloadPlayer {
  BOOL wasPlaying = _player && _player->state() == mf::State::Play;
  if (_player) {
    _player->shutdown();
    _player.reset();
  }
  ++_generation;
  BOOL empty = _doc.empty();
  _preview.hidden = empty;
  _placeholder.hidden = !empty;
  if (empty) {
    _playheadUs = 0;
    return [self updatePlayButton:NO];
  }
  _listener = std::make_unique<Listener>(self, _generation);
  _player = mf::Player::create(*_platform, _listener.get());
  std::string error;
  // Opened at the playhead: its first frame is the one there, with none at 0 before it.
  _openedAtUs = _seekOnReadyUs = std::min(_playheadUs, std::max<int64_t>(0, _doc.scene.durationUs() - 1));
  mf::Result r = _player->open(_doc.scene, mf::macos::targetFromView((__bridge void*)_preview), mf::OutputDriver::Vsync, &error, _openedAtUs);
  if (r != mf::Result::Ok) {
    _player.reset();
    _seekOnReadyUs = -1;
    [self updatePlayButton:NO];
    return [self showStatus:[NSString stringWithFormat:@"%s", error.c_str()] error:YES];
  }
  _player->setFilter(_doc.filter);
  [self showStatus:@"" error:NO];
  _playAfterSeek = wasPlaying;
}

- (void)player:(int)generation state:(mf::State)state {
  if (generation != _generation) return;
  if (state == mf::State::Ready && _seekOnReadyUs >= 0) {  // first ready after opening, at _openedAtUs
    int64_t target = _seekOnReadyUs;
    _seekOnReadyUs = -1;
    if (target != _openedAtUs) {  // the playhead moved while it opened: follow it
      _player->seek(target);
    } else {
      [self shownAfterOpening];
    }
  }
  [self updatePlayButton:_player->state() == mf::State::Play];
}

// The player's frame shows the scene as it is now, at the playhead.
- (void)shownAfterOpening {
  _overlay.provisional = NO;
  if (!_playAfterSeek) return;
  _playAfterSeek = NO;
  _player->play();
}

- (void)player:(int)generation seekCompleted:(int64_t)ptsUs {
  if (generation != _generation) return;
  [self shownAfterOpening];
}

- (void)player:(int)generation failed:(NSString*)reason {
  if (generation == _generation) [self showStatus:reason error:YES];
}

- (void)updatePlayButton:(BOOL)playing {
  _playButton.image = [NSImage imageWithSystemSymbolName:playing ? @"pause.fill" : @"play.fill" accessibilityDescription:nil];
}

- (void)togglePlay:(id)sender {
  if (!_player || _seekOnReadyUs >= 0) return;
  if (_player->state() == mf::State::Play) {
    _player->pause();
  } else if (_player->positionUs() >= _player->durationUs() - 50000) {  // at the end: from the start
    _playAfterSeek = YES;
    [self seekTo:0];
  } else {
    _player->play();
  }
}

// Seeking needs a paused player (A4): a seek while playing pauses.
// The playhead goes up to the timeline's end (where an added item then starts); the player, which
// has no frame there, shows the last one.
- (void)seekTo:(int64_t)us {
  [self movePlayhead:us];
  int64_t frame = std::min(_playheadUs, std::max<int64_t>(0, _doc.scene.durationUs() - 1));
  if (!_player || _seekOnReadyUs >= 0) {
    if (_seekOnReadyUs >= 0) _seekOnReadyUs = frame;
    return;
  }
  if (_player->state() == mf::State::Play) _player->pause();
  _player->seek(frame);
}

// Moves the playhead alone: the player's next open (or seek) goes there.
- (void)movePlayhead:(int64_t)us {
  _playheadUs = std::clamp<int64_t>(us, 0, std::max<int64_t>(0, _doc.scene.durationUs()));
  _timeline.playheadUs = _playheadUs;
  _overlay.timeUs = _playheadUs;
}

- (void)tick {
  if (_player && _seekOnReadyUs < 0 && _player->state() == mf::State::Play) {
    _playheadUs = _player->positionUs();
    _timeline.playheadUs = _playheadUs;
  _overlay.timeUs = _playheadUs;
  }
  _timeLabel.stringValue = [NSString stringWithFormat:@"%@ / %@", timeString(_playheadUs), timeString(_doc.scene.durationUs())];
}

@end

int main(int argc, const char** argv) {
  @autoreleasepool {
    NSMutableArray<NSString*>* files = [NSMutableArray array];
    for (int i = 1; i < argc; ++i) {
      NSString* arg = [NSString stringWithUTF8String:argv[i]];
      if ([arg hasPrefix:@"-"]) continue;
      if (!arg.absolutePath) arg = [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:arg];
      [files addObject:arg.stringByStandardizingPath];
    }
    NSApplication* app = [NSApplication sharedApplication];
    app.activationPolicy = NSApplicationActivationPolicyRegular;
    Editor* editor = [[Editor alloc] initWithFiles:files];
    app.delegate = editor;
    [app run];
  }
  return 0;
}
