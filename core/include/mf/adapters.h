#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mf/types.h"

namespace mf {

class AudioRing;

// Host time in nanoseconds, on the same base the display and speaker use.
class IClock {
 public:
  virtual ~IClock() = default;
  virtual int64_t nowNs() const = 0;
};

// Tracks are read independently, so a full queue on one never stalls the other (§2.3).
class IDemuxer {
 public:
  virtual ~IDemuxer() = default;
  virtual Result open(const MediaSource&, MediaInfo* out) = 0;
  virtual Result peekDtsUs(int track, int64_t* out) = 0;  // Ok | Eos | error
  virtual Result read(int track, Packet* out) = 0;        // Ok | Eos | error
  virtual Result seekTo(int64_t us) = 0;                  // both tracks restart at the sync sample <= us
};

class IVideoDecoder {
 public:
  virtual ~IVideoDecoder() = default;
  // onOutput is called from any thread when output may be ready; it must not block.
  // Called again for the next clip once the previous one has drained (Eos) or been flushed.
  virtual Result configure(const TrackInfo&, std::function<void()> onOutput) = 0;
  virtual Result queue(const Packet&) = 0;      // Ok | Again | CorruptFrame | DecoderFailed
  virtual void signalEos() = 0;                 // no more input until flush()
  virtual Result dequeue(VideoFrame* out) = 0;  // Ok | Again | Eos | CorruptFrame | DecoderFailed; PTS order
  virtual void flush() = 0;                     // never waits
};

// Decodes like IVideoDecoder: packets go in and their PCM comes out in the same order, possibly
// later (a browser decodes asynchronously).
class IAudioDecoder {
 public:
  virtual ~IAudioDecoder() = default;
  // onOutput is called from any thread when output may be ready; it must not block. Called again
  // for the next clip once the previous one's output has all been taken.
  virtual Result configure(const TrackInfo&, std::function<void()> onOutput) = 0;
  virtual Result queue(const Packet&) = 0;  // Ok | Again (full: retry after an output) | DecoderFailed
  virtual void signalEos() = 0;             // no more input until configure() or flush()
  // Decoded PCM, in packet order: Ok | Again (not decoded yet) | Eos (after signalEos, all taken)
  // | CorruptFrame (`out` holds silence for the packet's duration, or no samples: the lost audio is
  // a gap, mixed as silence; A14) | DecoderFailed.
  virtual Result dequeue(PcmBuffer* out) = 0;
  virtual void flush() = 0;  // drops what was queued and not taken; never waits
};

class IDisplay {
 public:
  // presentedNs is the actual time the frame reached the screen, or 0 if it was never shown.
  using PresentedFn = std::function<void(int64_t ptsUs, int64_t presentedNs)>;
  virtual ~IDisplay() = default;
  virtual Result attach(const RenderTarget&, PresentedFn) = 0;  // on the owner thread, before playback
  // Never blocks; draws the layers, caption and filter at that time. Reports composed.ptsUs.
  virtual void present(const ComposedFrame&, int64_t hostTimeNs) = 0;
  virtual bool visible() const = 0;
  virtual int64_t vsyncPeriodNs() const = 0;  // can change at run time
  // How long before its target time a frame must be handed over to be on screen at that time.
  virtual int64_t latencyNs() const { return 0; }
  // Host time of a recent vsync, to align present times with the real refresh (0: unknown).
  virtual int64_t vsyncGridNs() const { return 0; }
};

// Pulls PCM from the ring on a real-time thread.
class ISpeaker {
 public:
  virtual ~ISpeaker() = default;
  virtual Result open(int sampleRate, int channels, AudioRing* ring) = 0;
  virtual Result start() = 0;
  virtual void pause() = 0;
};

// Still images for image items (scene_graph_spec.md §3): decoded once, on T1 while probing.
class IImageLoader {
 public:
  virtual ~IImageLoader() = default;
  // out->image: a platform surface the display can draw (Mac OS: a BGRA CVPixelBuffer).
  virtual Result load(const MediaSource&, VideoFrame* out, int* width, int* height) = 0;
};

// A live camera and its microphone (the camera window).
struct CameraDevice {
  std::string id;    // stable across launches
  std::string name;  // for menus
  bool front = false;  // faces the user (Mac OS: the built-in camera): mirrored by default
};
class ICamera {
 public:
  // On the camera's thread. frame.image is a surface the display and an export sink can use
  // directly (Mac OS: an IOSurface-backed CVPixelBuffer); frame.ptsUs is its capture time, on
  // the host clock, in µs.
  using FrameFn = std::function<void(const VideoFrame& frame)>;
  // On the microphone's thread: interleaved PCM and the host time of its first sample.
  using AudioFn = std::function<void(const int16_t* pcm, int frames, int sampleRate, int channels, int64_t hostTimeNs)>;
  virtual ~ICamera() = default;
  virtual std::vector<CameraDevice> devices() = 0;
  // Starts `deviceId` ("": the default), with the microphone when onAudio is set. Ok, or
  // PermissionDenied, CaptureFailed. Calling it again switches devices.
  virtual Result start(const std::string& deviceId, FrameFn onFrame, AudioFn onAudio) = 0;
  // No callback runs after it returns.
  virtual void stop() = 0;
};

// Export (§2.5): renders composed frames and encodes them with the mixed audio into a file.
// Writes never block: Again means the encoder is busy, and the caller retries shortly.
// Destroying the sink cancels an unfinished file, and no callback runs after the destructor.
class IExportSink {
 public:
  virtual ~IExportSink() = default;
  virtual Result open(const ExportTarget&, const ExportSettings&, int sampleRate, int channels) = 0;  // channels 0: no audio
  virtual Result writeVideo(const ComposedFrame&) = 0;                                  // Ok | Again | WriteFailed
  virtual Result writeAudio(const int16_t* pcm, int frames, int64_t ptsUs) = 0;          // Ok | Again | WriteFailed
  virtual void endAudio() {}  // the last audio has been written
  // After the last write; `done` runs on any thread once the file is complete.
  virtual void finish(std::function<void(Result)> done) = 0;
};

class Stage {
 public:
  virtual ~Stage() = default;
  virtual Progress pump() = 0;  // never blocks
};

class IScheduler {
 public:
  virtual ~IScheduler() = default;
  virtual void start(std::array<Stage*, kStageCount> stages) = 0;
  virtual void wake(StageId) = 0;
  virtual void stop() = 0;  // stages are idle afterwards
};

class PlatformFactory {
 public:
  virtual ~PlatformFactory() = default;
  virtual std::unique_ptr<IDemuxer> createDemuxer() = 0;
  virtual std::unique_ptr<IVideoDecoder> createVideoDecoder() = 0;
  virtual std::unique_ptr<IAudioDecoder> createAudioDecoder() = 0;
  virtual std::unique_ptr<ISpeaker> createSpeaker() = 0;
  virtual std::unique_ptr<IDisplay> createDisplay() = 0;
  virtual std::unique_ptr<IScheduler> createScheduler() = 0;
  virtual std::unique_ptr<IExportSink> createExportSink() { return nullptr; }  // null: export unsupported
  virtual std::unique_ptr<IImageLoader> createImageLoader() { return nullptr; }  // null: no image items
  virtual std::unique_ptr<ICamera> createCamera() { return nullptr; }            // null: no camera
  virtual IClock& clock() = 0;
};

}  // namespace mf
