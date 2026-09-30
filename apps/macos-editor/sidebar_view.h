#pragma once

// The left pane: vertical tabs (Video, Image, Stickers, Emojis, Text, Audio, Effects, Project, Export)
// and what the selected tab offers. It collapses to the tab bar: click the open tab, or the
// button at the bottom of the bar; any tab opens it again. Its content follows the pane's width.
// Video, Image and Audio list the files imported so far; Image also has solid colors, and Audio
// a button to record a voice-over (the delegate records; `recording` shows it). Stickers are SF
// Symbols in color, rendered once into PNG files and added as image items; emojis and text
// presets are added as text items. A click adds the item at the playhead; a drag carries it to
// the preview or the timeline, which ask draggedPayload for it. Effects shows the selection's
// effects, and Project the project's settings (both views from the owner). Videos also has a Camera
// button, and next to it the camera recording's maximum length (kept across launches).
//
// The camera window has one too, with only some tabs (initWithFrame:tabs:) and the Audio tab as
// Music (musicMode): a click picks the music instead of adding it, and there's no voice-over.

#import <AppKit/AppKit.h>

#include "mf/scene.h"

// Which tabs a pane has (initWithFrame:tabs:), in this order.
typedef NS_OPTIONS(NSUInteger, SidebarTabs) {
  SidebarTabVideo = 1 << 0,
  SidebarTabImage = 1 << 1,
  SidebarTabStickers = 1 << 2,
  SidebarTabEmojis = 1 << 3,
  SidebarTabText = 1 << 4,
  SidebarTabAudio = 1 << 5,
  SidebarTabEffects = 1 << 6,
  SidebarTabProject = 1 << 7,
  SidebarTabExport = 1 << 8,
  SidebarTabsAll = (1 << 9) - 1,
};

// The pasteboard type of a drag out of the pane; what it carries is draggedPayload.
extern NSPasteboardType const SidebarDragType;

// What a click or a drag in the pane adds: a file (video, image or audio), or an item.
struct SidebarPayload {
  NSString* file = nil;
  bool hasItem = false;
  mf::SceneItem item;
  bool empty() const { return !file && !hasItem; }
};

@protocol SidebarDelegate
// Onto the timeline from the playhead: a video, image or audio file; several, one after another;
// or an item (a sticker, emoji, text or color).
- (void)sidebarAddFile:(NSString*)path;
- (void)sidebarAddFiles:(NSArray<NSString*>*)paths;
- (void)sidebarAddItem:(const mf::SceneItem&)item;
- (void)sidebarCollapsedChanged;  // the pane's width changes: lay out what's beside it
- (void)sidebarRecordVoiceOver;   // the Audio tab's record button: start, or stop, recording
@optional
- (void)sidebarOpenCamera:(int64_t)maxDurationUs;  // the Videos tab's Camera button, with the length chosen (0: unlimited)
- (void)sidebarDidSelectTab:(NSInteger)tab;  // a tab was opened (e.g. to restack views beside the preview)
@end

@interface SidebarView : NSView
@property(nonatomic, weak) id<SidebarDelegate> delegate;
@property(nonatomic) BOOL collapsed;  // setting it tells the delegate
@property(nonatomic) BOOL recording;  // a voice-over is being recorded: the record button stops it
@property(nonatomic) BOOL musicMode;  // the Audio tab is Music (see the top)
- (instancetype)initWithFrame:(NSRect)frame tabs:(SidebarTabs)tabs;  // initWithFrame: has them all
- (NSArray<NSString*>*)audioFiles;  // listed under Audio
+ (int64_t)cameraMaxDurationUs;     // the length chosen next to the Camera button (0: unlimited)
+ (CGFloat)collapsedWidth;  // the tab bar alone
- (void)rememberFile:(NSString*)path;  // lists it under its tab (Video, Image or Audio)
- (void)setEffectsView:(NSView*)view;  // what the Effects tab shows
- (void)showEffectsTab;                // opens the pane on the Effects tab
- (void)setProjectView:(NSView*)view;  // what the Project tab shows
- (void)setExportView:(NSView*)view;   // what the Export tab shows
- (SidebarPayload)draggedPayload;       // during a drag out of the pane; empty otherwise
// Shows `view` under `title` in place of the open tab's content, opening the pane if it's
// collapsed, until hidePanel (or a tab is clicked), which puts back what was there.
- (void)showPanel:(NSView*)view title:(NSString*)title;
- (void)hidePanel;
@end
