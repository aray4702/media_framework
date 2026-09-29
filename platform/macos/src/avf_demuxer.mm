// IDemuxer on AVAssetReader: one passthrough output per track, read independently.

#import <AVFoundation/AVFoundation.h>

#include <deque>

#include "mf/macos.h"

// The synchronous AVAsset/AVAssetTrack accessors are deprecated in favor of async loading.
// We load the keys first (below), after which these accessors don't block.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace mf::macos {
namespace {

int64_t toUs(CMTime t) { return CMTimeConvertScale(t, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value; }

std::shared_ptr<void> retainCF(CFTypeRef ref) { return std::shared_ptr<void>(const_cast<void*>(CFRetain(ref)), CFRelease); }

bool isSync(CMSampleBufferRef sb, CMItemCount index) {
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sb, false);
  if (!attachments || CFArrayGetCount(attachments) == 0) return true;
  CFIndex i = CFArrayGetCount(attachments) > index ? index : 0;
  auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, i));
  return !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
}

bool loadKeys(id<AVAsynchronousKeyValueLoading> object, NSArray<NSString*>* keys) {
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [object loadValuesAsynchronouslyForKeys:keys completionHandler:^{ dispatch_semaphore_signal(done); }];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);  // on T1, never the caller thread
  for (NSString* key in keys) {
    if ([object statusOfValueForKey:key error:nil] != AVKeyValueStatusLoaded) return false;
  }
  return true;
}

class AvfDemuxer : public IDemuxer {
 public:
  Result open(const MediaSource& source, MediaInfo* out) override {
    @autoreleasepool {
      NSURL* url = (__bridge NSURL*)source.native.get();
      if (!url || ![[NSFileManager defaultManager] isReadableFileAtPath:url.path]) return Result::FileOpenFailed;
      asset_ = [AVURLAsset URLAssetWithURL:url options:@{AVURLAssetPreferPreciseDurationAndTimingKey : @YES}];
      if (!loadKeys(asset_, @[ @"tracks", @"duration" ])) return Result::UnsupportedFormat;

      tracks_[kVideo] = [asset_ tracksWithMediaType:AVMediaTypeVideo].firstObject;
      tracks_[kAudio] = [asset_ tracksWithMediaType:AVMediaTypeAudio].firstObject;
      if (!tracks_[kVideo] && !tracks_[kAudio]) return Result::UnsupportedFormat;  // audio only (e.g. .m4a) is fine
      NSArray* trackKeys = @[ @"formatDescriptions", @"minFrameDuration", @"nominalFrameRate", @"preferredTransform" ];
      for (AVAssetTrack* t : tracks_) {
        if (t && !loadKeys(t, trackKeys)) return Result::MalformedMedia;
      }

      MediaInfo info;
      info.durationUs = toUs(asset_.duration);
      if (tracks_[kVideo] && !describeVideo(&info.video)) return Result::MalformedMedia;  // none: info.video stays empty
      if (tracks_[kAudio]) {
        TrackInfo a;
        if (describeAudio(&a)) info.audio = a;
        else tracks_[kAudio] = nil;
      }
      if (!tracks_[kVideo] && !tracks_[kAudio]) return Result::MalformedMedia;
      *out = info;
      return startReader(kCMTimeZero);
    }
  }

  Result peekDtsUs(int track, int64_t* out) override {
    Result r = fill(track);
    if (r != Result::Ok) return r;
    *out = pending_[track].front().dtsUs;
    return Result::Ok;
  }

  Result read(int track, Packet* out) override {
    Result r = fill(track);
    if (r != Result::Ok) return r;
    *out = std::move(pending_[track].front());
    pending_[track].pop_front();
    return Result::Ok;
  }

  // AVAssetReader can't seek in place (A13): find the sync sample <= us, start a new reader there.
  // Without video, the reader starts at us (every AAC packet is a sync sample).
  Result seekTo(int64_t us) override {
    @autoreleasepool {
      CMTime target = CMTimeMake(us, 1000000);
      CMTime start = target;
      AVAssetTrack* video = tracks_[kVideo];
      if (video.canProvideSampleCursors) {
        AVSampleCursor* cursor = [video makeSampleCursorWithPresentationTimeStamp:target];
        while (cursor && !cursor.currentSampleSyncInfo.sampleIsFullSync) {
          if ([cursor stepInDecodeOrderByCount:-1] != -1) break;
        }
        if (cursor) start = cursor.presentationTimeStamp;
      }
      return startReader(start);
    }
  }

 private:
  bool describeVideo(TrackInfo* v) {
    AVAssetTrack* t = tracks_[kVideo];
    auto fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
    if (!fd) return false;
    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(fd);
    v->supported = CMFormatDescriptionGetMediaSubType(fd) == kCMVideoCodecType_H264;
    v->width = dims.width;
    v->height = dims.height;
    if (CMTIME_IS_VALID(t.minFrameDuration) && t.minFrameDuration.value > 0) {
      v->frameDurationUs = toUs(t.minFrameDuration);
    } else if (t.nominalFrameRate > 0) {
      v->frameDurationUs = static_cast<int64_t>(1e6 / t.nominalFrameRate);
    }
    v->rotated = !CGAffineTransformIsIdentity(t.preferredTransform);
    v->format = retainCF(fd);
    return true;
  }

  bool describeAudio(TrackInfo* a) {
    auto fd = (__bridge CMAudioFormatDescriptionRef)tracks_[kAudio].formatDescriptions.firstObject;
    if (!fd) return false;
    const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd);
    if (!asbd) return false;
    bool aacLc = asbd->mFormatID == kAudioFormatMPEG4AAC && (asbd->mFormatFlags == 0 || asbd->mFormatFlags == kMPEG4Object_AAC_LC);
    a->supported = aacLc || asbd->mFormatID == kAudioFormatMPEGLayer3;  // the audio decoder handles both
    a->sampleRate = static_cast<int>(asbd->mSampleRate);
    a->channels = static_cast<int>(asbd->mChannelsPerFrame);
    a->format = retainCF(fd);
    return true;
  }

  Result startReader(CMTime start) {
    NSError* error = nil;
    reader_ = [AVAssetReader assetReaderWithAsset:asset_ error:&error];
    if (!reader_) return Result::MalformedMedia;
    reader_.timeRange = CMTimeRangeMake(start, kCMTimePositiveInfinity);
    for (int track : {kVideo, kAudio}) {
      pending_[track].clear();
      eos_[track] = !tracks_[track];
      outputs_[track] = nil;
      if (!tracks_[track]) continue;
      AVAssetReaderTrackOutput* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tracks_[track]
                                                                                    outputSettings:nil];
      output.alwaysCopiesSampleData = NO;
      if (![reader_ canAddOutput:output]) return Result::MalformedMedia;
      [reader_ addOutput:output];
      outputs_[track] = output;
    }
    return [reader_ startReading] ? Result::Ok : Result::MalformedMedia;
  }

  // Ensures pending_[track] has a packet, splitting multi-sample buffers (AAC) into packets.
  Result fill(int track) {
    @autoreleasepool {
      while (pending_[track].empty()) {
        if (eos_[track]) return Result::Eos;
        CMSampleBufferRef sb = [outputs_[track] copyNextSampleBuffer];
        if (!sb) {
          // A read failure (e.g. a truncated tail) ends the track, so what was readable still
          // plays. With nothing readable at all, preroll finds no frame: MalformedMedia.
          if (reader_.status == AVAssetReaderStatusFailed) NSLog(@"[mf] read failed; ending track: %@", reader_.error.localizedDescription);
          eos_[track] = true;
          return Result::Eos;
        }
        Result r = split(track, sb);
        CFRelease(sb);
        if (r != Result::Ok) return r;
      }
      return Result::Ok;
    }
  }

  Result split(int track, CMSampleBufferRef sb) {
    CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sb);
    CMItemCount count = CMSampleBufferGetNumSamples(sb);
    size_t offset = 0;
    for (CMItemCount i = 0; data && i < count; ++i) {
      CMSampleTimingInfo timing;
      if (CMSampleBufferGetSampleTimingInfo(sb, i, &timing) != noErr) return Result::MalformedMedia;
      size_t size = CMSampleBufferGetSampleSize(sb, i);
      if (size == 0 || offset + size > CMBlockBufferGetDataLength(data)) return Result::MalformedMedia;
      Packet p;
      p.track = track;
      p.ptsUs = toUs(timing.presentationTimeStamp);
      p.dtsUs = CMTIME_IS_VALID(timing.decodeTimeStamp) ? toUs(timing.decodeTimeStamp) : p.ptsUs;
      p.key = isSync(sb, i);
      p.data.resize(size);  // the core rejects samples over 16 MB (A8)
      if (CMBlockBufferCopyDataBytes(data, offset, size, p.data.data()) != kCMBlockBufferNoErr) {
        return Result::MalformedMedia;
      }
      offset += size;
      pending_[track].push_back(std::move(p));
    }
    return Result::Ok;
  }

  AVURLAsset* asset_ = nil;
  AVAssetReader* reader_ = nil;
  AVAssetTrack* tracks_[2] = {nil, nil};
  AVAssetReaderTrackOutput* outputs_[2] = {nil, nil};
  std::deque<Packet> pending_[2];
  bool eos_[2] = {true, true};
};

}  // namespace

std::unique_ptr<IDemuxer> createDemuxer() { return std::make_unique<AvfDemuxer>(); }

}  // namespace mf::macos
