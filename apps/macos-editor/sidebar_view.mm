#import "sidebar_view.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>

@interface SidebarContent : NSView  // flipped, so the list starts at the top
@end
@implementation SidebarContent
- (BOOL)isFlipped {
  return YES;
}
@end

NSPasteboardType const SidebarDragType = @"com.mediaframework.editor.sidebar-item";

// A button that can also be dragged: past a few points, the press becomes a drag of it.
@interface DragButton : NSButton <NSDraggingSource>
@property(nonatomic, copy) BOOL (^beginDrag)(void);  // records what's dragged; NO: nothing to drag
@property(nonatomic, copy) void (^endDrag)(void);
@end

@implementation DragButton
- (void)mouseDown:(NSEvent*)event {
  NSPoint start = event.locationInWindow;
  for (;;) {
    NSEvent* e = [self.window nextEventMatchingMask:NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged];
    if (e.type == NSEventTypeLeftMouseUp) {
      if (NSPointInRect([self convertPoint:e.locationInWindow fromView:nil], self.bounds)) [self sendAction:self.action to:self.target];
      return;
    }
    if (std::hypot(e.locationInWindow.x - start.x, e.locationInWindow.y - start.y) < 4) continue;
    if (!self.beginDrag || !self.beginDrag()) return;
    NSPasteboardItem* data = [NSPasteboardItem new];
    [data setString:@"" forType:SidebarDragType];
    NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:data];
    NSBitmapImageRep* rep = [self bitmapImageRepForCachingDisplayInRect:self.bounds];  // the button, as it looks
    [self cacheDisplayInRect:self.bounds toBitmapImageRep:rep];
    NSImage* image = [[NSImage alloc] initWithSize:self.bounds.size];
    [image addRepresentation:rep];
    [item setDraggingFrame:self.bounds contents:image];
    [self beginDraggingSessionWithItems:@[ item ] event:event source:self];
    return;
  }
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
  return context == NSDraggingContextWithinApplication ? NSDragOperationCopy : NSDragOperationNone;
}

- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation {
  if (self.endDrag) self.endDrag();
}
@end

// Runs a block when its button is clicked.
@interface ClickAction : NSObject
@property(nonatomic, copy) void (^run)(void);
@end
@implementation ClickAction
- (void)fire:(id)sender {
  self.run();
}
@end

namespace {
enum Tab { kVideo, kImage, kStickers, kEmojis, kText, kAudio, kProject, kTabs };
constexpr CGFloat kTabBarWidth = 72;
constexpr int64_t kStillUs = 5000000;

NSString* const kTabTitles[kTabs] = {@"Video", @"Image", @"Stickers", @"Emojis", @"Text", @"Audio", @"Project"};
NSString* const kTabSymbols[kTabs] = {@"film", @"photo", @"star.circle", @"face.smiling", @"textformat", @"music.note", @"gearshape"};

// An SF Symbol, and colors (0xRRGGBB) for its layers in order: one, or two for two-layer symbols.
struct Sticker {
  NSString* symbol;
  uint32_t colors[2];
  NSArray<NSColor*>* palette() const {
    NSMutableArray* out = [NSMutableArray array];
    for (uint32_t c : colors) {
      if (c) [out addObject:[NSColor colorWithSRGBRed:(c >> 16) / 255.0 green:(c >> 8 & 0xFF) / 255.0 blue:(c & 0xFF) / 255.0 alpha:1]];
    }
    return out;
  }
};

const Sticker kStickerList[] = {
    {@"star.fill", {0xFFCC1A}},
    {@"heart.fill", {0xF2334C}},
    {@"sun.max.fill", {0xFF9E1A}},
    {@"moon.stars.fill", {0x5959D9, 0xFFD940}},
    {@"cloud.sun.fill", {0xEBF0FA, 0xFFBF26}},
    {@"bolt.fill", {0xFFD91A}},
    {@"flame.fill", {0xFF731A}},
    {@"sparkles", {0xFFD133}},
    {@"crown.fill", {0xFABF26}},
    {@"party.popper.fill", {0x9959E6, 0xFF8C33}},
    {@"gift.fill", {0xE63340, 0xFFCC33}},
    {@"leaf.fill", {0x40B24C}},
    {@"snowflake", {0x66BFFF}},
    {@"drop.fill", {0x338CF2}},
    {@"pawprint.fill", {0x8C6140}},
    {@"hand.thumbsup.fill", {0x338CF2}},
    {@"checkmark.seal.fill", {0xFFFFFF, 0x33B259}},
    {@"music.note", {0xA659E6}},
    {@"camera.fill", {0x4C4C59}},
    {@"bubble.left.fill", {0x4099FF}},
};

NSString* const kEmojiList = @"😀😂😍🥳😎🤩🥹😭😡🤔👍👏🙌💪🎉🔥✨💯❤️💔⭐️🌈☀️🌙⚡️🌸🍀🍕🎂☕️⚽️🏖️✈️🚀📸🎵🎁🐶🐱🦄";

NSImage* symbolImage(const Sticker& s, CGFloat points) {
  NSImageSymbolConfiguration* config = [[NSImageSymbolConfiguration configurationWithPointSize:points weight:NSFontWeightRegular]
      configurationByApplyingConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:s.palette()]];
  return [[NSImage imageWithSystemSymbolName:s.symbol accessibilityDescription:nil] imageWithSymbolConfiguration:config];
}

// The sticker as a PNG of at most 512 px a side, rendered on first use.
NSString* stickerFile(const Sticker& s) {
  NSString* dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@"mf_editor_stickers"];
  NSString* path = [dir stringByAppendingPathComponent:[s.symbol stringByAppendingPathExtension:@"png"]];
  if ([NSFileManager.defaultManager fileExistsAtPath:path]) return path;
  [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
  NSImage* image = symbolImage(s, 400);
  if (!image) return nil;
  double k = 512 / std::max(image.size.width, image.size.height);
  NSInteger w = std::lround(image.size.width * k), h = std::lround(image.size.height * k);
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                  pixelsWide:w
                                                                  pixelsHigh:h
                                                               bitsPerSample:8
                                                             samplesPerPixel:4
                                                                    hasAlpha:YES
                                                                    isPlanar:NO
                                                              colorSpaceName:NSDeviceRGBColorSpace
                                                                 bytesPerRow:0
                                                                bitsPerPixel:0];
  [NSGraphicsContext saveGraphicsState];
  NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
  [image drawInRect:NSMakeRect(0, 0, w, h)];
  [NSGraphicsContext restoreGraphicsState];
  NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  return [png writeToFile:path atomically:YES] ? path : nil;
}

mf::SceneItem textItem(const std::string& text, const char* font, float size) {
  mf::SceneItem it;
  it.type = mf::ItemType::Text;
  it.text = text;
  it.style.font = font;
  it.style.size = size;
  it.durationUs = kStillUs;
  return it;
}
}  // namespace

@implementation SidebarView {
  Tab _tab;
  NSMutableArray<NSButton*>* _tabButtons;
  NSTextField* _title;
  NSStackView* _list;
  NSScrollView* _scroll;  // holds _list
  NSView* _projectView;
  NSButton* _collapseButton;
  NSMutableArray* _actions;  // ClickAction targets of the current tab's buttons
  NSMutableArray<NSString*>* _files[kTabs];
  SidebarPayload _dragged;
  NSView* _panel;              // shown by showPanel, over the tabs' content
  NSString* _panelTitle;
  BOOL _collapsedUnderPanel;   // the pane was collapsed when the panel opened it
}

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _tabButtons = [NSMutableArray array];
    _actions = [NSMutableArray array];
    for (int t = 0; t < kTabs; ++t) _files[t] = [NSMutableArray array];

    // The tab bar, down the left edge from the top.
    for (int t = 0; t < kTabs; ++t) {
      NSButton* b = [NSButton buttonWithTitle:kTabTitles[t]
                                        image:[NSImage imageWithSystemSymbolName:kTabSymbols[t] accessibilityDescription:kTabTitles[t]]
                                       target:self
                                       action:@selector(tabClicked:)];
      b.tag = t;
      b.bordered = NO;
      b.imagePosition = NSImageAbove;
      b.font = [NSFont systemFontOfSize:10];
      b.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:18 weight:NSFontWeightRegular];
      b.wantsLayer = YES;
      b.layer.cornerRadius = 8;
      b.frame = NSMakeRect(6, frame.size.height - 10 - (t + 1) * 54, kTabBarWidth - 12, 50);
      b.autoresizingMask = NSViewMinYMargin;
      [self addSubview:b];
      [_tabButtons addObject:b];
    }
    _collapseButton = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"sidebar.left" accessibilityDescription:@"Collapse"]
                                         target:self
                                         action:@selector(toggleCollapsed:)];
    _collapseButton.bordered = NO;
    _collapseButton.contentTintColor = NSColor.secondaryLabelColor;
    _collapseButton.toolTip = @"Hide or show the panel";
    _collapseButton.frame = NSMakeRect(6, 10, kTabBarWidth - 12, 30);
    [self addSubview:_collapseButton];
    NSBox* line = [[NSBox alloc] initWithFrame:NSMakeRect(kTabBarWidth, 0, 1, frame.size.height)];
    line.boxType = NSBoxSeparator;
    line.autoresizingMask = NSViewHeightSizable;
    [self addSubview:line];

    // The selected tab's content, to the right of the bar.
    CGFloat x = kTabBarWidth + 1, w = frame.size.width - x;
    _title = [NSTextField labelWithString:@""];
    _title.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    _title.frame = NSMakeRect(x + 14, frame.size.height - 34, w - 28, 22);
    _title.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [self addSubview:_title];
    NSScrollView* scroll = _scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(x, 0, w, frame.size.height - 42)];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    SidebarContent* content = [[SidebarContent alloc] init];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = content;
    _list = [NSStackView stackViewWithViews:@[]];
    _list.orientation = NSUserInterfaceLayoutOrientationVertical;
    _list.alignment = NSLayoutAttributeLeading;
    _list.spacing = 8;
    _list.edgeInsets = NSEdgeInsetsMake(4, 14, 14, 14);
    _list.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_list];
    NSView* clip = scroll.contentView;
    [NSLayoutConstraint activateConstraints:@[
      [content.topAnchor constraintEqualToAnchor:clip.topAnchor],
      [content.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
      [content.widthAnchor constraintEqualToAnchor:clip.widthAnchor],
      [_list.topAnchor constraintEqualToAnchor:content.topAnchor],
      [_list.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
      [_list.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
      [_list.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    [self addSubview:scroll];
    [self showTab:kVideo];
  }
  return self;
}

+ (CGFloat)collapsedWidth {
  return kTabBarWidth + 1;
}

// The open tab's button collapses the pane; any tab's button opens it.
- (void)tabClicked:(NSButton*)sender {
  if (_panel) {  // a tab takes the place of the panel
    [_panel removeFromSuperview];
    _panel = nil;
    _collapsedUnderPanel = NO;
    return [self showTab:Tab(sender.tag)];
  }
  if (!_collapsed && sender.tag == _tab) return [self setCollapsed:YES];
  if (_collapsed) [self setCollapsed:NO];
  [self showTab:Tab(sender.tag)];
}

- (void)toggleCollapsed:(id)sender {
  [self setCollapsed:!_collapsed];
}

// A new width: the open tab's content is laid out again for it.
- (void)setFrameSize:(NSSize)size {
  BOOL wider = size.width != self.frame.size.width;
  [super setFrameSize:size];
  if (wider && !_collapsed && _list) [self showTab:_tab];
}

- (void)setCollapsed:(BOOL)collapsed {
  _collapsed = collapsed;
  _title.hidden = collapsed;
  _scroll.hidden = collapsed || _panel || _tab == kProject;
  _projectView.hidden = collapsed || _panel || _tab != kProject;
  _panel.hidden = collapsed;
  _collapseButton.image = [NSImage imageWithSystemSymbolName:collapsed ? @"sidebar.right" : @"sidebar.left"
                                    accessibilityDescription:collapsed ? @"Expand" : @"Collapse"];
  if (collapsed) [self showTab:_tab];  // the open tab is highlighted only while its content shows
  [self.delegate sidebarCollapsedChanged];  // opening: the new width builds the content (setFrameSize)
}

- (void)showPanel:(NSView*)view title:(NSString*)title {
  _panelTitle = title;  // first: opening the pane below lays it out, title and all
  if (_panel != view) {
    [_panel removeFromSuperview];
    _panel = view;
    view.frame = _scroll.frame;
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self addSubview:view];
  }
  if (_collapsed) {
    _collapsedUnderPanel = YES;
    self.collapsed = NO;
  }
  [self showTab:_tab];  // with the panel: no tab highlighted, its content hidden
}

- (void)hidePanel {
  if (!_panel) return;
  [_panel removeFromSuperview];
  _panel = nil;
  if (_collapsedUnderPanel) {
    _collapsedUnderPanel = NO;
    self.collapsed = YES;
  } else {
    [self showTab:_tab];
  }
}

- (void)setProjectView:(NSView*)view {
  [_projectView removeFromSuperview];
  _projectView = view;
  view.frame = _scroll.frame;
  view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  view.hidden = _collapsed || _tab != kProject;
  [self addSubview:view];
}

- (void)showTab:(Tab)tab {
  _tab = tab;
  for (NSButton* b in _tabButtons) {
    bool on = b.tag == tab && !_collapsed && !_panel;
    b.layer.backgroundColor = on ? [NSColor.controlAccentColor colorWithAlphaComponent:0.18].CGColor : nil;
    b.contentTintColor = on ? NSColor.controlAccentColor : NSColor.secondaryLabelColor;
  }
  _title.stringValue = _panel ? (_panelTitle ?: @"") : kTabTitles[tab];
  _scroll.hidden = _collapsed || _panel || tab == kProject;
  _projectView.hidden = _collapsed || _panel || tab != kProject;
  _panel.hidden = _collapsed;
  for (NSView* v in _list.arrangedSubviews) [v removeFromSuperview];
  [_actions removeAllObjects];
  if (_collapsed) return;  // built when it opens, at its width
  switch (tab) {
    case kVideo:
    case kImage:
    case kAudio: [self buildFiles:tab]; break;
    case kStickers: [self buildStickers]; break;
    case kEmojis: [self buildEmojis]; break;
    case kText: [self buildText]; break;
    case kProject:
    case kTabs: break;
  }
}

- (void)rememberFile:(NSString*)path {
  Tab tab = [self tabOfFile:path];
  if ([_files[tab] containsObject:path]) return;
  [_files[tab] addObject:path];
  if (tab != _tab) return;
  dispatch_async(dispatch_get_main_queue(), ^{  // not now: the button being dragged may be in the list
    if (tab == self->_tab) [self showTab:tab];
  });
}

- (Tab)tabOfFile:(NSString*)path {
  UTType* type = [UTType typeWithFilenameExtension:path.pathExtension];
  if ([type conformsToType:UTTypeImage]) return kImage;
  if ([type conformsToType:UTTypeAudio]) return kAudio;
  return kVideo;
}

// --- Content --------------------------------------------------------------------------------

- (SidebarPayload)draggedPayload {
  return _dragged;
}

// A button that adds what `payload` makes: at the playhead when clicked, or where it's dropped.
- (NSButton*)button:(NSString*)title image:(NSImage*)image payload:(SidebarPayload (^)(void))payload {
  __weak SidebarView* weak = self;
  ClickAction* action = [ClickAction new];
  action.run = ^{
    SidebarPayload p = payload();
    if (p.empty()) return NSBeep();
    if (p.file) [weak.delegate sidebarAddFile:p.file];
    else [weak.delegate sidebarAddItem:p.item overlay:p.overlay];
  };
  [_actions addObject:action];
  DragButton* b = image ? [DragButton buttonWithTitle:title image:image target:action action:@selector(fire:)]
                        : [DragButton buttonWithTitle:title target:action action:@selector(fire:)];
  b.beginDrag = ^BOOL {
    SidebarView* s = weak;
    if (!s) return NO;
    s->_dragged = payload();
    return !s->_dragged.empty();
  };
  b.endDrag = ^{
    if (SidebarView* s = weak) s->_dragged = {};
  };
  return b;
}

- (NSButton*)button:(NSString*)title image:(NSImage*)image run:(void (^)(void))run {
  ClickAction* action = [ClickAction new];
  action.run = run;
  [_actions addObject:action];
  NSButton* b = image ? [NSButton buttonWithTitle:title image:image target:action action:@selector(fire:)]
                      : [NSButton buttonWithTitle:title target:action action:@selector(fire:)];
  return b;
}

- (void)hint:(NSString*)text {
  NSTextField* label = [NSTextField wrappingLabelWithString:text];
  label.textColor = NSColor.secondaryLabelColor;
  label.font = [NSFont systemFontOfSize:12];
  [label.widthAnchor constraintEqualToConstant:self.bounds.size.width - kTabBarWidth - 30].active = YES;
  [_list addArrangedSubview:label];
}

- (void)heading:(NSString*)text {
  NSTextField* label = [NSTextField labelWithString:text.uppercaseString];
  label.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
  label.textColor = NSColor.tertiaryLabelColor;
  if (_list.arrangedSubviews.count) [_list setCustomSpacing:16 afterView:_list.arrangedSubviews.lastObject];
  [_list addArrangedSubview:label];
}

// Square buttons in rows, as many to a row as the pane's width holds.
- (void)grid:(NSArray<NSButton*>*)buttons size:(CGFloat)size {
  CGFloat width = self.bounds.size.width - kTabBarWidth - 1 - 28;  // less the list's insets
  NSUInteger columns = std::max(1, int((width + 4) / (size + 4)));
  for (NSUInteger i = 0; i < buttons.count; i += columns) {
    NSArray* row = [buttons subarrayWithRange:NSMakeRange(i, std::min<NSUInteger>(columns, buttons.count - i))];
    for (NSButton* b in row) {
      b.bordered = NO;
      [b.widthAnchor constraintEqualToConstant:size].active = YES;
      [b.heightAnchor constraintEqualToConstant:size].active = YES;
    }
    NSStackView* line = [NSStackView stackViewWithViews:row];
    line.spacing = 4;
    [_list addArrangedSubview:line];
  }
}

- (void)buildFiles:(Tab)tab {
  static NSString* const kinds[kTabs] = {@"videos", @"images", nil, nil, nil, @"audio files", nil};
  __weak SidebarView* weak = self;
  NSButton* import = [self button:[NSString stringWithFormat:@"Import %@…", tab == kAudio ? @"Audio" : kTabTitles[tab]]
                            image:[NSImage imageWithSystemSymbolName:@"plus" accessibilityDescription:nil]
                              run:^{
                                [weak import:tab];
                              }];
  import.controlSize = NSControlSizeLarge;
  [_list addArrangedSubview:import];
  if (_files[tab].count == 0) {
    [self hint:[NSString stringWithFormat:@"Imported %@ are listed here. Click one to add it at the playhead, or drag it to the "
                                          @"preview or the timeline.", kinds[tab]]];
  }
  for (NSString* path in _files[tab]) {
    NSButton* row = [self button:path.lastPathComponent
                           image:[NSImage imageWithSystemSymbolName:kTabSymbols[tab] accessibilityDescription:nil]
                         payload:^{
                           SidebarPayload p;
                           p.file = path;
                           return p;
                         }];
    row.bordered = NO;
    row.imagePosition = NSImageLeading;
    row.alignment = NSTextAlignmentLeft;
    row.lineBreakMode = NSLineBreakByTruncatingMiddle;
    row.toolTip = path;
    [row.widthAnchor constraintEqualToConstant:self.bounds.size.width - kTabBarWidth - 30].active = YES;
    [_list addArrangedSubview:row];
  }

  if (tab == kImage) {  // solid colors: in sequence on the track, like an image
    [self heading:@"Solid colors"];
    static const float colors[][3] = {{0, 0, 0}, {1, 1, 1}, {0.98f, 0.82f, 0.25f}, {0.95f, 0.35f, 0.40f},
                                      {0.30f, 0.60f, 0.95f}, {0.35f, 0.75f, 0.45f}, {0.60f, 0.40f, 0.85f}, {0.15f, 0.20f, 0.30f}};
    NSMutableArray* swatches = [NSMutableArray array];
    for (const auto& c : colors) {
      NSButton* b = [self button:@""
                           image:nil
                         payload:^{
                           SidebarPayload p;
                           p.hasItem = true;
                           p.item.type = mf::ItemType::Color;
                           p.item.color = {c[0], c[1], c[2], 1};
                           p.item.durationUs = kStillUs;
                           return p;
                         }];
      b.wantsLayer = YES;
      b.layer.cornerRadius = 6;
      b.layer.borderWidth = 1;
      b.layer.borderColor = [NSColor colorWithWhite:0.5 alpha:0.5].CGColor;
      b.layer.backgroundColor = [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:1].CGColor;
      [swatches addObject:b];
    }
    [self grid:swatches size:44];
  }
}

- (void)import:(Tab)tab {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ tab == kImage ? UTTypeImage : tab == kAudio ? UTTypeAudio : UTTypeMovie ];
  panel.allowsMultipleSelection = YES;
  if ([panel runModal] != NSModalResponseOK) return;
  for (NSURL* url in panel.URLs) [self.delegate sidebarAddFile:url.path];  // it remembers the ones it adds
}

- (void)buildStickers {
  NSMutableArray* buttons = [NSMutableArray array];
  for (const Sticker& s : kStickerList) {
    NSImage* preview = symbolImage(s, 26);
    if (!preview) continue;  // not in this macOS version
    const Sticker* sticker = &s;
    NSButton* b = [self button:@""
                         image:preview
                       payload:^{
                         SidebarPayload p;
                         NSString* path = stickerFile(*sticker);
                         if (!path) return p;
                         p.hasItem = p.overlay = true;
                         p.item.type = mf::ItemType::Image;
                         p.item.src = path.UTF8String;
                         p.item.fit = mf::Fit::None;
                         p.item.transform.scale = mf::Animatable(0.6);
                         p.item.durationUs = kStillUs;
                         return p;
                       }];
    b.imagePosition = NSImageOnly;
    b.toolTip = s.symbol;
    [buttons addObject:b];
  }
  [self grid:buttons size:52];
}

- (void)buildEmojis {
  NSMutableArray* buttons = [NSMutableArray array];
  [kEmojiList enumerateSubstringsInRange:NSMakeRange(0, kEmojiList.length)
                              options:NSStringEnumerationByComposedCharacterSequences
                           usingBlock:^(NSString* emoji, NSRange, NSRange, BOOL*) {
                             NSButton* b = [self button:emoji
                                                  image:nil
                                                payload:^{
                                                  SidebarPayload p;
                                                  p.hasItem = p.overlay = true;
                                                  p.item = textItem(emoji.UTF8String, "system", 0.16f);
                                                  return p;
                                                }];
                             b.font = [NSFont systemFontOfSize:26];
                             [buttons addObject:b];
                           }];
  [self grid:buttons size:40];
}

- (void)buildText {
  struct Preset {
    NSString* label;
    const char* text;
    const char* font;
    float size;
    CGFloat shown;  // point size in the button
    bool caption;
  };
  static const Preset presets[] = {
      {@"Add a heading", "Heading", "system-bold", 0.10f, 22, false},
      {@"Add a subheading", "Subheading", "system-bold", 0.065f, 16, false},
      {@"Add body text", "Body text", "system", 0.045f, 13, false},
      {@"Add a caption", "Caption", "system", 0.045f, 12, true},
  };
  for (const Preset& p : presets) {
    const Preset* preset = &p;
    NSButton* b = [self button:p.label
                         image:nil
                       payload:^{
                         SidebarPayload out;
                         out.hasItem = out.overlay = true;
                         out.item = textItem(preset->text, preset->font, preset->size);
                         if (preset->caption) {  // bottom center, on a dark box
                           out.item.style.hasBox = true;
                           out.item.transform.y = mf::Animatable(0.94);
                           out.item.transform.anchorY = 1;
                         }
                         return out;
                       }];
    b.bezelStyle = NSBezelStyleFlexiblePush;
    b.font = p.caption ? [NSFont systemFontOfSize:p.shown] : [NSFont systemFontOfSize:p.shown weight:p.size > 0.05f ? NSFontWeightBold : NSFontWeightRegular];
    b.alignment = NSTextAlignmentLeft;
    [b.widthAnchor constraintEqualToConstant:self.bounds.size.width - kTabBarWidth - 30].active = YES;
    [b.heightAnchor constraintEqualToConstant:p.shown + 26].active = YES;
    [_list addArrangedSubview:b];
  }
  [self hint:@"Click to add text over the video at the playhead, or drag it where you want it. Change its words, font "
             @"and color in the properties."];
}

@end
