#import "preview_view.h"

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
