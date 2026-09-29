#pragma once

// The left pane: vertical tabs (Video, Image, Stickers, Emojis, Text, Audio, Project) and what
// the selected tab offers. It collapses to the tab bar: click the open tab, or the button at the
// bottom of the bar; any tab opens it again. Its content follows the pane's width. Video, Image
// and Audio list the files imported so far; Image also has solid colors. Stickers are SF
// Symbols in color, rendered once into PNG files and added as image items; emojis and text
// presets are added as text items. A click adds the item at the playhead; a drag carries it to
// the preview or the timeline, which ask draggedPayload for it. Project shows the project's
// settings.

#import <AppKit/AppKit.h>

#include "mf/scene.h"

// The pasteboard type of a drag out of the pane; what it carries is draggedPayload.
extern NSPasteboardType const SidebarDragType;

// What a click or a drag in the pane adds: a file (video, image or audio), or an item.
struct SidebarPayload {
  NSString* file = nil;
  bool hasItem = false;
  mf::SceneItem item;
  bool overlay = false;  // the item goes over the others (stickers, emojis, text), not in sequence
  bool empty() const { return !file && !hasItem; }
};

@protocol SidebarDelegate
- (void)sidebarAddFile:(NSString*)path;  // a video, image or audio file, onto the timeline
// overlay: over what's already there (stickers, emojis, text), on a track free at the
// playhead; else in sequence on the selected track (colors).
- (void)sidebarAddItem:(const mf::SceneItem&)item overlay:(BOOL)overlay;
- (void)sidebarCollapsedChanged;  // the pane's width changes: lay out what's beside it
@end

@interface SidebarView : NSView
@property(nonatomic, weak) id<SidebarDelegate> delegate;
@property(nonatomic) BOOL collapsed;  // setting it tells the delegate
+ (CGFloat)collapsedWidth;  // the tab bar alone
- (void)rememberFile:(NSString*)path;  // lists it under its tab (Video, Image or Audio)
- (void)setProjectView:(NSView*)view;  // what the Project tab shows
- (SidebarPayload)draggedPayload;       // during a drag out of the pane; empty otherwise
@end
