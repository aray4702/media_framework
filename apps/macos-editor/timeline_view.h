#pragma once

// The timeline: a ruler, then one row per track with the top layer first (audio rows tinted), each
// item a block at its start and duration (audio items with their waveform), a transition button
// where two videos or images meet, and the playhead. Where a transition overlaps two items, the
// one underneath shows as a dotted outline.
//
// Click an item to select it (it's drawn in front, with its length and a "…" button for its
// properties), an empty part of a row to select the track (its "…" is at the row's right), or a
// transition button to select that join (Selection::transition). Near either end of the selected
// item, handles trim it: the end one changes its length; the start one trims into the file,
// dragged left first growing into free space before the item, otherwise a ripple trim that
// keeps its place and moves the items after it. Drag an item to move it along its track, or
// onto another track that takes it (a dashed outline shows where it lands). Any other click
// moves the playhead; dragging in the ruler or on empty space scrubs. Grabbing a handle or a
// button leaves the playhead where it is. Space plays and pauses; Delete removes the selection.
// Things dragged over it (types registered by the owner) show where they would land, and the
// delegate adds them.

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
