#import "inspector_view.h"

#include <cmath>
#include <functional>

// Runs a block when its control acts, and another to show the model's current value.
@interface Binding : NSObject
@property(nonatomic, copy) void (^action)(id sender);
@property(nonatomic, copy) void (^sync)(void);
@end

@implementation Binding
- (void)fire:(id)sender {
  self.action(sender);
}
@end

@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped {
  return YES;
}
@end

namespace {
constexpr CGFloat kLabelWidth = 84, kControlWidth = 136, kValueWidth = 40;  // fits the left pane
constexpr double kUs = 1e6;

NSColor* toNS(const mf::Color& c) { return [NSColor colorWithSRGBRed:c.r green:c.g blue:c.b alpha:c.a]; }
mf::Color fromNS(NSColor* color) {
  NSColor* c = [color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
  return {float(c.redComponent), float(c.greenComponent), float(c.blueComponent), float(c.alphaComponent)};
}

// Edits here set a constant: any keyframes are dropped.
void setConstant(mf::Animatable& a, double v) {
  a.keys.clear();
  a.value = v;
}

using ItemRef = std::function<mf::SceneItem&()>;
using EffectsRef = std::function<mf::SceneEffects&()>;
}  // namespace

@implementation InspectorView {
  editor::Document* _doc;
  editor::Selection* _sel;
  NSStackView* _stack;
  NSMutableArray<Binding*>* _bindings;
}

- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc selection:(editor::Selection*)selection {
  if ((self = [super initWithFrame:frame])) {
    _doc = doc;
    _sel = selection;
    _bindings = [NSMutableArray array];
    NSColorPanel.sharedColorPanel.showsAlpha = YES;

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = YES;  // narrower than the rows (the left pane): scrolls sideways
    scroll.drawsBackground = NO;
    [self addSubview:scroll];

    FlippedView* content = [[FlippedView alloc] init];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = content;
    _stack = [NSStackView stackViewWithViews:@[]];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = 6;
    _stack.edgeInsets = NSEdgeInsetsMake(12, 14, 16, 14);
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_stack];
    NSView* clip = scroll.contentView;
    [NSLayoutConstraint activateConstraints:@[
      [content.topAnchor constraintEqualToAnchor:clip.topAnchor],
      [content.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
      [content.widthAnchor constraintGreaterThanOrEqualToAnchor:clip.widthAnchor],
      [_stack.topAnchor constraintEqualToAnchor:content.topAnchor],
      [_stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
      [_stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor],
      [_stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
    ]];
    NSLayoutConstraint* fill = [content.widthAnchor constraintEqualToAnchor:clip.widthAnchor];
    fill.priority = NSLayoutPriorityDefaultLow;  // the clip's width, unless the rows need more
    fill.active = YES;
    [self rebuild];
  }
  return self;
}

- (void)refresh {
  for (Binding* b in _bindings) b.sync();
}

- (void)rebuild {
  for (NSView* v in _stack.arrangedSubviews) [v removeFromSuperview];
  [_bindings removeAllObjects];
  if (_sel->track < 0) [self buildProject];
  else if (_sel->item < 0) [self buildTrack:_sel->track];
  else if (_sel->transition) [self buildTransition:_sel->item track:_sel->track];
  else [self buildItem:_sel->item track:_sel->track];
  [self refresh];
}

- (void)edited {
  [self.delegate inspectorEdited];
  [self refresh];
}

// --- Form rows ------------------------------------------------------------------------------

- (Binding*)bind:(NSControl*)control action:(void (^)(id))action sync:(void (^)(void))sync {
  Binding* b = [Binding new];
  __weak InspectorView* weak = self;
  b.action = ^(id sender) {
    action(sender);
    [weak edited];
  };
  b.sync = sync;
  control.target = b;
  control.action = @selector(fire:);
  [_bindings addObject:b];
  return b;
}

- (NSTextField*)label:(NSString*)text width:(CGFloat)width {
  NSTextField* label = [NSTextField labelWithString:text];
  label.textColor = NSColor.secondaryLabelColor;
  label.lineBreakMode = NSLineBreakByTruncatingTail;
  [label.widthAnchor constraintEqualToConstant:width].active = YES;
  return label;
}

- (void)row:(NSString*)title views:(NSArray<NSView*>*)views {
  NSMutableArray* all = [NSMutableArray arrayWithObject:[self label:title width:kLabelWidth]];
  [all addObjectsFromArray:views];
  NSStackView* row = [NSStackView stackViewWithViews:all];
  row.spacing = 6;
  [_stack addArrangedSubview:row];
}

- (void)section:(NSString*)title {
  NSTextField* label = [NSTextField labelWithString:title.uppercaseString];
  label.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
  label.textColor = NSColor.tertiaryLabelColor;
  if (_stack.arrangedSubviews.count) [_stack setCustomSpacing:18 afterView:_stack.arrangedSubviews.lastObject];
  [_stack addArrangedSubview:label];
}

- (void)slider:(NSString*)title min:(double)min max:(double)max get:(double (^)(void))get set:(void (^)(double))set {
  NSSlider* slider = [NSSlider sliderWithValue:min minValue:min maxValue:max target:nil action:nil];
  slider.continuous = YES;
  slider.controlSize = NSControlSizeSmall;
  [slider.widthAnchor constraintEqualToConstant:kControlWidth].active = YES;
  NSTextField* value = [self label:@"" width:kValueWidth];
  value.font = [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular];
  [self bind:slider
      action:^(NSSlider* s) {
        set(s.doubleValue);
      }
        sync:^{
          slider.doubleValue = get();
          value.stringValue = [NSString stringWithFormat:@"%.2f", get()];
        }];
  [self row:title views:@[ slider, value ]];
}

// A number typed in (seconds, or a plain value), applied on Return or when leaving the field.
- (void)number:(NSString*)title unit:(NSString*)unit get:(double (^)(void))get set:(void (^)(double))set {
  NSTextField* field = [NSTextField textFieldWithString:@""];
  field.controlSize = NSControlSizeSmall;
  field.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
  field.cell.sendsActionOnEndEditing = YES;
  [field.widthAnchor constraintEqualToConstant:80].active = YES;
  [self bind:field
      action:^(NSTextField* f) {
        set(f.doubleValue);
      }
        sync:^{
          if (!field.currentEditor) field.stringValue = [NSString stringWithFormat:@"%.2f", get()];
        }];
  [self row:title views:@[ field, [self label:unit width:kValueWidth] ]];
}

- (void)text:(NSString*)title get:(std::string (^)(void))get set:(void (^)(const std::string&))set {
  NSTextField* field = [NSTextField textFieldWithString:@""];
  field.cell.sendsActionOnEndEditing = YES;
  [field.widthAnchor constraintEqualToConstant:kControlWidth + kValueWidth].active = YES;
  [self bind:field
      action:^(NSTextField* f) {
        if (f.stringValue.length) set(f.stringValue.UTF8String);
      }
        sync:^{
          if (!field.currentEditor) field.stringValue = [NSString stringWithUTF8String:get().c_str()];
        }];
  [self row:title views:@[ field ]];
}

- (void)popup:(NSString*)title items:(NSArray<NSString*>*)items get:(int (^)(void))get set:(void (^)(int))set {
  NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  popup.controlSize = NSControlSizeSmall;
  [popup addItemsWithTitles:items];
  [popup.widthAnchor constraintEqualToConstant:kControlWidth].active = YES;
  [self bind:popup
      action:^(NSPopUpButton* p) {
        set(int(p.indexOfSelectedItem));
      }
        sync:^{
          [popup selectItemAtIndex:get()];
        }];
  [self row:title views:@[ popup ]];
}

- (void)check:(NSString*)title get:(bool (^)(void))get set:(void (^)(bool))set {
  [self check:title get:get set:set unavailable:nil];
}

// With `unavailable`, the box is grayed and off, and the reason shows next to it.
- (void)check:(NSString*)title get:(bool (^)(void))get set:(void (^)(bool))set unavailable:(NSString*)unavailable {
  NSButton* box = [NSButton checkboxWithTitle:title target:nil action:nil];
  box.enabled = !unavailable;
  [self bind:box
      action:^(NSButton* b) {
        set(b.state == NSControlStateValueOn);
      }
        sync:^{
          box.state = !unavailable && get() ? NSControlStateValueOn : NSControlStateValueOff;
        }];
  if (!unavailable) return [self row:@"" views:@[ box ]];
  NSTextField* note = [NSTextField labelWithString:unavailable];
  note.textColor = NSColor.secondaryLabelColor;
  note.font = [NSFont systemFontOfSize:11];
  [self row:@"" views:@[ box, note ]];
}

- (void)color:(NSString*)title get:(mf::Color (^)(void))get set:(void (^)(const mf::Color&))set {
  NSColorWell* well = [[NSColorWell alloc] initWithFrame:NSMakeRect(0, 0, 44, 22)];
  [well.widthAnchor constraintEqualToConstant:44].active = YES;
  [well.heightAnchor constraintEqualToConstant:22].active = YES;
  [self bind:well
      action:^(NSColorWell* w) {
        set(fromNS(w.color));
      }
        sync:^{
          well.color = toNS(get());
        }];
  [self row:title views:@[ well ]];
}

- (void)buttons:(NSArray<NSString*>*)titles actions:(NSArray*)actions {
  NSMutableArray* views = [NSMutableArray array];
  for (NSUInteger i = 0; i < titles.count; ++i) {
    NSButton* button = [NSButton buttonWithTitle:titles[i] target:nil action:nil];
    button.controlSize = NSControlSizeSmall;
    void (^run)(void) = actions[i];
    Binding* b = [Binding new];
    b.action = ^(id) {
      run();
    };
    b.sync = ^{
    };
    button.target = b;
    button.action = @selector(fire:);
    [_bindings addObject:b];
    [views addObject:button];
  }
  NSStackView* row = [NSStackView stackViewWithViews:views];
  row.spacing = 6;
  if (_stack.arrangedSubviews.count) [_stack setCustomSpacing:14 afterView:_stack.arrangedSubviews.lastObject];
  [_stack addArrangedSubview:row];
}

// A value in [min, max] that's an Animatable of the model.
- (void)animatable:(NSString*)title min:(double)min max:(double)max ref:(std::function<mf::Animatable&()>)ref {
  [self slider:title min:min max:max
           get:^{
             return ref().value;
           }
           set:^(double v) {
             setConstant(ref(), v);
           }];
}

// --- Sections -------------------------------------------------------------------------------

- (void)buildProject {
  editor::Document* doc = _doc;
  __weak InspectorView* weak = self;
  [self section:@"Project"];
  static const int sizes[][2] = {{1920, 1080}, {1080, 1920}, {1080, 1080}, {1280, 720}};
  [self popup:@"Size"
      items:@[ @"1920 × 1080 (16:9)", @"1080 × 1920 (9:16)", @"1080 × 1080 (1:1)", @"1280 × 720 (16:9)" ]
      get:^{
        for (int i = 0; i < 4; ++i) {
          if (doc->scene.output.width == sizes[i][0] && doc->scene.output.height == sizes[i][1]) return i;
        }
        return 0;
      }
      set:^(int i) {
        doc->scene.output.width = sizes[i][0];
        doc->scene.output.height = sizes[i][1];
      }];
  static const int rates[] = {24, 25, 30, 60};
  [self popup:@"Frame rate"
      items:@[ @"24 fps", @"25 fps", @"30 fps", @"60 fps" ]
      get:^{
        for (int i = 0; i < 4; ++i) {
          if (doc->scene.output.fpsNum == rates[i]) return i;
        }
        return 2;
      }
      set:^(int i) {
        doc->scene.output.fpsNum = rates[i];
        doc->scene.output.fpsDen = 1;
      }];
  [self color:@"Background"
          get:^{
            return doc->scene.output.background;
          }
          set:^(const mf::Color& c) {
            doc->scene.output.background = c;
          }];

  [self section:@"Filter (whole video)"];
  [self slider:@"Brightness" min:-1 max:1
           get:^{
             return double(doc->filter.brightness);
           }
           set:^(double v) {
             doc->filter.brightness = float(v);
           }];
  [self slider:@"Contrast" min:0 max:2
           get:^{
             return double(doc->filter.contrast);
           }
           set:^(double v) {
             doc->filter.contrast = float(v);
           }];

  [self section:@"Tracks"];
  auto add = ^(bool video) {
    int t = doc->addTrack(video);
    if (t < 0) return NSBeep();
    InspectorView* s = weak;
    *s->_sel = {t, -1};
    [s.delegate inspectorSelectionChanged];
  };
  [self buttons:@[ @"Add Video Track", @"Add Audio Track" ]
        actions:@[ ^{ add(true); }, ^{ add(false); } ]];
}

- (void)buildTrack:(int)t {
  editor::Document* doc = _doc;
  __weak InspectorView* weak = self;
  auto track = [doc, t]() -> mf::SceneTrack& { return doc->track(t); };
  bool video = track().video;
  [self section:video ? @"Video track" : @"Audio track"];
  [self check:@"Enabled"
          get:^{
            return track().enabled;
          }
          set:^(bool on) {
            track().enabled = on;
          }];
  if (video) {
    [self slider:@"Opacity" min:0 max:1
             get:^{
               return double(track().opacity);
             }
             set:^(double v) {
               track().opacity = float(v);
             }];
    [self effects:[track]() -> mf::SceneEffects& { return track().effects; } title:@"Track effects"];
  } else {
    [self slider:@"Gain" min:0 max:4
             get:^{
               return double(track().gain);
             }
             set:^(double v) {
               track().gain = float(v);
             }];
  }
  auto move = ^(int delta) {
    InspectorView* s = weak;
    s->_sel->track = doc->moveTrack(t, delta);
    [s.delegate inspectorSelectionChanged];
  };
  [self buttons:@[ @"Move Up", @"Move Down", @"Delete Track" ]
        actions:@[ ^{ move(+1); }, ^{ move(-1); }, ^{ [weak.delegate inspectorDeleteSelection]; } ]];
}

- (void)effects:(EffectsRef)fx title:(NSString*)title {
  [self section:title];
  // Changing a parameter turns its effect on.
  // The blocks copy `f`, a local: a block in a lambda would otherwise keep a reference to `fx`.
  auto param = [&](NSString* name, double min, double max, bool mf::SceneEffects::*on, mf::Animatable mf::SceneEffects::*value) {
    EffectsRef f = fx;
    [self slider:name min:min max:max
             get:^{
               return (f().*value).value;
             }
             set:^(double v) {
               f().*on = true;
               setConstant(f().*value, v);
             }];
  };
  auto toggle = [&](NSString* name, bool mf::SceneEffects::*on) {
    EffectsRef f = fx;
    [self check:name
            get:^{
              return f().*on;
            }
            set:^(bool v) {
              f().*on = v;
            }];
  };
  toggle(@"Color adjust", &mf::SceneEffects::colorAdjust);
  param(@"Brightness", -1, 1, &mf::SceneEffects::colorAdjust, &mf::SceneEffects::brightness);
  param(@"Contrast", 0, 2, &mf::SceneEffects::colorAdjust, &mf::SceneEffects::contrast);
  param(@"Saturation", 0, 2, &mf::SceneEffects::colorAdjust, &mf::SceneEffects::saturation);
  toggle(@"Blur", &mf::SceneEffects::blur);
  param(@"Radius", 0, 0.1, &mf::SceneEffects::blur, &mf::SceneEffects::blurRadius);
  toggle(@"Crop", &mf::SceneEffects::crop);  // at most 0.45 a side: never the whole image (R7)
  param(@"Left", 0, 0.45, &mf::SceneEffects::crop, &mf::SceneEffects::cropLeft);
  param(@"Top", 0, 0.45, &mf::SceneEffects::crop, &mf::SceneEffects::cropTop);
  param(@"Right", 0, 0.45, &mf::SceneEffects::crop, &mf::SceneEffects::cropRight);
  param(@"Bottom", 0, 0.45, &mf::SceneEffects::crop, &mf::SceneEffects::cropBottom);
  toggle(@"Chroma key", &mf::SceneEffects::chromaKey);
  [self color:@"Key color"
          get:^{
            return fx().keyColor;
          }
          set:^(const mf::Color& c) {
            fx().chromaKey = true;
            fx().keyColor = c;
          }];
  [self slider:@"Tolerance" min:0 max:1
           get:^{
             return double(fx().keyTolerance);
           }
           set:^(double v) {
             fx().chromaKey = true;
             fx().keyTolerance = float(v);
           }];
  [self slider:@"Softness" min:0 max:1
           get:^{
             return double(fx().keySoftness);
           }
           set:^(double v) {
             fx().chromaKey = true;
             fx().keySoftness = float(v);
           }];
}

- (void)buildItem:(int)k track:(int)t {
  editor::Document* doc = _doc;
  __weak InspectorView* weak = self;
  ItemRef item = [doc, t, k]() -> mf::SceneItem& { return doc->item(t, k); };
  mf::ItemType type = item().type;
  bool media = type == mf::ItemType::Video || type == mf::ItemType::Audio;
  static NSString* const names[] = {@"Video", @"Image", @"Text", @"Color", @"Audio"};
  [self section:names[int(type)]];
  if (!item().src.empty()) {
    NSTextField* file = [NSTextField labelWithString:[NSString stringWithUTF8String:item().src.c_str()].lastPathComponent];
    file.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [file.widthAnchor constraintLessThanOrEqualToConstant:kLabelWidth + kControlWidth + kValueWidth].active = YES;
    [_stack addArrangedSubview:file];
  }

  [self number:@"Start" unit:@"s"
           get:^{
             return item().startUs / kUs;
           }
           set:^(double v) {
             doc->moveItem(t, k, std::llround(v * kUs));
           }];
  [self number:@"Duration" unit:@"s"
           get:^{
             return item().durationUs / kUs;
           }
           set:^(double v) {
             doc->setDuration(t, k, std::llround(v * kUs));
           }];
  if (media) {
    [self number:@"In (trim)" unit:@"s"
             get:^{
               return item().inUs / kUs;
             }
             set:^(double v) {
               doc->setIn(t, k, std::llround(v * kUs));
             }];
    [self number:@"Speed" unit:@"×"
             get:^{
               return item().speed;
             }
             set:^(double v) {
               doc->setSpeed(t, k, v);
             }];
  }

  if (media) {  // near the top: whether a video plays its sound is a first thing to check
    [self section:@"Audio"];
    // A video's own sound can be muted only when its file has audio that plays.
    NSString* problem = type == mf::ItemType::Video && self.audioProblem ? self.audioProblem(item()) : nil;
    [self check:@"Mute"
            get:^{
              return item().mute;
            }
            set:^(bool on) {
              item().mute = on;
            }
    unavailable:problem];
    if (!problem) {
      [self animatable:@"Gain" min:0 max:4 ref:[item]() -> mf::Animatable& { return item().gain; }];
      [self animatable:@"Pan" min:-1 max:1 ref:[item]() -> mf::Animatable& { return item().pan; }];
    }
    if (type == mf::ItemType::Video && !problem) {  // its sound onto an audio track, to edit apart
      [self buttons:@[ @"Detach Audio" ]
            actions:@[ ^{
              InspectorView* s = weak;
              int video = t, sound = -1;
              int at = doc->detachAudio(&video, k, &sound);
              if (at < 0 || !s) return NSBeep();
              *s->_sel = {at, sound};
              [s.delegate inspectorSelectionChanged];
            } ]];
    }
  }

  if (type == mf::ItemType::Text) [self textItem:item];
  if (type == mf::ItemType::Color) {
    [self color:@"Color"
            get:^{
              return item().color;
            }
            set:^(const mf::Color& c) {
              item().color = c;
            }];
  }

  if (type != mf::ItemType::Audio) {
    [self section:@"Transform"];
    [self animatable:@"X" min:-0.5 max:1.5 ref:[item]() -> mf::Animatable& { return item().transform.x; }];
    [self animatable:@"Y" min:-0.5 max:1.5 ref:[item]() -> mf::Animatable& { return item().transform.y; }];
    [self animatable:@"Scale" min:0.1 max:4 ref:[item]() -> mf::Animatable& { return item().transform.scale; }];
    [self animatable:@"Rotation" min:-180 max:180 ref:[item]() -> mf::Animatable& { return item().transform.rotation; }];
    [self animatable:@"Opacity" min:0 max:1 ref:[item]() -> mf::Animatable& { return item().opacity; }];
    if (type != mf::ItemType::Text) {
      [self popup:@"Fit"
          items:@[ @"Contain", @"Cover", @"Fill", @"None" ]
          get:^{
            return int(item().fit);
          }
          set:^(int i) {
            item().fit = mf::Fit(i);
          }];
    }
    [self popup:@"Blend"
        items:@[ @"Normal", @"Add", @"Multiply", @"Screen" ]
        get:^{
          return int(item().blend);
        }
        set:^(int i) {
          item().blend = mf::Blend(i);
        }];
    [self effects:[item]() -> mf::SceneEffects& { return item().effects; } title:@"Effects"];
  }

  [self buttons:@[ @"Delete Item" ] actions:@[ ^{ [weak.delegate inspectorDeleteSelection]; } ]];
}

- (void)textItem:(ItemRef)item {
  [self section:@"Text"];
  [self text:@"Text"
         get:^{
           return item().text;
         }
         set:^(const std::string& s) {
           item().text = s;
         }];
  static NSArray<NSString*>* const fonts = @[ @"system", @"system-bold", @"Helvetica Neue", @"Georgia", @"Menlo", @"Marker Felt" ];
  [self popup:@"Font"
      items:fonts
      get:^{
        NSUInteger i = [fonts indexOfObject:[NSString stringWithUTF8String:item().style.font.c_str()]];
        return int(i == NSNotFound ? 0 : i);
      }
      set:^(int i) {
        item().style.font = fonts[i].UTF8String;
      }];
  [self slider:@"Size" min:0.02 max:0.3
           get:^{
             return double(item().style.size);
           }
           set:^(double v) {
             item().style.size = float(v);
           }];
  [self color:@"Color"
          get:^{
            return item().style.color;
          }
          set:^(const mf::Color& c) {
            item().style.color = c;
          }];
  [self popup:@"Align"
      items:@[ @"Left", @"Center", @"Right" ]
      get:^{
        return int(item().style.align);
      }
      set:^(int i) {
        item().style.align = mf::TextAlign(i);
      }];
  [self check:@"Background box"
          get:^{
            return item().style.hasBox;
          }
          set:^(bool on) {
            item().style.hasBox = on;
          }];
  [self color:@"Box color"
          get:^{
            return item().style.box;
          }
          set:^(const mf::Color& c) {
            item().style.hasBox = true;
            item().style.box = c;
          }];
}

// The join of items k - 1 and k: its transition, or none (a cut with no transition).
- (void)buildTransition:(int)k track:(int)t {
  editor::Document* doc = _doc;
  __weak InspectorView* weak = self;
  NSString* (^nameOf)(const mf::SceneItem&) = ^NSString*(const mf::SceneItem& it) {
    if (it.type == mf::ItemType::Text) return [NSString stringWithUTF8String:it.text.c_str()];
    return it.type == mf::ItemType::Color ? @"Color" : [NSString stringWithUTF8String:it.src.c_str()].lastPathComponent;
  };
  NSTextField* between = [NSTextField labelWithString:[NSString stringWithFormat:@"%@  →  %@", nameOf(doc->item(t, k - 1)), nameOf(doc->item(t, k))]];
  between.lineBreakMode = NSLineBreakByTruncatingMiddle;
  [between.widthAnchor constraintLessThanOrEqualToConstant:kLabelWidth + kControlWidth + kValueWidth].active = YES;
  [_stack addArrangedSubview:between];
  [self section:@"Transition"];
  // Its settings, kept while the transition is off so that turning it on again restores them.
  auto current = [doc, t, k]() {
    mf::SceneTransition x;
    x.kind = mf::SceneTransitionKind::Crossfade;
    x.durationUs = 1000000;
    if (const mf::SceneTransition* on = doc->transitionInto(t, k)) x = *on;
    return x;
  };
  [self popup:@"Kind"
      items:@[ @"None", @"Cut", @"Crossfade", @"Push", @"Slide", @"Wipe" ]
      get:^{
        const mf::SceneTransition* x = doc->transitionInto(t, k);
        return x ? int(x->kind) + 1 : 0;
      }
      set:^(int i) {
        if (i == 0) return doc->setTransition(t, k, nullptr);
        mf::SceneTransition x = current();
        x.kind = mf::SceneTransitionKind(i - 1);
        if (x.kind != mf::SceneTransitionKind::Cut && x.durationUs == 0) x.durationUs = 1000000;
        doc->setTransition(t, k, &x);
      }];
  [self popup:@"Direction"
      items:@[ @"Left", @"Right", @"Up", @"Down" ]
      get:^{
        return int(current().direction);
      }
      set:^(int i) {
        if (!doc->transitionInto(t, k)) return;
        mf::SceneTransition x = current();
        x.direction = mf::Direction(i);
        doc->setTransition(t, k, &x);
      }];
  [self number:@"Duration" unit:@"s"
           get:^{
             const mf::SceneTransition* x = doc->transitionInto(t, k);
             return x ? x->durationUs / kUs : 0.0;
           }
           set:^(double v) {
             if (!doc->transitionInto(t, k)) return;
             mf::SceneTransition x = current();
             x.durationUs = std::llround(std::max(0.0, v) * kUs);
             doc->setTransition(t, k, &x);
           }];
  // The easings of §4.3 that have names; any other shows as Linear until changed.
  static const mf::Easing easings[] = {mf::Easing{}, mf::Easing::bezier(0.42f, 0, 1, 1), mf::Easing::bezier(0, 0, 0.58f, 1),
                                       mf::Easing::bezier(0.42f, 0, 0.58f, 1)};
  [self popup:@"Easing"
      items:@[ @"Linear", @"Ease in", @"Ease out", @"Ease in-out" ]
      get:^{
        mf::Easing e = current().easing;
        for (int i = 1; i < 4; ++i) {
          const mf::Easing& n = easings[i];
          if (e.kind == mf::Easing::Kind::Bezier && e.x1 == n.x1 && e.y1 == n.y1 && e.x2 == n.x2 && e.y2 == n.y2) return i;
        }
        return 0;
      }
      set:^(int i) {
        if (!doc->transitionInto(t, k)) return;
        mf::SceneTransition x = current();
        x.easing = easings[i];
        doc->setTransition(t, k, &x);
      }];
  [self popup:@"Audio"
      items:@[ @"Equal gain", @"Equal power", @"Cut" ]
      get:^{
        return int(current().audio);
      }
      set:^(int i) {
        if (!doc->transitionInto(t, k)) return;
        mf::SceneTransition x = current();
        x.audio = mf::AudioFade(i);
        doc->setTransition(t, k, &x);
      }];
  [self buttons:@[ @"Remove Transition" ]
        actions:@[ ^{
          doc->setTransition(t, k, nullptr);
          [weak edited];
        } ]];
}

@end
