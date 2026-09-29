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
}

- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc selection:(editor::Selection*)selection {
  if ((self = [super initWithFrame:frame])) {
    _doc = doc;
    _sel = selection;
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

- (void)drawRect:(NSRect)dirty {
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
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  _drag = [self dragAt:p];
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
