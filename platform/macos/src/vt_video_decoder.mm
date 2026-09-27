// IVideoDecoder on VTDecompressionSession. Frames stay in IOSurface-backed CVPixelBuffers.
// VideoToolbox outputs in decode order, so frames are reordered by PTS here (§2.2).

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <map>
#include <mutex>

#include "mf/macos.h"

namespace mf::macos {
namespace {

constexpr size_t kMaxInFlight = 4;

// Reorder depth: the H.264 DPB size for the stream's level (the spec's fallback when the SPS
// has no max_num_reorder_frames). Baseline profile has no B-frames.
int reorderDepth(CMVideoFormatDescriptionRef fd) {
  CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(fd);
  NSDictionary* atoms = (__bridge NSDictionary*)CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
  NSData* avcC = atoms[@"avcC"];
  if (avcC.length < 4) return 16;
  const uint8_t* b = static_cast<const uint8_t*>(avcC.bytes);
  if (b[1] == 66) return 0;  // baseline
  static const std::map<int, int> maxDpbMbs = {
      {10, 396},    {11, 900},    {12, 2376},   {13, 2376},   {20, 2376},   {21, 4752},
      {22, 8100},   {30, 8100},   {31, 18000},  {32, 20480},  {40, 32768},  {41, 32768},
      {42, 34816},  {50, 110400}, {51, 184320}, {52, 184320}, {60, 696320}, {61, 696320}, {62, 696320}};
  auto it = maxDpbMbs.find(b[3]);
  if (it == maxDpbMbs.end()) return 16;
  int frameMbs = ((dims.width + 15) / 16) * ((dims.height + 15) / 16);
  return std::max(1, std::min(16, it->second / std::max(1, frameMbs)));
}

class VtVideoDecoder : public IVideoDecoder {
 public:
  ~VtVideoDecoder() override { destroySession(); }

  // Called again for each clip on this lane, once the previous one has drained: the session
  // is kept when it accepts the new format, otherwise replaced.
  Result configure(const TrackInfo& track, std::function<void()> onOutput) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++generation_;
      reorder_.clear();
      errors_ = 0;
      eos_ = false;
    }
    formatHolder_ = track.format;
    format_ = static_cast<CMVideoFormatDescriptionRef>(track.format.get());
    onOutput_ = std::move(onOutput);
    depth_ = static_cast<size_t>(reorderDepth(format_));
    if (session_ && VTDecompressionSessionCanAcceptFormatDescription(session_, format_)) return Result::Ok;
    destroySession();
    recreated_ = false;
    return createSession();
  }

  Result queue(const Packet& p) override {
    uint64_t id;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (inFlight_.size() >= kMaxInFlight) return Result::Again;
      id = ++nextId_;
      inFlight_[id] = {generation_, p.serial};
    }
    OSStatus status = decode(p, id);
    if (status == noErr) return Result::Ok;
    {
      std::lock_guard<std::mutex> lock(mu_);
      inFlight_.erase(id);  // the callback may or may not have run for a failed submit
    }
    if (status == kVTInvalidSessionErr && !recreated_) {  // e.g. after sleep or a GPU switch
      recreated_ = true;
      destroySession();
      return createSession() == Result::Ok ? Result::CorruptFrame : Result::DecoderFailed;
    }
    return status == kVTVideoDecoderBadDataErr ? Result::CorruptFrame : Result::DecoderFailed;
  }

  void signalEos() override {
    std::lock_guard<std::mutex> lock(mu_);
    eos_ = true;
  }

  Result dequeue(VideoFrame* out) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (errors_ > 0) {
      --errors_;
      return Result::CorruptFrame;
    }
    bool draining = eos_ && inFlight_.empty();
    if (!reorder_.empty() && (reorder_.size() > depth_ || draining)) {
      auto first = reorder_.begin();
      *out = std::move(first->second);
      reorder_.erase(first);
      return Result::Ok;
    }
    return draining ? Result::Eos : Result::Again;
  }

  // Never waits for in-flight frames: late outputs of an old generation are discarded.
  void flush() override {
    std::lock_guard<std::mutex> lock(mu_);
    ++generation_;
    reorder_.clear();
    errors_ = 0;
    eos_ = false;
  }

 private:
  struct Meta {
    uint32_t generation, serial;
  };

  Result createSession() {
    NSDictionary* attrs = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
      (id)kCVPixelBufferMetalCompatibilityKey : @YES,
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    VTDecompressionOutputCallbackRecord callback{&VtVideoDecoder::onDecoded, this};
    OSStatus s = VTDecompressionSessionCreate(nullptr, format_, nullptr, (__bridge CFDictionaryRef)attrs, &callback, &session_);
    if (s == kVTCouldNotFindVideoDecoderErr) return Result::NoDecoder;
    return s == noErr ? Result::Ok : Result::DecoderFailed;
  }

  void destroySession() {
    if (!session_) return;
    VTDecompressionSessionWaitForAsynchronousFrames(session_);  // shutdown, or a drained session: returns at once
    VTDecompressionSessionInvalidate(session_);
    CFRelease(session_);
    session_ = nullptr;
  }

  OSStatus decode(const Packet& p, uint64_t id) {
    CMBlockBufferRef block = nullptr;
    OSStatus s = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, p.data.size(), nullptr, nullptr, 0, p.data.size(),
                                                    kCMBlockBufferAssureMemoryNowFlag, &block);
    if (s != noErr) return s;
    CMBlockBufferReplaceDataBytes(p.data.data(), block, 0, p.data.size());
    CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake(p.ptsUs, 1000000), CMTimeMake(p.dtsUs, 1000000)};
    size_t size = p.data.size();
    CMSampleBufferRef sample = nullptr;
    s = CMSampleBufferCreateReady(nullptr, block, format_, 1, 1, &timing, 1, &size, &sample);
    CFRelease(block);
    if (s != noErr) return s;
    VTDecodeInfoFlags info = 0;
    s = VTDecompressionSessionDecodeFrame(session_, sample, kVTDecodeFrame_EnableAsynchronousDecompression,
                                          reinterpret_cast<void*>(id), &info);
    CFRelease(sample);
    return s;
  }

  static void onDecoded(void* self, void* frameRef, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                        CMTime pts, CMTime) {
    static_cast<VtVideoDecoder*>(self)->decoded(reinterpret_cast<uint64_t>(frameRef), status, flags, image, pts);
  }

  void decoded(uint64_t id, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = inFlight_.find(id);
      if (it == inFlight_.end()) return;
      Meta meta = it->second;
      inFlight_.erase(it);
      if (meta.generation == generation_) {
        if (status != noErr || !image || (flags & kVTDecodeInfo_FrameDropped)) {
          ++errors_;
        } else {
          VideoFrame f;
          f.ptsUs = CMTimeConvertScale(pts, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
          f.serial = meta.serial;
          f.image = std::shared_ptr<void>(CVPixelBufferRetain(image), [](void* pb) { CVPixelBufferRelease((CVPixelBufferRef)pb); });
          reorder_.emplace(f.ptsUs, std::move(f));
        }
      }
    }
    onOutput_();
  }

  std::shared_ptr<void> formatHolder_;
  CMVideoFormatDescriptionRef format_ = nullptr;
  VTDecompressionSessionRef session_ = nullptr;
  std::function<void()> onOutput_;
  size_t depth_ = 16;
  bool recreated_ = false;  // T2 only

  std::mutex mu_;  // guards everything below; the output callback runs on a VideoToolbox thread
  std::map<uint64_t, Meta> inFlight_;
  std::multimap<int64_t, VideoFrame> reorder_;
  uint64_t nextId_ = 0;
  uint32_t generation_ = 0;
  int errors_ = 0;
  bool eos_ = false;
};

}  // namespace

std::unique_ptr<IVideoDecoder> createVideoDecoder() { return std::make_unique<VtVideoDecoder>(); }

}  // namespace mf::macos
