#import "inspector_view.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "mf/effects.h"

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

// The effect of this plugin type among `e`'s; with `add`, added at its defaults when missing.
mf::ScenePluginEffect* pluginEffect(mf::SceneEffects& e, const mf::EffectInfo& info, bool add) {
  for (mf::ScenePluginEffect& p : e.plugins) {
    if (p.type == info.type) return &p;
  }
  if (!add) return nullptr;
  mf::ScenePluginEffect p;
  p.type = info.type;
  for (const mf::EffectParamInfo& param : info.params) p.params.emplace_back(param.defaultValue);
  e.plugins.push_back(std::move(p));
  return &e.plugins.back();
}

NSString* capitalized(const std::string& name) {  // "smooth" → "Smooth"
  NSString* s = [NSString stringWithUTF8String:name.c_str()];
  return s.length ? [[s substringToIndex:1].uppercaseString stringByAppendingString:[s substringFromIndex:1]] : s;
}

// An effect the Effects tab offers: a built-in one, or a loaded plugin's.
struct EffectKind {
  std::string type;
  NSString* name;
  const mf::EffectInfo* plugin = nullptr;
};

EffectKind kindOf(const std::string& type) {
  if (type == "colorAdjust") return {type, @"Color Adjust"};
  if (type == "blur") return {type, @"Blur"};
  if (type == "crop") return {type, @"Crop"};
  if (type == "chromaKey") return {type, @"Chroma Key"};
  const mf::EffectInfo* info = mf::findEffect(type);
  return {type, capitalized(info && !info->displayName.empty() ? info->displayName : type), info};
}

// Every effect that can be added: the built-in ones, then each loaded plugin's.
std::vector<EffectKind> allEffects() {
  std::vector<EffectKind> out;
  for (const char* t : {"colorAdjust", "blur", "crop", "chromaKey"}) out.push_back(kindOf(t));
  for (const mf::EffectInfo* info : mf::registeredEffects()) out.push_back(kindOf(info->type));
  return out;
}

// The effects on `e`, in the order they apply (§4.4): crop, chroma key, color adjust, plugins, blur.
std::vector<EffectKind> appliedEffects(const mf::SceneEffects& e) {
  std::vector<EffectKind> out;
  if (e.crop) out.push_back(kindOf("crop"));
  if (e.chromaKey) out.push_back(kindOf("chromaKey"));
  if (e.colorAdjust) out.push_back(kindOf("colorAdjust"));
  for (const mf::ScenePluginEffect& p : e.plugins) out.push_back(kindOf(p.type));
  if (e.blur) out.push_back(kindOf("blur"));
  return out;
}

bool hasEffect(const mf::SceneEffects& e, const std::string& type) {
  for (const EffectKind& k : appliedEffects(e)) {
    if (k.type == type) return true;
  }
  return false;
}

// Adds the effect at its defaults, or takes it off (back to its defaults).
void setEffect(mf::SceneEffects& e, const EffectKind& k, bool on) {
  static const mf::SceneEffects fresh;
  if (k.type == "colorAdjust") {
    e.colorAdjust = on;
    e.brightness = fresh.brightness;
    e.contrast = fresh.contrast;
    e.saturation = fresh.saturation;
  } else if (k.type == "blur") {
    e.blur = on;
    e.blurRadius = mf::Animatable(0.01);  // a radius to see: the default, 0, shows nothing
  } else if (k.type == "crop") {
    e.crop = on;
    e.cropLeft = e.cropTop = e.cropRight = e.cropBottom = fresh.cropLeft;
  } else if (k.type == "chromaKey") {
    e.chromaKey = on;
    e.keyColor = {0, 1, 0, 1};  // green screen
    e.keyTolerance = fresh.keyTolerance;
    e.keySoftness = fresh.keySoftness;
  } else {
    auto& list = e.plugins;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const mf::ScenePluginEffect& p) { return p.type == k.type; }), list.end());
    if (on && k.plugin) pluginEffect(e, *k.plugin, true);
  }
}

NSString* effectNames(const mf::SceneEffects& e) {
  NSMutableArray* names = [NSMutableArray array];
  for (const EffectKind& k : appliedEffects(e)) [names addObject:k.name];
  return names.count ? [names componentsJoinedByString:@", "] : @"None";
}
}  // namespace

@implementation InspectorView {
  editor::Document* _doc;
  editor::Selection* _sel;
  NSStackView* _stack;
  NSMutableArray<Binding*>* _bindings;
  std::string _effectsShown;  // the effects page: what it was built for (the target and its effects)
  BOOL _rebuildPending;
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

- (void)setEffectsPage:(BOOL)effectsPage {
  _effectsPage = effectsPage;
  [self rebuild];
}

// The effects page's rows depend on which effects are on: when that changes (added, removed, or
// e.g. a crop dragged in the preview), the page is built again, just after this event, since the
// control that changed it may be one of its rows.
- (void)refresh {
  if (_effectsPage && [self effectsSignature] != _effectsShown && !_rebuildPending) {
    _rebuildPending = YES;
    __weak InspectorView* weak = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      InspectorView* s = weak;
      if (!s) return;
      s->_rebuildPending = NO;
      [s rebuild];
    });
  }
  for (Binding* b in _bindings) b.sync();
}

- (void)rebuild {
  for (NSView* v in _stack.arrangedSubviews) [v removeFromSuperview];
  [_bindings removeAllObjects];
  if (_effectsPage) [self buildEffects];
  else if (_sel->track < 0) [self buildProject];
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
  [self popup:title items:items width:kControlWidth get:get set:set];
}

- (void)popup:(NSString*)title items:(NSArray<NSString*>*)items width:(CGFloat)width get:(int (^)(void))get set:(void (^)(int))set {
  NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
  popup.controlSize = NSControlSizeSmall;
  [popup addItemsWithTitles:items];
  [popup.widthAnchor constraintEqualToConstant:width].active = YES;
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
  // Landscape and square, then portrait: 9:16 and 4:5, and typical phone screens (even sizes
  // only, as the engine needs: the iPhone 15/16 Pro's 1179 × 2556 is odd).
  struct Size {
    int width, height;
    NSString* label;
  };
  static const std::vector<Size> presets = {
      {1920, 1080, @"16:9"},    {1280, 720, @"16:9"},     {1080, 1080, @"1:1"},         {1080, 1350, @"4:5"},
      {1080, 1920, @"9:16"},    {720, 1280, @"9:16"},     {1080, 2340, @"Android"},     {1080, 2400, @"Android"},
      {1170, 2532, @"iPhone"},  {1284, 2778, @"iPhone Max"}, {1290, 2796, @"iPhone Pro Max"},
  };
  // A size from an opened document that isn't one of these is listed too, as it is.
  std::vector<Size> sizes = presets;
  const mf::SceneOutput& now = doc->scene.output;
  if (std::none_of(sizes.begin(), sizes.end(), [&](const Size& s) { return s.width == now.width && s.height == now.height; })) {
    sizes.push_back({now.width, now.height, @"this project"});
  }
  NSMutableArray* titles = [NSMutableArray array];
  for (const Size& s : sizes) [titles addObject:[NSString stringWithFormat:@"%d × %d  %@", s.width, s.height, s.label]];
  [self popup:@"Size"
      items:titles
      width:180
      get:^{
        for (size_t i = 0; i < sizes.size(); ++i) {
          if (doc->scene.output.width == sizes[i].width && doc->scene.output.height == sizes[i].height) return int(i);
        }
        return -1;
      }
      set:^(int i) {
        doc->scene.output.width = sizes[i].width;
        doc->scene.output.height = sizes[i].height;
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
    [self effectsSummary:[track]() -> mf::SceneEffects& { return track().effects; }];
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

// In the properties window: which effects are on. They're added and edited in the Effects tab.
- (void)effectsSummary:(EffectsRef)fx {
  [self section:@"Effects"];
  NSTextField* names = [NSTextField wrappingLabelWithString:@""];
  names.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
  [names.widthAnchor constraintEqualToConstant:kControlWidth + kValueWidth].active = YES;
  Binding* b = [Binding new];
  b.sync = ^{
    names.stringValue = effectNames(fx());
  };
  [_bindings addObject:b];
  [self row:@"Applied" views:@[ names ]];
  __weak InspectorView* weak = self;
  [self buttons:@[ @"Edit Effects…" ] actions:@[ ^{ [weak.delegate inspectorShowEffects]; } ]];
}

// The effects page's target: the selected visual item, or video track. Empty when there's none.
- (EffectsRef)effectsTarget:(NSString**)what {
  editor::Document* doc = _doc;
  int t = _sel->track, k = _sel->item;
  if (t < 0 || _sel->transition || t >= doc->tracks()) return {};
  if (k < 0) {
    if (!doc->track(t).video) return {};
    *what = @"Video track";
    return [doc, t]() -> mf::SceneEffects& { return doc->track(t).effects; };
  }
  const mf::SceneItem& it = doc->item(t, k);
  if (it.type == mf::ItemType::Audio) return {};
  static NSString* const kinds[] = {@"Video", @"Image", @"Text", @"Color", @"Audio"};
  NSString* detail = !it.src.empty() ? [NSString stringWithUTF8String:it.src.c_str()].lastPathComponent
                     : it.type == mf::ItemType::Text ? [NSString stringWithUTF8String:it.text.c_str()] : nil;
  *what = detail.length ? [NSString stringWithFormat:@"%@: %@", kinds[int(it.type)], detail] : kinds[int(it.type)];
  return [doc, t, k]() -> mf::SceneEffects& { return doc->item(t, k).effects; };
}

- (std::string)effectsSignature {
  NSString* what = nil;
  EffectsRef fx = [self effectsTarget:&what];
  std::string s = std::to_string(_sel->track) + "/" + std::to_string(_sel->item) + (_sel->transition ? "t" : "");
  if (fx) {
    for (const EffectKind& k : appliedEffects(fx())) s += " " + k.type;
  }
  return s;
}

- (void)note:(NSString*)text {
  NSTextField* label = [NSTextField wrappingLabelWithString:text];
  label.textColor = NSColor.secondaryLabelColor;
  [label.widthAnchor constraintEqualToConstant:kLabelWidth + kControlWidth + kValueWidth].active = YES;
  [_stack addArrangedSubview:label];
}

// An applied effect's heading, with a button that takes it off.
- (void)effectHeading:(NSString*)title remove:(void (^)(void))remove {
  [self section:title];
  NSTextField* label = (NSTextField*)_stack.arrangedSubviews.lastObject;
  [_stack removeArrangedSubview:label];
  NSButton* button = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"minus.circle" accessibilityDescription:@"Remove"]
                                        target:nil
                                        action:nil];
  button.bordered = NO;
  button.contentTintColor = NSColor.secondaryLabelColor;
  button.toolTip = [NSString stringWithFormat:@"Remove %@", title];
  __weak InspectorView* weak = self;
  Binding* b = [Binding new];
  b.action = ^(id) {
    remove();
    [weak edited];
  };
  b.sync = ^{
  };
  button.target = b;
  button.action = @selector(fire:);
  [_bindings addObject:b];
  NSView* spacer = [NSView new];
  [spacer setContentHuggingPriority:NSLayoutPriorityDefaultLow forOrientation:NSLayoutConstraintOrientationHorizontal];
  NSStackView* row = [NSStackView stackViewWithViews:@[ label, spacer, button ]];
  [row.widthAnchor constraintEqualToConstant:kLabelWidth + kControlWidth + kValueWidth + 12].active = YES;
  [_stack addArrangedSubview:row];
}

// The Effects tab: the effects on the selected item or video track, in the order they apply, each
// with its parameters and a remove button; and a menu of the effects to add (built in, and each
// loaded plugin's).
- (void)buildEffects {
  NSString* what = nil;
  EffectsRef fx = [self effectsTarget:&what];
  _effectsShown = [self effectsSignature];
  if (!fx) {
    [self note:@"Select a video, image, text or color item, or a video track, to add effects to it."];
    return;
  }
  [self section:what];

  // Add Effect: a pull-down of the effects not on yet (an effect goes on at most once, 8 in all).
  auto offered = std::make_shared<std::vector<EffectKind>>();
  for (const EffectKind& k : allEffects()) {
    if (!hasEffect(fx(), k.type)) offered->push_back(k);
  }
  NSPopUpButton* add = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:YES];
  add.controlSize = NSControlSizeSmall;
  [add.menu addItemWithTitle:@"Add Effect" action:nil keyEquivalent:@""];  // a pull-down's first item is its title
  for (const EffectKind& k : *offered) {
    NSMenuItem* item = [add.menu addItemWithTitle:k.name action:nil keyEquivalent:@""];
    item.toolTip = k.plugin ? @"From an effect plugin" : nil;
  }
  add.enabled = !offered->empty() && fx().count() < 8;
  [add.widthAnchor constraintEqualToConstant:kControlWidth].active = YES;
  EffectsRef f = fx;
  [self bind:add
      action:^(NSPopUpButton* p) {
        NSInteger i = p.indexOfSelectedItem - 1;
        if (i >= 0 && i < NSInteger(offered->size())) setEffect(f(), (*offered)[size_t(i)], true);
      }
        sync:^{
        }];
  [self row:@"" views:@[ add ]];

  std::vector<EffectKind> applied = appliedEffects(fx());
  if (applied.empty()) {
    [self note:@"No effects yet. Add one above; they apply in the order listed."];
    return;
  }
  for (const EffectKind& k : applied) {
    EffectKind kind = k;
    [self effectHeading:k.name
                 remove:^{
                   setEffect(f(), kind, false);
                 }];
    [self effectParams:kind of:f];
  }
}

// An applied effect's parameters.
- (void)effectParams:(const EffectKind&)k of:(EffectsRef)f {
  // The blocks copy `g`, a local: a block in a lambda would otherwise keep a reference to `f`.
  auto param = [&](NSString* name, double min, double max, mf::Animatable mf::SceneEffects::*value) {
    EffectsRef g = f;
    [self slider:name min:min max:max
             get:^{
               return (g().*value).value;
             }
             set:^(double v) {
               setConstant(g().*value, v);
             }];
  };
  if (k.type == "colorAdjust") {
    param(@"Brightness", -1, 1, &mf::SceneEffects::brightness);
    param(@"Contrast", 0, 2, &mf::SceneEffects::contrast);
    param(@"Saturation", 0, 2, &mf::SceneEffects::saturation);
  } else if (k.type == "blur") {
    param(@"Radius", 0, 0.1, &mf::SceneEffects::blurRadius);
  } else if (k.type == "crop") {  // at most 0.45 a side: never the whole image (R7)
    param(@"Left", 0, 0.45, &mf::SceneEffects::cropLeft);
    param(@"Top", 0, 0.45, &mf::SceneEffects::cropTop);
    param(@"Right", 0, 0.45, &mf::SceneEffects::cropRight);
    param(@"Bottom", 0, 0.45, &mf::SceneEffects::cropBottom);
  } else if (k.type == "chromaKey") {
    [self color:@"Key color"
            get:^{
              return f().keyColor;
            }
            set:^(const mf::Color& c) {
              f().keyColor = c;
            }];
    [self slider:@"Tolerance" min:0 max:1
             get:^{
               return double(f().keyTolerance);
             }
             set:^(double v) {
               f().keyTolerance = float(v);
             }];
    [self slider:@"Softness" min:0 max:1
             get:^{
               return double(f().keySoftness);
             }
             set:^(double v) {
               f().keySoftness = float(v);
             }];
  } else if (const mf::EffectInfo* info = k.plugin) {
    for (size_t j = 0; j < info->params.size(); ++j) {
      const mf::EffectParamInfo& p = info->params[j];
      double byDefault = p.defaultValue;
      [self slider:capitalized(p.name) min:p.min max:p.max
               get:^{
                 mf::ScenePluginEffect* e = pluginEffect(f(), *info, false);
                 return e ? e->params[j].value : byDefault;
               }
               set:^(double v) {
                 if (mf::ScenePluginEffect* e = pluginEffect(f(), *info, false)) setConstant(e->params[j], v);
               }];
    }
  } else {
    [self note:@"Its plugin isn't loaded: it's kept in the project, but not drawn or editable."];
  }
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
    [self check:@"Mirror"
            get:^{
              return item().transform.flipX;
            }
            set:^(bool on) {
              item().transform.flipX = on;
            }];
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
    [self effectsSummary:[item]() -> mf::SceneEffects& { return item().effects; }];
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
