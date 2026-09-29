#import "timeline_view.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr CGFloat kGutter = 10, kRulerHeight = 26, kVideoRow = 50, kAudioRow = 44;
constexpr CGFloat kHandle = 8, kHoverZone = 14;  // trim handles: their width, and how near an end shows them
constexpr CGFloat kMoreWidth = 24, kMoreHeight = 16;  // the "…" button
constexpr double kUs = 1e6;

NSString* nameOf(const mf::SceneItem& it) {
  switch (it.type) {
    case mf::ItemType::Text: return [NSString stringWithUTF8String:it.text.c_str()];
    case mf::ItemType::Color: return @"Color";
    default: return [NSString stringWithUTF8String:it.src.c_str()].lastPathComponent;
  }
}

NSColor* colorOf(mf::ItemType type) {
  switch (type) {
    case mf::ItemType::Video: return [NSColor colorWithSRGBRed:0.36 green:0.55 blue:0.93 alpha:1];
    case mf::ItemType::Image: return [NSColor colorWithSRGBRed:0.35 green:0.72 blue:0.64 alpha:1];
    case mf::ItemType::Text: return [NSColor colorWithSRGBRed:0.45 green:0.80 blue:0.88 alpha:1];
    case mf::ItemType::Color: return [NSColor colorWithSRGBRed:0.66 green:0.52 blue:0.86 alpha:1];
    case mf::ItemType::Audio: return [NSColor colorWithSRGBRed:0.44 green:0.74 blue:0.40 alpha:1];
  }
  return NSColor.grayColor;
}

enum class Drag { None, Seek, Move, TrimStart, TrimEnd };
}  // namespace

@implementation TimelineView {
  editor::Document* _doc;
  editor::Selection* _sel;
  Drag _drag;
  CGFloat _downX;
  int64_t _origStartUs, _origDurationUs;
  BOOL _handles;  // the mouse is near an end of the selected item: its trim handles show
  BOOL _dropping;  // something is dragged over: where it would land shows at _dropAt
  NSPoint _dropAt;
  NSTrackingArea* _tracking;
}

- (instancetype)initWithDocument:(editor::Document*)doc selection:(editor::Selection*)selection {
  if ((self = [super initWithFrame:NSMakeRect(0, 0, 800, 200)])) {
    _doc = doc;
    _sel = selection;
    _pixelsPerSecond = 40;
  }
  return self;
}

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                           options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                   NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                             owner:self
                                          userInfo:nil];
  [self addTrackingArea:_tracking];
}

// The ruler and the track's "…" button stay in view while scrolling: redraw when the view scrolls.
- (void)viewDidMoveToSuperview {
  [super viewDidMoveToSuperview];
  NSView* clip = self.superview;
  if (!clip) return;
  clip.postsBoundsChangedNotifications = YES;
  clip.postsFrameChangedNotifications = YES;
  NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
  [nc addObserver:self selector:@selector(clipChanged:) name:NSViewBoundsDidChangeNotification object:clip];
  [nc addObserver:self selector:@selector(clipChanged:) name:NSViewFrameDidChangeNotification object:clip];
}

- (void)clipChanged:(NSNotification*)note {
  if (note.name == NSViewFrameDidChangeNotification) [self reload];
  else self.needsDisplay = YES;
}

- (void)dealloc {
  [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)setPixelsPerSecond:(double)pps {
  _pixelsPerSecond = pps;
  [self reload];
}

- (void)setPlayheadUs:(int64_t)us {
  if (us == _playheadUs) return;
  _playheadUs = us;
  self.needsDisplay = YES;
}

- (void)reload {
  NSSize visible = self.superview ? self.superview.bounds.size : self.frame.size;
  double seconds = _doc->scene.durationUs() / kUs + 30;  // room to drag items past the end
  CGFloat height = kRulerHeight;
  for (int r = 0; r < _doc->tracks(); ++r) height += [self rowHeight:r];
  [self setFrameSize:NSMakeSize(std::max<CGFloat>(visible.width, kGutter + seconds * _pixelsPerSecond),
                                std::max<CGFloat>(visible.height, height))];
  self.needsDisplay = YES;
}

// --- Geometry: rows are tracks from the top one down --------------------------------------

- (int)trackOfRow:(int)r {
  return _doc->tracks() - 1 - r;
}

- (CGFloat)rowHeight:(int)r {
  return _doc->track([self trackOfRow:r]).video ? kVideoRow : kAudioRow;
}

- (CGFloat)rowTop:(int)r {
  CGFloat y = kRulerHeight;
  for (int i = 0; i < r; ++i) y += [self rowHeight:i];
  return y;
}

- (int)rowAt:(CGFloat)y {
  for (int r = 0; r < _doc->tracks(); ++r) {
    CGFloat top = [self rowTop:r];
    if (y >= top && y < top + [self rowHeight:r]) return r;
  }
  return -1;
}

- (CGFloat)xOf:(int64_t)us {
  return kGutter + us / kUs * _pixelsPerSecond;
}

- (int64_t)usAt:(CGFloat)x {
  return std::max<int64_t>(0, std::llround((x - kGutter) / _pixelsPerSecond * kUs));
}

- (NSRect)rectOfItem:(int)k row:(int)r {
  const mf::SceneItem& it = _doc->item([self trackOfRow:r], k);
  CGFloat x0 = [self xOf:it.startUs], x1 = [self xOf:it.endUs()];
  return NSMakeRect(x0, [self rowTop:r] + 4, std::max<CGFloat>(4, x1 - x0), [self rowHeight:r] - 8);
}

- (int)rowOfTrack:(int)t {
  return _doc->tracks() - 1 - t;
}

// Which end of the selected item p is near: -1 the start, +1 the end, 0 neither.
- (int)edgeAt:(NSPoint)p {
  if (_sel->track < 0 || _sel->item < 0) return 0;
  NSRect box = [self rectOfItem:_sel->item row:[self rowOfTrack:_sel->track]];
  if (p.y < NSMinY(box) || p.y > NSMaxY(box) || p.x < NSMinX(self.visibleRect) + kGutter) return 0;
  CGFloat zone = std::min(kHoverZone, NSWidth(box) / 3);
  if (p.x >= NSMaxX(box) - zone && p.x <= NSMaxX(box) + kHandle / 2) return +1;
  if (p.x >= NSMinX(box) - kHandle / 2 && p.x <= NSMinX(box) + zone) return -1;
  return 0;
}

// The "…" button of the selection: at the selected item's right end, left of its trim zone, or
// at the right of the selected track's row, in view. Empty when nothing is selected or the item
// is too short.
- (NSRect)moreButtonRect {
  if (_sel->track < 0) return NSZeroRect;
  int r = [self rowOfTrack:_sel->track];
  if (_sel->item < 0) {
    CGFloat top = [self rowTop:r], h = [self rowHeight:r];
    return NSMakeRect(NSMaxX(self.visibleRect) - kMoreWidth - 10, top + (h - kMoreHeight) / 2, kMoreWidth, kMoreHeight);
  }
  NSRect box = [self rectOfItem:_sel->item row:r];
  if (NSWidth(box) < kMoreWidth + 2 * kHoverZone + 20) return NSZeroRect;
  return NSMakeRect(NSMaxX(box) - kHoverZone - 2 - kMoreWidth, NSMinY(box) + 4, kMoreWidth, kMoreHeight);
}

- (void)drawMoreButton {
  NSRect b = [self moreButtonRect];
  if (NSIsEmptyRect(b)) return;
  [[NSColor colorWithWhite:0 alpha:0.55] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:b xRadius:4 yRadius:4] fill];
  [NSColor.whiteColor setFill];
  for (int i = -1; i <= 1; ++i) {
    [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(NSMidX(b) + i * 6 - 1.5, NSMidY(b) - 1.5, 3, 3)] fill];
  }
}

- (void)showHandles:(BOOL)show {
  if (show != _handles) {
    _handles = show;
    self.needsDisplay = YES;
  }
  [show ? NSCursor.resizeLeftRightCursor : NSCursor.arrowCursor set];
}

// --- Drawing --------------------------------------------------------------------------------

- (void)drawRect:(NSRect)dirty {
  NSRect visible = self.visibleRect;
  [NSColor.textBackgroundColor setFill];
  NSRectFill(dirty);

  for (int r = 0; r < _doc->tracks(); ++r) [self drawRow:r];

  // The playhead, under the sticky ruler.
  CGFloat px = [self xOf:_playheadUs];
  [NSColor.labelColor setFill];
  NSRectFill(NSMakeRect(px - 0.5, NSMinY(visible), 1.5, NSHeight(visible)));

  if (_dropping) [self drawDropMark];
  [self drawRuler:visible];
}

- (void)drawRow:(int)r {
  int t = [self trackOfRow:r];
  const mf::SceneTrack& track = _doc->track(t);
  CGFloat top = [self rowTop:r], h = [self rowHeight:r];
  NSRect lane = NSMakeRect(kGutter, top + 2, self.bounds.size.width - kGutter, h - 4);
  [[NSColor.labelColor colorWithAlphaComponent:_sel->track == t && _sel->item < 0 ? 0.12 : 0.05] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:lane xRadius:4 yRadius:4] fill];

  NSDictionary* text = @{NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
                         NSForegroundColorAttributeName : [NSColor colorWithWhite:0.1 alpha:1]};
  for (int k = 0; k < int(track.items.size()); ++k) {
    const mf::SceneItem& it = track.items[k];
    NSRect box = [self rectOfItem:k row:r];
    bool selected = _sel->track == t && _sel->item == k;
    NSColor* fill = colorOf(it.type);
    if (selected) fill = [fill blendedColorWithFraction:0.25 ofColor:NSColor.whiteColor];
    if (!track.enabled) fill = [fill colorWithAlphaComponent:0.4];
    [fill setFill];
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:box xRadius:5 yRadius:5];
    [shape fill];
    if (it.type == mf::ItemType::Color) {  // a swatch of its color
      [[NSColor colorWithSRGBRed:it.color.r green:it.color.g blue:it.color.b alpha:it.color.a] setFill];
      NSRectFill(NSMakeRect(NSMinX(box) + 6, NSMidY(box) - 6, 12, 12));
    }
    [(selected ? NSColor.controlAccentColor : [NSColor colorWithWhite:0 alpha:0.25]) setStroke];
    shape.lineWidth = selected ? 3 : 1;
    [shape stroke];
    // The name starts after the swatch, and after the overlap with the item before (its transition).
    CGFloat left = NSMinX(box) + (it.type == mf::ItemType::Color ? 24 : selected ? 12 : 8);
    if (k > 0 && track.items[k - 1].endUs() > it.startUs) left = std::max(left, [self xOf:track.items[k - 1].endUs()] + 6);
    // Selected: the name at the top, its length at the bottom left.
    NSRect label = selected ? NSMakeRect(0, NSMinY(box) + 4, 0, 16) : NSInsetRect(box, 0, (box.size.height - 16) / 2);
    label.size.width = NSMaxX(box) - 4 - left;
    if (selected && !NSIsEmptyRect([self moreButtonRect])) label.size.width = NSMinX([self moreButtonRect]) - 4 - left;
    label.origin.x = left;
    if (label.size.width > 8) {
      [nameOf(it) drawWithRect:label options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                    attributes:text];
    }
    if (selected) {
      [self drawLength:it.durationUs in:box left:left];
      if (_handles) [self drawHandles:box];
      [self drawMoreButton];
    }
  }
  if (_sel->track == t && _sel->item < 0) [self drawMoreButton];

  // Transitions: a bow tie over the part where the two items overlap.
  for (const mf::SceneTransition& x : track.transitions) {
    const mf::SceneItem& b = track.items[x.from + 1];
    CGFloat cx = ([self xOf:b.startUs] + [self xOf:track.items[x.from].endUs()]) / 2, cy = top + h / 2, s = 7;
    NSBezierPath* tie = [NSBezierPath bezierPath];
    [tie moveToPoint:NSMakePoint(cx - s, cy - s)];
    [tie lineToPoint:NSMakePoint(cx + s, cy + s)];
    [tie lineToPoint:NSMakePoint(cx + s, cy - s)];
    [tie lineToPoint:NSMakePoint(cx - s, cy + s)];
    [tie closePath];
    [NSColor.whiteColor setFill];
    [tie fill];
    [[NSColor colorWithWhite:0 alpha:0.6] setStroke];
    [tie stroke];
  }
}

// The item's length in seconds, on a dark tag at the bottom left of its block.
- (void)drawLength:(int64_t)us in:(NSRect)box left:(CGFloat)left {
  NSString* length = [NSString stringWithFormat:@"%.1f s", us / kUs];
  NSDictionary* attrs = @{NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightSemibold],
                          NSForegroundColorAttributeName : NSColor.whiteColor};
  NSSize size = [length sizeWithAttributes:attrs];
  NSRect tag = NSMakeRect(left - 3, NSMaxY(box) - size.height - 5, size.width + 6, size.height + 1);
  if (NSMaxX(tag) > NSMaxX(box) - 4) return;  // too short to show it
  [[NSColor colorWithWhite:0 alpha:0.55] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:tag xRadius:3 yRadius:3] fill];
  [length drawAtPoint:NSMakePoint(NSMinX(tag) + 3, NSMinY(tag) + 0.5) withAttributes:attrs];
}

// Grips at both ends of the selected item.
- (void)drawHandles:(NSRect)box {
  for (int side = 0; side < 2; ++side) {
    NSRect grip = NSMakeRect(side ? NSMaxX(box) - kHandle : NSMinX(box), NSMinY(box), kHandle, NSHeight(box));
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:grip xRadius:3 yRadius:3];
    [NSColor.whiteColor setFill];
    [shape fill];
    [NSColor.controlAccentColor setStroke];
    shape.lineWidth = 1.5;
    [shape stroke];
    [[NSColor colorWithWhite:0.45 alpha:1] setFill];
    for (CGFloat dx : {-1.5, 1.5}) NSRectFill(NSMakeRect(NSMidX(grip) + dx - 0.5, NSMidY(grip) - 6, 1, 12));
  }
}

- (void)drawRuler:(NSRect)visible {
  NSRect bar = NSMakeRect(NSMinX(visible), NSMinY(visible), NSWidth(visible), kRulerHeight);
  [NSColor.windowBackgroundColor setFill];
  NSRectFill(bar);
  double step = 1;  // seconds between labels, at least 70 points apart
  for (double s : {0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0}) {
    step = s;
    if (s * _pixelsPerSecond >= 70) break;
  }
  NSDictionary* attrs = @{NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightRegular],
                          NSForegroundColorAttributeName : NSColor.secondaryLabelColor};
  double first = std::floor(std::max(0.0, (NSMinX(visible) - kGutter) / _pixelsPerSecond) / step) * step;
  double last = (NSMaxX(visible) - kGutter) / _pixelsPerSecond;
  [NSColor.tertiaryLabelColor setFill];
  for (double s = first; s <= last; s += step / 5) {
    bool major = std::fmod(s + 1e-9, step) < 1e-6;
    CGFloat x = std::round([self xOf:int64_t(s * kUs)]);
    NSRectFill(NSMakeRect(x, NSMaxY(bar) - (major ? 10 : 5), 1, major ? 10 : 5));
    if (major) {
      NSString* label = s >= 60 ? [NSString stringWithFormat:@"%d:%02d", int(s) / 60, int(s) % 60]
                                : [NSString stringWithFormat:step < 1 ? @"%.1fs" : @"%.0fs", s];
      [label drawAtPoint:NSMakePoint(x + 3, NSMinY(bar) + 3) withAttributes:attrs];
    }
  }
  // The playhead's handle.
  CGFloat px = [self xOf:_playheadUs];
  NSBezierPath* handle = [NSBezierPath bezierPath];
  [handle moveToPoint:NSMakePoint(px - 6, NSMaxY(bar) - 10)];
  [handle lineToPoint:NSMakePoint(px + 6, NSMaxY(bar) - 10)];
  [handle lineToPoint:NSMakePoint(px, NSMaxY(bar))];
  [handle closePath];
  [NSColor.labelColor setFill];
  [handle fill];
  [NSColor.separatorColor setFill];
  NSRectFill(NSMakeRect(NSMinX(bar), NSMaxY(bar) - 1, NSWidth(bar), 1));
}

// --- Dropping --------------------------------------------------------------------------------

// Where a drop at _dropAt lands: a line at its time, over its row (or all rows, below them).
- (void)drawDropMark {
  int r = [self rowAt:_dropAt.y];
  CGFloat x = std::max<CGFloat>(kGutter, _dropAt.x);
  NSRect line = r >= 0 ? NSMakeRect(x - 1, [self rowTop:r], 2, [self rowHeight:r])
                       : NSMakeRect(x - 1, kRulerHeight, 2, self.bounds.size.height - kRulerHeight);
  if (r >= 0) {
    [[NSColor.controlAccentColor colorWithAlphaComponent:0.15] setFill];
    NSRectFillUsingOperation(NSMakeRect(kGutter, [self rowTop:r] + 2, self.bounds.size.width - kGutter, [self rowHeight:r] - 4),
                             NSCompositingOperationSourceOver);
  }
  [NSColor.controlAccentColor setFill];
  NSRectFill(line);
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  return [self draggingUpdated:info];
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
  _dropping = YES;
  _dropAt = [self convertPoint:info.draggingLocation fromView:nil];
  self.needsDisplay = YES;
  return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)info {
  _dropping = NO;
  self.needsDisplay = YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  _dropping = NO;
  self.needsDisplay = YES;
  NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
  int r = p.y >= kRulerHeight ? [self rowAt:p.y] : -1;
  return [self.delegate timelineDrop:info atUs:[self usAt:p.x] track:r >= 0 ? [self trackOfRow:r] : -1];
}

// --- Mouse and keys -------------------------------------------------------------------------

- (void)mouseDown:(NSEvent*)event {
  [self.window makeFirstResponder:self];
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  NSRect visible = self.visibleRect;
  _drag = Drag::None;
  _downX = p.x;
  NSRect more = [self moreButtonRect];
  if (NSPointInRect(p, NSInsetRect(more, -2, -2))) {  // "…": the properties, and the playhead stays
    [self.delegate timelineShowProperties:more];
    return;
  }
  if (int edge = [self edgeAt:p]) {  // a handle of the selected item: trim, and the playhead stays
    const mf::SceneItem& it = _doc->item(_sel->track, _sel->item);
    _drag = edge < 0 ? Drag::TrimStart : Drag::TrimEnd;
    _origStartUs = it.startUs;
    _origDurationUs = it.durationUs;
    return;
  }
  // Any other click moves the playhead there, whatever else it does.
  bool inLanes = p.x >= NSMinX(visible) + kGutter;
  if (inLanes) [self.delegate timelineSeek:[self usAt:p.x]];
  if (p.y < NSMinY(visible) + kRulerHeight) {
    _drag = Drag::Seek;
    if (!inLanes) [self.delegate timelineSeek:[self usAt:NSMinX(visible) + kGutter]];
    return;
  }
  int r = [self rowAt:p.y];
  editor::Selection sel;
  if (r >= 0) {
    sel.track = [self trackOfRow:r];
    if (inLanes) {
      _drag = Drag::Seek;  // on no item: dragging scrubs, like in the ruler
      const std::vector<mf::SceneItem>& items = _doc->track(sel.track).items;
      for (int k = int(items.size()) - 1; k >= 0; --k) {  // a transition's second item is on top
        NSRect box = [self rectOfItem:k row:r];
        if (!NSPointInRect(p, box)) continue;
        sel.item = k;
        _drag = Drag::Move;
        _origStartUs = items[k].startUs;
        _origDurationUs = items[k].durationUs;
        break;
      }
    }
  }
  if (sel.track != _sel->track || sel.item != _sel->item) {
    *_sel = sel;
    _handles = NO;
    self.needsDisplay = YES;
    [self.delegate timelineSelectionChanged];
  }
}

- (void)mouseDragged:(NSEvent*)event {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  int64_t dUs = std::llround((p.x - _downX) / _pixelsPerSecond * kUs);
  switch (_drag) {
    case Drag::None: return;
    case Drag::Seek: [self.delegate timelineSeek:[self usAt:std::max(p.x, NSMinX(self.visibleRect) + kGutter)]]; return;
    case Drag::Move: _doc->moveItem(_sel->track, _sel->item, _origStartUs + dUs); break;
    case Drag::TrimStart: _doc->trimStart(_sel->track, _sel->item, _origDurationUs - dUs); break;  // right: shorter
    case Drag::TrimEnd: _doc->setDuration(_sel->track, _sel->item, _origDurationUs + dUs); break;
  }
  self.needsDisplay = YES;
  [self.delegate timelineEdited];
}

- (void)mouseUp:(NSEvent*)event {
  if (_drag == Drag::Move || _drag == Drag::TrimStart || _drag == Drag::TrimEnd) [self reload];  // the length may have changed
  _drag = Drag::None;
  [self mouseMoved:event];
}

- (void)mouseMoved:(NSEvent*)event {
  if (_drag != Drag::None) return;
  [self showHandles:[self edgeAt:[self convertPoint:event.locationInWindow fromView:nil]] != 0];
}

- (void)mouseExited:(NSEvent*)event {
  if (_drag == Drag::None) [self showHandles:NO];
}

- (void)keyDown:(NSEvent*)event {
  NSString* key = event.charactersIgnoringModifiers;
  if ([key isEqualToString:@" "]) {
    [self.delegate timelineTogglePlay];
  } else if (key.length && ([key characterAtIndex:0] == NSDeleteCharacter || [key characterAtIndex:0] == NSDeleteFunctionKey)) {
    [self.delegate timelineDeleteSelection];
  } else {
    [super keyDown:event];
  }
}

@end
