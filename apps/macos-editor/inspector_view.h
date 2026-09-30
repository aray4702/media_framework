#pragma once

// The properties of the selection: an item (timing, audio, content, transform, which effects it
// has; a video has a Mute box, grayed when its file has no audio that plays), the join of two
// items (its transition: kind, direction, duration, easing, audio fade), a track
// (enabled, opacity or gain, which effects it has, order), or with nothing selected the project
// (output size, frame rate, background, the global filter). Values are constants: keyframes set
// elsewhere are replaced when a value is edited here.
//
// With effectsPage, it's the Effects tab instead: the effects of the selected visual item or
// video track, each with its parameters and a remove button, and a menu to add one (the built-in
// effects and each loaded plugin's).

#import <AppKit/AppKit.h>

#include "document.h"

@protocol InspectorDelegate
- (void)inspectorEdited;            // values changed
- (void)inspectorSelectionChanged;  // tracks were added or moved, and the selection with them
- (void)inspectorDeleteSelection;
- (void)inspectorShowEffects;       // open the Effects tab
@end

@interface InspectorView : NSView
@property(nonatomic, weak) id<InspectorDelegate> delegate;
// Why a video item's own audio can't play ("No audio track"), or nil when it can.
@property(nonatomic, copy) NSString* (^audioProblem)(const mf::SceneItem& item);
@property(nonatomic) BOOL effectsPage;  // the Effects tab; setting it rebuilds
- (instancetype)initWithFrame:(NSRect)frame document:(editor::Document*)doc selection:(editor::Selection*)selection;
- (void)rebuild;  // the selection changed
- (void)refresh;  // values changed elsewhere (e.g. an item dragged in the timeline)
@end
