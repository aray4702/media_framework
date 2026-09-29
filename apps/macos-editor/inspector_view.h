#pragma once

// The properties of the selection: an item (timing, audio, content, transform, effects; a video
// has a Mute box, grayed when its file has no audio that plays), the join of two
// items (its transition: kind, direction, duration, easing, audio fade), a track
// (enabled, opacity or gain, effects, order), or with nothing selected the project (output size,
// frame rate, background, the global filter). Values are constants: keyframes set elsewhere are
// replaced when a value is edited here.

#import <AppKit/AppKit.h>

#include "document.h"

@protocol InspectorDelegate
- (void)inspectorEdited;            // values changed
- (void)inspectorSelectionChanged;  // tracks were added or moved, and the selection with them
- (void)inspectorDeleteSelection;
@end

@interface InspectorView : NSView
@property(nonatomic, weak) id<InspectorDelegate> delegate;
// Why a video item's own audio can't play ("No audio track"), or nil when it can.
@property(nonatomic, copy) NSString* (^audioProblem)(const mf::SceneItem& item);
- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc selection:(editor::Selection*)selection;
- (void)rebuild;  // the selection changed
- (void)refresh;  // values changed elsewhere (e.g. an item dragged in the timeline)
@end
