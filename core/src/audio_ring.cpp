#include "mf/audio_ring.h"

#include <algorithm>
#include <cstring>

namespace mf {
namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;
constexpr auto kAcquire = std::memory_order_acquire;
constexpr auto kRelease = std::memory_order_release;

template <typename F>
void seqWrite(std::atomic<uint32_t>& seq, F&& write) {
  uint32_t s = seq.load(kRelaxed);
  seq.store(s + 1, kRelaxed);
  std::atomic_thread_fence(kRelease);
  write();
  seq.store(s + 2, kRelease);
}

template <typename F>
void seqRead(const std::atomic<uint32_t>& seq, F&& read) {
  for (;;) {
    uint32_t s = seq.load(kAcquire);
    if (s & 1) continue;
    read();
    std::atomic_thread_fence(kAcquire);
    if (seq.load(kRelaxed) == s) return;
  }
}

}  // namespace

void AudioRing::open(int sampleRate, int channels, int capacityFrames) {
  rate_ = sampleRate;
  channels_ = channels;
  capacity_ = static_cast<uint64_t>(capacityFrames);
  buf_.assign(capacity_ * channels_, 0);
}

int AudioRing::write(const int16_t* pcm, int frames) {
  uint64_t w = write_.load(kRelaxed);
  uint64_t r = read_.load(kAcquire);
  uint64_t n = std::min<uint64_t>(static_cast<uint64_t>(frames), capacity_ - (w - r));
  for (uint64_t done = 0; done < n;) {
    uint64_t pos = (w + done) % capacity_;
    uint64_t chunk = std::min(n - done, capacity_ - pos);
    std::memcpy(&buf_[pos * channels_], pcm + done * channels_, chunk * channels_ * sizeof(int16_t));
    done += chunk;
  }
  write_.store(w + n, kRelease);
  return static_cast<int>(n);
}

void AudioRing::flush(int64_t basePtsUs, uint32_t tag) {
  eos_.store(false, kRelease);
  uint64_t w = write_.load(kRelaxed);
  uint32_t gen = fGen_.load(kRelaxed) + 1;
  seqWrite(flushSeq_, [&] {
    fGen_.store(gen, kRelaxed);
    fWrite_.store(w, kRelaxed);
    fBase_.store(basePtsUs, kRelaxed);
    fTag_.store(tag, kRelaxed);
  });
}

void AudioRing::markEos() { eos_.store(true, kRelease); }

void AudioRing::consume(int16_t* out, int frames, int64_t audibleHostNs) {
  FlushRecord f = readFlush();
  uint64_t r = read_.load(kRelaxed);
  if (f.gen != seenGen_) {  // skip data written before the flush
    seenGen_ = f.gen;
    r = f.writeIndex;
    consumedInGen_ = 0;
  }
  uint64_t w = write_.load(kAcquire);
  uint64_t n = std::min<uint64_t>(static_cast<uint64_t>(frames), w - r);
  for (uint64_t done = 0; done < n;) {
    uint64_t pos = (r + done) % capacity_;
    uint64_t chunk = std::min(n - done, capacity_ - pos);
    std::memcpy(out + done * channels_, &buf_[pos * channels_], chunk * channels_ * sizeof(int16_t));
    done += chunk;
  }
  // Underrun: pad with silence, which the clock does not count.
  std::memset(out + n * channels_, 0, (frames - n) * channels_ * sizeof(int16_t));
  read_.store(r + n, kRelease);

  if (n == 0) return;
  int64_t before = consumedInGen_;
  seqWrite(snapSeq_, [&] {
    sGen_.store(f.gen, kRelaxed);
    sBefore_.store(before, kRelaxed);
    sFrames_.store(static_cast<int32_t>(n), kRelaxed);
    sAudible_.store(audibleHostNs, kRelaxed);
  });
  consumedInGen_ += static_cast<int64_t>(n);
}

AudioRing::FlushRecord AudioRing::readFlush() const {
  FlushRecord f{};
  seqRead(flushSeq_, [&] {
    f = {fGen_.load(kRelaxed), fWrite_.load(kRelaxed), fBase_.load(kRelaxed), fTag_.load(kRelaxed)};
  });
  return f;
}

AudioRing::Snapshot AudioRing::readSnapshot() const {
  Snapshot s{};
  seqRead(snapSeq_, [&] {
    s = {sGen_.load(kRelaxed), sBefore_.load(kRelaxed), sFrames_.load(kRelaxed), sAudible_.load(kRelaxed)};
  });
  return s;
}

bool AudioRing::clockUs(int64_t nowNs, int64_t* ptsUs, uint32_t* tag) const {
  FlushRecord f = readFlush();
  Snapshot s = readSnapshot();
  *tag = f.tag;
  if (s.gen != f.gen) {
    *ptsUs = f.basePtsUs;
    return false;
  }
  // Callbacks run ahead of the speaker, so audibleNs is usually in the future: extrapolate
  // backwards as well as forwards, but never past the last frame delivered (underrun hold).
  int64_t elapsedNs = std::clamp<int64_t>(nowNs - s.audibleNs, -1000000000, 1000000000);
  int64_t heard = std::clamp<int64_t>(s.framesBefore + elapsedNs * rate_ / 1000000000, 0, s.framesBefore + s.frames);
  *ptsUs = f.basePtsUs + heard * 1000000 / rate_;
  return true;
}

bool AudioRing::drained(int64_t nowNs) const {
  if (!eos_.load(kAcquire)) return false;
  FlushRecord f = readFlush();
  Snapshot s = readSnapshot();
  uint64_t w = write_.load(kAcquire);
  if (s.gen != f.gen) return w == f.writeIndex;  // not started: drained only if nothing was written
  return read_.load(kAcquire) == w && nowNs >= s.audibleNs + int64_t{s.frames} * 1000000000 / rate_;
}

}  // namespace mf
