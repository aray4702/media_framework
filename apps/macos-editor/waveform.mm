#import "waveform.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cmath>

namespace editor {
namespace {

constexpr int kRate = 8000;  // decoded at this rate, mono: plenty for peaks at 50 per second

std::vector<float> readPeaks(AVAsset* asset, AVAssetTrack* track) {
  NSError* error = nil;
  AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
  if (!reader) return {};
  NSDictionary* pcm = @{
    AVFormatIDKey : @(kAudioFormatLinearPCM),
    AVLinearPCMBitDepthKey : @32,
    AVLinearPCMIsFloatKey : @YES,
    AVLinearPCMIsBigEndianKey : @NO,
    AVLinearPCMIsNonInterleaved : @NO,
    AVSampleRateKey : @(kRate),
    AVNumberOfChannelsKey : @1,
  };
  AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:pcm];
  if (![reader canAddOutput:output]) return {};
  [reader addOutput:output];
  if (![reader startReading]) return {};

  std::vector<float> peaks, samples;
  const int per = kRate / kPeaksPerSecond;
  float peak = 0;
  int count = 0;
  while (CMSampleBufferRef buffer = [output copyNextSampleBuffer]) {
    CMBlockBufferRef data = CMSampleBufferGetDataBuffer(buffer);
    size_t bytes = data ? CMBlockBufferGetDataLength(data) : 0;
    samples.resize(bytes / sizeof(float));
    if (bytes && CMBlockBufferCopyDataBytes(data, 0, samples.size() * sizeof(float), samples.data()) == kCMBlockBufferNoErr) {
      for (float s : samples) {
        peak = std::max(peak, std::fabs(s));
        if (++count == per) {
          peaks.push_back(peak);
          peak = 0;
          count = 0;
        }
      }
    }
    CFRelease(buffer);
  }
  if (count) peaks.push_back(peak);
  float loudest = peaks.empty() ? 0 : *std::max_element(peaks.begin(), peaks.end());
  if (loudest > 0) {
    for (float& p : peaks) p /= loudest;
  }
  return peaks;
}

}  // namespace

void loadPeaks(NSString* path, void (^done)(std::vector<float> peaks)) {
  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
  [asset loadTracksWithMediaType:AVMediaTypeAudio
               completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError* error) {  // off the main thread
                 std::vector<float> peaks = tracks.count ? readPeaks(asset, tracks.firstObject) : std::vector<float>{};
                 dispatch_async(dispatch_get_main_queue(), ^{
                   done(peaks);
                 });
               }];
}

}  // namespace editor
