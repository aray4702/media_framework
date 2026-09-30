// ICamera on AVFoundation: an AVCaptureSession with a video data output (NV12 in the video range,
// the decoder's format, so the compositor draws camera frames as it draws decoded ones) and,
// when asked for, an audio data output (16-bit interleaved PCM). Sample times are on the host
// clock, the base hostNowNs() uses. Each output delivers on its own serial queue; the session
// starts and stops on a third, since startRunning blocks.

#import <AVFoundation/AVFoundation.h>

#include <mutex>

#include "mf/macos.h"

using mf::ICamera;

@interface MFCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate>
@property(nonatomic) AVCaptureOutput* videoOutput;
@end

@implementation MFCaptureDelegate {
 @public
  ICamera::FrameFn onFrame;
  ICamera::AudioFn onAudio;
}

static int64_t hostUs(CMTime t) { return CMTimeConvertScale(t, 1000000, kCMTimeRoundingMethod_Default).value; }

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection {
  if (output == self.videoOutput) {
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels || !onFrame) return;
    mf::VideoFrame f;
    f.ptsUs = hostUs(CMSampleBufferGetPresentationTimeStamp(sample));
    f.image = std::shared_ptr<void>(CVPixelBufferRetain(pixels), [](void* p) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p)); });
    onFrame(f);
    return;
  }
  if (!onAudio) return;
  const AudioStreamBasicDescription* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sample));
  if (!asbd || asbd->mBitsPerChannel != 16 || (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved)) return;
  AudioBufferList list;
  CMBlockBufferRef block = nullptr;
  if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, &list, sizeof(list), nullptr, nullptr, 0, &block) != noErr) return;
  int channels = int(asbd->mChannelsPerFrame);
  int frames = int(CMSampleBufferGetNumSamples(sample));
  int64_t hostNs = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1000000000, kCMTimeRoundingMethod_Default).value;
  onAudio(static_cast<const int16_t*>(list.mBuffers[0].mData), frames, int(asbd->mSampleRate), channels, hostNs);
  CFRelease(block);
}

- (void)captureOutput:(AVCaptureOutput*)output didDropSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection*)connection {
  // Late frames are discarded (alwaysDiscardsLateVideoFrames): the preview shows the next one.
}
@end

namespace mf::macos {
namespace {

bool authorized(AVMediaType type) { return [AVCaptureDevice authorizationStatusForMediaType:type] == AVAuthorizationStatusAuthorized; }

NSArray<AVCaptureDevice*>* videoDevices() {
  NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
  if (@available(macOS 14.0, *)) {
    [types addObjectsFromArray:@[ AVCaptureDeviceTypeExternal, AVCaptureDeviceTypeContinuityCamera ]];
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
  }
  return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified]
      .devices;
}

class AvCamera : public ICamera {
 public:
  AvCamera() {
    sessionQueue_ = dispatch_queue_create("mf.camera.session", DISPATCH_QUEUE_SERIAL);
    videoQueue_ = dispatch_queue_create("mf.camera.video", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    audioQueue_ = dispatch_queue_create("mf.camera.audio", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
  }
  ~AvCamera() override { stop(); }

  std::vector<CameraDevice> devices() override {
    std::vector<CameraDevice> out;
    @autoreleasepool {
      for (AVCaptureDevice* d in videoDevices()) {
        // On a Mac the built-in camera faces the user; its position is often unspecified.
        bool front = d.position == AVCaptureDevicePositionFront || [d.deviceType isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera];
        out.push_back({d.uniqueID.UTF8String, d.localizedName.UTF8String, front});
      }
    }
    return out;
  }

  Result start(const std::string& deviceId, FrameFn onFrame, AudioFn onAudio) override {
    stop();
    if (!authorized(AVMediaTypeVideo) || (onAudio && !authorized(AVMediaTypeAudio))) return Result::PermissionDenied;
    @autoreleasepool {
      AVCaptureDevice* device = deviceId.empty() ? [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo]
                                                 : [AVCaptureDevice deviceWithUniqueID:[NSString stringWithUTF8String:deviceId.c_str()]];
      if (!device) return Result::CaptureFailed;
      AVCaptureSession* session = [AVCaptureSession new];
      [session beginConfiguration];
      session.sessionPreset = [session canSetSessionPreset:AVCaptureSessionPreset1920x1080] ? AVCaptureSessionPreset1920x1080 : AVCaptureSessionPresetHigh;
      NSError* error = nil;
      AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
      if (!input || ![session canAddInput:input]) {
        NSLog(@"[mf] camera: %@", error);
        return Result::CaptureFailed;
      }
      [session addInput:input];

      MFCaptureDelegate* delegate = [MFCaptureDelegate new];
      delegate->onFrame = std::move(onFrame);
      delegate->onAudio = std::move(onAudio);
      AVCaptureVideoDataOutput* video = [AVCaptureVideoDataOutput new];
      video.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                              (id)kCVPixelBufferMetalCompatibilityKey : @YES};
      video.alwaysDiscardsLateVideoFrames = YES;
      [video setSampleBufferDelegate:delegate queue:videoQueue_];
      if (![session canAddOutput:video]) return Result::CaptureFailed;
      [session addOutput:video];
      delegate.videoOutput = video;

      if (delegate->onAudio) {
        AVCaptureDevice* mic = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
        AVCaptureDeviceInput* micInput = mic ? [AVCaptureDeviceInput deviceInputWithDevice:mic error:&error] : nil;
        AVCaptureAudioDataOutput* audio = [AVCaptureAudioDataOutput new];
        audio.audioSettings = @{AVFormatIDKey : @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey : @16, AVLinearPCMIsFloatKey : @NO,
                                AVLinearPCMIsNonInterleaved : @NO, AVLinearPCMIsBigEndianKey : @NO};
        [audio setSampleBufferDelegate:delegate queue:audioQueue_];
        if (!micInput || ![session canAddInput:micInput] || ![session canAddOutput:audio]) {
          NSLog(@"[mf] camera: no microphone: %@", error);
          return Result::CaptureFailed;
        }
        [session addInput:micInput];
        [session addOutput:audio];
      }
      [session commitConfiguration];
      {
        std::lock_guard<std::mutex> lock(mu_);
        session_ = session;
        delegate_ = delegate;
      }
      dispatch_async(sessionQueue_, ^{
        [session startRunning];  // blocks until the camera runs
      });
    }
    return Result::Ok;
  }

  void stop() override {
    AVCaptureSession* session;
    {
      std::lock_guard<std::mutex> lock(mu_);
      session = session_;
      session_ = nil;
    }
    if (!session) return;
    dispatch_sync(sessionQueue_, ^{
      [session stopRunning];
    });
    // No callback after this returns: wait out any in progress, then drop them.
    dispatch_sync(videoQueue_, ^{
    });
    dispatch_sync(audioQueue_, ^{
    });
    std::lock_guard<std::mutex> lock(mu_);
    if (delegate_) {
      delegate_->onFrame = nullptr;
      delegate_->onAudio = nullptr;
      delegate_ = nil;
    }
  }

 private:
  std::mutex mu_;
  AVCaptureSession* session_ = nil;
  MFCaptureDelegate* delegate_ = nil;
  dispatch_queue_t sessionQueue_, videoQueue_, audioQueue_;
};

}  // namespace

std::unique_ptr<ICamera> createCamera() { return std::make_unique<AvCamera>(); }

CameraAccess cameraAccess(bool microphone) {
  auto status = [](AVMediaType type) {
    switch ([AVCaptureDevice authorizationStatusForMediaType:type]) {
      case AVAuthorizationStatusAuthorized: return CameraAccess::Granted;
      case AVAuthorizationStatusNotDetermined: return CameraAccess::NotAsked;
      default: return CameraAccess::Denied;
    }
  };
  CameraAccess video = status(AVMediaTypeVideo);
  if (!microphone || video != CameraAccess::Granted) return video;
  return status(AVMediaTypeAudio);
}

void requestCameraAccess(bool microphone, std::function<void(bool granted)> done) {
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL video) {
                             if (!video || !microphone) return done(video);
                             [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                                                      completionHandler:^(BOOL audio) {
                                                        done(audio);
                                                      }];
                           }];
}

}  // namespace mf::macos
