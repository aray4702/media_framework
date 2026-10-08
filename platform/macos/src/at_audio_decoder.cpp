// IAudioDecoder on AudioConverter: AAC-LC or MP3 packets to S16 interleaved PCM, one packet at a
// time (1024 frames for AAC, 1152 for MP3: the format's frames per packet). AudioConverter is
// synchronous: a packet is decoded as it is queued, and its PCM waits to be taken.

#include <AudioToolbox/AudioToolbox.h>
#include <CoreMedia/CoreMedia.h>

#include <deque>

#include "mf/macos.h"

namespace mf::macos {
namespace {

constexpr OSStatus kNoMoreInput = 'nmor';

class AtAudioDecoder : public IAudioDecoder {
 public:
  ~AtAudioDecoder() override {
    if (converter_) AudioConverterDispose(converter_);
  }

  Result configure(const TrackInfo& track, std::function<void()>) override {
    out_.clear();
    eos_ = false;
    if (converter_) {  // the next clip on this lane
      AudioConverterDispose(converter_);
      converter_ = nullptr;
    }
    auto fd = static_cast<CMAudioFormatDescriptionRef>(track.format.get());
    const AudioStreamBasicDescription* in = CMAudioFormatDescriptionGetStreamBasicDescription(fd);
    if (!in) return Result::UnsupportedFormat;
    channels_ = static_cast<int>(in->mChannelsPerFrame);
    framesPerPacket_ = in->mFramesPerPacket ? in->mFramesPerPacket : 1024;

    AudioStreamBasicDescription out{};
    out.mSampleRate = in->mSampleRate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    out.mChannelsPerFrame = in->mChannelsPerFrame;
    out.mBitsPerChannel = 16;
    out.mBytesPerFrame = 2 * in->mChannelsPerFrame;
    out.mFramesPerPacket = 1;
    out.mBytesPerPacket = out.mBytesPerFrame;
    if (AudioConverterNew(in, &out, &converter_) != noErr) return Result::NoDecoder;

    size_t cookieSize = 0;
    const void* cookie = CMAudioFormatDescriptionGetMagicCookie(fd, &cookieSize);
    if (cookie && AudioConverterSetProperty(converter_, kAudioConverterDecompressionMagicCookie,
                                            static_cast<UInt32>(cookieSize), cookie) != noErr) {
      return Result::NoDecoder;
    }
    return Result::Ok;
  }

  Result queue(const Packet& p) override {
    if (out_.size() >= kMaxQueued) return Result::Again;
    Output o;
    o.result = decode(p, &o.pcm);
    out_.push_back(std::move(o));
    return Result::Ok;
  }

  void signalEos() override { eos_ = true; }

  Result dequeue(PcmBuffer* out) override {
    if (out_.empty()) return eos_ ? Result::Eos : Result::Again;
    Result r = out_.front().result;
    *out = std::move(out_.front().pcm);
    out_.pop_front();
    return r;
  }

  void flush() override {
    out_.clear();
    eos_ = false;
    if (converter_) AudioConverterReset(converter_);
  }

 private:
  static constexpr size_t kMaxQueued = 4;
  struct Output {
    Result result = Result::Ok;
    PcmBuffer pcm;
  };

  Result decode(const Packet& p, PcmBuffer* out) {
    out->ptsUs = p.ptsUs;
    out->samples.assign(size_t(framesPerPacket_) * channels_, 0);
    Input input{&p, false, {}};
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = static_cast<UInt32>(channels_);
    list.mBuffers[0].mDataByteSize = static_cast<UInt32>(out->samples.size() * sizeof(int16_t));
    list.mBuffers[0].mData = out->samples.data();
    UInt32 frames = framesPerPacket_;
    OSStatus s = AudioConverterFillComplexBuffer(converter_, &AtAudioDecoder::supply, &input, &frames, &list, nullptr);
    if (s != noErr && s != kNoMoreInput) return Result::CorruptFrame;  // samples stay silent (A14)
    out->samples.resize(size_t(frames) * channels_);
    return Result::Ok;
  }

  struct Input {
    const Packet* packet;
    bool used;
    AudioStreamPacketDescription desc;
  };

  // Hands the converter exactly one packet per decode() call.
  static OSStatus supply(AudioConverterRef, UInt32* packets, AudioBufferList* data,
                         AudioStreamPacketDescription** descs, void* ref) {
    auto* in = static_cast<Input*>(ref);
    if (in->used) {
      *packets = 0;
      return kNoMoreInput;
    }
    in->used = true;
    in->desc = {0, 0, static_cast<UInt32>(in->packet->data.size())};
    data->mBuffers[0].mData = const_cast<uint8_t*>(in->packet->data.data());
    data->mBuffers[0].mDataByteSize = static_cast<UInt32>(in->packet->data.size());
    *packets = 1;
    if (descs) *descs = &in->desc;
    return noErr;
  }

  AudioConverterRef converter_ = nullptr;
  std::deque<Output> out_;
  bool eos_ = false;
  int channels_ = 2;
  UInt32 framesPerPacket_ = 1024;
};

}  // namespace

std::unique_ptr<IAudioDecoder> createAudioDecoder() { return std::make_unique<AtAudioDecoder>(); }

}  // namespace mf::macos
