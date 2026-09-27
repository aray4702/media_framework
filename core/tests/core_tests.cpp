#include <thread>

#include "../src/av_sync.h"
#include "../src/bounded_queue.h"
#include "../src/master_clock.h"
#include "../src/timeline.h"
#include "fakes.h"
#include "test.h"

using namespace mf;
using fake::kMs;

// --- BoundedQueue ---------------------------------------------------------------------

static Packet packet(int64_t ptsUs, size_t bytes) {
  Packet p;
  p.ptsUs = ptsUs;
  p.data.resize(bytes);
  return p;
}

TEST(queue_caps_by_count_bytes_and_duration) {
  BoundedQueue<Packet> byCount({2});
  Packet a = packet(0, 1), b = packet(1, 1), c = packet(2, 1);
  CHECK(byCount.tryPush(a));
  CHECK(byCount.tryPush(b));
  CHECK(!byCount.tryPush(c));
  CHECK_EQ(c.data.size(), size_t(1));  // not moved from on failure

  BoundedQueue<Packet> byBytes({100, 10});
  Packet big = packet(0, 10), more = packet(1, 1);
  CHECK(byBytes.tryPush(big));  // empty queue accepts one oversized item
  CHECK(!byBytes.tryPush(more));

  BoundedQueue<Packet> byDuration({100, 1000, 2000000});
  Packet p0 = packet(0, 1), p1 = packet(2000000, 1), p2 = packet(2100000, 1);
  CHECK(byDuration.tryPush(p0));
  CHECK(byDuration.tryPush(p1));
  CHECK(!byDuration.tryPush(p2));
  Packet out;
  CHECK(byDuration.tryPop(&out));
  CHECK(byDuration.tryPush(p2));
}

// --- AudioRing --------------------------------------------------------------------------

TEST(ring_clock_holds_before_audio_and_during_underrun) {
  AudioRing ring;
  ring.open(48000, 2, 9600);
  ring.flush(1000000, 7);
  int64_t pts;
  uint32_t tag;
  CHECK(!ring.clockUs(0, &pts, &tag));  // no callback yet: hold at the base
  CHECK_EQ(pts, 1000000);
  CHECK_EQ(tag, 7u);

  std::vector<int16_t> pcm(480 * 2, 1), out(480 * 2);
  CHECK_EQ(ring.write(pcm.data(), 480), 480);
  ring.consume(out.data(), 480, 100 * kMs);
  CHECK(ring.clockUs(105 * kMs, &pts, &tag));
  CHECK_EQ(pts, 1005000);
  ring.consume(out.data(), 480, 110 * kMs);  // underrun: nothing left
  CHECK(out[0] == 0);
  CHECK(ring.clockUs(200 * kMs, &pts, &tag));
  CHECK_EQ(pts, 1010000);  // stopped at the last frame heard
}

TEST(ring_clock_is_smooth_when_callbacks_run_ahead) {
  AudioRing ring;
  ring.open(48000, 1, 9600);
  ring.flush(0, 1);
  std::vector<int16_t> pcm(4800, 1), out(480);
  ring.write(pcm.data(), 4800);
  ring.consume(out.data(), 480, 100 * kMs);  // heard from 100 ms
  ring.consume(out.data(), 480, 110 * kMs);  // delivered early: heard from 110 ms
  int64_t pts;
  uint32_t tag;
  CHECK(ring.clockUs(105 * kMs, &pts, &tag));
  CHECK_EQ(pts, 5000);  // still inside the first buffer, not jumped to the second
  CHECK(ring.clockUs(115 * kMs, &pts, &tag));
  CHECK_EQ(pts, 15000);
}

TEST(ring_flush_discards_old_data) {
  AudioRing ring;
  ring.open(48000, 1, 1000);
  std::vector<int16_t> oldPcm(500, 1), newPcm(100, 2), out(100);
  ring.flush(0, 1);
  ring.write(oldPcm.data(), 500);
  ring.flush(5000000, 2);
  ring.write(newPcm.data(), 100);
  ring.consume(out.data(), 100, 0);
  CHECK(out[0] == 2 && out[99] == 2);
  int64_t pts;
  uint32_t tag;
  CHECK(ring.clockUs(0, &pts, &tag));
  CHECK_EQ(pts, 5000000);
  CHECK_EQ(tag, 2u);
}

TEST(ring_drained_after_eos_is_heard) {
  AudioRing ring;
  ring.open(48000, 1, 1000);
  ring.flush(0, 1);
  std::vector<int16_t> pcm(480, 1), out(480);
  ring.write(pcm.data(), 480);
  ring.markEos();
  CHECK(!ring.drained(0));
  ring.consume(out.data(), 480, 0);
  CHECK(!ring.drained(5 * kMs));
  CHECK(ring.drained(10 * kMs));

  AudioRing empty;  // audio track with no samples after the seek point
  empty.open(48000, 1, 1000);
  empty.flush(0, 1);
  empty.markEos();
  CHECK(empty.drained(0));
}

// --- AvSync -----------------------------------------------------------------------------

struct SyncRun {
  int presented = 0, rateCapped = 0, late = 0;
};

// Feeds frames to AvSync the way T3 does, with a clock running in real time from 0.
static SyncRun runSync(const std::vector<int64_t>& ptsUs, int64_t frameDurUs) {
  AvSync sync;
  SyncRun r;
  int64_t now = 0;
  for (int64_t pts : ptsUs) {
    for (;;) {
      SyncDecision d = sync.decide(pts, now / 1000, now, 16666667, frameDurUs);
      if (d.kind == SyncDecision::Kind::Wait) {
        now = d.atNs;
        continue;
      }
      if (d.kind == SyncDecision::Kind::Present) ++r.presented;
      if (d.kind == SyncDecision::Kind::RateCapDrop) ++r.rateCapped;
      if (d.kind == SyncDecision::Kind::LateDrop) ++r.late;
      break;
    }
  }
  return r;
}

TEST(avsync_caps_120fps_to_60hz) {
  std::vector<int64_t> pts;
  for (int i = 0; i < 240; ++i) pts.push_back(int64_t{i} * 1000000 / 120);
  SyncRun r = runSync(pts, 8333);
  CHECK_EQ(r.late, 0);
  CHECK(r.rateCapped >= 110 && r.rateCapped <= 130);
}

TEST(avsync_jittered_60fps_is_not_capped) {
  std::vector<int64_t> pts;  // 1000 Hz timescale: 16/17 ms gaps
  for (int i = 0; i < 240; ++i) pts.push_back((i * 1000 + 30) / 60 * 1000);
  SyncRun r = runSync(pts, 16667);
  CHECK_EQ(r.rateCapped, 0);
  CHECK_EQ(r.late, 0);
  CHECK_EQ(r.presented, 240);
}

TEST(avsync_drops_late_frames_and_waits_for_early_ones) {
  AvSync sync;
  CHECK(sync.decide(0, 100000, 0, 16666667, 33333).kind == SyncDecision::Kind::LateDrop);
  SyncDecision d = sync.decide(100000, 0, 0, 16666667, 33333);
  CHECK(d.kind == SyncDecision::Kind::Wait);
  CHECK_EQ(d.atNs, 100 * kMs - 16666667);
}

// --- MasterClock ------------------------------------------------------------------------

TEST(clock_system_pause_and_resume) {
  AudioRing ring;
  MasterClock c(ring);
  c.reset(0, 1);
  c.start(0);
  CHECK_EQ(c.nowUs(10 * kMs), 10000);
  c.stop(10 * kMs);
  CHECK_EQ(c.nowUs(50 * kMs), 10000);
  c.start(100 * kMs);
  CHECK_EQ(c.nowUs(110 * kMs), 20000);
}

TEST(clock_hands_off_to_system_when_audio_ends) {
  AudioRing ring;
  ring.open(48000, 1, 1000);
  ring.flush(0, 1);
  std::vector<int16_t> pcm(480, 1), out(480);
  ring.write(pcm.data(), 480);
  ring.markEos();
  MasterClock c(ring);
  c.setAudio(true);
  c.reset(0, 1);
  c.start(0);
  ring.consume(out.data(), 480, 0);
  CHECK_EQ(c.nowUs(5 * kMs), 5000);
  CHECK_EQ(c.nowUs(20 * kMs), 10000);  // audio heard in full: hand off here
  CHECK(!c.usingAudio());
  CHECK_EQ(c.nowUs(30 * kMs), 20000);  // keeps running on the steady clock
}

// --- Player -----------------------------------------------------------------------------

static bool contains(const std::vector<State>& v, State s) { return std::find(v.begin(), v.end(), s) != v.end(); }

TEST(player_open_is_async_and_prerolls_first_frame) {
  fake::Harness h;
  CHECK(h.open() == Result::Ok);
  CHECK(h.player->state() == State::Start);  // probing happens on T1
  CHECK(h.player->play() == Result::InvalidState);
  CHECK(h.player->seek(0) == Result::InvalidState);
  h.run(20);
  CHECK(h.player->state() == State::Ready);
  CHECK_EQ(h.listener.firstFrames, 1);
  CHECK_EQ(h.lastShown(), 0);
  CHECK_EQ(h.player->durationUs(), 2000000);
  CHECK(h.player->metrics().ttffMs >= 0);
}

TEST(player_rejects_calls_in_wrong_state) {
  fake::Harness h;
  CHECK(h.player->play() == Result::InvalidState);
  CHECK(h.player->pause() == Result::InvalidState);
  CHECK(h.open() == Result::Ok);
  CHECK(h.open() == Result::InvalidState);
  h.run(20);
  CHECK(h.player->pause() == Result::InvalidState);
  CHECK(h.player->play() == Result::Ok);
  CHECK(h.player->seek(0) == Result::InvalidState);  // no seek in PLAY (A4)
  CHECK(h.player->play() == Result::InvalidState);
}

TEST(player_rejects_other_threads) {
  fake::Harness h;
  Result r = Result::Ok;
  std::thread t([&] { r = h.player->open(MediaSource{}, RenderTarget{}); });
  t.join();
  CHECK(r == Result::WrongThread);
}

TEST(player_shutdown_is_idempotent) {
  fake::Harness h;
  h.open();
  h.run(20);
  CHECK(h.player->shutdown() == Result::Ok);
  CHECK(h.player->shutdown() == Result::Ok);
  CHECK(h.player->state() == State::Shutdown);
  CHECK(h.player->play() == Result::InvalidState);
}

TEST(player_plays_in_sync_to_end_then_restarts) {
  fake::Clip clip;
  clip.durationUs = 1000000;
  fake::Harness h(clip);
  h.open();
  h.run(20);
  CHECK(h.player->play() == Result::Ok);
  h.run(1500);
  CHECK_EQ(h.listener.ended, 1);
  CHECK(h.player->state() == State::Ready);
  CHECK_EQ(h.player->positionUs(), 1000000);
  MetricsReport m = h.player->metrics();
  CHECK(m.presented >= 28);
  CHECK_EQ(m.lateDrops, 0);
  CHECK_EQ(m.janks, 0);
  CHECK(m.avSamples > 20);
  CHECK(m.avP95AbsMs <= 10);

  CHECK(h.player->play() == Result::Ok);  // restarts at 0 (A5)
  h.run(100);
  CHECK(h.player->state() == State::Play);
  CHECK(h.lastShown() < 200000);
}

TEST(player_seeks_to_exact_frame) {
  fake::Harness h;
  h.open();
  h.run(20);
  CHECK(h.player->seek(1250000) == Result::Ok);
  h.run(50);
  CHECK_EQ(h.listener.seeks.size(), size_t(1));
  int64_t expected = 37 * 1000000 / 30;  // last frame at or before 1.25 s
  CHECK_EQ(h.listener.seeks.back(), expected);
  CHECK_EQ(h.lastShown(), expected);
  CHECK_EQ(h.player->positionUs(), expected);
  CHECK(h.player->metrics().decodeOnly > 0);
}

TEST(player_scrub_honors_only_the_latest_seek) {
  fake::Harness h;
  h.open();
  h.run(20);
  h.player->seek(300000);
  h.player->seek(1500000);  // overwrites the pending slot
  h.run(50);
  CHECK_EQ(h.listener.seeks.size(), size_t(1));
  CHECK_EQ(h.listener.seeks.back(), 1500000);

  // A seek arriving while another is in flight: the in-flight one completes at once,
  // then the latest runs exactly.
  h.player->seek(500000);
  h.platform.scheduler->pumpOnce(StageId::Source);  // starts the 0.5 s seek
  h.player->seek(1800000);
  h.run(50);
  CHECK_EQ(h.listener.seeks.size(), size_t(3));
  CHECK_EQ(h.listener.seeks.back(), 1800000);
}

TEST(player_audio_shorter_than_video_still_ends) {
  fake::Clip clip;
  clip.audioDurationUs = 1000000;
  fake::Harness h(clip);
  h.open();
  h.run(20);
  h.player->play();
  h.run(2500);
  CHECK_EQ(h.listener.ended, 1);
  CHECK(h.player->metrics().presented >= 55);
}

TEST(player_plays_video_only_clip) {
  fake::Clip clip;
  clip.audio = false;
  clip.durationUs = 1000000;
  fake::Harness h(clip);
  h.open();
  h.run(20);
  h.player->play();
  h.run(1200);
  CHECK_EQ(h.listener.ended, 1);
  CHECK_EQ(h.player->metrics().lateDrops, 0);
}

TEST(player_unsupported_audio_warns_and_plays_video) {
  fake::Clip clip;
  clip.audioSupported = false;
  clip.durationUs = 1000000;
  fake::Harness h(clip);
  h.open();
  h.run(20);
  CHECK(h.listener.warnings.size() == 1 && h.listener.warnings[0] == Warning::AudioUnsupported);
  h.player->play();
  h.run(1200);
  CHECK_EQ(h.listener.ended, 1);
}

TEST(player_skips_corrupt_frames_to_next_keyframe) {
  fake::Clip clip;
  clip.corruptFrames = {10};
  fake::Harness h(clip);
  h.open();
  h.run(20);
  h.player->play();
  h.run(2500);
  CHECK_EQ(h.listener.ended, 1);
  MetricsReport m = h.player->metrics();
  CHECK_EQ(m.corruptSkips, int64_t{20});  // frame 10 fails, 11..29 skipped
  CHECK_EQ(m.lateDrops, 0);
}

TEST(player_open_failure_goes_to_error) {
  fake::Clip clip;
  clip.failOpen = true;
  fake::Harness h(clip);
  h.open();
  h.run(5);
  CHECK(h.player->state() == State::Error);
  CHECK(h.listener.errors.size() == 1 && h.listener.errors[0] == Result::FileOpenFailed);
  CHECK(contains(h.listener.states, State::Error));
  CHECK(h.player->shutdown() == Result::Ok);  // the only way out (A6)
}

TEST(player_decoder_failure_during_seek_goes_to_error) {
  fake::Clip clip;
  clip.failDecodeAtUs = 1000000;
  fake::Harness h(clip);
  h.open();
  h.run(20);
  CHECK(h.player->state() == State::Ready);
  h.player->seek(1100000);
  h.run(20);
  CHECK(h.player->state() == State::Error);
  CHECK(h.listener.errors.size() == 1 && h.listener.errors[0] == Result::DecoderFailed);
}

// --- Composition --------------------------------------------------------------------------

TEST(timeline_layout_overlaps_clips_by_the_transition) {
  TimelineLayout l;
  l.build({2000000, 3000000, 2000000}, {TransitionKind::SlideLeft, 1000000});
  CHECK_EQ(l.transitionUs(), 1000000);
  CHECK_EQ(l.startUs(1), 1000000);
  CHECK_EQ(l.startUs(2), 3000000);
  CHECK_EQ(l.durationUs(), 5000000);
  CHECK_EQ(l.firstActive(1500000), 0);
  CHECK_EQ(l.lastActive(1500000), 1);
  CHECK_EQ(l.firstActive(2000000), 1);
  CHECK_EQ(l.firstActive(9000000), 2);
  // Mid-transition, sliding left: the outgoing clip is half out, the incoming one half in.
  CHECK(l.offsetX(0, 1500000) == -0.5f);
  CHECK(l.offsetX(1, 1500000) == 0.5f);
  CHECK(l.offsetX(1, 2500000) == 0.0f);
  CHECK(l.gain(0, 1250000) == 0.75f);
  CHECK(l.gain(1, 1250000) == 0.25f);
  CHECK(l.gain(1, 2500000) == 1.0f);
  CHECK(l.gain(0, 2000000) == 0.0f);

  l.build({2000000, 600000}, {TransitionKind::SlideRight, 1000000});
  CHECK_EQ(l.transitionUs(), 300000);  // at most half the shortest clip
  CHECK_EQ(l.durationUs(), 2300000);
  CHECK(l.offsetX(0, 1850000) == 0.5f);
  CHECK(l.offsetX(1, 1850000) == -0.5f);

  l.build({1000000, 1000000}, {TransitionKind::Cut, 1000000});
  CHECK_EQ(l.transitionUs(), 0);
  CHECK_EQ(l.durationUs(), 2000000);
  CHECK(l.offsetX(1, 1000000) == 0.0f);
}

static std::vector<fake::Clip> clips(int n, int64_t durationUs, bool audio = true) {
  fake::Clip c;
  c.durationUs = durationUs;
  c.audio = audio;
  return std::vector<fake::Clip>(n, c);
}

TEST(composition_seek_into_a_transition_shows_both_clips) {
  fake::Harness h(clips(2, 2000000));
  Timeline t;
  t.transition = {TransitionKind::SlideLeft, 1000000};
  CHECK(h.open(t) == Result::Ok);
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 3000000);
  CHECK_EQ(h.lastComposed().layerCount, 1);

  h.player->seek(1500000);
  h.run(50);
  CHECK_EQ(h.listener.seeks.back(), 1500000);
  const ComposedFrame& f = h.lastComposed();
  CHECK_EQ(f.ptsUs, 1500000);
  CHECK_EQ(f.layerCount, 2);
  CHECK_EQ(f.layers[0].frame.clip, 0);  // outgoing, at 1.5 s of its own time
  CHECK_EQ(f.layers[0].frame.ptsUs, 1500000);
  CHECK(f.layers[0].offsetX == -0.5f);
  CHECK_EQ(f.layers[1].frame.clip, 1);  // incoming, 0.5 s into it
  CHECK_EQ(f.layers[1].frame.ptsUs, 500000);
  CHECK(f.layers[1].offsetX == 0.5f);
}

TEST(composition_plays_through_a_transition_with_an_audio_crossfade) {
  fake::Harness h(clips(2, 2000000));
  Timeline t;
  t.transition = {TransitionKind::SlideLeft, 1000000};
  h.open(t);
  h.run(20);
  h.player->play();
  h.run(3500);
  CHECK_EQ(h.listener.ended, 1);
  CHECK_EQ(h.player->positionUs(), 3000000);
  MetricsReport m = h.player->metrics();
  CHECK(m.presented >= 85);
  CHECK_EQ(m.lateDrops, 0);
  CHECK_EQ(m.janks, 0);
  CHECK(m.avP95AbsMs <= 10);

  int transitionFrames = 0;
  int64_t prev = -1;
  bool ordered = true;
  for (const ComposedFrame& f : h.platform.display->composed) {
    if (f.layerCount == 2) ++transitionFrames;
    ordered &= f.ptsUs > prev;
    prev = f.ptsUs;
  }
  CHECK(ordered);
  CHECK(transitionFrames >= 28);  // 1 s at 30 fps
  CHECK_EQ(h.lastComposed().layers[0].frame.clip, 1);

  // Both clips play a constant level, and their gains sum to 1: the crossfade is seamless.
  const std::vector<int16_t>& heard = h.platform.speaker->heard;
  auto first = std::find_if(heard.begin(), heard.end(), [](int16_t s) { return s != 0; });
  auto last = std::find_if(heard.rbegin(), heard.rend(), [](int16_t s) { return s != 0; }).base();
  CHECK(last - first >= 2 * 48000 * 29 / 10);  // about 3 s of stereo
  CHECK(std::all_of(first, last, [](int16_t s) { return s >= 99 && s <= 101; }));
}

TEST(composition_fades_to_a_clip_without_audio) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  c[1].audio = false;
  fake::Harness h(c);
  Timeline t;
  t.transition = {TransitionKind::SlideRight, 1000000};
  h.open(t);
  h.run(20);
  h.player->play();
  h.run(3500);
  CHECK_EQ(h.listener.ended, 1);
  CHECK(h.listener.warnings.empty());
  const std::vector<int16_t>& heard = h.platform.speaker->heard;
  CHECK(std::count(heard.begin(), heard.end(), int16_t{100}) > 0);
  CHECK(std::count_if(heard.begin(), heard.end(), [](int16_t s) { return s > 10 && s < 90; }) > 48000);
}

TEST(composition_reuses_a_lane_for_the_third_clip) {
  fake::Harness h(clips(3, 1000000));
  Timeline t;
  t.transition = {TransitionKind::SlideLeft, 250000};
  h.open(t);
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 2500000);
  h.player->play();
  h.run(3000);
  CHECK_EQ(h.listener.ended, 1);
  CHECK(h.player->metrics().presented >= 70);
  CHECK_EQ(h.player->metrics().lateDrops, 0);
  CHECK_EQ(h.lastComposed().layers[0].frame.clip, 2);

  h.player->seek(2000000);
  h.run(50);
  CHECK_EQ(h.listener.seeks.back(), 2000000);
  CHECK_EQ(h.lastComposed().layerCount, 1);
  CHECK_EQ(h.lastComposed().layers[0].frame.clip, 2);
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 500000);
}

TEST(composition_cut_has_no_overlap) {
  fake::Harness h(clips(2, 1000000));
  Timeline t;
  t.transition.kind = TransitionKind::Cut;
  h.open(t);
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 2000000);
  h.player->play();
  h.run(2500);
  CHECK_EQ(h.listener.ended, 1);
  for (const ComposedFrame& f : h.platform.display->composed) CHECK_EQ(f.layerCount, 1);
}

TEST(composition_shows_captions_in_their_time_range) {
  fake::Harness h;
  Timeline t;
  t.texts = {{"hello", 0, 1000000}};
  h.open(t);
  h.run(20);
  h.player->seek(500000);
  h.run(50);
  CHECK(h.lastComposed().text && *h.lastComposed().text == "hello");
  h.player->seek(1500000);
  h.run(50);
  CHECK(!h.lastComposed().text);
}

TEST(composition_filter_applies_at_once_even_when_paused) {
  fake::Harness h;
  Timeline t;
  t.filter = {-0.1f, 0.9f};
  h.open(t);
  h.run(20);
  CHECK(h.lastComposed().filter == (VideoFilter{-0.1f, 0.9f}));

  size_t shown = h.platform.display->composed.size();
  CHECK(h.player->setFilter({0.2f, 1.5f}) == Result::Ok);
  h.run(5);
  CHECK_EQ(h.platform.display->composed.size(), shown + 1);  // the paused frame is redrawn
  CHECK(h.lastComposed().filter == (VideoFilter{0.2f, 1.5f}));
  CHECK_EQ(h.lastComposed().ptsUs, 0);

  h.player->play();
  h.run(100);
  CHECK(h.lastComposed().filter == (VideoFilter{0.2f, 1.5f}));
  CHECK(h.player->setFilter({0, 3}) == Result::InvalidArgument);
  CHECK(h.player->setFilter({NAN, 1}) == Result::InvalidArgument);
}

TEST(composition_rejects_bad_timelines) {
  fake::Harness h;
  Timeline empty;
  CHECK(h.player->open(empty, RenderTarget{}) == Result::InvalidArgument);
  Timeline tooMany;
  tooMany.clips.resize(Timeline::kMaxClips + 1);
  CHECK(h.player->open(tooMany, RenderTarget{}) == Result::InvalidArgument);
  Timeline badText;
  badText.texts = {{"x", 1000, 1000}};
  CHECK(h.open(badText) == Result::InvalidArgument);
  CHECK(h.open(Timeline{}) == Result::Ok);  // nothing was opened by the failed calls
}

int main() {
  for (const test::Case& c : test::cases()) {
    int before = test::failures();
    c.fn();
    std::fprintf(stderr, "%s %s\n", test::failures() == before ? "PASS" : "FAIL", c.name);
  }
  std::fprintf(stderr, "%zu tests, %d failed checks\n", test::cases().size(), test::failures());
  return test::failures() == 0 ? 0 : 1;
}
