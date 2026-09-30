#pragma once

// The camera window: record from the camera in segments, with stickers, emojis, text, effects and
// music (see editor::CaptureSession for the rules).
//
// Left, a pane with the Stickers, Emojis, Text, Effects and Music tabs. In the middle, the camera
// at the project's aspect ratio, filling it, with the overlays and effects drawn live
// (mf::LivePreview); overlays are moved, scaled, rotated and edited there as in the editor's
// preview, and the Delete key removes the selected one. At the bottom, the segments recorded
// against the maximum length, the camera, Mirror and Microphone, the music, Record (it starts and
// stops a segment), Delete Last, Cancel and Done.
//
// Each segment is recorded (mf::SegmentRecorder) into `folder`, with a journal (session.json)
// written after each one, so an unfinished recording can be recovered. The music plays while
// recording, from where the last segment left it. Effects, Mirror, the camera and the microphone
// are locked while recording. Done hands the session to the delegate to insert into the project;
// closing with segments recorded asks whether to add them, discard them, or keep recording.

#import <AppKit/AppKit.h>

#include "capture_session.h"
#include "mf/adapters.h"

@protocol CameraWindowDelegate
// Done: insert the recording into the project. NO (after telling the user why) keeps the window open.
- (BOOL)cameraWindowAdd:(const editor::CaptureSession&)session;
- (void)cameraWindowClosed;
@end

@interface CameraWindowController : NSWindowController <NSWindowDelegate>
@property(nonatomic, weak) id<CameraWindowDelegate> delegate;
// A new recording, its files in `folder` (made when the first segment is recorded); or, with a
// journal (read from `folder`/session.json), the recording it recovers. `audioFiles`: the
// project's imported audio, listed under Music.
- (instancetype)initWithPlatform:(mf::PlatformFactory*)platform
                          output:(const mf::SceneOutput&)output
                     maxDuration:(int64_t)maxDurationUs
                          folder:(NSString*)folder
                         journal:(const mf::Scene*)journal
                      audioFiles:(NSArray<NSString*>*)audioFiles;
- (void)setMaxDuration:(int64_t)maxDurationUs;  // chosen again in the Videos tab while open
+ (NSString*)journalName;                      // "session.json"
@end
