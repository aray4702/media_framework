#pragma once

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

// The player draws into its CAMetalLayer. Resized, the last frame keeps its aspect ratio (never
// stretched) until `resized` has it drawn again at the new size.
@interface PreviewView : NSView
@property(nonatomic, copy) void (^resized)(void);
@end
