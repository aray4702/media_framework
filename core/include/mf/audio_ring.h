#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace mf {

// Single-producer (T4), single-consumer (real-time render callback) PCM ring that also
// carries the audio clock. The consumer side never locks or allocates.
//
// Flush protocol: only the consumer moves the read index. The producer publishes a flush
// record; on its next callback the consumer skips to the write index recorded there.
class AudioRing {
 public:
  void open(int sampleRate, int channels, int capacityFrames);  // allocates; before playback
  int sampleRate() const { return rate_; }
  int channels() const { return channels_; }

  // Producer.
  int write(const int16_t* pcm, int frames);  // frames accepted; never blocks
  void flush(int64_t basePtsUs, uint32_t tag);
  void markEos();

  // Consumer. `audibleHostNs` is when out[0] reaches the speaker.
  void consume(int16_t* out, int frames, int64_t audibleHostNs);

  // Any thread. PTS audible at nowNs. Returns false (and the flush base) until the consumer
  // has started on the current generation. Stops advancing during an underrun.
  bool clockUs(int64_t nowNs, int64_t* ptsUs, uint32_t* tag) const;
  // True once the producer marked EOS and every written frame has been heard.
  bool drained(int64_t nowNs) const;

 private:
  struct FlushRecord {
    uint32_t gen;
    uint64_t writeIndex;
    int64_t basePtsUs;
    uint32_t tag;
  };
  struct Snapshot {
    uint32_t gen;
    int64_t framesBefore;
    int32_t frames;
    int64_t audibleNs;
  };
  FlushRecord readFlush() const;
  Snapshot readSnapshot() const;

  int rate_ = 48000, channels_ = 2;
  uint64_t capacity_ = 0;
  std::vector<int16_t> buf_;
  std::atomic<uint64_t> write_{0}, read_{0};
  std::atomic<bool> eos_{false};

  // Flush record (seqlock, written by the producer).
  std::atomic<uint32_t> flushSeq_{0}, fGen_{0}, fTag_{0};
  std::atomic<uint64_t> fWrite_{0};
  std::atomic<int64_t> fBase_{0};

  // Clock snapshot of the last callback that delivered audio (seqlock, written by the
  // consumer). Empty callbacks don't overwrite it, so the clock stops at the end of the last
  // frame heard. gen = ~0 means nothing heard yet.
  std::atomic<uint32_t> snapSeq_{0}, sGen_{~0u};
  std::atomic<int64_t> sBefore_{0}, sAudible_{0};
  std::atomic<int32_t> sFrames_{0};

  // Consumer-only.
  uint32_t seenGen_ = 0;
  int64_t consumedInGen_ = 0;
};

}  // namespace mf
