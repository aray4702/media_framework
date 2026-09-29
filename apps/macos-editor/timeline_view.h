#pragma once

// The timeline: a ruler, then one row per track with the top layer first, each item as a
// block at its start and duration, a mark where a transition joins two items, and the playhead.
// Click an item to select it, or an empty part of a track's row to select the track. The
// selected item is highlighted with its length at its bottom left and a "…" button at its
// right end that opens its properties (a selected track has one at the right of its row); near
// either end it shows handles, which trim it when dragged.
// The start handle is a ripple trim: the item keeps its place, plays from later or earlier in
// its file, and the items after it on the track move by as much. Grabbing a handle leaves the
// playhead where it is. Drag an item to move it. Any other click in the lanes or the ruler
// moves the playhead there;
// dragging in the ruler or on empty space scrubs. Space plays and pauses; Delete
// removes the selection. Things dragged over it (types registered by the owner) show where they
// would land, and the delegate adds them.

#import <AppKit/AppKit.h>

#include "document.h"

@protocol TimelineDelegate
- (void)timelineSelectionChanged;
- (void)timelineEdited;  // the scene changed
- (void)timelineSeek:(int64_t)us;
- (void)timelineTogglePlay;
- (void)timelineDeleteSelection;
// The "…" button of the selected item or track was clicked: show its properties next to `button`
// (in the timeline's coordinates).
- (void)timelineShowProperties:(NSRect)button;
// Something was dropped at `us` on the row of `track` (-1: below the rows). YES: it was added.
- (BOOL)timelineDrop:(id<NSDraggingInfo>)info atUs:(int64_t)us track:(int)track;
@end

@interface TimelineView : NSView
@property(nonatomic, weak) id<TimelineDelegate> delegate;
@property(nonatomic) double pixelsPerSecond;
@property(nonatomic) int64_t playheadUs;
- (instancetype)initWithDocument:(editor::Document*)doc selection:(editor::Selection*)selection;
- (void)reload;  // the tracks or items changed: resize and redraw
@end
