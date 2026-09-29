// A small video editor on the media framework. Left, the whole height: tabs of things to add
// (video, images, stickers, emojis, text, audio). Right of it: the preview of the scene with
// play/pause and the time, then the timeline (tracks and their items). The properties of the
// selection open in a floating window from its "…" button. The document is an mf::Scene; after each edit the player reopens it at the same
// time (the player has no live scene update), a moment after the last change.
//
//   mf_editor [file ...]   adds the files (video, image, audio) to the timeline

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
  BOOL _redrawPending;
  int64_t _playheadUs;
  int64_t _seekOnReadyUs;  // -1: none
  BOOL _playAfterSeek;
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
  for (NSString* path in _initialFiles) [self addFile:path];
  [self structureChanged];
  [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 target:self selector:@selector(tick) userInfo:nil repeats:YES];
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
  _window.title = @"Media Editor";
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

// --- Adding items ---------------------------------------------------------------------------

// The selected track if it takes this kind of item, else the top track that does, else a new one.
- (int)trackFor:(bool)video {
  if (_sel.track >= 0 && _doc.track(_sel.track).video == video) return _sel.track;
  for (int t = _doc.tracks() - 1; t >= 0; --t) {
    if (_doc.track(t).video == video) return t;
  }
  return _doc.addTrack(video);
}

// For an item drawn over the others: the highest video track free at the playhead for the
// item's length, above any track in use then, else a new track on top.
- (int)overlayTrackFor:(int64_t)durationUs {
  int free = -1;
  for (int t = _doc.tracks() - 1; t >= 0; --t) {
    if (!_doc.track(t).video) continue;
    bool busy = false;
    for (const mf::SceneItem& it : _doc.track(t).items) busy |= it.startUs < _playheadUs + durationUs && it.endUs() > _playheadUs;
    if (busy) break;
    free = t;
  }
  return free >= 0 ? free : _doc.addTrack(true);
}

- (void)insert:(mf::SceneItem)item video:(bool)video lengthUs:(int64_t)length {
  [self insert:std::move(item) onTrack:[self trackFor:video] lengthUs:length];
}

- (void)insert:(mf::SceneItem)item onTrack:(int)t lengthUs:(int64_t)length {
  if (t < 0) return [self showStatus:@"At most 16 tracks" error:YES];
  int k = _doc.insertItem(t, std::move(item), _playheadUs, length);
  _sel = {t, k};
  [self structureChanged];
}

- (void)sidebarAddFile:(NSString*)path {
  [self addFile:path];
}

- (void)sidebarAddItem:(const mf::SceneItem&)item overlay:(BOOL)overlay {
  mf::SceneItem it = item;
  if (!it.src.empty()) it.source = mf::macos::sourceFromPath(it.src);  // stickers: a PNG file
  if (overlay) [self insert:std::move(it) onTrack:[self overlayTrackFor:it.durationUs] lengthUs:0];
  else [self insert:std::move(it) video:true lengthUs:0];
}

- (void)addMedia:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ UTTypeMovie, UTTypeImage, UTTypeAudio ];
  panel.allowsMultipleSelection = YES;
  if ([panel runModal] != NSModalResponseOK) return;
  for (NSURL* url in panel.URLs) [self addFile:url.path];
}

- (void)addFile:(NSString*)path {
  mf::SceneItem it;
  int64_t length;
  if ([self item:&it fromFile:path lengthUs:&length]) [self insert:it video:it.type != mf::ItemType::Audio lengthUs:length];
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

// What's dropped, as items: the left pane's (one), or files from the Finder. `overlay`: the
// item goes over the others; `length`: a video's or audio file's.
struct Dropped {
  mf::SceneItem item;
  int64_t length = 0;
  bool overlay = false;
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
      d.overlay = p.overlay;
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

// On the preview: at the playhead, over the others, placed where it's dropped. A video stays
// centered (it fills the output), a color goes in sequence (it would cover everything), and
// audio goes on an audio track.
- (BOOL)overlayDrop:(id<NSDraggingInfo>)info at:(NSPoint)position {
  std::vector<Dropped> items = [self droppedItems:info];
  for (Dropped& d : items) {
    mf::ItemType type = d.item.type;
    if (type == mf::ItemType::Audio || type == mf::ItemType::Color) {
      [self insert:std::move(d.item) video:type != mf::ItemType::Audio lengthUs:d.length];
      continue;
    }
    if (type != mf::ItemType::Video) {
      d.item.transform.x = mf::Animatable(position.x);
      d.item.transform.y = mf::Animatable(position.y);
    }
    int64_t duration = d.item.durationUs;
    [self insert:std::move(d.item) onTrack:[self overlayTrackFor:duration] lengthUs:d.length];
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
  [self updatePropertiesTitle];
  [self scheduleReload];
}

// An open properties window follows the selection.
- (void)timelineSelectionChanged {
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
    [_projectInspector rebuild];
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
  [_exportView refresh];
  if (_reloadPending) return;
  _reloadPending = YES;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    self->_reloadPending = NO;
    [self reloadPlayer];
  });
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
  mf::Result r = _player->open(_doc.scene, mf::macos::targetFromView((__bridge void*)_preview), mf::OutputDriver::Vsync, &error);
  if (r != mf::Result::Ok) {
    _player.reset();
    [self updatePlayButton:NO];
    return [self showStatus:[NSString stringWithFormat:@"%s", error.c_str()] error:YES];
  }
  _player->setFilter(_doc.filter);
  [self showStatus:@"" error:NO];
  _seekOnReadyUs = std::min(_playheadUs, std::max<int64_t>(0, _doc.scene.durationUs() - 1));
  _playAfterSeek = wasPlaying;
}

- (void)player:(int)generation state:(mf::State)state {
  if (generation != _generation) return;
  if (state == mf::State::Ready && _seekOnReadyUs >= 0) {  // first ready after opening: go back to the playhead
    _player->seek(_seekOnReadyUs);
    _seekOnReadyUs = -1;
  }
  [self updatePlayButton:state == mf::State::Play];
}

- (void)player:(int)generation seekCompleted:(int64_t)ptsUs {
  if (generation != _generation || !_playAfterSeek) return;
  _playAfterSeek = NO;
  _player->play();
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
- (void)seekTo:(int64_t)us {
  _playheadUs = std::clamp<int64_t>(us, 0, std::max<int64_t>(0, _doc.scene.durationUs() - 1));
  _timeline.playheadUs = _playheadUs;
  _overlay.timeUs = _playheadUs;
  if (!_player || _seekOnReadyUs >= 0) {
    if (_seekOnReadyUs >= 0) _seekOnReadyUs = _playheadUs;
    return;
  }
  if (_player->state() == mf::State::Play) _player->pause();
  _player->seek(_playheadUs);
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
