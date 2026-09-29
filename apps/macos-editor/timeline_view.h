#pragma once

// The timeline: a ruler, then one row per track with the top layer first, each item as a
// block at its start and duration, a transition button where two videos or images meet, and the
// playhead. Clicking a transition button selects that join (Selection::transition). Where a
// transition overlaps two items, the one underneath shows as a dotted outline.
// Click an item to select it, or an empty part of a track's row to select the track. The
// selected item is highlighted with its length before its name and a "…" button at its
// right end that opens its properties (a selected track has one at the right of its row); near
// either end it shows handles, which trim it when dragged.
// The start handle trims into the file: dragged left, the item's start first moves into the free
// space before it; otherwise (and dragged right) it's a ripple trim, the item keeps its place and
// the items after it on the track move by as much. Grabbing a handle leaves the
// playhead where it is. Drag an item to move it. Any other click in the lanes or the ruler
// moves the playhead there;
// dragging in the ruler or on empty space scrubs. Space plays and pauses; Delete
// removes the selection. Audio rows are tinted, and audio items show their waveform. Things dragged over it (types registered by the owner) show where they
// would land, and the delegate adds them.

#import <AppKit/AppKit.h>

#include <vector>

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
// An audio item's file as peaks (editor::kPeaksPerSecond of media time), or null while loading.
@property(nonatomic, copy) const std::vector<float>* (^peaks)(const mf::SceneItem& item);
- (instancetype)initWithDocument:(editor::Document*)doc selection:(editor::Selection*)selection;
- (void)reload;  // the tracks or items changed: resize and redraw
@end
