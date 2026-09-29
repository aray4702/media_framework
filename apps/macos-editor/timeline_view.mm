#import "timeline_view.h"

#include <algorithm>
#include <cmath>

#include "waveform.h"

namespace {
// Rows hold their items 4 points in from top and bottom: items 28 (video) and 24 (audio) high.
constexpr CGFloat kGutter = 10, kRulerHeight = 26, kVideoRow = 36, kAudioRow = 32;
constexpr CGFloat kHandle = 8, kHoverZone = 14;  // trim handles: their width, and how near an end shows them
constexpr CGFloat kMoreWidth = 24, kMoreHeight = 16;  // the "…" button
constexpr CGFloat kJunction = 16;                     // the transition button where two items meet
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
  mf::SceneTrack _origTrack;  // a start trim or a move applies to the track as it was when the drag began
  bool _recording;            // a voice-over is being recorded: shown from _recordStartUs
  int64_t _recordStartUs, _recordUs;
  std::vector<float> _recordLevels;  // one per 1/10 s
  int _toTrack;               // a move over another track that takes the item: that track, else -1
  int64_t _toStartUs;         // and where on it the item would land
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
  if (_sel->track < 0 || _sel->item < 0 || _sel->transition) return 0;
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
  if (_sel->track < 0 || _sel->transition) return NSZeroRect;
  int r = [self rowOfTrack:_sel->track];
  if (_sel->item < 0) {
    CGFloat top = [self rowTop:r], h = [self rowHeight:r];
    return NSMakeRect(NSMaxX(self.visibleRect) - kMoreWidth - 10, top + (h - kMoreHeight) / 2, kMoreWidth, kMoreHeight);
  }
  NSRect box = [self rectOfItem:_sel->item row:r];
  CGFloat right = NSMaxX(box) - kHoverZone - 2;
  if (_doc->junction(_sel->track, _sel->item + 1)) {  // clear of the transition button at its end
    right = std::min(right, NSMinX([self junctionRect:_sel->item + 1 row:r]) - 4);
  }
  if (right - kMoreWidth < NSMinX(box) + kHoverZone + 20) return NSZeroRect;
  return NSMakeRect(right - kMoreWidth, NSMidY(box) - kMoreHeight / 2, kMoreWidth, kMoreHeight);
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
  if (_drag == Drag::Move && _toTrack >= 0) [self drawLanding];
  if (_recording) [self drawRecording];
  [self drawRuler:visible];
}

- (void)drawRow:(int)r {
  int t = [self trackOfRow:r];
  const mf::SceneTrack& track = _doc->track(t);
  CGFloat top = [self rowTop:r], h = [self rowHeight:r];
  NSRect lane = NSMakeRect(kGutter, top + 2, self.bounds.size.width - kGutter, h - 4);
  NSColor* tint = track.video ? NSColor.labelColor : NSColor.systemGreenColor;  // audio rows: greener
  [[tint colorWithAlphaComponent:(_sel->track == t && _sel->item < 0 ? 0.12 : 0.05) * (track.video ? 1 : 1.6)] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:lane xRadius:4 yRadius:4] fill];

  // The selected item in front of the others (a transition overlaps two items), then the
  // transition buttons, then its trim handles and "…" button over everything.
  int chosen = _sel->track == t && _sel->item >= 0 && !_sel->transition ? _sel->item : -1;
  for (int k = 0; k < int(track.items.size()); ++k) {
    if (k != chosen) [self drawItem:k row:r selected:false];
  }
  if (chosen >= 0) [self drawItem:chosen row:r selected:true];
  for (const mf::SceneTransition& x : track.transitions) {  // the item underneath an overlap, dotted
    int a = x.from, b = x.from + 1, under = chosen == a ? b : a;
    CGFloat x0 = [self xOf:track.items[b].startUs], x1 = [self xOf:track.items[a].endUs()];
    if (x1 > x0) [self drawHidden:under row:r in:NSMakeRect(x0, [self rowTop:r], x1 - x0, [self rowHeight:r])];
  }
  for (int k = 1; k < int(track.items.size()); ++k) {  // where two items meet
    if (_doc->junction(t, k)) [self drawJunction:k row:r];
  }
  if (chosen >= 0 && _handles) [self drawHandles:[self rectOfItem:chosen row:r]];
  if (_sel->track == t && !_sel->transition) [self drawMoreButton];  // the item's, or the track's
}

// The part of item k's outline inside `overlap`, dotted: where another item covers it.
- (void)drawHidden:(int)k row:(int)r in:(NSRect)overlap {
  NSRect box = NSInsetRect([self rectOfItem:k row:r], 0.75, 0.75);
  [NSGraphicsContext saveGraphicsState];
  NSRectClip(overlap);
  NSBezierPath* outline = [NSBezierPath bezierPathWithRoundedRect:box xRadius:5 yRadius:5];
  const CGFloat dots[] = {0, 3.5};  // round dots: zero-length dashes with round caps
  [outline setLineDash:dots count:2 phase:0];
  outline.lineCapStyle = NSLineCapStyleRound;
  outline.lineWidth = 1.8;
  [[NSColor colorWithWhite:0.1 alpha:0.75] setStroke];
  [outline stroke];
  [NSGraphicsContext restoreGraphicsState];
}

// An item's block: its color, waveform (audio), and name; selected, lighter and outlined, with
// its length before the name.
- (void)drawItem:(int)k row:(int)r selected:(bool)selected {
  int t = [self trackOfRow:r];
  const mf::SceneTrack& track = _doc->track(t);
  NSDictionary* text = @{NSFontAttributeName : [NSFont systemFontOfSize:12 weight:NSFontWeightMedium],
                         NSForegroundColorAttributeName : [NSColor colorWithWhite:0.1 alpha:1]};
  const mf::SceneItem& it = track.items[k];
  NSRect box = [self rectOfItem:k row:r];
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
  if (it.type == mf::ItemType::Audio && self.peaks) {
    if (const std::vector<float>* peaks = self.peaks(it)) [self drawWaveform:*peaks of:it in:box];
  }
  // The name starts after the swatch, the overlap with the item before (its transition), and
  // the transition button where it meets that item.
  CGFloat left = NSMinX(box) + (it.type == mf::ItemType::Color ? 24 : selected ? 12 : 8);
  if (k > 0 && track.items[k - 1].endUs() > it.startUs) left = std::max(left, [self xOf:track.items[k - 1].endUs()] + 6);
  if (_doc->junction(t, k)) left = std::max(left, NSMinX(box) + kJunction / 2 + 5);  // clear of the button on the join
  // Selected: its length first, then the name, on one line.
  if (selected) left = [self drawLength:it.durationUs in:box left:left];
  NSRect label = NSInsetRect(box, 0, (box.size.height - 16) / 2);
  label.size.width = NSMaxX(box) - 4 - left;
  if (selected && !NSIsEmptyRect([self moreButtonRect])) label.size.width = NSMinX([self moreButtonRect]) - 4 - left;
  label.origin.x = left;
  if (label.size.width > 8) {
    [nameOf(it) drawWithRect:label options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
                  attributes:text];
  }
}

// The transition button between items k - 1 and k: "+" at a cut, a bow tie when a transition is
// set (filled), ringed while selected.
- (void)drawJunction:(int)k row:(int)r {
  int t = [self trackOfRow:r];
  NSRect b = [self junctionRect:k row:r];
  bool set = _doc->transitionInto(t, k), selected = _sel->track == t && _sel->item == k && _sel->transition;
  NSBezierPath* circle = [NSBezierPath bezierPathWithOvalInRect:b];
  [(set ? NSColor.controlAccentColor : NSColor.whiteColor) setFill];
  [circle fill];
  [(selected ? NSColor.labelColor : [NSColor colorWithWhite:0 alpha:0.35]) setStroke];
  circle.lineWidth = selected ? 2.5 : 1;
  [circle stroke];
  NSColor* mark = set ? NSColor.whiteColor : [NSColor colorWithWhite:0.25 alpha:1];
  CGFloat cx = NSMidX(b), cy = NSMidY(b), s = 4;
  NSBezierPath* glyph = [NSBezierPath bezierPath];
  if (set) {  // a bow tie
    [glyph moveToPoint:NSMakePoint(cx - s, cy - s)];
    [glyph lineToPoint:NSMakePoint(cx + s, cy + s)];
    [glyph lineToPoint:NSMakePoint(cx + s, cy - s)];
    [glyph lineToPoint:NSMakePoint(cx - s, cy + s)];
    [glyph closePath];
    [mark setFill];
    [glyph fill];
  } else {  // a plus
    [glyph moveToPoint:NSMakePoint(cx - s, cy)];
    [glyph lineToPoint:NSMakePoint(cx + s, cy)];
    [glyph moveToPoint:NSMakePoint(cx, cy - s)];
    [glyph lineToPoint:NSMakePoint(cx, cy + s)];
    glyph.lineWidth = 1.8;
    [mark setStroke];
    [glyph stroke];
  }
}

// Centered on the join: the middle of the overlap when a transition is set, else the cut.
- (NSRect)junctionRect:(int)k row:(int)r {
  const mf::SceneTrack& track = _doc->track([self trackOfRow:r]);
  CGFloat x = ([self xOf:track.items[k].startUs] + [self xOf:track.items[k - 1].endUs()]) / 2;
  CGFloat y = [self rowTop:r] + [self rowHeight:r] / 2;
  return NSMakeRect(x - kJunction / 2, y - kJunction / 2, kJunction, kJunction);
}

// The junction whose button is at p: its row's track and the second item, or NO.
- (BOOL)junctionAt:(NSPoint)p track:(int*)t item:(int*)k {
  int r = [self rowAt:p.y];
  if (r < 0) return NO;
  *t = [self trackOfRow:r];
  for (int i = 1; i < int(_doc->track(*t).items.size()); ++i) {
    NSRect b = [self junctionRect:i row:r];
    if (_doc->junction(*t, i) && std::hypot(p.x - NSMidX(b), p.y - NSMidY(b)) <= kJunction / 2 + 2) {
      *k = i;
      return YES;
    }
  }
  return NO;
}

// The audio item's waveform across its block: for each 2-point column, the loudest peak of the
// media time it covers (from `in`, at the item's speed).
- (void)drawWaveform:(const std::vector<float>&)peaks of:(const mf::SceneItem&)it in:(NSRect)box {
  NSRect shown = NSIntersectionRect(box, self.visibleRect);
  CGFloat mid = NSMidY(box), half = NSHeight(box) / 2 - 3;
  double perPoint = 1e6 / _pixelsPerSecond * it.speed;  // media µs per point
  [[NSColor colorWithWhite:0 alpha:0.3] setFill];
  for (CGFloat x = std::floor(NSMinX(shown)); x < NSMaxX(shown); x += 2) {
    double m0 = it.inUs + (x - NSMinX(box)) * perPoint, m1 = m0 + 2 * perPoint;
    size_t b0 = size_t(std::max(0.0, m0 * editor::kPeaksPerSecond / 1e6));
    size_t b1 = std::max(b0 + 1, size_t(std::max(0.0, m1 * editor::kPeaksPerSecond / 1e6)));
    float peak = 0;
    for (size_t b = b0; b < b1 && b < peaks.size(); ++b) peak = std::max(peak, peaks[b]);
    CGFloat h = std::max<CGFloat>(0.5, peak * half);
    NSRectFillUsingOperation(NSMakeRect(x, mid - h, 1.5, 2 * h), NSCompositingOperationSourceOver);
  }
}

// The item's length in seconds, on a dark tag at `left`, centered on the block. Returns where
// the name goes after it (`left` itself when the block is too short for the tag).
- (CGFloat)drawLength:(int64_t)us in:(NSRect)box left:(CGFloat)left {
  NSString* length = [NSString stringWithFormat:@"%.1f s", us / kUs];
  NSDictionary* attrs = @{NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:10 weight:NSFontWeightSemibold],
                          NSForegroundColorAttributeName : NSColor.whiteColor};
  NSSize size = [length sizeWithAttributes:attrs];
  NSRect tag = NSMakeRect(left - 3, NSMidY(box) - (size.height + 1) / 2, size.width + 6, size.height + 1);
  if (NSMaxX(tag) > NSMaxX(box) - 4) return left;
  [[NSColor colorWithWhite:0 alpha:0.55] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:tag xRadius:3 yRadius:3] fill];
  [length drawAtPoint:NSMakePoint(NSMinX(tag) + 3, NSMinY(tag) + 0.5) withAttributes:attrs];
  return NSMaxX(tag) + 6;
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

// --- Recording a voice-over -----------------------------------------------------------------

- (void)showRecordingFrom:(int64_t)startUs length:(int64_t)lengthUs levels:(const std::vector<float>&)levels {
  _recording = true;
  _recordStartUs = startUs;
  _recordUs = lengthUs;
  _recordLevels = levels;
  CGFloat bottom = [self rowTop:_doc->tracks()] + kAudioRow;  // room for a new track's row
  if (self.frame.size.height < bottom) [self setFrameSize:NSMakeSize(self.frame.size.width, bottom)];
  CGFloat end = [self xOf:startUs + lengthUs];
  if (end + 40 > self.frame.size.width) [self reload];  // wider as it grows
  self.needsDisplay = YES;
}

- (void)hideRecording {
  _recording = false;
  _recordLevels.clear();
  [self reload];
}

// The recording so far: a red block with its levels, where it will go.
- (void)drawRecording {
  int t = _doc->freeTrack(false, _recordStartUs, _recordStartUs + std::max<int64_t>(1, _recordUs));
  int r = t >= 0 ? [self rowOfTrack:t] : _doc->tracks();  // none free: a new track, below the others
  CGFloat top = [self rowTop:r], h = t >= 0 ? [self rowHeight:r] : kAudioRow;
  NSColor* red = NSColor.systemRedColor;
  if (t < 0) {  // the row the new track will have
    NSRect lane = NSMakeRect(kGutter, top + 2, self.bounds.size.width - kGutter, h - 4);
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:lane xRadius:4 yRadius:4];
    [[red colorWithAlphaComponent:0.06] setFill];
    [shape fill];
    const CGFloat dash[] = {4, 3};
    [shape setLineDash:dash count:2 phase:0];
    [[red colorWithAlphaComponent:0.4] setStroke];
    [shape stroke];
  }
  CGFloat x0 = [self xOf:_recordStartUs], x1 = std::max(x0 + 3, [self xOf:_recordStartUs + _recordUs]);
  NSRect box = NSMakeRect(x0, top + 4, x1 - x0, h - 8);
  NSBezierPath* block = [NSBezierPath bezierPathWithRoundedRect:box xRadius:5 yRadius:5];
  [[red colorWithAlphaComponent:0.3] setFill];
  [block fill];
  // The levels: a bar per 1/10 s, centered like the waveforms.
  [NSGraphicsContext saveGraphicsState];
  [block addClip];
  [[red colorWithAlphaComponent:0.85] setFill];
  CGFloat step = _pixelsPerSecond / 10, mid = NSMidY(box), half = NSHeight(box) / 2 - 3;
  for (size_t i = 0; i < _recordLevels.size(); ++i) {
    CGFloat level = std::max<CGFloat>(0.04, _recordLevels[i]);
    NSRectFill(NSMakeRect(x0 + i * step, mid - level * half, std::max<CGFloat>(1, step - 1), 2 * level * half));
  }
  [NSGraphicsContext restoreGraphicsState];
  [red setStroke];
  block.lineWidth = 1.5;
  [block stroke];
  NSString* label = t >= 0 ? @"● Recording" : @"● Recording  (new audio track)";
  NSDictionary* attrs = @{NSFontAttributeName : [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold], NSForegroundColorAttributeName : red};
  [label drawAtPoint:NSMakePoint(x1 + 6, NSMidY(box) - 7) withAttributes:attrs];
}

// --- Moving to another track -----------------------------------------------------------------

// Where an item put on track t at `us` lands: there, or right after the item playing then.
- (int64_t)landingOn:(int)t at:(int64_t)us {
  const std::vector<mf::SceneItem>& items = _doc->track(t).items;
  int k = 0;
  while (k < int(items.size()) && items[k].startUs <= us) ++k;
  return k > 0 ? std::max(us, items[k - 1].endUs()) : us;
}

// The moving item's outline, dashed, where it would land on the other track.
- (void)drawLanding {
  int r = [self rowOfTrack:_toTrack];
  CGFloat x0 = [self xOf:_toStartUs], x1 = [self xOf:_toStartUs + _origDurationUs];
  NSRect box = NSMakeRect(x0, [self rowTop:r] + 4, std::max<CGFloat>(4, x1 - x0), [self rowHeight:r] - 8);
  NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:box xRadius:5 yRadius:5];
  [[NSColor.controlAccentColor colorWithAlphaComponent:0.2] setFill];
  [shape fill];
  const CGFloat dash[] = {5, 3};
  [shape setLineDash:dash count:2 phase:0];
  shape.lineWidth = 2;
  [NSColor.controlAccentColor setStroke];
  [shape stroke];
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
  // Front first: the selected item's "…", the transition buttons (their handles hide under the
  // mouse), its trim zones, then the items.
  NSRect more = [self moreButtonRect];
  if (NSPointInRect(p, NSInsetRect(more, -2, -2))) {  // "…": the properties, and the playhead stays
    [self.delegate timelineShowProperties:more];
    return;
  }
  int jt, jk;
  if ([self junctionAt:p track:&jt item:&jk]) {  // select the join; the playhead stays
    editor::Selection sel{jt, jk, true};
    if (sel != *_sel) {
      *_sel = sel;
      _handles = NO;
      self.needsDisplay = YES;
      [self.delegate timelineSelectionChanged];
    }
    return;
  }
  if (int edge = [self edgeAt:p]) {  // a handle of the selected item: trim, and the playhead stays
    const mf::SceneItem& it = _doc->item(_sel->track, _sel->item);
    _drag = edge < 0 ? Drag::TrimStart : Drag::TrimEnd;
    _origStartUs = it.startUs;
    _origDurationUs = it.durationUs;
    _origTrack = _doc->track(_sel->track);
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
      int chosen = _sel->track == sel.track && _sel->item >= 0 && !_sel->transition ? _sel->item : -1;
      for (int i = int(items.size()); i >= 0; --i) {  // the selected item first (in front), then the later on top
        int k = i == int(items.size()) ? chosen : i;
        if (k < 0 || (i < int(items.size()) && k == chosen)) continue;
        NSRect box = [self rectOfItem:k row:r];
        if (!NSPointInRect(p, box)) continue;
        sel.item = k;
        _drag = Drag::Move;
        _origStartUs = items[k].startUs;
        _origDurationUs = items[k].durationUs;
        _origTrack = _doc->track(sel.track);
        _toTrack = -1;
        break;
      }
    }
  }
  if (sel != *_sel) {
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
    case Drag::Move: {
      // Over another track that takes the item: it stays where it was, and where it would land
      // there shows. Over its own: it moves along it.
      _doc->track(_sel->track) = _origTrack;
      int r = [self rowAt:p.y];
      int to = r >= 0 ? [self trackOfRow:r] : -1;
      bool visual = _doc->item(_sel->track, _sel->item).type != mf::ItemType::Audio;
      if (to >= 0 && to != _sel->track && _doc->track(to).video == visual) {
        _toTrack = to;
        _toStartUs = [self landingOn:to at:std::max<int64_t>(0, _origStartUs + dUs)];
      } else {
        _toTrack = -1;
        _doc->moveItem(_sel->track, _sel->item, _origStartUs + dUs);
      }
      break;
    }
    case Drag::TrimStart:  // right: shorter. From the drag's start, so dragging back gives the space back
      _doc->track(_sel->track) = _origTrack;
      _doc->trimStart(_sel->track, _sel->item, _origDurationUs - dUs);
      break;
    case Drag::TrimEnd: _doc->setDuration(_sel->track, _sel->item, _origDurationUs + dUs); break;
  }
  self.needsDisplay = YES;
  [self.delegate timelineEdited];
}

- (void)mouseUp:(NSEvent*)event {
  if (_drag == Drag::Move && _toTrack >= 0) {  // dropped on another track
    editor::Selection moved = *_sel;
    int k = _doc->moveToTrack(&moved.track, moved.item, _toTrack, _toStartUs);
    _toTrack = -1;
    if (k >= 0) {
      moved.item = k;
      *_sel = moved;
      [self.delegate timelineSelectionChanged];
      [self.delegate timelineEdited];
    }
  }
  if (_drag == Drag::Move || _drag == Drag::TrimStart || _drag == Drag::TrimEnd) [self reload];  // the length may have changed
  _drag = Drag::None;
  [self mouseMoved:event];
}

// Near an end of the selected item, its trim handles show; over a transition button they don't
// (the button takes the click).
- (void)mouseMoved:(NSEvent*)event {
  if (_drag != Drag::None) return;
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  int t, k;
  if ([self junctionAt:p track:&t item:&k]) {
    [self showHandles:NO];
    [NSCursor.pointingHandCursor set];
    return;
  }
  [self showHandles:[self edgeAt:p] != 0];
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
