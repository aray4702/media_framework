#pragma once

// Records the microphone into an AAC .m4a file (48 kHz mono) for a voice-over, with
// AVAudioRecorder. The app asks for microphone access the first time (the executable carries
// the usage description in its embedded Info.plist).

#import <Foundation/Foundation.h>

@interface VoiceRecorder : NSObject
@property(nonatomic, readonly) BOOL recording;
@property(nonatomic, readonly) NSTimeInterval seconds;  // recorded so far
@property(nonatomic, readonly) float level;             // the input's peak since last asked, 0 to 1

// Asks for microphone access when it hasn't been decided; `done` runs on the main queue.
- (void)requestAccess:(void (^)(BOOL granted))done;
// Starts recording into `url` (replaced). NO, with `error` saying why, when it can't.
- (BOOL)startInto:(NSURL*)url error:(NSString**)error;
// Stops and finishes the file; returns how long it is, in seconds.
- (NSTimeInterval)stop;
@end
