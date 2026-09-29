#import "preview_overlay.h"

#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr CGFloat kHandle = 8;         // corner and side handles, in points
constexpr CGFloat kRotateOffset = 26;  // the rotate handle, beyond the top edge
constexpr CGFloat kHit = 8;            // how near a handle a click takes it
constexpr double kMaxCrop = 0.45;      // a side: never the whole image (R7)

enum class Drag { None, Move, Scale, Rotate, CropLeft, CropTop, CropRight, CropBottom };

// An item as drawn: its box after crop, fit and scale, rotated about its anchor, in canvas pixels.
struct Box {
  NSPoint anchor;
  double w = 0, h = 0;    // scaled
  double ax = 0.5, ay = 0.5;
  double rad = 0;
  double visibleU = 1, visibleV = 1;  // the part of the source left after crop

  NSPoint at(double u, double v) const {  // the box's point at fractions (u, v)
    double dx = (u - ax) * w, dy = (v - ay) * h;
    return NSMakePoint(anchor.x + dx * std::cos(rad) - dy * std::sin(rad), anchor.y + dx * std::sin(rad) + dy * std::cos(rad));
  }
  bool contains(NSPoint p) const {
    double dx = p.x - anchor.x, dy = p.y - anchor.y;
    double lx = dx * std::cos(rad) + dy * std::sin(rad), ly = -dx * std::sin(rad) + dy * std::cos(rad);
    double u = ax + lx / w, v = ay + ly / h;
    return w > 0 && h > 0 && u >= 0 && u <= 1 && v >= 0 && v <= 1;
  }
};

// The text's rasterized size in canvas pixels, as the compositor makes it (§4.5).
NSSize textSize(const mf::SceneItem& it, double W, double H) {
  const mf::TextStyle& style = it.style;
  int lineHeight = std::max(4, int(std::lround(style.size * H)));
  int maxWidth = std::max(lineHeight, int(std::lround(style.maxWidth * W)));
  CGFloat points = lineHeight / 1.2;
  CTFontRef font;
  if (style.font == "system" || style.font == "system-bold") {
    font = CTFontCreateUIFontForLanguage(style.font == "system" ? kCTFontUIFontSystem : kCTFontUIFontEmphasizedSystem, points, nullptr);
  } else {
    CFStringRef name = CFStringCreateWithCString(nullptr, style.font.c_str(), kCFStringEncodingUTF8);
    font = CTFontCreateWithName(name, points, nullptr);
    CFRelease(name);
  }
  NSString* string = [NSString stringWithUTF8String:it.text.c_str()] ?: @"";
  NSAttributedString* text = [[NSAttributedString alloc] initWithString:string attributes:@{(id)kCTFontAttributeName : (__bridge id)font}];
  CFRelease(font);
  CTFramesetterRef setter = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)text);
  CGFloat pad = style.hasBox ? lineHeight * 0.3 : 2;
  CGSize fit = CTFramesetterSuggestFrameSizeWithConstraints(setter, CFRangeMake(0, 0), nullptr,
                                                            CGSizeMake(std::max<CGFloat>(1, maxWidth - 2 * pad), CGFLOAT_MAX), nullptr);
  CFRelease(setter);
  return NSMakeSize(std::ceil(fit.width + 2 * pad), std::ceil(fit.height + 2 * pad));
}

// A box's handles, in view coordinates: the corners (scale), the sides' middles (crop, in the
// order left, top, right, bottom), and the rotate handle, beyond the top edge's middle.
struct Handles {
  NSPoint corners[4], sides[4], rotate, topMiddle;
};

double distance(NSPoint a, NSPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }

void setConstant(mf::Animatable& a, double v) {
  a.keys.clear();
  a.value = v;
}
}  // namespace

@implementation PreviewOverlay {
  editor::Document* _doc;
  editor::Selection* _sel;
  NSTrackingArea* _tracking;
  Drag _drag;
  NSPoint _down;  // view coordinates
  Box _box;       // the selected item's, when the drag started
  double _x0, _y0, _scale0, _rotation0, _crop0;
  BOOL _dropping;  // something is dragged over
  NSCache<NSString*, NSImage*>* _images;  // by path, for drawing provisionally
  NSTextField* _editor;       // over a text item while its words are edited
  editor::Selection _edited;  // that item
  std::string _before;        // its text before, for Escape
}

- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc selection:(editor::Selection*)selection {
  if ((self = [super initWithFrame:frame])) {
    _doc = doc;
    _sel = selection;
    _images = [NSCache new];
    self.wantsLayer = YES;  // drawn over the preview's Metal layer
  }
  return self;
}

- (BOOL)isFlipped {
  return YES;
}

- (void)setTimeUs:(int64_t)us {
  if (us == _timeUs) return;
  _timeUs = us;
  self.needsDisplay = YES;
}

- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  if (_tracking) [self removeTrackingArea:_tracking];
  _tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                           options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                             owner:self
                                          userInfo:nil];
  [self addTrackingArea:_tracking];
}

// --- Geometry -------------------------------------------------------------------------------

// The canvas letterboxed into the view, as the player draws it: its scale and top-left corner.
- (double)viewScale {
  const mf::SceneOutput& o = _doc->scene.output;
  return std::min(self.bounds.size.width / o.width, self.bounds.size.height / o.height);
}

- (NSPoint)toView:(NSPoint)c {
  const mf::SceneOutput& o = _doc->scene.output;
  double k = [self viewScale];
  return NSMakePoint((self.bounds.size.width - o.width * k) / 2 + c.x * k, (self.bounds.size.height - o.height * k) / 2 + c.y * k);
}

- (bool)box:(Box*)box ofTrack:(int)t item:(int)k {
  const mf::SceneItem& it = _doc->item(t, k);
  double W = _doc->scene.output.width, H = _doc->scene.output.height;
  int64_t local = _timeUs - it.startUs;
  NSSize natural = NSMakeSize(W, H);  // a color fills the output
  switch (it.type) {
    case mf::ItemType::Video:
    case mf::ItemType::Image: natural = self.naturalSize ? self.naturalSize(it) : NSZeroSize; break;
    case mf::ItemType::Text: natural = textSize(it, W, H); break;
    case mf::ItemType::Color: break;
    case mf::ItemType::Audio: return false;
  }
  if (natural.width <= 0 || natural.height <= 0) return false;
  const mf::SceneEffects& e = it.effects;
  double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  if (e.crop) {
    u0 = e.cropLeft.at(local), v0 = e.cropTop.at(local), u1 = 1 - e.cropRight.at(local), v1 = 1 - e.cropBottom.at(local);
  }
  if (u1 <= u0 || v1 <= v0) return false;
  double cw = natural.width * (u1 - u0), ch = natural.height * (v1 - v0), bw = cw, bh = ch;
  mf::Fit fit = it.type == mf::ItemType::Text ? mf::Fit::None : it.fit;
  if (fit == mf::Fit::Contain || fit == mf::Fit::Cover) {
    double f = fit == mf::Fit::Contain ? std::min(W / cw, H / ch) : std::max(W / cw, H / ch);
    bw = cw * f;
    bh = ch * f;
  } else if (fit == mf::Fit::Fill) {
    bw = W;
    bh = H;
  }
  const mf::SceneTransform& tr = it.transform;
  double scale = tr.scale.at(local);
  box->anchor = NSMakePoint(tr.x.at(local) * W, tr.y.at(local) * H);
  box->w = bw * scale;
  box->h = bh * scale;
  box->ax = tr.anchorX;
  box->ay = tr.anchorY;
  box->rad = tr.rotation.at(local) * M_PI / 180;
  box->visibleU = u1 - u0;
  box->visibleV = v1 - v0;
  return true;
}

- (bool)visible:(int)t item:(int)k {
  const mf::SceneTrack& track = _doc->track(t);
  const mf::SceneItem& it = track.items[k];
  return track.video && track.enabled && it.type != mf::ItemType::Audio && _timeUs >= it.startUs && _timeUs < it.endUs();
}

// The selected item's box, when it's visible at the playhead.
- (bool)selectedBox:(Box*)box {
  return _sel->track >= 0 && _sel->item >= 0 && !_sel->transition && [self visible:_sel->track item:_sel->item] &&
         [self box:box ofTrack:_sel->track item:_sel->item];
}

- (Handles)handlesOf:(const Box&)b {
  Handles h;
  const double cu[4] = {0, 1, 1, 0}, cv[4] = {0, 0, 1, 1};
  for (int i = 0; i < 4; ++i) h.corners[i] = [self toView:b.at(cu[i], cv[i])];
  const double su[4] = {0, 0.5, 1, 0.5}, sv[4] = {0.5, 0, 0.5, 1};
  for (int i = 0; i < 4; ++i) h.sides[i] = [self toView:b.at(su[i], sv[i])];
  NSPoint center = [self toView:b.at(0.5, 0.5)];
  h.topMiddle = h.sides[1];
  double len = std::max(1.0, distance(h.topMiddle, center));
  h.rotate = NSMakePoint(h.topMiddle.x + (h.topMiddle.x - center.x) / len * kRotateOffset,
                         h.topMiddle.y + (h.topMiddle.y - center.y) / len * kRotateOffset);
  return h;
}

// What a press at p (view coordinates) on the selected item would do.
- (Drag)dragAt:(NSPoint)p {
  Box b;
  if (![self selectedBox:&b]) return Drag::None;
  Handles h = [self handlesOf:b];
  if (distance(p, h.rotate) <= kHit + 2) return Drag::Rotate;
  for (const NSPoint& c : h.corners) {
    if (distance(p, c) <= kHit) return Drag::Scale;
  }
  const Drag crops[4] = {Drag::CropLeft, Drag::CropTop, Drag::CropRight, Drag::CropBottom};
  for (int i = 0; i < 4; ++i) {
    if (distance(p, h.sides[i]) <= kHit) return crops[i];
  }
  double k = [self viewScale];
  NSPoint origin = [self toView:NSZeroPoint];
  return b.contains(NSMakePoint((p.x - origin.x) / k, (p.y - origin.y) / k)) ? Drag::Move : Drag::None;
}

// --- Drawing --------------------------------------------------------------------------------

- (void)setProvisional:(BOOL)provisional {
  _provisional = provisional;
  self.needsDisplay = YES;
}

// The selected item as the player will draw it: its image (cropped), text (in its style, on its
// box) or color, in its box, rotated and at its opacity. Effects and blend modes are left to the
// player's frame, which follows in a moment.
- (void)drawProvisional {
  Box b;
  if (![self selectedBox:&b]) return;
  const mf::SceneItem& it = _doc->item(_sel->track, _sel->item);
  int64_t local = _timeUs - it.startUs;
  double k = [self viewScale];
  NSPoint anchor = [self toView:b.anchor];
  NSRect box = NSMakeRect(-b.ax * b.w * k, -b.ay * b.h * k, b.w * k, b.h * k);  // about the anchor
  [NSGraphicsContext saveGraphicsState];
  NSAffineTransform* place = [NSAffineTransform transform];
  [place translateXBy:anchor.x yBy:anchor.y];
  [place rotateByRadians:b.rad];
  [place concat];
  CGContextSetAlpha(NSGraphicsContext.currentContext.CGContext, std::clamp(it.opacity.at(local), 0.0, 1.0));
  switch (it.type) {
    case mf::ItemType::Image: {
      NSImage* image = [self imageAt:it.src];
      const mf::SceneEffects& e = it.effects;
      double u0 = e.crop ? e.cropLeft.at(local) : 0, v0 = e.crop ? e.cropTop.at(local) : 0;
      NSSize size = image.size;
      NSRect from = NSMakeRect(u0 * size.width, (1 - v0 - b.visibleV) * size.height, b.visibleU * size.width, b.visibleV * size.height);
      [image drawInRect:box fromRect:from operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
      break;
    }
    case mf::ItemType::Text: {
      const mf::TextStyle& style = it.style;
      CGFloat points = style.size * _doc->scene.output.height * k / 1.2;
      if (style.hasBox) {
        [[NSColor colorWithSRGBRed:style.box.r green:style.box.g blue:style.box.b alpha:style.box.a] setFill];
        CGFloat pad = style.size * _doc->scene.output.height * k * 0.3;
        [[NSBezierPath bezierPathWithRoundedRect:box xRadius:pad yRadius:pad] fill];
      }
      NSFont* font = style.font == "system"        ? [NSFont systemFontOfSize:points]
                     : style.font == "system-bold" ? [NSFont boldSystemFontOfSize:points]
                                                   : [NSFont fontWithName:[NSString stringWithUTF8String:style.font.c_str()] size:points];
      NSMutableParagraphStyle* paragraph = [NSMutableParagraphStyle new];
      paragraph.alignment = style.align == mf::TextAlign::Left ? NSTextAlignmentLeft
                            : style.align == mf::TextAlign::Right ? NSTextAlignmentRight : NSTextAlignmentCenter;
      NSDictionary* attrs = @{
        NSFontAttributeName : font ?: [NSFont systemFontOfSize:points],
        NSForegroundColorAttributeName : [NSColor colorWithSRGBRed:style.color.r green:style.color.g blue:style.color.b alpha:style.color.a],
        NSParagraphStyleAttributeName : paragraph,
      };
      NSString* text = [NSString stringWithUTF8String:it.text.c_str()];
      NSRect inside = NSInsetRect(box, style.hasBox ? style.size * _doc->scene.output.height * k * 0.3 : 2, 0);
      NSRect used = [text boundingRectWithSize:NSMakeSize(inside.size.width, CGFLOAT_MAX) options:NSStringDrawingUsesLineFragmentOrigin attributes:attrs];
      inside.origin.y = NSMidY(box) - used.size.height / 2;  // centered in its box, as rasterized
      inside.size.height = used.size.height;
      [text drawWithRect:inside options:NSStringDrawingUsesLineFragmentOrigin attributes:attrs];
      break;
    }
    case mf::ItemType::Color:
      [[NSColor colorWithSRGBRed:it.color.r green:it.color.g blue:it.color.b alpha:it.color.a] setFill];
      NSRectFillUsingOperation(box, NSCompositingOperationSourceOver);
      break;
    default: break;  // a video: its frames come from the player
  }
  [NSGraphicsContext restoreGraphicsState];
}

// Images read once per file, for drawing provisionally.
- (NSImage*)imageAt:(const std::string&)src {
  NSString* path = [NSString stringWithUTF8String:src.c_str()];
  NSImage* image = [_images objectForKey:path];
  if (!image && (image = [[NSImage alloc] initWithContentsOfFile:path])) [_images setObject:image forKey:path];
  return image;
}

- (void)drawRect:(NSRect)dirty {
  if (_provisional) [self drawProvisional];
  if (_dropping) {  // the canvas, where a drop would go
    const mf::SceneOutput& o = _doc->scene.output;
    double k = [self viewScale];
    NSPoint origin = [self toView:NSZeroPoint];
    NSBezierPath* canvas = [NSBezierPath bezierPathWithRect:NSInsetRect(NSMakeRect(origin.x, origin.y, o.width * k, o.height * k), 2, 2)];
    const CGFloat dash[] = {6, 4};
    [canvas setLineDash:dash count:2 phase:0];
    canvas.lineWidth = 3;
    [NSColor.controlAccentColor setStroke];
    [canvas stroke];
  }
  Box b;
  if (![self selectedBox:&b]) return;
  Handles h = [self handlesOf:b];
  NSColor* accent = NSColor.controlAccentColor;

  NSBezierPath* outline = [NSBezierPath bezierPath];
  [outline moveToPoint:h.corners[0]];
  for (int i = 1; i < 4; ++i) [outline lineToPoint:h.corners[i]];
  [outline closePath];
  [[NSColor colorWithWhite:0 alpha:0.35] setStroke];  // a dark edge, so the outline shows on white too
  outline.lineWidth = 3.5;
  [outline stroke];
  [accent setStroke];
  outline.lineWidth = 1.5;
  [outline stroke];

  NSBezierPath* stem = [NSBezierPath bezierPath];
  [stem moveToPoint:h.topMiddle];
  [stem lineToPoint:h.rotate];
  stem.lineWidth = 1.5;
  [stem stroke];

  auto knob = [&](NSBezierPath* shape) {
    [NSColor.whiteColor setFill];
    [shape fill];
    [accent setStroke];
    shape.lineWidth = 1.5;
    [shape stroke];
  };
  for (const NSPoint& c : h.corners) {
    knob([NSBezierPath bezierPathWithRect:NSMakeRect(c.x - kHandle / 2, c.y - kHandle / 2, kHandle, kHandle)]);
  }
  for (int i = 0; i < 4; ++i) {  // crop: a bar along its side
    bool across = i == 0 || i == 2;
    NSPoint s = h.sides[i];
    NSRect bar = across ? NSMakeRect(s.x - 2.5, s.y - 8, 5, 16) : NSMakeRect(s.x - 8, s.y - 2.5, 16, 5);
    NSAffineTransform* turn = [NSAffineTransform transform];  // along the side, as the item turns
    [turn translateXBy:s.x yBy:s.y];
    [turn rotateByRadians:b.rad];
    [turn translateXBy:-s.x yBy:-s.y];
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:bar xRadius:2.5 yRadius:2.5];
    [shape transformUsingAffineTransform:turn];
    knob(shape);
  }
  knob([NSBezierPath bezierPathWithOvalInRect:NSMakeRect(h.rotate.x - 6, h.rotate.y - 6, 12, 12)]);
  NSImage* arrow = [NSImage imageWithSystemSymbolName:@"arrow.clockwise" accessibilityDescription:nil];
  [arrow drawInRect:NSMakeRect(h.rotate.x - 4, h.rotate.y - 4, 8, 8)];
}

// --- Editing text -----------------------------------------------------------------------------

// A field over the selected text item, upright at its center, in its font, color and alignment
// at the preview's scale.
- (void)editText {
  Box b;
  if (![self selectedBox:&b]) return;
  mf::SceneItem& it = _doc->item(_sel->track, _sel->item);
  const mf::TextStyle& style = it.style;
  double k = [self viewScale];
  CGFloat points = std::max(6.0, style.size * _doc->scene.output.height * k / 1.2);  // a line is about 1.2 × the font size
  NSFont* font = style.font == "system"        ? [NSFont systemFontOfSize:points]
                 : style.font == "system-bold" ? [NSFont boldSystemFontOfSize:points]
                                               : [NSFont fontWithName:[NSString stringWithUTF8String:style.font.c_str()] size:points];
  _editor = [[NSTextField alloc] initWithFrame:NSZeroRect];
  _editor.font = font ?: [NSFont systemFontOfSize:points];
  _editor.textColor = [NSColor colorWithSRGBRed:style.color.r green:style.color.g blue:style.color.b alpha:1];
  _editor.backgroundColor = [NSColor colorWithWhite:0 alpha:0.45];  // over the rendered text, which lags a little
  _editor.drawsBackground = YES;
  _editor.bordered = NO;
  _editor.focusRingType = NSFocusRingTypeExterior;
  _editor.alignment = style.align == mf::TextAlign::Left ? NSTextAlignmentLeft : style.align == mf::TextAlign::Right ? NSTextAlignmentRight
                                                                                                                   : NSTextAlignmentCenter;
  _editor.usesSingleLineMode = NO;
  _editor.cell.wraps = YES;
  _editor.cell.scrollable = NO;
  _editor.stringValue = [NSString stringWithUTF8String:it.text.c_str()];
  _editor.delegate = self;
  _edited = *_sel;
  _before = it.text;
  [self placeEditor];
  [self addSubview:_editor];
  [self.window makeFirstResponder:_editor];
  [_editor.currentEditor selectAll:nil];
  self.needsDisplay = YES;
}

// Over the item's box as it is now (it grows with the text, wrapping at the style's max width),
// centered on it, a little larger so the last characters typed don't wrap early.
- (void)placeEditor {
  Box b;
  if (!_editor || ![self selectedBox:&b]) return;
  double k = [self viewScale];
  NSPoint center = [self toView:b.at(0.5, 0.5)];
  CGFloat w = std::max<CGFloat>(80, b.w * k + 16), h = std::max<CGFloat>(_editor.font.pointSize * 1.4, b.h * k + 6);
  _editor.frame = NSMakeRect(center.x - w / 2, center.y - h / 2, w, h);
}

- (void)stopEditing {
  if (!_editor) return;
  NSTextField* field = _editor;
  _editor = nil;  // first: removing the field ends its editing, which calls back here
  [field removeFromSuperview];
  [self.window makeFirstResponder:self];
  self.needsDisplay = YES;
}

// The words as typed become the item's (not while empty: a text item needs some).
- (void)controlTextDidChange:(NSNotification*)note {
  if (!_editor || _editor.stringValue.length == 0 || _edited != *_sel) return;
  _doc->item(_edited.track, _edited.item).text = _editor.stringValue.UTF8String;
  [self placeEditor];
  self.needsDisplay = YES;  // the outline follows the new size
  [self.delegate overlayEdited];
}

- (void)controlTextDidEndEditing:(NSNotification*)note {
  [self stopEditing];
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)view doCommandBySelector:(SEL)command {
  if (command == @selector(insertNewline:)) {  // Return: done
    [self stopEditing];
    return YES;
  }
  if (command == @selector(insertLineBreak:) || command == @selector(insertNewlineIgnoringFieldEditor:)) {  // Option-Return
    [view insertNewlineIgnoringFieldEditor:nil];
    return YES;
  }
  if (command == @selector(cancelOperation:)) {  // Escape: the text as it was
    if (_edited == *_sel) {
      _doc->item(_edited.track, _edited.item).text = _before;
      [self.delegate overlayEdited];
    }
    [self stopEditing];
    return YES;
  }
  return NO;
}

// --- Dropping --------------------------------------------------------------------------------

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  _dropping = YES;
  self.needsDisplay = YES;
  return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)info {
  return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)info {
  _dropping = NO;
  self.needsDisplay = YES;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  _dropping = NO;
  self.needsDisplay = YES;
  NSPoint p = [self convertPoint:info.draggingLocation fromView:nil], origin = [self toView:NSZeroPoint];
  const mf::SceneOutput& o = _doc->scene.output;
  double k = [self viewScale];
  NSPoint position = NSMakePoint(std::clamp((p.x - origin.x) / k / o.width, 0.0, 1.0), std::clamp((p.y - origin.y) / k / o.height, 0.0, 1.0));
  return [self.delegate overlayDrop:info at:position];
}

// --- Mouse ----------------------------------------------------------------------------------

- (void)mouseMoved:(NSEvent*)event {
  switch ([self dragAt:[self convertPoint:event.locationInWindow fromView:nil]]) {
    case Drag::Move: [NSCursor.openHandCursor set]; break;
    case Drag::Scale:
    case Drag::Rotate: [NSCursor.crosshairCursor set]; break;
    case Drag::CropLeft:
    case Drag::CropRight: [NSCursor.resizeLeftRightCursor set]; break;
    case Drag::CropTop:
    case Drag::CropBottom: [NSCursor.resizeUpDownCursor set]; break;
    case Drag::None: [NSCursor.arrowCursor set]; break;
  }
}

- (void)mouseDown:(NSEvent*)event {
  [self stopEditing];
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  _drag = [self dragAt:p];
  if (_drag == Drag::Move && event.clickCount >= 2 && _doc->item(_sel->track, _sel->item).type == mf::ItemType::Text) {
    _drag = Drag::None;
    return [self editText];
  }
  if (_drag == Drag::None) {  // select what's under the click, top first
    editor::Selection sel;
    double k = [self viewScale];
    NSPoint origin = [self toView:NSZeroPoint], c = NSMakePoint((p.x - origin.x) / k, (p.y - origin.y) / k);
    for (int t = _doc->tracks() - 1; t >= 0 && sel.track < 0; --t) {
      for (int i = int(_doc->track(t).items.size()) - 1; i >= 0; --i) {
        Box b;
        if ([self visible:t item:i] && [self box:&b ofTrack:t item:i] && b.contains(c)) {
          sel = {t, i};
          break;
        }
      }
    }
    if (sel != *_sel) {
      *_sel = sel;
      self.needsDisplay = YES;
      [self.delegate overlaySelectionChanged];
    }
    if (sel.track < 0) return;
    _drag = Drag::Move;
  }
  // Where the drag starts from.
  [self selectedBox:&_box];
  mf::SceneItem& it = _doc->item(_sel->track, _sel->item);
  int64_t local = _timeUs - it.startUs;
  _down = p;
  _x0 = it.transform.x.at(local);
  _y0 = it.transform.y.at(local);
  _scale0 = it.transform.scale.at(local);
  _rotation0 = it.transform.rotation.at(local);
  const mf::SceneEffects& e = it.effects;
  switch (_drag) {
    case Drag::CropLeft: _crop0 = e.crop ? e.cropLeft.at(local) : 0; break;
    case Drag::CropTop: _crop0 = e.crop ? e.cropTop.at(local) : 0; break;
    case Drag::CropRight: _crop0 = e.crop ? e.cropRight.at(local) : 0; break;
    case Drag::CropBottom: _crop0 = e.crop ? e.cropBottom.at(local) : 0; break;
    default: break;
  }
  if (_drag == Drag::Move) [NSCursor.closedHandCursor set];
}

- (void)mouseDragged:(NSEvent*)event {
  if (_drag == Drag::None || _sel->item < 0 || _sel->transition) return;
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  mf::SceneItem& it = _doc->item(_sel->track, _sel->item);
  mf::SceneTransform& tr = it.transform;
  double k = [self viewScale], W = _doc->scene.output.width, H = _doc->scene.output.height;
  double dx = (p.x - _down.x) / k, dy = (p.y - _down.y) / k;  // canvas pixels
  NSPoint anchor = [self toView:_box.anchor];
  switch (_drag) {
    case Drag::Move:
      setConstant(tr.x, _x0 + dx / W);
      setConstant(tr.y, _y0 + dy / H);
      break;
    case Drag::Scale:
      setConstant(tr.scale, std::clamp(_scale0 * distance(p, anchor) / std::max(1.0, distance(_down, anchor)), 0.02, 100.0));
      break;
    case Drag::Rotate: {
      double turn = std::atan2(p.y - anchor.y, p.x - anchor.x) - std::atan2(_down.y - anchor.y, _down.x - anchor.x);
      double degrees = _rotation0 + turn * 180 / M_PI;
      if (event.modifierFlags & NSEventModifierFlagShift) degrees = std::round(degrees / 15) * 15;
      setConstant(tr.rotation, degrees);
      break;
    }
    default: {  // crop: the drag along the box's own axes, as a fraction of the source
      double lx = dx * std::cos(_box.rad) + dy * std::sin(_box.rad), ly = -dx * std::sin(_box.rad) + dy * std::cos(_box.rad);
      mf::SceneEffects& e = it.effects;
      if (!e.crop) {
        e.crop = true;
        for (mf::Animatable* side : {&e.cropLeft, &e.cropTop, &e.cropRight, &e.cropBottom}) setConstant(*side, 0);
      }
      auto crop = [&](mf::Animatable& side, double inward, double length, double visible) {
        setConstant(side, std::clamp(_crop0 + inward / std::max(1.0, length) * visible, 0.0, kMaxCrop));
      };
      if (_drag == Drag::CropLeft) crop(e.cropLeft, lx, _box.w, _box.visibleU);
      if (_drag == Drag::CropRight) crop(e.cropRight, -lx, _box.w, _box.visibleU);
      if (_drag == Drag::CropTop) crop(e.cropTop, ly, _box.h, _box.visibleV);
      if (_drag == Drag::CropBottom) crop(e.cropBottom, -ly, _box.h, _box.visibleV);
      break;
    }
  }
  self.needsDisplay = YES;
  [self.delegate overlayEdited];
}

- (void)mouseUp:(NSEvent*)event {
  _drag = Drag::None;
  [self mouseMoved:event];
}

@end
