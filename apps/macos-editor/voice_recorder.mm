#import "voice_recorder.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cmath>

@implementation VoiceRecorder {
  AVAudioRecorder* _recorder;
}

- (BOOL)recording {
  return _recorder.recording;
}

- (NSTimeInterval)seconds {
  return _recorder.recording ? _recorder.currentTime : 0;
}

- (float)level {
  if (!_recorder.recording) return 0;
  [_recorder updateMeters];
  float db = [_recorder peakPowerForChannel:0];  // -160 (silence) to 0 dB
  return std::clamp(std::pow(10.0f, db / 20), 0.0f, 1.0f);
}

- (void)requestAccess:(void (^)(BOOL))done {
  AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
  if (status == AVAuthorizationStatusAuthorized) return done(YES);
  if (status != AVAuthorizationStatusNotDetermined) return done(NO);  // denied or restricted: only System Settings changes it
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                           completionHandler:^(BOOL granted) {  // asks the user, once
                             dispatch_async(dispatch_get_main_queue(), ^{
                               done(granted);
                             });
                           }];
}

- (BOOL)startInto:(NSURL*)url error:(NSString**)error {
  [[NSFileManager defaultManager] createDirectoryAtURL:url.URLByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
  NSDictionary* settings = @{
    AVFormatIDKey : @(kAudioFormatMPEG4AAC),  // AAC-LC: what the engine plays
    AVSampleRateKey : @48000,
    AVNumberOfChannelsKey : @1,
    AVEncoderAudioQualityKey : @(AVAudioQualityHigh),
  };
  NSError* failure = nil;
  _recorder = [[AVAudioRecorder alloc] initWithURL:url settings:settings error:&failure];
  _recorder.meteringEnabled = YES;  // for the level shown while recording
  if (!_recorder || ![_recorder prepareToRecord] || ![_recorder record]) {
    if (error) *error = failure.localizedDescription ?: @"The microphone can't record.";
    _recorder = nil;
    return NO;
  }
  return YES;
}

- (NSTimeInterval)stop {
  NSTimeInterval seconds = _recorder.currentTime;
  [_recorder stop];  // finishes the file
  _recorder = nil;
  return seconds;
}

@end
