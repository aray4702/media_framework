#include "mp4_demuxer.h"

#include <algorithm>
#include <cstring>

namespace mf::web {
namespace {

class MemorySource : public ByteSource {
 public:
  explicit MemorySource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
  int64_t size() const override { return int64_t(bytes_.size()); }
  bool read(int64_t offset, void* out, size_t bytes) override {
    if (offset < 0 || uint64_t(offset) + bytes > bytes_.size()) return false;
    std::memcpy(out, bytes_.data() + offset, bytes);
    return true;
  }

 private:
  std::vector<uint8_t> bytes_;
};

// Big-endian reads over a span. A read past the end sets ok = false and returns 0.
struct Reader {
  const uint8_t* p;
  size_t n;
  size_t at = 0;
  bool ok = true;

  bool need(size_t k) {
    if (!ok || at + k > n) {
      ok = false;
      return false;
    }
    return true;
  }
  uint64_t be(int bytes) {
    if (!need(size_t(bytes))) return 0;
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = v << 8 | p[at++];
    return v;
  }
  uint8_t u8() { return uint8_t(be(1)); }
  uint16_t u16() { return uint16_t(be(2)); }
  uint32_t u32() { return uint32_t(be(4)); }
  uint64_t u64() { return be(8); }
  void skip(size_t k) {
    if (need(k)) at += k;
  }
};

struct Box {
  uint32_t type = 0;
  size_t payload = 0, end = 0;  // offsets in the buffer
};

constexpr uint32_t fourcc(const char (&s)[5]) {
  return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

// The child boxes of buf[begin, end). False when one runs past the end.
bool children(const std::vector<uint8_t>& buf, size_t begin, size_t end, std::vector<Box>* out) {
  out->clear();
  size_t at = begin;
  while (at + 8 <= end) {
    Reader r{buf.data() + at, end - at};
    uint64_t size = r.u32();
    uint32_t type = r.u32();
    size_t header = 8;
    if (size == 1) {
      size = r.u64();
      header = 16;
    } else if (size == 0) {
      size = end - at;
    }
    if (!r.ok || size < header || at + size > end) return false;
    out->push_back({type, at + header, size_t(at + size)});
    at += size;
  }
  return true;
}

const Box* find(const std::vector<Box>& boxes, uint32_t type) {
  for (const Box& b : boxes) {
    if (b.type == type) return &b;
  }
  return nullptr;
}

// Media units to µs, rounding half away from zero (as CMTimeConvertScale does on macOS).
int64_t toUs(int64_t value, int64_t timescale) {
  if (timescale <= 0) return 0;
  __int128 v = __int128(value) * 1000000;
  __int128 half = timescale / 2;
  return int64_t(v >= 0 ? (v + half) / timescale : (v - half) / timescale);
}

struct Sample {
  int64_t offset = 0;
  uint32_t size = 0;
  int64_t dtsUs = 0, ptsUs = 0, durUs = 0;
  bool key = true;
  int8_t dependedOn = -1;  // from sdtp: 1 depended on, 2 not (disposable); -1 unknown
};

struct Track {
  uint32_t handler = 0;
  TrackInfo info;
  bool usable = false;  // a format this platform decodes was described
  std::vector<Sample> samples;
  size_t next = 0;
  int nalLengthSize = 0;  // H.264: the NAL unit length prefix size
};

// One MPEG-4 descriptor: its tag and payload [payload, end).
bool descriptor(Reader& r, uint8_t* tag, size_t* end) {
  *tag = r.u8();
  size_t size = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t b = r.u8();
    size = size << 7 | (b & 0x7f);
    if (!(b & 0x80)) break;
  }
  *end = r.at + size;
  return r.ok && *end <= r.n;
}

// The AudioSpecificConfig and object type from an esds box.
bool parseEsds(const uint8_t* p, size_t n, uint8_t* objectType, std::vector<uint8_t>* asc) {
  Reader r{p, n};
  r.skip(4);  // version, flags
  uint8_t tag;
  size_t end;
  if (!descriptor(r, &tag, &end) || tag != 3) return false;
  r.skip(2);  // ES_ID
  uint8_t flags = r.u8();
  if (flags & 0x80) r.skip(2);
  if (flags & 0x40) r.skip(r.u8());
  if (flags & 0x20) r.skip(2);
  if (!descriptor(r, &tag, &end) || tag != 4) return false;
  *objectType = r.u8();
  r.skip(12);  // stream type, buffer size, bit rates
  while (r.ok && r.at < end) {
    size_t inner;
    if (!descriptor(r, &tag, &inner)) return false;
    if (tag == 5) {
      asc->assign(p + r.at, p + inner);
      return true;
    }
    r.at = inner;
  }
  return r.ok;  // no specific info (MP3)
}

char hexDigit(int v) { return "0123456789abcdef"[v & 15]; }

// The sample description: H.264 (avc1) or AAC-LC / MP3 (mp4a). Unknown formats leave usable false.
void parseStsd(const std::vector<uint8_t>& buf, const Box& stsd, Track& t) {
  Reader r{buf.data() + stsd.payload, stsd.end - stsd.payload};
  r.skip(4);
  if (r.u32() == 0) return;
  size_t entryStart = stsd.payload + r.at;
  Reader er{buf.data() + entryStart, stsd.end - entryStart};
  uint32_t size = er.u32(), type = er.u32();
  if (!er.ok || size < 8 || entryStart + size > stsd.end) return;
  size_t entryEnd = entryStart + size;
  auto config = std::make_shared<CodecConfig>();
  std::vector<Box> kids;
  if (type == fourcc("avc1")) {
    er.skip(8 + 16);
    t.info.width = er.u16();
    t.info.height = er.u16();
    if (!er.ok || !children(buf, entryStart + 8 + 78, entryEnd, &kids)) return;
    const Box* avcC = find(kids, fourcc("avcC"));
    if (!avcC || avcC->end - avcC->payload < 7) return;
    config->description.assign(buf.begin() + avcC->payload, buf.begin() + avcC->end);
    const std::vector<uint8_t>& d = config->description;
    config->codec = "avc1.";
    for (int i = 1; i <= 3; ++i) {
      config->codec += hexDigit(d[size_t(i)] >> 4);
      config->codec += hexDigit(d[size_t(i)]);
    }
    t.nalLengthSize = (d[4] & 3) + 1;
    t.info.supported = true;
  } else if (type == fourcc("mp4a")) {
    er.skip(8);
    uint16_t version = er.u16();
    er.skip(6);
    t.info.channels = er.u16();
    er.skip(6);
    t.info.sampleRate = int(er.u32() >> 16);
    size_t kidsAt = entryStart + 8 + 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
    if (!er.ok || kidsAt > entryEnd || !children(buf, kidsAt, entryEnd, &kids)) return;
    const Box* esds = find(kids, fourcc("esds"));
    std::vector<Box> wave;  // QuickTime nests esds in a wave box
    if (const Box* w = find(kids, fourcc("wave")); !esds && w && children(buf, w->payload, w->end, &wave)) {
      esds = find(wave, fourcc("esds"));
    }
    uint8_t objectType = 0;
    if (!esds || !parseEsds(buf.data() + esds->payload, esds->end - esds->payload, &objectType, &config->description)) return;
    if (objectType == 0x40 && !config->description.empty()) {
      int aot = config->description[0] >> 3;
      t.info.supported = aot == 2;  // AAC-LC, as on macOS
      config->codec = "mp4a.40." + std::to_string(aot);
    } else if (objectType == 0x69 || objectType == 0x6B) {
      t.info.supported = true;
      config->codec = "mp3";
      config->description.clear();
    }
  } else {
    return;
  }
  t.info.format = config;
  t.usable = true;
}

// The sample table into samples with their file offsets and times in µs. As on macOS, the edit
// list is not applied (README: edit lists are not supported): times are the media's own, so the
// AAC priming frame is at 0 and B-frame delay shows as video starting a little after 0.
bool buildSamples(const std::vector<uint8_t>& buf, const std::vector<Box>& stbl, int64_t timescale, Track& t) {
  const Box *stts = find(stbl, fourcc("stts")), *stsz = find(stbl, fourcc("stsz")), *stsc = find(stbl, fourcc("stsc"));
  const Box *stco = find(stbl, fourcc("stco")), *co64 = find(stbl, fourcc("co64"));
  const Box *ctts = find(stbl, fourcc("ctts")), *stss = find(stbl, fourcc("stss")), *sdtp = find(stbl, fourcc("sdtp"));
  if (!stts || !stsz || !stsc || (!stco && !co64)) return false;
  auto reader = [&](const Box* b) { return Reader{buf.data() + b->payload, b->end - b->payload}; };

  Reader sz = reader(stsz);
  sz.skip(4);
  uint32_t fixed = sz.u32(), count = sz.u32();
  if (!sz.ok || count > 50000000) return false;
  std::vector<Sample>& s = t.samples;
  s.resize(count);
  for (uint32_t i = 0; i < count; ++i) s[i].size = fixed ? fixed : sz.u32();

  Reader co = reader(stco ? stco : co64);
  co.skip(4);
  uint32_t chunks = co.u32();
  std::vector<int64_t> chunkOffsets(chunks);
  for (uint32_t i = 0; i < chunks; ++i) chunkOffsets[i] = int64_t(stco ? co.u32() : co.u64());

  Reader sc = reader(stsc);
  sc.skip(4);
  uint32_t entries = sc.u32();
  struct Run {
    uint32_t firstChunk, perChunk;
  };
  std::vector<Run> runs(entries);
  for (uint32_t i = 0; i < entries; ++i) {
    runs[i].firstChunk = sc.u32();
    runs[i].perChunk = sc.u32();
    sc.skip(4);
  }
  if (!sz.ok || !co.ok || !sc.ok) return false;
  size_t k = 0;
  for (uint32_t r = 0; r < entries && k < count; ++r) {
    uint32_t last = r + 1 < entries ? runs[r + 1].firstChunk : chunks + 1;
    for (uint32_t c = runs[r].firstChunk; c < last && c >= 1 && c <= chunks && k < count; ++c) {
      int64_t offset = chunkOffsets[c - 1];
      for (uint32_t j = 0; j < runs[r].perChunk && k < count; ++j, ++k) {
        s[k].offset = offset;
        offset += s[k].size;
      }
    }
  }
  if (k != count) return false;

  // Decode times; presentation = decode + composition offset.
  std::vector<int64_t> dts(count), cto(count, 0), dur(count, 0);
  Reader tt = reader(stts);
  tt.skip(4);
  uint32_t ttEntries = tt.u32();
  int64_t clock = 0;
  k = 0;
  for (uint32_t e = 0; e < ttEntries && k < count; ++e) {
    uint32_t n = tt.u32(), delta = tt.u32();
    for (uint32_t j = 0; j < n && k < count; ++j, ++k) {
      dts[k] = clock;
      dur[k] = delta;
      clock += delta;
    }
  }
  if (ctts) {
    Reader ct = reader(ctts);
    ct.skip(4);
    uint32_t ctEntries = ct.u32();
    k = 0;
    for (uint32_t e = 0; e < ctEntries && k < count; ++e) {
      uint32_t n = ct.u32();
      int32_t offset = int32_t(ct.u32());  // signed in version 1, and in practice in version 0 too
      for (uint32_t j = 0; j < n && k < count; ++j) cto[k++] = offset;
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    s[i].dtsUs = toUs(dts[i], timescale);
    s[i].ptsUs = toUs(dts[i] + cto[i], timescale);
    s[i].durUs = toUs(dur[i], timescale);
  }
  if (stss) {
    for (Sample& x : s) x.key = false;
    Reader ss = reader(stss);
    ss.skip(4);
    uint32_t n = ss.u32();
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t number = ss.u32();
      if (number >= 1 && number <= count) s[number - 1].key = true;
    }
  }
  if (sdtp) {
    Reader dp = reader(sdtp);
    dp.skip(4);
    for (uint32_t i = 0; i < count && dp.at < dp.n; ++i) {
      uint8_t b = dp.u8();
      int dependedOn = (b >> 2) & 3;
      s[i].dependedOn = int8_t(dependedOn == 0 ? -1 : dependedOn);
    }
  }
  if (count > 0 && t.info.frameDurationUs == 0) {
    int64_t minDur = 0;
    for (int64_t d : dur) {
      if (d > 0 && (minDur == 0 || d < minDur)) minDur = d;
    }
    t.info.frameDurationUs = toUs(minDur, timescale);
  }
  return true;
}

// The longest run of B-frames (samples shown before one decoded earlier), as the macOS demuxer
// probes it, over at most the first 5000 samples.
int longestBFrameRun(const std::vector<Sample>& s) {
  int64_t latest = INT64_MIN;
  int run = 0, longest = 0;
  for (size_t i = 0; i < s.size() && i < 5000; ++i) {
    if (s[i].ptsUs < latest) {
      longest = std::max(longest, ++run);
    } else {
      run = 0;
      latest = s[i].ptsUs;
    }
  }
  return longest;
}

// Every H.264 slice in the sample has nal_ref_idc 0: no other picture references it.
bool slicesUnreferenced(const uint8_t* data, size_t size, int lengthSize) {
  bool slice = false;
  for (size_t at = 0; at + size_t(lengthSize) < size;) {
    size_t length = 0;
    for (int k = 0; k < lengthSize; ++k) length = length << 8 | data[at + size_t(k)];
    at += size_t(lengthSize);
    if (length == 0 || at + length > size) return false;
    uint8_t header = data[at];
    int type = header & 0x1f;
    if (type == 1 || type == 5) {
      if (header & 0x60) return false;
      slice = true;
    }
    at += length;
  }
  return slice;
}

bool isIdentity(const uint32_t m[9]) {
  static const uint32_t id[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  return std::equal(m, m + 9, id);
}

class Mp4Demuxer : public IDemuxer {
 public:
  Result open(const MediaSource& source, MediaInfo* out) override {
    src_ = std::static_pointer_cast<ByteSource>(source.native);
    if (!src_) return Result::FileOpenFailed;
    // The top-level boxes: only moov is read whole.
    int64_t at = 0, fileSize = src_->size();
    std::vector<uint8_t> moov;
    while (at + 8 <= fileSize) {
      uint8_t h[16];
      if (!src_->read(at, h, size_t(std::min<int64_t>(16, fileSize - at)))) return Result::MalformedMedia;
      Reader r{h, 16};
      uint64_t size = r.u32();
      uint32_t type = r.u32();
      int64_t header = 8;
      if (size == 1) {
        size = r.u64();
        header = 16;
      } else if (size == 0) {
        size = uint64_t(fileSize - at);
      }
      if (size < uint64_t(header)) return Result::MalformedMedia;
      if (type == fourcc("moov")) {
        if (at + int64_t(size) > fileSize || size > (256u << 20)) return Result::MalformedMedia;
        moov.resize(size_t(size - uint64_t(header)));
        if (!src_->read(at + header, moov.data(), moov.size())) return Result::MalformedMedia;
        break;
      }
      at += int64_t(size);
    }
    if (moov.empty()) return Result::UnsupportedFormat;
    Result r = parseMoov(moov);
    if (r != Result::Ok) return r;
    if (!video_ && !audio_) return Result::UnsupportedFormat;
    MediaInfo info;
    info.durationUs = durationUs_;
    if (video_) {
      info.video = video_->info;
      info.video.maxBFrames = longestBFrameRun(video_->samples);
    }
    if (audio_) info.audio = audio_->info;
    *out = info;
    for (auto& t : tracks_) t->next = 0;  // both tracks from their first sample
    return Result::Ok;
  }

  Result peekDtsUs(int track, int64_t* out) override {
    Track* t = trackFor(track);
    if (!t || t->next >= t->samples.size()) return Result::Eos;
    *out = t->samples[t->next].dtsUs;
    return Result::Ok;
  }

  // A sample past the end of the file ends the track (a truncated tail): what was readable plays.
  Result read(int track, Packet* out) override {
    Track* t = trackFor(track);
    if (!t || t->next >= t->samples.size()) return Result::Eos;
    const Sample& s = t->samples[t->next];
    Packet p;
    p.track = track;
    p.ptsUs = s.ptsUs;
    p.dtsUs = s.dtsUs;
    p.key = s.key;
    p.data.resize(s.size);
    if (s.size == 0 || !src_->read(s.offset, p.data.data(), s.size)) {
      t->next = t->samples.size();
      return Result::Eos;
    }
    if (track == kVideo && !s.key) {
      p.disposable = s.dependedOn >= 0 ? s.dependedOn == 2 : slicesUnreferenced(p.data.data(), p.data.size(), t->nalLengthSize);
    }
    ++t->next;
    *out = std::move(p);
    return Result::Ok;
  }

  // Video restarts at the sync sample at or before `us` in decode order, and audio at the first
  // sample still playing at that sync sample's time (or at `us` without video).
  Result seekTo(int64_t us) override {
    int64_t start = us;
    if (video_ && !video_->samples.empty()) {
      const std::vector<Sample>& s = video_->samples;
      size_t at = 0;  // the sample shown at `us`: the latest presentation at or before it
      int64_t best = INT64_MIN;
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i].ptsUs <= us && s[i].ptsUs > best) {
          best = s[i].ptsUs;
          at = i;
        }
      }
      while (at > 0 && !s[at].key) --at;
      video_->next = at;
      start = s[at].ptsUs;
    }
    if (audio_) {
      const std::vector<Sample>& s = audio_->samples;
      size_t at = 0;
      while (at < s.size() && s[at].ptsUs + s[at].durUs <= start) ++at;
      audio_->next = at;
    }
    return Result::Ok;
  }

 private:
  Track* trackFor(int track) { return track == kVideo ? video_ : audio_; }

  Result parseMoov(const std::vector<uint8_t>& moov) {
    std::vector<Box> top;
    if (!children(moov, 0, moov.size(), &top)) return Result::MalformedMedia;
    int64_t movieTimescale = 1000;
    if (const Box* mvhd = find(top, fourcc("mvhd"))) {
      Reader r{moov.data() + mvhd->payload, mvhd->end - mvhd->payload};
      uint8_t version = r.u8();
      r.skip(3 + (version == 1 ? 16 : 8));
      movieTimescale = r.u32();
      int64_t duration = int64_t(version == 1 ? r.u64() : r.u32());
      if (!r.ok || movieTimescale <= 0) return Result::MalformedMedia;
      durationUs_ = toUs(duration, movieTimescale);
    }
    for (const Box& trak : top) {
      if (trak.type != fourcc("trak")) continue;
      auto t = std::make_unique<Track>();
      Result r = parseTrak(moov, trak, *t);
      if (r != Result::Ok) return r;
      if (t->handler == fourcc("vide") && !video_ && t->usable) {
        video_ = t.get();
        tracks_.push_back(std::move(t));
      } else if (t->handler == fourcc("soun") && !audio_ && t->usable) {
        audio_ = t.get();
        tracks_.push_back(std::move(t));
      }
    }
    return Result::Ok;
  }

  Result parseTrak(const std::vector<uint8_t>& buf, const Box& trak, Track& t) {
    std::vector<Box> kids, mdia, minf, stbl;
    if (!children(buf, trak.payload, trak.end, &kids)) return Result::MalformedMedia;
    if (const Box* tkhd = find(kids, fourcc("tkhd"))) {
      Reader r{buf.data() + tkhd->payload, tkhd->end - tkhd->payload};
      uint8_t version = r.u8();
      r.skip(3 + (version == 1 ? 32 : 20) + 16);
      uint32_t m[9];
      for (uint32_t& v : m) v = r.u32();
      if (r.ok) t.info.rotated = !isIdentity(m);
    }
    const Box* mdiaBox = find(kids, fourcc("mdia"));
    if (!mdiaBox || !children(buf, mdiaBox->payload, mdiaBox->end, &mdia)) return Result::MalformedMedia;
    const Box *mdhd = find(mdia, fourcc("mdhd")), *hdlr = find(mdia, fourcc("hdlr")), *minfBox = find(mdia, fourcc("minf"));
    if (!mdhd || !hdlr || !minfBox) return Result::MalformedMedia;
    Reader h{buf.data() + hdlr->payload, hdlr->end - hdlr->payload};
    h.skip(8);
    t.handler = h.u32();
    if (t.handler != fourcc("vide") && t.handler != fourcc("soun")) return Result::Ok;
    Reader m{buf.data() + mdhd->payload, mdhd->end - mdhd->payload};
    uint8_t version = m.u8();
    m.skip(3 + (version == 1 ? 16 : 8));
    int64_t timescale = m.u32();
    if (!m.ok || timescale <= 0) return Result::MalformedMedia;
    if (!children(buf, minfBox->payload, minfBox->end, &minf)) return Result::MalformedMedia;
    const Box* stblBox = find(minf, fourcc("stbl"));
    if (!stblBox || !children(buf, stblBox->payload, stblBox->end, &stbl)) return Result::MalformedMedia;
    const Box* stsd = find(stbl, fourcc("stsd"));
    if (!stsd) return Result::MalformedMedia;
    parseStsd(buf, *stsd, t);
    if (!t.usable) return Result::Ok;  // a format nobody here decodes: the track is left out
    if (!buildSamples(buf, stbl, timescale, t)) return Result::MalformedMedia;
    return Result::Ok;
  }

  std::shared_ptr<ByteSource> src_;
  std::vector<std::unique_ptr<Track>> tracks_;
  Track* video_ = nullptr;
  Track* audio_ = nullptr;
  int64_t durationUs_ = 0;
};

}  // namespace

std::shared_ptr<ByteSource> memorySource(std::vector<uint8_t> bytes) { return std::make_shared<MemorySource>(std::move(bytes)); }

MediaSource mediaSource(std::shared_ptr<ByteSource> source) { return MediaSource{std::move(source)}; }

std::unique_ptr<IDemuxer> createMp4Demuxer() { return std::make_unique<Mp4Demuxer>(); }

}  // namespace mf::web
