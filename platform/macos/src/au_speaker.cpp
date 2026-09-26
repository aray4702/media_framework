// ISpeaker on the default-output AudioUnit. The render callback runs on a real-time thread:
// it only copies from the AudioRing (no locks, no allocation).

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include "mf/audio_ring.h"
#include "mf/macos.h"

namespace mf::macos {
namespace {

UInt32 deviceUInt32(AudioObjectID object, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
  AudioObjectPropertyAddress addr{selector, scope, kAudioObjectPropertyElementMain};
  UInt32 value = 0, size = sizeof(value);
  AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value);
  return value;
}

class AuSpeaker : public ISpeaker {
 public:
  ~AuSpeaker() override {
    if (!unit_) return;
    AudioOutputUnitStop(unit_);
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
  }

  Result open(int sampleRate, int channels, AudioRing* ring) override {
    ring_ = ring;
    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component || AudioComponentInstanceNew(component, &unit_) != noErr) return Result::AudioDeviceFailed;

    AudioStreamBasicDescription format{};
    format.mSampleRate = sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = static_cast<UInt32>(channels);
    format.mBitsPerChannel = 16;
    format.mBytesPerFrame = 2 * format.mChannelsPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = format.mBytesPerFrame;
    AURenderCallbackStruct callback{&AuSpeaker::render, this};
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)) ||
        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)) ||
        AudioUnitInitialize(unit_)) {
      return Result::AudioDeviceFailed;
    }
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    timebaseNumer_ = tb.numer;
    timebaseDenom_ = tb.denom;
    latencyNs_ = outputLatencyNs();
    return Result::Ok;
  }

  Result start() override { return AudioOutputUnitStart(unit_) == noErr ? Result::Ok : Result::AudioDeviceFailed; }
  void pause() override { AudioOutputUnitStop(unit_); }

 private:
  // Time from the callback's host time until the sound is audible: device + stream latency,
  // plus the AudioUnit's own (sample-rate conversion) latency.
  int64_t outputLatencyNs() {
    AudioDeviceID device = 0;
    UInt32 size = sizeof(device);
    AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, &size);
    Float64 rate = 0;
    AudioObjectPropertyAddress rateAddr{kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
    size = sizeof(rate);
    AudioObjectGetPropertyData(device, &rateAddr, 0, nullptr, &size, &rate);
    if (rate <= 0) rate = 48000;

    UInt32 frames = deviceUInt32(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput);
    AudioStreamID stream = 0;
    AudioObjectPropertyAddress streamsAddr{kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput,
                                           kAudioObjectPropertyElementMain};
    size = sizeof(stream);
    if (AudioObjectGetPropertyData(device, &streamsAddr, 0, nullptr, &size, &stream) == noErr && stream) {
      frames += deviceUInt32(stream, kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    }
    Float64 unitLatency = 0;
    size = sizeof(unitLatency);
    AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &unitLatency, &size);
    return static_cast<int64_t>((double(frames) / rate + unitLatency) * 1e9);
  }

  static OSStatus render(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp* ts, UInt32, UInt32 frames,
                         AudioBufferList* data) {
    auto* s = static_cast<AuSpeaker*>(self);
    int64_t hostNs = static_cast<int64_t>(ts->mHostTime * s->timebaseNumer_ / s->timebaseDenom_);
    s->ring_->consume(static_cast<int16_t*>(data->mBuffers[0].mData), static_cast<int>(frames), hostNs + s->latencyNs_);
    return noErr;
  }

  AudioComponentInstance unit_ = nullptr;
  AudioRing* ring_ = nullptr;
  uint64_t timebaseNumer_ = 1, timebaseDenom_ = 1;
  int64_t latencyNs_ = 0;
};

}  // namespace

std::unique_ptr<ISpeaker> createSpeaker() { return std::make_unique<AuSpeaker>(); }

}  // namespace mf::macos
