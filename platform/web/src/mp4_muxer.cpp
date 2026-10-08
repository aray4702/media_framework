#include "mp4_muxer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace mf::web {

bool MemoryWriter::append(const uint8_t* data, size_t size) {
  bytes.insert(bytes.end(), data, data + size);
  return true;
}

bool MemoryWriter::patch(uint64_t offset, const uint8_t* data, size_t size) {
  if (offset + size > bytes.size()) return false;
  std::memcpy(bytes.data() + offset, data, size);
  return true;
}

namespace {

// Big-endian box writing.
struct Out {
  std::vector<uint8_t> b;
  void u8(uint32_t v) { b.push_back(uint8_t(v)); }
  void u16(uint32_t v) { u8(v >> 8), u8(v); }
  void u24(uint32_t v) { u8(v >> 16), u16(v); }
  void u32(uint32_t v) { u16(v >> 16), u16(v); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)), u32(uint32_t(v)); }
  void zeros(size_t n) { b.insert(b.end(), n, 0); }
  void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
  void fourcc(const char* s) { b.insert(b.end(), s, s + 4); }
};

std::vector<uint8_t> box(const char* type, const std::vector<uint8_t>& body) {
  Out o;
  o.u32(uint32_t(8 + body.size()));
  o.fourcc(type);
  o.bytes(body);
  return o.b;
}

// A full box: version and flags, then the body.
std::vector<uint8_t> fullBox(const char* type, uint8_t version, uint32_t flags, const std::vector<uint8_t>& body) {
  Out o;
  o.u8(version);
  o.u24(flags);
  o.bytes(body);
  return box(type, o.b);
}

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts) {
  std::vector<uint8_t> out;
  for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

void matrix(Out& o) {
  for (uint32_t v : {0x10000u, 0u, 0u, 0u, 0x10000u, 0u, 0u, 0u, 0x40000000u}) o.u32(v);
}

int64_t ticks(int64_t us, uint32_t timescale) { return std::llround(double(us) * timescale / 1e6); }

// An MPEG-4 descriptor with a 4-byte length.
std::vector<uint8_t> descriptor(uint8_t tag, const std::vector<uint8_t>& body) {
  Out o;
  o.u8(tag);
  uint32_t n = uint32_t(body.size());
  o.u8(0x80 | ((n >> 21) & 0x7f)), o.u8(0x80 | ((n >> 14) & 0x7f)), o.u8(0x80 | ((n >> 7) & 0x7f)), o.u8(n & 0x7f);
  o.bytes(body);
  return o.b;
}

}  // namespace

int Mp4Muxer::addTrack(const Track& t) {
  tracks_.push_back({t, t.video ? 90000u : uint32_t(std::max(1, t.sampleRate)), {}});
  return int(tracks_.size()) - 1;
}

// ftyp, then an mdat with a 64-bit size patched in finish().
bool Mp4Muxer::start() {
  Out o;
  o.u32(8 + 16);
  o.fourcc("ftyp");
  o.fourcc("isom");
  o.u32(0x200);
  o.fourcc("isom");
  o.fourcc("mp41");
  mdatStart_ = out_.size() + o.b.size();
  o.u32(1);
  o.fourcc("mdat");
  o.u64(0);
  started_ = true;
  return out_.append(o.b.data(), o.b.size());
}

bool Mp4Muxer::write(int track, const uint8_t* data, size_t size, int64_t ptsUs, int64_t durationUs, bool key) {
  if (!ok_ || track < 0 || track >= int(tracks_.size())) return false;
  if (!started_ && !(ok_ = start())) return false;
  tracks_[size_t(track)].samples.push_back({out_.size(), uint32_t(size), ptsUs, durationUs, key});
  return ok_ = out_.append(data, size);
}

// Decode times are the presentation times in order, starting at 0; each sample's composition
// offset (ctts, signed) brings back its own presentation time. So B-frames round-trip, and a
// stream whose first frame shows after 0 (B-frame delay) keeps its times without an edit list.
Mp4Muxer::Timing Mp4Muxer::timing(const State& t) {
  const std::vector<Sample>& s = t.samples;
  size_t n = s.size();
  Timing x{std::vector<int64_t>(n), std::vector<int64_t>(n), std::vector<int64_t>(n), 0};
  std::vector<int64_t> sorted(n);
  for (size_t i = 0; i < n; ++i) x.pts[i] = sorted[i] = ticks(s[i].ptsUs, t.timescale);
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 0; i < n; ++i) x.dts[i] = sorted[i] - sorted[0];
  for (size_t i = 0; i < n; ++i) {
    if (i + 1 < n) {
      x.dur[i] = x.dts[i + 1] - x.dts[i];
    } else {
      x.dur[i] = s[i].durationUs > 0 ? ticks(s[i].durationUs, t.timescale) : (i > 0 ? x.dur[i - 1] : 0);
    }
  }
  if (n) x.end = sorted[n - 1] + x.dur[n - 1];
  return x;
}

std::vector<uint8_t> Mp4Muxer::trak(const State& t, uint32_t id, uint64_t movieDurationMs) const {
  const std::vector<Sample>& s = t.samples;
  const uint32_t ts = t.timescale;
  size_t n = s.size();
  Timing x = timing(t);
  const std::vector<int64_t>&pts = x.pts, &dts = x.dts, &dur = x.dur;
  // The media runs to the presentation end, so the last frames shown aren't cut off (without an
  // edit list, the track plays for its duration).
  int64_t mediaDuration = x.end;
  bool reordered = false, allKey = true;
  for (size_t i = 0; i < n; ++i) {
    reordered |= pts[i] != dts[i];
    allKey &= s[i].key;
  }

  Out tkhd;
  tkhd.zeros(8);  // creation, modification
  tkhd.u32(id);
  tkhd.zeros(4);
  tkhd.u32(uint32_t(movieDurationMs));
  tkhd.zeros(8);
  tkhd.u16(0);                              // layer
  tkhd.u16(0);                              // alternate group
  tkhd.u16(t.track.video ? 0 : 0x0100);     // volume
  tkhd.u16(0);
  matrix(tkhd);
  tkhd.u32(uint32_t(t.track.width) << 16);
  tkhd.u32(uint32_t(t.track.height) << 16);

  Out mdhd;
  mdhd.zeros(8);
  mdhd.u32(ts);
  mdhd.u32(uint32_t(mediaDuration));
  mdhd.u16(0x55c4);  // "und"
  mdhd.u16(0);

  Out hdlr;
  hdlr.zeros(4);
  hdlr.fourcc(t.track.video ? "vide" : "soun");
  hdlr.zeros(12);
  std::string name = t.track.video ? "VideoHandler" : "SoundHandler";
  hdlr.b.insert(hdlr.b.end(), name.begin(), name.end());
  hdlr.u8(0);

  Out entry;
  entry.zeros(6);
  entry.u16(1);  // data reference index
  std::vector<uint8_t> sampleEntry;
  if (t.track.video) {
    entry.zeros(16);
    entry.u16(uint32_t(t.track.width));
    entry.u16(uint32_t(t.track.height));
    entry.u32(0x00480000), entry.u32(0x00480000);  // 72 dpi
    entry.zeros(4);
    entry.u16(1);  // frames per sample
    entry.zeros(32);
    entry.u16(0x18);
    entry.u16(0xffff);
    entry.bytes(box("avcC", t.track.config));
    sampleEntry = box("avc1", entry.b);
  } else {
    entry.zeros(8);
    entry.u16(uint32_t(t.track.channels));
    entry.u16(16);
    entry.zeros(4);
    entry.u32(uint32_t(t.track.sampleRate) << 16);
    Out dcd;
    dcd.u8(0x40);  // AAC
    dcd.u8(0x15);  // audio stream
    dcd.u24(0);
    dcd.u32(0), dcd.u32(0);  // bit rates
    dcd.bytes(descriptor(5, t.track.config));
    Out es;
    es.u16(id);
    es.u8(0);
    es.bytes(descriptor(4, dcd.b));
    es.bytes(descriptor(6, {0x02}));
    entry.bytes(fullBox("esds", 0, 0, descriptor(3, es.b)));
    sampleEntry = box("mp4a", entry.b);
  }

  Out stsd;
  stsd.u32(1);
  stsd.bytes(sampleEntry);

  Out stts;  // run-length decode durations
  std::vector<std::pair<uint32_t, uint32_t>> runs;
  for (size_t i = 0; i < n; ++i) {
    if (!runs.empty() && runs.back().second == uint32_t(dur[i])) ++runs.back().first;
    else runs.push_back({1, uint32_t(dur[i])});
  }
  stts.u32(uint32_t(runs.size()));
  for (auto [count, delta] : runs) stts.u32(count), stts.u32(delta);

  Out ctts;
  if (reordered) {
    std::vector<std::pair<uint32_t, int32_t>> offs;
    for (size_t i = 0; i < n; ++i) {
      int32_t o = int32_t(pts[i] - dts[i]);
      if (!offs.empty() && offs.back().second == o) ++offs.back().first;
      else offs.push_back({1, o});
    }
    ctts.u32(uint32_t(offs.size()));
    for (auto [count, o] : offs) ctts.u32(count), ctts.u32(uint32_t(o));
  }

  Out stss;
  if (!allKey) {
    std::vector<uint32_t> keys;
    for (size_t i = 0; i < n; ++i) {
      if (s[i].key) keys.push_back(uint32_t(i + 1));
    }
    stss.u32(uint32_t(keys.size()));
    for (uint32_t k : keys) stss.u32(k);
  }

  Out stsc;  // one sample per chunk
  stsc.u32(1);
  stsc.u32(1), stsc.u32(1), stsc.u32(1);

  Out stsz;
  stsz.u32(0);
  stsz.u32(uint32_t(n));
  for (const Sample& x : s) stsz.u32(x.size);

  bool large = n && s.back().offset > 0xffffffffull;
  Out stco;
  stco.u32(uint32_t(n));
  for (const Sample& x : s) large ? stco.u64(x.offset) : stco.u32(uint32_t(x.offset));

  std::vector<uint8_t> stbl = concat({fullBox("stsd", 0, 0, stsd.b), fullBox("stts", 0, 0, stts.b)});
  if (reordered) stbl = concat({stbl, fullBox("ctts", 1, 0, ctts.b)});
  if (!allKey) stbl = concat({stbl, fullBox("stss", 0, 0, stss.b)});
  stbl = concat({stbl, fullBox("stsc", 0, 0, stsc.b), fullBox("stsz", 0, 0, stsz.b), fullBox(large ? "co64" : "stco", 0, 0, stco.b)});

  Out vmhd;
  vmhd.zeros(8);
  Out smhd;
  smhd.zeros(4);
  Out dref;
  dref.u32(1);
  dref.bytes(fullBox("url ", 0, 1, {}));
  std::vector<uint8_t> minf = concat({t.track.video ? fullBox("vmhd", 0, 1, vmhd.b) : fullBox("smhd", 0, 0, smhd.b),
                                      box("dinf", fullBox("dref", 0, 0, dref.b)), box("stbl", stbl)});
  std::vector<uint8_t> mdia = concat({fullBox("mdhd", 0, 0, mdhd.b), fullBox("hdlr", 0, 0, hdlr.b), box("minf", minf)});
  return box("trak", concat({fullBox("tkhd", 0, 3, tkhd.b), box("mdia", mdia)}));
}

bool Mp4Muxer::finish() {
  if (!started_ && !(ok_ = ok_ && start())) return false;
  if (!ok_) return false;
  // The mdat's size, now that it is written.
  Out size;
  size.u64(out_.size() - mdatStart_);
  if (!out_.patch(mdatStart_ + 8, size.b.data(), 8)) return false;

  uint64_t durationMs = 0;
  for (const State& t : tracks_) {
    if (!t.samples.empty()) durationMs = std::max<uint64_t>(durationMs, uint64_t((timing(t).end * 1000 + t.timescale - 1) / t.timescale));
  }
  Out mvhd;
  mvhd.zeros(8);
  mvhd.u32(1000);
  mvhd.u32(uint32_t(durationMs));
  mvhd.u32(0x10000);  // rate
  mvhd.u16(0x0100);   // volume
  mvhd.zeros(10);
  matrix(mvhd);
  mvhd.zeros(24);
  mvhd.u32(uint32_t(tracks_.size() + 1));  // next track id
  std::vector<uint8_t> moov = fullBox("mvhd", 0, 0, mvhd.b);
  for (size_t i = 0; i < tracks_.size(); ++i) moov = concat({moov, trak(tracks_[i], uint32_t(i + 1), durationMs)});
  moov = box("moov", moov);
  return ok_ = out_.append(moov.data(), moov.size());
}

}  // namespace mf::web
