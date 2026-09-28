// IExportSink on AVAssetWriter (§2.5): composed frames are rendered by MetalCompositor into
// the writer's BGRA pixel buffers and encoded to H.264; the mixed PCM is encoded to AAC.
// Writes return at once: the work runs on one serial queue per track, each waiting for its
// writer input. Separate queues matter: the writer holds one input back until the other
// catches up, so a single queue could wait on itself.

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <unistd.h>

#include <atomic>
#include <vector>

#include "metal_compositor.h"
#include "mf/macos.h"

namespace mf::macos {
namespace {

constexpr int kMaxQueuedVideo = 3;
constexpr int kMaxQueuedAudio = 8;

class AvfExportSink : public IExportSink {
 public:
  ~AvfExportSink() override {
    cancelled_ = true;
    if (videoQueue_) dispatch_sync(videoQueue_, ^{});
    if (audioQueue_) dispatch_sync(audioQueue_, ^{});
    if (finishing_) dispatch_semaphore_wait(finished_, DISPATCH_TIME_FOREVER);  // no callback after we return
    if (writer_ && writer_.status == AVAssetWriterStatusWriting) [writer_ cancelWriting];
    if (cache_) CFRelease(cache_);
    if (audioFormat_) CFRelease(audioFormat_);
  }

  Result open(const ExportTarget& target, const ExportSettings& s, int sampleRate, int channels) override {
    @autoreleasepool {
      NSURL* url = (__bridge NSURL*)target.native.get();
      if (!url) return Result::InvalidArgument;
      [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
      NSError* error = nil;
      writer_ = [AVAssetWriter assetWriterWithURL:url fileType:AVFileTypeMPEG4 error:&error];
      if (!writer_) return Result::FileOpenFailed;

      NSDictionary* video = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(s.width),
        AVVideoHeightKey : @(s.height),
        AVVideoCompressionPropertiesKey : @{
          AVVideoAverageBitRateKey : @(s.videoBitrate),
          AVVideoExpectedSourceFrameRateKey : @(s.fps),
          AVVideoMaxKeyFrameIntervalKey : @(s.fps),
          AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
        },
      };
      videoInput_ = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:video];
      videoInput_.expectsMediaDataInRealTime = NO;
      NSDictionary* pixels = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
        (id)kCVPixelBufferWidthKey : @(s.width),
        (id)kCVPixelBufferHeightKey : @(s.height),
        (id)kCVPixelBufferMetalCompatibilityKey : @YES,
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
      };
      adaptor_ = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput_
                                                                                 sourcePixelBufferAttributes:pixels];
      if (![writer_ canAddInput:videoInput_]) return Result::FileOpenFailed;
      [writer_ addInput:videoInput_];

      if (channels > 0) {
        AudioChannelLayout layout{};
        layout.mChannelLayoutTag = channels == 1 ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo;
        NSDictionary* audio = @{
          AVFormatIDKey : @(kAudioFormatMPEG4AAC),
          AVSampleRateKey : @(sampleRate),
          AVNumberOfChannelsKey : @(channels),
          AVEncoderBitRateKey : @(s.audioBitrate),
          AVChannelLayoutKey : [NSData dataWithBytes:&layout length:sizeof(layout)],
        };
        audioInput_ = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:audio];
        audioInput_.expectsMediaDataInRealTime = NO;
        if (![writer_ canAddInput:audioInput_]) return Result::FileOpenFailed;
        [writer_ addInput:audioInput_];
        AudioStreamBasicDescription pcm{};
        pcm.mSampleRate = sampleRate;
        pcm.mFormatID = kAudioFormatLinearPCM;
        pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        pcm.mBytesPerPacket = pcm.mBytesPerFrame = 2 * channels;
        pcm.mFramesPerPacket = 1;
        pcm.mChannelsPerFrame = channels;
        pcm.mBitsPerChannel = 16;
        if (CMAudioFormatDescriptionCreate(nullptr, &pcm, 0, nullptr, 0, nullptr, nullptr, &audioFormat_) != noErr) {
          return Result::FileOpenFailed;
        }
        sampleRate_ = sampleRate;
        channels_ = channels;
      }

      if (![writer_ startWriting]) return Result::FileOpenFailed;
      [writer_ startSessionAtSourceTime:kCMTimeZero];

      device_ = MTLCreateSystemDefaultDevice();
      if (!device_ || !compositor_.init(device_, MTLPixelFormatBGRA8Unorm)) return Result::Unsupported;
      queue_ = [device_ newCommandQueue];
      if (CVMetalTextureCacheCreate(nullptr, nullptr, device_, nullptr, &cache_) != kCVReturnSuccess) return Result::Unsupported;
      videoQueue_ = dispatch_queue_create("mf.export-video", DISPATCH_QUEUE_SERIAL);
      audioQueue_ = dispatch_queue_create("mf.export-audio", DISPATCH_QUEUE_SERIAL);
      return Result::Ok;
    }
  }

  Result writeVideo(const ComposedFrame& frame) override {
    if (failed_) return Result::WriteFailed;
    if (queuedVideo_ >= kMaxQueuedVideo) return Result::Again;
    ++queuedVideo_;
    ComposedFrame f = frame;
    dispatch_async(videoQueue_, ^{
      if (!cancelled_ && !failed_ && !encodeVideo(f)) failed_ = true;
      --queuedVideo_;
    });
    return Result::Ok;
  }

  Result writeAudio(const int16_t* pcm, int frames, int64_t ptsUs) override {
    if (failed_) return Result::WriteFailed;
    if (!audioInput_) return Result::Ok;
    if (queuedAudio_ >= kMaxQueuedAudio) return Result::Again;
    ++queuedAudio_;
    auto data = std::make_shared<std::vector<int16_t>>(pcm, pcm + size_t(frames) * channels_);
    int64_t sample = ptsUs * sampleRate_ / 1000000;
    dispatch_async(audioQueue_, ^{
      if (!cancelled_ && !failed_ && !encodeAudio(*data, sample)) failed_ = true;
      --queuedAudio_;
    });
    return Result::Ok;
  }

  // Lets the writer stop waiting for audio to interleave with the video still to come.
  void endAudio() override {
    if (!audioInput_) return;
    dispatch_async(audioQueue_, ^{
      if (!cancelled_ && !failed_) [audioInput_ markAsFinished];
      audioEnded_ = true;
    });
  }

  void finish(std::function<void(Result)> done) override {
    finishing_ = true;
    dispatch_async(videoQueue_, ^{
      dispatch_sync(audioQueue_, ^{});  // everything written
      if (cancelled_ || failed_) {
        if (!cancelled_) done(Result::WriteFailed);
        dispatch_semaphore_signal(finished_);
        return;
      }
      [videoInput_ markAsFinished];
      if (!audioEnded_) [audioInput_ markAsFinished];
      [writer_ finishWritingWithCompletionHandler:^{
        if (!cancelled_) {
          if (writer_.status != AVAssetWriterStatusCompleted) NSLog(@"[mf] export failed: %@", writer_.error);
          done(writer_.status == AVAssetWriterStatusCompleted ? Result::Ok : Result::WriteFailed);
        }
        dispatch_semaphore_signal(finished_);
      }];
    });
  }

 private:
  // Waits (on this track's queue) until the writer takes more data for the input.
  bool waitReady(AVAssetWriterInput* input) {
    while (!input.readyForMoreMediaData) {
      if (cancelled_ || writer_.status != AVAssetWriterStatusWriting) return false;
      usleep(1000);
    }
    return true;
  }

  bool encodeVideo(const ComposedFrame& f) {
    @autoreleasepool {
      CVPixelBufferRef pixels = nullptr;
      if (!adaptor_.pixelBufferPool || CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor_.pixelBufferPool, &pixels) != kCVReturnSuccess) {
        return false;
      }
      CVMetalTextureRef target = nullptr;
      CVMetalTextureCacheCreateTextureFromImage(nullptr, cache_, pixels, nullptr, MTLPixelFormatBGRA8Unorm,
                                                CVPixelBufferGetWidth(pixels), CVPixelBufferGetHeight(pixels), 0, &target);
      bool ok = target != nullptr;
      if (ok) {
        id<MTLCommandBuffer> cmd = [queue_ commandBuffer];
        compositor_.encode(f, CVMetalTextureGetTexture(target), cmd);
        [cmd commit];
        [cmd waitUntilCompleted];  // on this queue only; the pipeline threads never wait
        ok = cmd.status == MTLCommandBufferStatusCompleted && waitReady(videoInput_) &&
             [adaptor_ appendPixelBuffer:pixels withPresentationTime:CMTimeMake(f.ptsUs, 1000000)];
        CFRelease(target);
      }
      CVPixelBufferRelease(pixels);
      return ok;
    }
  }

  bool encodeAudio(const std::vector<int16_t>& pcm, int64_t sample) {
    @autoreleasepool {
      size_t bytes = pcm.size() * sizeof(int16_t);
      CMBlockBufferRef block = nullptr;
      if (CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, nullptr, nullptr, 0, bytes,
                                             kCMBlockBufferAssureMemoryNowFlag, &block) != kCMBlockBufferNoErr) {
        return false;
      }
      CMBlockBufferReplaceDataBytes(pcm.data(), block, 0, bytes);
      CMSampleBufferRef buffer = nullptr;
      OSStatus s = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
          nullptr, block, audioFormat_, static_cast<CMItemCount>(pcm.size() / channels_), CMTimeMake(sample, sampleRate_),
          nullptr, &buffer);
      CFRelease(block);
      if (s != noErr) return false;
      bool ok = waitReady(audioInput_) && [audioInput_ appendSampleBuffer:buffer];
      CFRelease(buffer);
      return ok;
    }
  }

  AVAssetWriter* writer_ = nil;
  AVAssetWriterInput* videoInput_ = nil;
  AVAssetWriterInput* audioInput_ = nil;
  AVAssetWriterInputPixelBufferAdaptor* adaptor_ = nil;
  CMAudioFormatDescriptionRef audioFormat_ = nullptr;
  int sampleRate_ = 0, channels_ = 0;

  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  CVMetalTextureCacheRef cache_ = nullptr;
  MetalCompositor compositor_;  // video queue only

  dispatch_queue_t videoQueue_ = nullptr, audioQueue_ = nullptr;
  dispatch_semaphore_t finished_ = dispatch_semaphore_create(0);
  std::atomic<int> queuedVideo_{0}, queuedAudio_{0};
  std::atomic<bool> failed_{false}, cancelled_{false}, finishing_{false};
  bool audioEnded_ = false;  // audio queue only
};

}  // namespace

std::unique_ptr<IExportSink> createExportSink() { return std::make_unique<AvfExportSink>(); }

}  // namespace mf::macos
