#import "export_view.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "mf/exporter.h"
#include "mf/macos.h"

@interface ExportContent : NSView  // flipped, so the list starts at the top
@end
@implementation ExportContent
- (BOOL)isFlipped {
  return YES;
}
@end

@interface ExportView ()
- (void)exporter:(int)generation finished:(NSString*)error;  // nil: the file is written
- (void)cardChanged;
- (void)removeCard:(id)card;
@end

namespace {

struct Format {
  NSString* title;
  NSString* extension;
  mf::VideoCodec codec;
  NSString* codecName;  // in file names
};
const Format kFormats[] = {
    {@"MP4 (H.264)", @"mp4", mf::VideoCodec::H264, @"H.264"},
    {@"MP4 (HEVC)", @"mp4", mf::VideoCodec::HEVC, @"HEVC"},
    {@"MOV (H.264)", @"mov", mf::VideoCodec::H264, @"H.264"},
    {@"MOV (HEVC)", @"mov", mf::VideoCodec::HEVC, @"HEVC"},
};
// The project's size; a height for the shorter side at the project's aspect ratio; or a fixed
// size (as in the Project tab), whatever its aspect ratio.
struct Resolution {
  int shorter;  // at the project's aspect ratio (0: not)
  int width, height;
  NSString* label;
};
const Resolution kResolutions[] = {
    {0, 0, 0, nil},  // the project's
    {2160, 0, 0, nil},          {1080, 0, 0, nil},          {720, 0, 0, nil},          {480, 0, 0, nil},
    {0, 1920, 1080, @"16:9"},   {0, 1280, 720, @"16:9"},    {0, 1080, 1080, @"1:1"},   {0, 1080, 1350, @"4:5"},
    {0, 1080, 1920, @"9:16"},   {0, 720, 1280, @"9:16"},    {0, 1080, 2340, @"Android"}, {0, 1080, 2400, @"Android"},
    {0, 1170, 2532, @"iPhone"}, {0, 1284, 2778, @"iPhone Max"}, {0, 1290, 2796, @"iPhone Pro Max"},
};
const int kRates[] = {0, 24, 25, 30, 50, 60};       // 0: the project's rate

// Bits per pixel per frame for H.264 (HEVC: 40% less). High at 1080p30 is 7.5 Mb/s.
struct Quality {
  NSString* title;
  double bitsPerPixel;
};
const Quality kQualities[] = {{@"Low", 0.05}, {@"Medium", 0.08}, {@"High", 0.12}, {@"Maximum", 0.2}};
constexpr int kDefaultQuality = 2;

// Callbacks arrive on the exporter's threads: hop to the main thread. The generation drops a
// late callback from an export already cancelled.
class Watcher : public mf::ExportListener {
 public:
  Watcher(ExportView* view, int generation) : view_(view), generation_(generation) {}
  void onCompleted() override { post(nil); }
  void onError(mf::Result r, const std::string& reason) override {
    post([NSString stringWithFormat:@"%s: %s", mf::toString(r), reason.c_str()]);
  }

 private:
  void post(NSString* error) {
    __weak ExportView* weak = view_;
    int g = generation_;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weak exporter:g finished:error];
    });
  }
  __weak ExportView* view_;
  int generation_;
};

NSString* timeString(int64_t us) {
  int64_t tenths = us / 100000;
  return [NSString stringWithFormat:@"%lld:%02lld.%lld", tenths / 600, tenths / 10 % 60, tenths % 10];
}

// One file to write: where, and the scene at its size and rate with its encoder settings.
struct Job {
  NSURL* url;
  mf::Scene scene;
  mf::ExportSettings settings;
};

}  // namespace

// One output: its format, resolution (with, when its aspect ratio isn't the project's, how the
// project fits it and where a crop sits), frame rate and quality, and a line saying what they
// come to.
@interface OutputCard : NSView
@property(nonatomic, weak) ExportView* owner;
- (instancetype)initWithDocument:(editor::Document*)doc;
- (void)setNumber:(int)number removable:(BOOL)removable;
- (void)refresh;  // the project's size and rate in the menus, and the summary
- (void)setEnabled:(BOOL)enabled;
- (const Format&)format;
- (NSSize)size;
- (int)rate;
- (int)bitrate;
- (NSString*)fileSuffix;  // e.g. "720p25 HEVC", naming it among several outputs
- (BOOL)reshaped;           // its aspect ratio isn't the project's: framing applies
- (mf::FrameFit)frameFit;
- (float)cropX;
- (float)cropY;
@end

@implementation OutputCard {
  editor::Document* _doc;
  NSTextField* _title;
  NSButton* _remove;
  NSPopUpButton *_format, *_resolution, *_rate, *_quality, *_framing;
  NSSlider* _crop;  // Fill: where the crop sits, along the side that's cut
  NSTextField *_cropStart, *_cropEnd;
  NSTextField* _summary;
}

- (instancetype)initWithDocument:(editor::Document*)doc {
  if ((self = [super initWithFrame:NSZeroRect])) {
    _doc = doc;
    auto label = [](NSString* text) {
      NSTextField* l = [NSTextField labelWithString:text];
      l.textColor = NSColor.secondaryLabelColor;
      [l.widthAnchor constraintEqualToConstant:84].active = YES;
      return l;
    };
    auto popup = [&](NSArray<NSString*>* titles) {
      NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
      [p addItemsWithTitles:titles];
      p.target = self;
      p.action = @selector(changed:);
      [p.widthAnchor constraintEqualToConstant:180].active = YES;
      return p;
    };
    NSMutableArray *formats = [NSMutableArray array], *sizes = [NSMutableArray array],
                   *rates = [NSMutableArray arrayWithObject:@"Project"], *qualities = [NSMutableArray array];
    for (const Format& f : kFormats) [formats addObject:f.title];
    for (const Resolution& r : kResolutions) {
      [sizes addObject:r.shorter ? [NSString stringWithFormat:@"%dp", r.shorter]
                       : r.width ? [NSString stringWithFormat:@"%d × %d  %@", r.width, r.height, r.label]
                                 : @"Project"];
    }
    for (int r : kRates) {
      if (r) [rates addObject:[NSString stringWithFormat:@"%d fps", r]];
    }
    for (const Quality& q : kQualities) [qualities addObject:q.title];
    _format = popup(formats);
    _resolution = popup(sizes);
    _rate = popup(rates);
    _quality = popup(qualities);
    [_quality selectItemAtIndex:kDefaultQuality];
    _framing = popup(@[ @"Fit: show all, with bands", @"Fill: crop to cover" ]);
    _crop = [NSSlider sliderWithValue:0.5 minValue:0 maxValue:1 target:self action:@selector(changed:)];
    _crop.continuous = NO;
    _crop.controlSize = NSControlSizeSmall;
    [_crop.widthAnchor constraintEqualToConstant:120].active = YES;
    auto end = [](NSString* text) {
      NSTextField* l = [NSTextField labelWithString:text];
      l.font = [NSFont systemFontOfSize:10];
      l.textColor = NSColor.tertiaryLabelColor;
      return l;
    };
    _cropStart = end(@"Left");
    _cropEnd = end(@"Right");

    _title = [NSTextField labelWithString:@""];
    _title.font = [NSFont systemFontOfSize:11 weight:NSFontWeightSemibold];
    _title.textColor = NSColor.tertiaryLabelColor;
    _remove = [NSButton buttonWithImage:[NSImage imageWithSystemSymbolName:@"minus.circle" accessibilityDescription:@"Remove"]
                                 target:self
                                 action:@selector(remove:)];
    _remove.bordered = NO;
    _remove.toolTip = @"Remove this output";
    NSStackView* header = [NSStackView stackViewWithViews:@[ _title, _remove ]];
    _summary = [NSTextField wrappingLabelWithString:@""];
    _summary.textColor = NSColor.secondaryLabelColor;
    _summary.font = [NSFont systemFontOfSize:11];
    [_summary.widthAnchor constraintLessThanOrEqualToConstant:270].active = YES;

    auto row = [&](NSString* title, NSView* control) {
      NSStackView* r = [NSStackView stackViewWithViews:@[ label(title), control ]];
      r.spacing = 6;
      return r;
    };
    NSStackView* stack = [NSStackView stackViewWithViews:@[
      header, row(@"Format", _format), row(@"Resolution", _resolution), row(@"Framing", _framing),
      row(@"Crop position", [NSStackView stackViewWithViews:@[ _cropStart, _crop, _cropEnd ]]), row(@"Frame rate", _rate),
      row(@"Quality", _quality), _summary
    ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 8;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
      [stack.topAnchor constraintEqualToAnchor:self.topAnchor], [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
      [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor]
    ]];
    [self refresh];
  }
  return self;
}

- (void)setNumber:(int)number removable:(BOOL)removable {
  _title.stringValue = [NSString stringWithFormat:@"OUTPUT %d", number];
  _remove.hidden = !removable;
}

- (void)setEnabled:(BOOL)enabled {
  _format.enabled = _resolution.enabled = _rate.enabled = _quality.enabled = _remove.enabled = enabled;
  [self refresh];  // framing and crop: only while enabled, and when they apply
  if (!enabled) _framing.enabled = _crop.enabled = NO;
}

- (void)changed:(id)sender {
  [self refresh];
  [self.owner cardChanged];
}

- (void)remove:(id)sender {
  [self.owner removeCard:self];
}

- (const Format&)format {
  return kFormats[std::max<NSInteger>(0, _format.indexOfSelectedItem)];
}

// The project's size, a standard height for its shorter side at its aspect ratio (even), or a
// fixed size.
- (NSSize)size {
  const mf::SceneOutput& o = _doc->scene.output;
  const Resolution& r = kResolutions[std::max<NSInteger>(0, _resolution.indexOfSelectedItem)];
  if (r.width) return NSMakeSize(r.width, r.height);
  int h = r.shorter;
  if (h == 0) return NSMakeSize(o.width, o.height);
  double longer = double(h) * std::max(o.width, o.height) / std::min(o.width, o.height);
  int other = std::min(8192, int(std::lround(longer / 2)) * 2);
  return o.width >= o.height ? NSMakeSize(other, h) : NSMakeSize(h, other);
}

- (int)rate {
  int r = kRates[std::max<NSInteger>(0, _rate.indexOfSelectedItem)];
  const mf::SceneOutput& o = _doc->scene.output;
  return r ? r : std::max(1, int(std::lround(double(o.fpsNum) / std::max(1, o.fpsDen))));
}

- (int)bitrate {
  NSSize size = [self size];
  double bpp = kQualities[std::max<NSInteger>(0, _quality.indexOfSelectedItem)].bitsPerPixel;
  if ([self format].codec == mf::VideoCodec::HEVC) bpp *= 0.6;
  return std::max(500000, int(size.width * size.height * [self rate] * bpp));
}

- (NSString*)fileSuffix {
  NSSize size = [self size];
  if (kResolutions[std::max<NSInteger>(0, _resolution.indexOfSelectedItem)].width) {  // a fixed size: all of it
    return [NSString stringWithFormat:@"%.0fx%.0f %dfps %@", size.width, size.height, [self rate], [self format].codecName];
  }
  return [NSString stringWithFormat:@"%.0fp%d %@", std::min(size.width, size.height), [self rate], [self format].codecName];
}

- (BOOL)reshaped {
  const mf::SceneOutput& o = _doc->scene.output;
  NSSize size = [self size];
  return std::fabs(size.width / size.height - double(o.width) / o.height) > 0.005;
}

- (mf::FrameFit)frameFit {
  return _framing.indexOfSelectedItem == 1 ? mf::FrameFit::Fill : mf::FrameFit::Fit;
}

// A frame relatively taller than the project cuts its sides, so the crop moves left to right;
// a relatively wider one cuts top and bottom, and it moves top to bottom.
- (BOOL)cropsSides {
  const mf::SceneOutput& o = _doc->scene.output;
  NSSize size = [self size];
  return size.width / size.height < double(o.width) / o.height;
}

- (float)cropX {
  return [self cropsSides] ? float(_crop.doubleValue) : 0.5f;
}

- (float)cropY {
  return [self cropsSides] ? 0.5f : float(_crop.doubleValue);
}

- (void)refresh {
  const mf::SceneOutput& o = _doc->scene.output;
  [_resolution itemAtIndex:0].title = [NSString stringWithFormat:@"Project (%d × %d)", o.width, o.height];
  [_rate itemAtIndex:0].title = [NSString stringWithFormat:@"Project (%d fps)", int(std::lround(double(o.fpsNum) / std::max(1, o.fpsDen)))];
  [_resolution synchronizeTitleAndSelectedItem];
  [_rate synchronizeTitleAndSelectedItem];
  NSSize size = [self size];
  double seconds = _doc->scene.durationUs() / 1e6;
  BOOL reshaped = [self reshaped], fill = [self frameFit] == mf::FrameFit::Fill;
  _framing.enabled = _resolution.enabled && reshaped;  // the same shape: nothing to fit or crop
  _crop.enabled = _framing.enabled && fill;
  _cropStart.stringValue = [self cropsSides] ? @"Left" : @"Top";
  _cropEnd.stringValue = [self cropsSides] ? @"Right" : @"Bottom";
  NSString* shape = !reshaped ? @"" : fill ? @" · cropped" : @" · bands";
  _summary.stringValue = [NSString stringWithFormat:@"%.0f × %.0f%@ · %d fps · %.1f Mb/s · about %.0f MB", size.width, size.height, shape,
                                                    [self rate], [self bitrate] / 1e6, ([self bitrate] + 192000) * seconds / 8e6];
}

@end

@implementation ExportView {
  editor::Document* _doc;
  mf::PlatformFactory* _platform;
  NSStackView* _stack;
  NSMutableArray<OutputCard*>* _cards;
  NSTextField* _length;
  NSButton *_addButton, *_exportButton, *_cancelButton, *_revealButton;
  NSProgressIndicator* _progress;
  NSTextField* _status;

  std::vector<Job> _jobs;  // the files of this export, written one after another
  size_t _current;
  NSMutableArray<NSURL*>* _written;
  std::unique_ptr<Watcher> _watcher;
  std::unique_ptr<mf::Exporter> _exporter;
  int _generation;
  NSTimer* _timer;
}

- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc platform:(mf::PlatformFactory*)platform {
  if ((self = [super initWithFrame:frame])) {
    _doc = doc;
    _platform = platform;
    _cards = [NSMutableArray array];
    _written = [NSMutableArray array];

    _length = [NSTextField labelWithString:@""];
    _length.textColor = NSColor.secondaryLabelColor;
    _addButton = [NSButton buttonWithTitle:@"Add Output" image:[NSImage imageWithSystemSymbolName:@"plus" accessibilityDescription:nil]
                                    target:self
                                    action:@selector(addOutput:)];
    _addButton.imagePosition = NSImageLeading;
    _exportButton = [NSButton buttonWithTitle:@"Export…" target:self action:@selector(exportMovie:)];
    _exportButton.controlSize = NSControlSizeLarge;
    _progress = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    _progress.style = NSProgressIndicatorStyleBar;
    _progress.indeterminate = NO;
    _progress.minValue = 0;
    _progress.maxValue = 1;
    [_progress.widthAnchor constraintEqualToConstant:270].active = YES;
    _status = [NSTextField wrappingLabelWithString:@""];
    _status.font = [NSFont systemFontOfSize:12];
    [_status.widthAnchor constraintLessThanOrEqualToConstant:270].active = YES;
    _cancelButton = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancelExport:)];
    _revealButton = [NSButton buttonWithTitle:@"Show in Finder" target:self action:@selector(reveal:)];

    // Cards, then the buttons and progress, scrolling when there are many outputs.
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:self.bounds];
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    ExportContent* content = [[ExportContent alloc] init];
    content.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.documentView = content;
    _stack = [NSStackView stackViewWithViews:@[
      _length, _addButton, _exportButton, _progress, _status, [NSStackView stackViewWithViews:@[ _cancelButton, _revealButton ]]
    ]];
    _stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    _stack.alignment = NSLayoutAttributeLeading;
    _stack.spacing = 10;
    _stack.edgeInsets = NSEdgeInsetsMake(4, 14, 14, 14);
    _stack.translatesAutoresizingMaskIntoConstraints = NO;
    [content addSubview:_stack];
    NSView* clip = scroll.contentView;
    [NSLayoutConstraint activateConstraints:@[
      [content.topAnchor constraintEqualToAnchor:clip.topAnchor], [content.leadingAnchor constraintEqualToAnchor:clip.leadingAnchor],
      [content.widthAnchor constraintEqualToAnchor:clip.widthAnchor], [_stack.topAnchor constraintEqualToAnchor:content.topAnchor],
      [_stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor],
      [_stack.trailingAnchor constraintLessThanOrEqualToAnchor:content.trailingAnchor],
      [_stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor]
    ]];
    [self addSubview:scroll];
    [self addOutput:nil];
    [self showBusy:NO];
    _revealButton.hidden = YES;
  }
  return self;
}

// --- Outputs --------------------------------------------------------------------------------

- (void)addOutput:(id)sender {
  OutputCard* card = [[OutputCard alloc] initWithDocument:_doc];
  card.owner = self;
  [_cards addObject:card];
  [_stack insertArrangedSubview:card atIndex:_cards.count];  // after the length line and the cards before it
  [_stack setCustomSpacing:20 afterView:card];
  [self renumber];
}

- (void)removeCard:(id)card {
  if (_cards.count < 2 || _exporter) return;
  [_cards removeObject:card];
  [card removeFromSuperview];
  [self renumber];
}

- (void)renumber {
  for (NSUInteger i = 0; i < _cards.count; ++i) [_cards[i] setNumber:int(i + 1) removable:_cards.count > 1];
  [self refresh];
}

- (void)cardChanged {
  [self refresh];
}

- (void)refresh {
  for (OutputCard* card in _cards) [card refresh];
  _length.stringValue = _doc->empty() ? @"Add something to the timeline to export it."
                                      : [NSString stringWithFormat:@"%@ long, %lu file%@", timeString(_doc->scene.durationUs()),
                                                                   (unsigned long)_cards.count, _cards.count == 1 ? @"" : @"s"];
  _exportButton.enabled = !_exporter && !_doc->empty();
}

// --- Exporting ------------------------------------------------------------------------------

- (void)showBusy:(BOOL)busy {
  _progress.hidden = !busy;
  _cancelButton.hidden = !busy;
  _addButton.enabled = !busy;
  for (OutputCard* card in _cards) [card setEnabled:!busy];
  _exportButton.enabled = !busy && !_doc->empty();
}

// Asks for the name and folder: one output takes the name as it is; several each add what they
// are to it ("Trip 720p25 HEVC.mov").
- (void)exportMovie:(id)sender {
  NSSavePanel* panel = [NSSavePanel savePanel];
  if (_cards.count == 1) {
    const Format& format = [_cards[0] format];
    panel.allowedContentTypes = @[ [format.extension isEqualToString:@"mov"] ? UTTypeQuickTimeMovie : UTTypeMPEG4Movie ];
    panel.nameFieldStringValue = [@"Untitled" stringByAppendingPathExtension:format.extension];
  } else {
    panel.message = [NSString stringWithFormat:@"%lu files are named after this, one per output.", (unsigned long)_cards.count];
    panel.nameFieldStringValue = @"Untitled";
  }
  if ([panel runModal] != NSModalResponseOK) return;
  NSArray<NSURL*>* urls = [self urlsFor:panel.URL];
  NSMutableArray* existing = [NSMutableArray array];
  for (NSURL* url in urls) {
    if ([url checkResourceIsReachableAndReturnError:nil]) [existing addObject:url.lastPathComponent];
  }
  if (existing.count && urls.count > 1) {  // the panel asked about one name, not these
    NSAlert* alert = [NSAlert new];
    alert.messageText = [NSString stringWithFormat:@"Replace %lu existing file%@?", (unsigned long)existing.count, existing.count == 1 ? @"" : @"s"];
    alert.informativeText = [existing componentsJoinedByString:@"\n"];
    [alert addButtonWithTitle:@"Replace"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;
  }
  [self exportTo:urls];
}

// One URL per output: `chosen` itself for a single output, else its name plus each output's
// suffix, numbered when two outputs are alike.
- (NSArray<NSURL*>*)urlsFor:(NSURL*)chosen {
  if (_cards.count == 1) return @[ chosen ];
  NSURL* folder = chosen.URLByDeletingLastPathComponent;
  NSString* base = chosen.lastPathComponent.stringByDeletingPathExtension;
  NSMutableArray<NSURL*>* urls = [NSMutableArray array];
  NSMutableSet* names = [NSMutableSet set];
  for (OutputCard* card in _cards) {
    NSString* name = [NSString stringWithFormat:@"%@ %@", base, [card fileSuffix]];
    NSString* unique = name;
    for (int n = 2; [names containsObject:unique]; ++n) unique = [NSString stringWithFormat:@"%@ %d", name, n];
    [names addObject:unique];
    [urls addObject:[folder URLByAppendingPathComponent:[unique stringByAppendingPathExtension:[card format].extension]]];
  }
  return urls;
}

// The project as it is now, once per output, into those files.
- (void)exportTo:(NSArray<NSURL*>*)urls {
  _jobs.clear();
  for (NSUInteger i = 0; i < _cards.count && i < urls.count; ++i) {
    OutputCard* card = _cards[i];
    Job job{urls[i], _doc->scene, {}};
    // Drawn at the project's size, then scaled to the chosen one: every item keeps its size
    // relative to the frame (stickers and other items at their pixel size included).
    NSSize size = [card size];
    job.settings.frameWidth = int(size.width);
    job.settings.frameHeight = int(size.height);
    job.settings.frameFit = [card frameFit];  // another aspect ratio: bands of the background, or a crop
    job.settings.cropX = [card cropX];
    job.settings.cropY = [card cropY];
    job.scene.output.fpsNum = [card rate];
    job.scene.output.fpsDen = 1;
    job.settings.videoBitrate = [card bitrate];
    job.settings.codec = [card format].codec;
    _jobs.push_back(job);
  }
  [_written removeAllObjects];
  _revealButton.hidden = YES;
  _progress.doubleValue = 0;
  _current = 0;
  [self showBusy:YES];
  _timer = [NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(tick) userInfo:nil repeats:YES];
  [self startJob];
}

- (void)startJob {
  const Job& job = _jobs[_current];
  _watcher = std::make_unique<Watcher>(self, ++_generation);
  _exporter = mf::Exporter::create(*_platform, _watcher.get());
  std::string error;
  mf::Result r = _exporter->start(job.scene, mf::macos::exportTargetFromPath(job.url.path.UTF8String), job.settings, &error);
  if (r != mf::Result::Ok) {
    [self stop];
    return [self finishWith:[NSString stringWithFormat:@"Can't export %@: %s", job.url.lastPathComponent,
                                                       error.empty() ? mf::toString(r) : error.c_str()]];
  }
  [self tick];
}

- (void)tick {
  if (!_exporter) return;
  double done = (_current + _exporter->progress()) / _jobs.size();
  _progress.doubleValue = done;
  NSString* file = _jobs[_current].url.lastPathComponent;
  [self show:_jobs.size() == 1 ? [NSString stringWithFormat:@"Exporting %@… %.0f%%", file, done * 100]
                               : [NSString stringWithFormat:@"File %zu of %zu, %@… %.0f%%", _current + 1, _jobs.size(), file, done * 100]
       error:NO];
}

// Ends the current exporter; the timer and controls too unless another file follows.
- (void)stop {
  if (_exporter) _exporter->shutdown();  // joins its threads; no callback after this
  _exporter.reset();
  _watcher.reset();
  [_timer invalidate];
  _timer = nil;
  [self showBusy:NO];
}

- (void)exporter:(int)generation finished:(NSString*)error {
  if (generation != _generation || !_exporter) return;
  NSURL* url = _jobs[_current].url;
  _exporter->shutdown();
  _exporter.reset();
  _watcher.reset();
  if (error) {
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    [self stop];
    return [self finishWith:[NSString stringWithFormat:@"%@ failed. %@", url.lastPathComponent, error]];
  }
  [_written addObject:url];
  if (++_current < _jobs.size()) return [self startJob];
  [self stop];
  [self finishWith:nil];
}

// The outcome: what was written, and why it stopped when it did.
- (void)finishWith:(NSString*)problem {
  double bytes = 0;
  for (NSURL* url in _written) {
    NSNumber* size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    bytes += size.doubleValue;
  }
  NSString* written = _written.count == 1 ? [NSString stringWithFormat:@"Exported %@ (%.1f MB).", _written[0].lastPathComponent, bytes / 1e6]
                                          : [NSString stringWithFormat:@"Exported %lu files (%.1f MB).", (unsigned long)_written.count, bytes / 1e6];
  if (!problem) {
    [self show:written error:NO];
  } else {
    [self show:_written.count ? [NSString stringWithFormat:@"%@ %@", problem, written] : problem error:YES];
  }
  _revealButton.hidden = _written.count == 0;
}

- (void)cancelExport:(id)sender {
  [self cancel];
}

- (void)cancel {
  if (!_exporter) return;
  NSURL* unfinished = _jobs[_current].url;
  [self stop];
  [[NSFileManager defaultManager] removeItemAtURL:unfinished error:nil];  // an unfinished file is no use
  [self finishWith:@"Export cancelled."];
  if (_written.count) _status.textColor = NSColor.labelColor;
}

- (void)reveal:(id)sender {
  if (_written.count) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:_written];
}

- (void)show:(NSString*)text error:(BOOL)error {
  _status.stringValue = text;
  _status.textColor = error ? NSColor.systemRedColor : NSColor.labelColor;
}

@end
