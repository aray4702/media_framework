#pragma once

// A transparent view over the preview. It outlines the selected item where it is drawn at the
// playhead, and lets the item be edited there: drag inside to move it, a corner to scale it,
// the round handle to rotate it (Shift: 15° steps), the middle of a side to crop that side.
// A click selects the topmost item under it, or nothing. Values are set as constants, like the
// properties do. Transition offsets (push, slide) are not taken into account. Things dragged
// over it (types registered by the owner) outline the canvas, and the delegate adds them.

#import <AppKit/AppKit.h>

#include "document.h"

@protocol PreviewOverlayDelegate
- (void)overlaySelectionChanged;
- (void)overlayEdited;  // the selected item's transform or crop changed
// Something was dropped at `position` (fractions of the output). YES: it was added.
- (BOOL)overlayDrop:(id<NSDraggingInfo>)info at:(NSPoint)position;
@end

@interface PreviewOverlay : NSView
@property(nonatomic, weak) id<PreviewOverlayDelegate> delegate;
@property(nonatomic) int64_t timeUs;  // the playhead: what's visible, and animated values
// The pixel size of a video or image item's source; zero when unknown.
@property(nonatomic, copy) NSSize (^naturalSize)(const mf::SceneItem& item);
- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc selection:(editor::Selection*)selection;
@end
