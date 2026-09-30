#include <cmath>
#include <fstream>
#include <sstream>
#include <thread>

#include "../src/av_sync.h"
#include "../src/bounded_queue.h"
#include "../src/master_clock.h"
#include "../src/json.h"
#include "../src/layout.h"
#include "mf/effects.h"
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

// Opened at a start position, the only frame shown is the one there: none at 0 before it.
TEST(player_opens_at_a_start_position) {
  fake::Harness h;
  Scene scene;
  scene.output.width = scene.output.height = 0;
  scene.output.fpsNum = 0;
  scene.output.sampleRate = scene.output.channels = 0;
  SceneTrack track;
  SceneItem item;
  item.type = ItemType::Video;
  track.items.push_back(item);
  scene.tracks.push_back(track);
  CHECK(h.player->open(scene, RenderTarget{}, OutputDriver::Auto, nullptr, 1250000) == Result::Ok);
  h.run(50);
  CHECK(h.player->state() == State::Ready);
  CHECK_EQ(h.listener.firstFrames, 1);
  CHECK(h.listener.seeks.empty());
  CHECK_EQ(h.platform.display->shown.size(), size_t(1));
  CHECK_EQ(h.lastShown(), 1250000);  // composed at the start position
  CHECK_EQ(h.player->positionUs(), 1250000);
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 37 * 1000000 / 30);  // the picture: last frame at or before it
}

// A start past the end shows the last frame.
TEST(player_open_past_the_end_shows_the_last_frame) {
  fake::Harness h;
  Scene scene;
  scene.output.width = scene.output.height = 0;
  scene.output.fpsNum = 0;
  scene.output.sampleRate = scene.output.channels = 0;
  SceneTrack track;
  SceneItem item;
  item.type = ItemType::Video;
  track.items.push_back(item);
  scene.tracks.push_back(track);
  CHECK(h.player->open(scene, RenderTarget{}, OutputDriver::Auto, nullptr, 9000000) == Result::Ok);
  h.run(50);
  CHECK(h.player->state() == State::Ready);
  CHECK_EQ(h.lastShown(), h.player->durationUs() - 1);  // composed at the clamped end
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 59 * 1000000 / 30);  // the picture is the last frame
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
  CHECK_EQ(h.listener.seeks.back(), 1250000);  // composed at the seek target
  CHECK_EQ(h.lastShown(), 1250000);
  CHECK_EQ(h.player->positionUs(), 1250000);
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 37 * 1000000 / 30);  // the picture: last frame at or before it
  CHECK(h.player->metrics().decodeOnly > 0);
}

TEST(player_redraws_an_appearance_edit_without_reopening) {
  fake::Harness h;
  Scene scene;
  SceneTrack track;
  SceneItem text;
  text.type = ItemType::Text;
  text.text = "Hi";
  text.durationUs = 1000000;
  text.transform.x = Animatable(0.5);
  track.items.push_back(text);
  scene.tracks.push_back(track);
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(30);
  CHECK(h.player->state() == State::Ready);
  size_t frames = h.platform.display->composed.size();
  int64_t decoded = h.player->metrics().decodeOnly;

  scene.tracks[0].items[0].transform.x = Animatable(0.25);
  scene.tracks[0].items[0].text = "Yo";
  CHECK(h.player->updateAppearance(scene) == Result::Ok);
  h.run(10);
  CHECK_EQ(h.platform.display->composed.size(), frames + 1);
  const ComposedFrame& f = h.lastComposed();
  CHECK_EQ(f.layers.size(), size_t(1));
  CHECK(f.layers[0].x == 0.25f);
  CHECK(*f.layers[0].text == "Yo");
  CHECK(h.listener.seeks.empty());
  CHECK_EQ(h.player->metrics().decodeOnly, decoded);

  scene.tracks[0].items[0].durationUs = 2000000;  // timing: the open scene stays as it is
  CHECK(h.player->updateAppearance(scene) == Result::InvalidArgument);
  h.run(5);
  CHECK_EQ(h.platform.display->composed.size(), frames + 1);
}

// Scrubbing slowly right: the picture only moves toward the playhead. A scrub seek's shortcut
// (the first frame decoded, usually the keyframe) isn't shown when it's farther from the target
// than the frame already on screen: it would jump back to the keyframe, then forward again.
TEST(player_slow_scrub_never_jumps_back_to_a_keyframe) {
  fake::Harness h;  // 30 fps, a keyframe every second
  h.open();
  h.run(20);
  h.player->seek(1800000);  // exact: frame 54
  h.run(50);
  CHECK_EQ(h.lastShown(), 1800000);
  auto& s = *h.platform.scheduler;
  int64_t previous = h.lastShown();
  for (int64_t target = 1850000; target <= 1950000; target += 50000) {
    size_t before = h.platform.display->shown.size();
    h.player->seek(target);
    s.pumpOnce(StageId::Source);  // starts this seek at the keyframe, 1 s
    h.player->seek(target + 20000);  // a newer one is pending: a scrub
    s.pumpOnce(StageId::Source);  // reads the keyframe
    s.pumpOnce(StageId::VideoDecode);  // decodes it
    s.pumpOnce(StageId::Composition);  // only the keyframe is ready
    h.run(50);
    for (size_t i = before; i < h.platform.display->shown.size(); ++i) {
      int64_t shown = h.platform.display->shown[i];
      if (shown < previous) std::fprintf(stderr, "  scrubbing right to %lld showed %lld after %lld\n", (long long)target, (long long)shown, (long long)previous);
      CHECK(shown >= previous);
      previous = shown;
    }
  }
  CHECK(previous >= 1933333);  // the last scrub target's frame
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

static std::string textOf(const ComposedFrame& f) {
  for (const ComposedLayer& l : f.layers) {
    if (l.kind == ComposedLayer::Kind::Text) return *l.text;
  }
  return "";
}

static int videoLayers(const ComposedFrame& f) {
  int n = 0;
  for (const ComposedLayer& l : f.layers) n += l.kind == ComposedLayer::Kind::Video;
  return n;
}

static std::vector<fake::Clip> clips(int n, int64_t durationUs, bool audio = true) {
  fake::Clip c;
  c.durationUs = durationUs;
  c.audio = audio;
  return std::vector<fake::Clip>(n, c);
}

// The clips back to back on one video track (clip k plays fake clip k), joined by pushes of
// `transitionUs`. The output takes the first clip's size, rate and audio format.
static Scene clipScene(const std::vector<fake::Clip>& c, int64_t transitionUs = 0, Direction direction = Direction::Left) {
  Scene s;
  s.output.width = s.output.height = s.output.fpsNum = 0;
  s.output.sampleRate = s.output.channels = 0;
  SceneTrack track;
  int64_t start = 0;
  for (size_t k = 0; k < c.size(); ++k) {
    SceneItem it;
    it.type = ItemType::Video;
    it.source = fake::clipSource(int(k));
    it.startUs = start;
    it.durationUs = c[k].durationUs;
    track.items.push_back(it);
    if (k + 1 < c.size() && transitionUs > 0) {
      SceneTransition x;
      x.from = int(k);
      x.kind = SceneTransitionKind::Push;
      x.direction = direction;
      x.durationUs = transitionUs;
      track.transitions.push_back(x);
    }
    start += c[k].durationUs - transitionUs;
  }
  s.tracks.push_back(track);
  return s;
}

// A caption at the bottom, on a track of its own.
static void addCaption(Scene* s, const std::string& text, int64_t startUs, int64_t endUs) {
  SceneItem it;
  it.type = ItemType::Text;
  it.text = text;
  it.startUs = startUs;
  it.durationUs = endUs - startUs;
  it.style.hasBox = true;
  it.transform.y = Animatable(0.96);
  it.transform.anchorY = 1;
  SceneTrack track;
  track.items.push_back(it);
  s->tracks.push_back(track);
}

TEST(composition_seek_into_a_transition_shows_both_clips) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  fake::Harness h(c);
  CHECK(h.openScene(clipScene(c, 1000000)) == Result::Ok);
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 3000000);
  CHECK_EQ(h.lastComposed().layers.size(), size_t(1));

  h.player->seek(1500000);
  h.run(50);
  CHECK_EQ(h.listener.seeks.back(), 1500000);
  const ComposedFrame& f = h.lastComposed();
  CHECK_EQ(f.ptsUs, 1500000);
  CHECK_EQ(f.layers.size(), size_t(2));
  CHECK_EQ(f.layers[0].frame.item, 0);  // outgoing, at 1.5 s of its own time
  CHECK_EQ(f.layers[0].frame.ptsUs, 1500000);
  CHECK(f.layers[0].offsetX == -0.5f);
  CHECK_EQ(f.layers[1].frame.item, 1);  // incoming, 0.5 s into it
  CHECK_EQ(f.layers[1].frame.ptsUs, 500000);
  CHECK(f.layers[1].offsetX == 0.5f);
}

// Opened at a time in a later image of a track, like a sticker added at the playhead after
// others: the first frame has it.
TEST(composition_open_at_a_later_image_shows_it) {
  fake::Clip clip;
  clip.durationUs = 40000000;
  fake::Harness h(clip);
  Scene scene;
  SceneTrack video, stickers;
  SceneItem v;
  v.type = ItemType::Video;
  video.items = {v};
  for (int64_t start : {0, 7000000, 13300100}) {  // the last one starts between video frames
    SceneItem image;
    image.type = ItemType::Image;
    image.startUs = start;
    image.durationUs = 5000000;
    stickers.items.push_back(image);
  }
  scene.tracks = {video, stickers};
  CHECK(h.player->open(scene, RenderTarget{}, OutputDriver::Vsync, nullptr, 13300100) == Result::Ok);
  h.run(300);
  CHECK(h.player->state() == State::Ready);
  const ComposedFrame& f = h.lastComposed();
  CHECK_EQ(f.ptsUs, 13300100);  // composed at the open time, between frames
  CHECK_EQ(f.layers.size(), size_t(2));  // the video, and the image that starts there
  CHECK_EQ(f.layers[0].frame.ptsUs, 399 * 1000000 / 30);  // the picture is the frame before it
  CHECK(f.layers[1].kind == ComposedLayer::Kind::Image);
}

TEST(composition_plays_through_a_transition_with_an_audio_crossfade) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  fake::Harness h(c);
  h.openScene(clipScene(c, 1000000));
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
    if (f.layers.size() == 2) ++transitionFrames;
    ordered &= f.ptsUs > prev;
    prev = f.ptsUs;
  }
  CHECK(ordered);
  CHECK(transitionFrames >= 28);  // 1 s at 30 fps
  CHECK_EQ(h.lastComposed().layers[0].frame.item, 1);

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
  h.openScene(clipScene(c, 1000000, Direction::Right));
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
  std::vector<fake::Clip> c = clips(3, 1000000);
  fake::Harness h(c);
  h.openScene(clipScene(c, 250000));
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 2500000);
  h.player->play();
  h.run(3000);
  CHECK_EQ(h.listener.ended, 1);
  CHECK(h.player->metrics().presented >= 70);
  CHECK_EQ(h.player->metrics().lateDrops, 0);
  CHECK_EQ(h.lastComposed().layers[0].frame.item, 2);

  h.player->seek(2000000);
  h.run(50);
  CHECK_EQ(h.listener.seeks.back(), 2000000);
  CHECK_EQ(h.lastComposed().layers.size(), size_t(1));
  CHECK_EQ(h.lastComposed().layers[0].frame.item, 2);
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 500000);
}

TEST(composition_cut_has_no_overlap) {
  std::vector<fake::Clip> c = clips(2, 1000000);
  fake::Harness h(c);
  h.openScene(clipScene(c));
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 2000000);
  h.player->play();
  h.run(2500);
  CHECK_EQ(h.listener.ended, 1);
  for (const ComposedFrame& f : h.platform.display->composed) CHECK(f.layers.size() <= 1);
}

TEST(composition_shows_captions_in_their_time_range) {
  fake::Harness h;
  Scene s = clipScene({fake::Clip{}});
  addCaption(&s, "hello", 0, 1000000);
  h.openScene(s);
  h.run(20);
  h.player->seek(500000);
  h.run(50);
  CHECK(textOf(h.lastComposed()) == "hello");
  h.player->seek(1500000);
  h.run(50);
  CHECK(textOf(h.lastComposed()).empty());
}

TEST(composition_filter_applies_at_once_even_when_paused) {
  fake::Harness h;
  h.open();
  CHECK(h.player->setFilter({-0.1f, 0.9f}) == Result::Ok);  // before the first frame
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

TEST(scene_duration_0_plays_to_the_end_of_the_file) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  fake::Harness h(c);
  Scene s = clipScene(c);
  s.tracks[0].items[0].durationUs = 0;  // 2 s
  s.tracks[0].items[1].inUs = 500000;
  s.tracks[0].items[1].durationUs = 0;  // the 1.5 s after `in`
  CHECK(h.openScene(s) == Result::Ok);
  h.run(20);
  CHECK_EQ(h.player->durationUs(), 3500000);
  h.player->seek(2500000);
  h.run(50);
  CHECK_EQ(h.lastComposed().layers.size(), size_t(1));
  CHECK_EQ(h.lastComposed().layers[0].frame.item, 1);
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 1000000);  // in + 0.5 s

  fake::Harness single;  // open(MediaSource): one video, to the end of the file
  CHECK(single.open() == Result::Ok);
  single.run(20);
  CHECK_EQ(single.player->durationUs(), 2000000);
}

TEST(scene_duration_0_is_checked_once_probed) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  {
    fake::Harness h(c);
    Scene s = clipScene(c);
    s.tracks[0].items[0].durationUs = 0;
    s.tracks[0].items[1].startUs = 1000000;  // overlaps the first once its length is known (R2)
    CHECK(h.openScene(s) == Result::Ok);
    h.run(20);
    CHECK(h.player->state() == State::Error);
    CHECK(h.listener.errors == std::vector<Result>{Result::InvalidArgument});
  }
  {
    fake::Harness h(c);
    Scene s = clipScene(c);
    s.tracks[0].items[0].durationUs = 0;
    s.tracks[0].items[0].inUs = 3000000;  // past the end of the file (R10)
    CHECK(h.openScene(s) == Result::Ok);
    h.run(20);
    CHECK(h.listener.errors == std::vector<Result>{Result::MalformedMedia});
  }
  {
    fake::Harness h(c);
    Scene s = clipScene(c);
    addCaption(&s, "x", 0, 1000000);
    s.tracks[1].items[0].durationUs = 0;  // only video and audio items have a file to end with
    std::string error;
    CHECK(h.openScene(s, OutputDriver::Auto, &error) == Result::InvalidArgument);
    CHECK(!error.empty());
  }
}

// --- Output drivers -----------------------------------------------------------------------

// Plays two clips (1 s slide) to the end and counts the frames shown mid-slide.
static int transitionFramesShown(std::vector<fake::Clip> c, OutputDriver driver, fake::Harness** keep = nullptr) {
  static std::unique_ptr<fake::Harness> h;
  h = std::make_unique<fake::Harness>(c);
  h->openScene(clipScene(c, 1000000), driver);
  h->run(20);
  h->player->play();
  h->run(3500);
  CHECK_EQ(h->listener.ended, 1);
  CHECK_EQ(h->player->metrics().lateDrops, 0);
  int n = 0;
  for (const ComposedFrame& f : h->platform.display->composed) n += f.layers.size() == 2;
  if (keep) *keep = h.get();
  return n;
}

TEST(driver_leading_clip_follows_the_highest_frame_rate) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  CHECK(transitionFramesShown(c, OutputDriver::LeadingClip) <= 31);  // 30 fps both: one per frame
  c[0].fps = 60;  // the outgoing clip is faster: it leads the slide
  CHECK(transitionFramesShown(c, OutputDriver::LeadingClip) >= 57);
}

TEST(driver_vsync_moves_the_slide_every_refresh) {
  fake::Harness* h = nullptr;
  CHECK(transitionFramesShown(clips(2, 2000000), OutputDriver::Vsync, &h) >= 57);  // 60 Hz, 30 fps clips
  for (const ComposedFrame& f : h->platform.display->composed) {
    CHECK(f.presentAtNs == 0 || f.layers.size() < 2 || std::abs(f.layers[1].offsetX - (1.0 - (f.ptsUs - 1000000) / 1e6)) < 1e-3);
  }
}

TEST(driver_vsync_skips_refreshes_with_nothing_new) {
  std::vector<fake::Clip> c = clips(1, 1000000);
  fake::Harness h(c);
  h.openScene(clipScene(c), OutputDriver::Vsync);
  h.run(20);
  h.player->play();
  h.run(1500);
  CHECK_EQ(h.listener.ended, 1);
  MetricsReport m = h.player->metrics();
  CHECK(m.presented >= 28 && m.presented <= 31);  // one per source frame, not one per refresh
  CHECK_EQ(m.janks, 0);
  CHECK(m.avP95AbsMs <= 10);
}

// --- Export -------------------------------------------------------------------------------

// The latest fake frame (i * 1 s / fps) at or before a clip's local time.
static int64_t latestFrameAt(int64_t localUs, int fps) {
  int64_t i = 0;
  while ((i + 1) * 1000000 / fps <= localUs) ++i;
  return i * 1000000 / fps;
}

// Export needs the output size and rate: 1920x1080 at `fps`, 48 kHz stereo.
static Scene exportScene(const std::vector<fake::Clip>& c, int64_t transitionUs, int fps = 30) {
  Scene s = clipScene(c, transitionUs);
  s.output = SceneOutput{};
  s.output.fpsNum = fps;
  return s;
}

TEST(export_writes_every_grid_frame_with_exact_layers) {
  std::vector<fake::Clip> c = clips(2, 2000000);
  fake::ExportHarness h(c);
  Scene s = exportScene(c, 1000000, 24);  // not the clips' 30 fps: each output frame samples the clips
  addCaption(&s, "caption", 0, 500000);
  CHECK(h.start(s) == Result::Ok);
  h.run(20000);
  CHECK_EQ(h.listener.completed, 1);
  CHECK(h.exporter->progress() == 1.0);
  fake::ExportSink& sink = *h.platform.exportSink;
  CHECK(sink.finished);
  CHECK_EQ(sink.settings.fps, 24);
  CHECK_EQ(sink.video.size(), size_t(72));  // 3 s at 24 fps
  int transition = 0;
  for (size_t n = 0; n < sink.video.size(); ++n) {
    const ComposedFrame& f = sink.video[n];
    CHECK_EQ(f.ptsUs, int64_t(n) * 1000000 / 24);
    transition += videoLayers(f) == 2;
    for (size_t i = 0; i < f.layers.size(); ++i) {
      if (f.layers[i].kind != ComposedLayer::Kind::Video) continue;
      int64_t start = f.layers[i].frame.item == 0 ? 0 : 1000000;
      CHECK_EQ(f.layers[i].frame.ptsUs, latestFrameAt(f.ptsUs - start, 30));
    }
    CHECK_EQ(!textOf(f).empty(), f.ptsUs < 500000);
  }
  CHECK_EQ(transition, 24);
  CHECK_EQ(sink.audioFrames, int64_t{3 * 48000});
  CHECK(sink.contiguous);
  auto loud = [](int16_t v) { return v < 99 || v > 101; };
  CHECK(std::none_of(sink.samples.begin(), sink.samples.end(), loud));  // the crossfade is level
}

TEST(export_keeps_going_when_the_encoder_is_busy) {
  std::vector<fake::Clip> c = clips(2, 1000000);
  fake::ExportHarness h(c, true, 3);
  CHECK(h.start(exportScene(c, 500000)) == Result::Ok);
  h.run(20000);
  CHECK_EQ(h.listener.completed, 1);
  CHECK_EQ(h.platform.exportSink->video.size(), size_t(45));  // 1.5 s at 30 fps
  CHECK_EQ(h.platform.exportSink->audioFrames, int64_t{72000});
}

TEST(export_rejects_bad_settings_and_missing_support) {
  std::vector<fake::Clip> c = clips(1, 1000000);
  fake::ExportHarness h(c);
  Scene odd = exportScene(c, 0);
  odd.output.width = 1279;
  CHECK(h.start(odd) == Result::InvalidArgument);
  Scene noRate = exportScene(c, 0);
  noRate.output.fpsNum = 0;
  CHECK(h.start(noRate) == Result::InvalidArgument);
  ExportSettings noBitrate;
  noBitrate.videoBitrate = 0;
  CHECK(h.start(exportScene(c, 0), noBitrate) == Result::InvalidArgument);
  fake::ExportHarness none(c, false);
  CHECK(none.start(exportScene(c, 0)) == Result::Unsupported);
}

// --- Scene graph ----------------------------------------------------------------------------

TEST(json_parses_values_and_reports_errors) {
  json::Value v;
  std::string error;
  CHECK(json::parse(R"({"a": [1, -2.5e3, true, null], "b": "caf\u00e9 \ud83d\ude00\n"})", &v, &error));
  CHECK(v.find("a")->array[1].number == -2500);
  CHECK(v.find("b")->string == "caf\xc3\xa9 \xf0\x9f\x98\x80\n");
  CHECK(!json::parse("{\"a\": 1,}", &v, &error));
  CHECK(error.find("line 1") == 0);
  CHECK(!json::parse("{\"a\": 1, \"a\": 2}", &v, &error) && error.find("duplicate key") != std::string::npos);
  CHECK(!json::parse(std::string(100, '['), &v, &error) && error.find("nested too deeply") != std::string::npos);
  CHECK(!json::parse("\"\\ud800\"", &v, &error));
  CHECK(!json::parse("01", &v, &error));
}

// src "clipN" plays fake clip N.
static MediaSource resolveClip(const std::string& src) { return fake::clipSource(std::atoi(src.c_str() + 4)); }

static Scene sceneFrom(const std::string& text) {
  Scene scene;
  std::string error;
  Result r = parseScene(text, resolveClip, &scene, &error);
  if (r != Result::Ok) std::fprintf(stderr, "  scene error: %s\n", error.c_str());
  CHECK(r == Result::Ok);
  return scene;
}

// A document with one video track holding `items`.
static std::string doc(const std::string& items, const std::string& output = R"({"width": 640, "height": 360, "fps": 30})") {
  return R"({"version": 1, "output": )" + output + R"(, "tracks": [{"kind": "video", "items": [)" + items + "]}]}";
}

// Scrubbing a scene with a video and something else visible (a caption): no frame goes out with
// the video missing, even when composition runs before the video has decoded anything.
TEST(player_scrub_never_shows_a_frame_without_its_video) {
  fake::Harness h;
  h.openScene(sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "video", "items": [{"type": "video", "src": "clip0", "start": 0, "duration": 2}]},
      {"kind": "video", "items": [{"type": "text", "text": "Hi", "start": 0, "duration": 2}]}]})"));
  h.run(30);
  CHECK(h.player->state() == State::Ready);
  size_t before = h.platform.display->composed.size();
  auto& s = *h.platform.scheduler;
  for (int64_t target : {500000, 1200000, 300000}) {
    h.player->seek(target);
    s.pumpOnce(StageId::Source);  // starts this seek
    h.player->seek(target + 100000);  // a newer one is pending: a scrub
    s.pumpOnce(StageId::Composition);  // before the decoder has produced a frame
    h.run(5);
  }
  h.run(50);
  const auto& composed = h.platform.display->composed;
  CHECK(composed.size() > before);
  for (size_t i = before; i < composed.size(); ++i) {
    if (composed[i].eos) continue;
    bool video = false;
    for (const ComposedLayer& l : composed[i].layers) video |= l.kind == ComposedLayer::Kind::Video;
    if (!video) std::fprintf(stderr, "  frame %zu at %lld has no video\n", i, (long long)composed[i].ptsUs);
    CHECK(video);
  }
}

TEST(scene_parses_the_example_document) {
  std::string path = std::string(__FILE__).substr(0, std::string(__FILE__).rfind("core/tests/")) + "schema/examples/two_clips_logo_title.json";
  std::ifstream file(path);
  std::stringstream text;
  text << file.rdbuf();
  Scene scene;
  std::string error;
  CHECK(parseScene(text.str(), [](const std::string&) { return MediaSource{}; }, &scene, &error) == Result::Ok);
  CHECK_EQ(scene.tracks.size(), size_t(4));
  CHECK_EQ(scene.durationUs(), 16000000);
  const SceneTrack& main = scene.tracks[0];
  CHECK_EQ(main.items.size(), size_t(2));
  CHECK_EQ(main.transitions.size(), size_t(1));
  CHECK(main.transitions[0].kind == SceneTransitionKind::Push && main.transitions[0].audio == AudioFade::EqualPower);
  CHECK_EQ(main.items[0].inUs, 1000000);
  CHECK(main.items[0].effects.colorAdjust && main.items[0].effects.saturation.value == 1.2);
  CHECK(main.items[1].fit == Fit::Cover);
  CHECK_EQ(scene.tracks[1].items[0].opacity.keys.size(), size_t(4));
  CHECK(scene.tracks[2].items[0].style.hasBox && scene.tracks[2].items[0].style.size == 0.06f);
  CHECK(!scene.tracks[3].video && scene.tracks[3].gain == 0.8f);
}

// Saving: parse → serialize → parse → serialize gives the same text, and the values survive.
static std::string reserialized(const std::string& text, Scene* scene) {
  std::string error;
  CHECK(parseScene(text, [](const std::string&) { return MediaSource{}; }, scene, &error) == Result::Ok);
  if (!error.empty()) std::fprintf(stderr, "  %s\n", error.c_str());
  return serializeScene(*scene);
}

TEST(scene_serializes_the_example_documents_back) {
  std::string dir = std::string(__FILE__).substr(0, std::string(__FILE__).rfind("core/tests/")) + "schema/examples/";
  for (const char* name : {"two_clips_logo_title.json", "demo_clips.json"}) {
    std::ifstream file(dir + name);
    std::stringstream text;
    text << file.rdbuf();
    Scene original, saved;
    std::string first = reserialized(text.str(), &original);
    std::string second = reserialized(first, &saved);
    CHECK(first == second);
    CHECK_EQ(saved.tracks.size(), original.tracks.size());
    CHECK_EQ(saved.durationUs(), original.durationUs());
    for (size_t t = 0; t < original.tracks.size(); ++t) {
      CHECK_EQ(saved.tracks[t].items.size(), original.tracks[t].items.size());
      CHECK_EQ(saved.tracks[t].transitions.size(), original.tracks[t].transitions.size());
    }
  }
}

TEST(scene_serializes_every_field_back) {
  Scene s;
  s.output = {1080, 1920, 30000, 1001, 44100, 1, {0.1f, 0.2f, 0.3f, 0.5f}};
  SceneTrack v;
  v.id = "main";
  v.opacity = 0.8f;
  v.effects.blur = true;
  v.effects.blurRadius = Animatable(0.02);
  SceneItem a, b;
  a.type = b.type = ItemType::Video;
  a.id = "a";
  a.src = "/clips/a.mp4";
  a.durationUs = 4000000;
  a.inUs = 1500000;
  a.speed = 2;
  a.mute = true;
  a.pan = Animatable(-0.5);
  a.transform.anchorX = 0;
  a.transform.x.keys = {{0, 0.25, Easing::bezier(0.1f, 0.2f, 0.3f, 0.4f)}, {2000000, 0.75, Easing{Easing::Kind::Hold}}, {4000000, 0.5, std::nullopt}};
  a.transform.x.easing = Easing::bezier(0.42f, 0, 0.58f, 1);
  a.effects.crop = a.effects.chromaKey = a.effects.colorAdjust = true;
  a.effects.cropLeft = Animatable(0.1);
  a.effects.keyColor = {0, 1, 0, 1};
  a.effects.saturation = Animatable(1.4);
  b.id = "b";
  b.src = "/clips/b.mp4";
  b.startUs = 3000000;
  b.durationUs = 5000000;
  b.blend = Blend::Screen;
  b.fit = Fit::Cover;
  v.items = {a, b};
  SceneTransition x;
  x.from = 0;
  x.kind = SceneTransitionKind::Wipe;
  x.direction = Direction::Up;
  x.durationUs = 1000000;
  x.easing = Easing::bezier(0, 0, 0.58f, 1);
  x.audio = AudioFade::EqualPower;
  v.transitions = {x};
  SceneTrack words;
  words.enabled = false;
  SceneItem t;
  t.type = ItemType::Text;
  t.text = "Say \"hi\"\nto 🌈";
  t.durationUs = 33333;
  t.style.font = "Georgia";
  t.style.hasBox = true;
  t.style.align = TextAlign::Right;
  t.opacity.keys = {{0, 0, std::nullopt}, {33333, 1, std::nullopt}};
  words.items = {t};
  SceneTrack sound;
  sound.video = false;
  sound.gain = 1.5f;
  SceneItem m;
  m.type = ItemType::Audio;
  m.src = "/music/m.mp3";
  m.durationUs = 8000000;
  m.gain = Animatable(0.5);
  sound.items = {m};
  s.tracks = {sound, v, words};
  s.metadata = "{\"editor\": {\"playhead\": 2.5}, \"tags\": [\"a\", \"b\"]}";
  CHECK(validateScene(s, nullptr) == Result::Ok);

  std::string first = serializeScene(s, [](const std::string& src) { return src.substr(1); });  // as a relative path
  Scene back;
  std::string second = reserialized(first, &back);
  CHECK(second == serializeScene(s, [](const std::string& src) { return src.substr(1); }));
  CHECK(back.output.fpsNum == 30000 && back.output.fpsDen == 1001 && back.output.sampleRate == 44100 && back.output.channels == 1);
  CHECK(back.output.background.a > 0.49f && back.output.background.a < 0.51f);
  CHECK(!back.tracks[0].video && back.tracks[0].gain == 1.5f && back.tracks[0].items[0].src == "music/m.mp3");
  CHECK(back.metadata.find("\"playhead\": 2.5") != std::string::npos && back.metadata.find("\"tags\"") != std::string::npos);
  const SceneItem& ba = back.tracks[1].items[0];
  CHECK(ba.src == "clips/a.mp4" && ba.inUs == 1500000 && ba.speed == 2 && ba.mute && ba.pan.value == -0.5);
  CHECK_EQ(ba.transform.x.keys.size(), size_t(3));
  CHECK(ba.transform.x.keys[0].easing && ba.transform.x.keys[0].easing->x2 == 0.3f);
  CHECK(ba.transform.x.keys[1].easing && ba.transform.x.keys[1].easing->kind == Easing::Kind::Hold && !ba.transform.x.keys[2].easing);
  CHECK(ba.transform.x.easing.x1 == 0.42f && ba.transform.anchorX == 0);
  CHECK(ba.effects.crop && ba.effects.chromaKey && ba.effects.colorAdjust && ba.effects.saturation.value == 1.4);
  CHECK(back.tracks[1].items[1].blend == Blend::Screen && back.tracks[1].items[1].fit == Fit::Cover);
  CHECK(back.tracks[1].effects.blur && back.tracks[1].opacity == 0.8f);
  const SceneTransition& bx = back.tracks[1].transitions[0];
  CHECK(bx.kind == SceneTransitionKind::Wipe && bx.direction == Direction::Up && bx.audio == AudioFade::EqualPower && bx.easing.x2 == 0.58f);
  const SceneItem& bt = back.tracks[2].items[0];
  CHECK(!back.tracks[2].enabled && bt.text == "Say \"hi\"\nto 🌈" && bt.durationUs == 33333);
  CHECK(bt.style.font == "Georgia" && bt.style.hasBox && bt.style.align == TextAlign::Right && bt.opacity.keys.size() == 2);
}

TEST(scene_rejects_invalid_documents_with_a_reason) {
  const std::string red = R"("type": "color", "color": "#ff0000")";
  struct Case {
    std::string text, reason;
  };
  std::vector<Case> cases = {
      {"{", "not valid JSON"},
      {doc("{" + red + R"(, "start": 0, "duration": 2}, {)" + red + R"(, "start": 1, "duration": 2})"), "without a transition"},
      {doc("{" + red + R"(, "start": 0, "duration": 2}, {"type": "transition", "kind": "crossfade", "duration": 1}, {)" + red +
           R"(, "start": 1.5, "duration": 2})"),
       "must start exactly"},
      {doc(R"({"type": "transition", "kind": "crossfade", "duration": 1}, {)" + red + R"(, "start": 0, "duration": 2})"), "sit between"},
      {doc("{" + red + R"(, "start": 0, "duration": 2, "opacity": {"keys": [[1, 0], [0.5, 1]]}})"), "key times must increase"},
      {doc("{" + red + R"(, "start": 0, "duration": 2, "opacity": 1.5})"), "between 0 and 1"},
      {doc("{" + red + R"(, "start": 0, "duration": 2, "colour": "#fff"})"), "unknown field 'colour'"},
      {doc("{" + red + R"(, "start": 0, "duration": 2})", R"({"width": 641, "height": 360, "fps": 30})"), "even"},
      {doc("{" + red + R"(, "id": "a", "start": 0, "duration": 1}, {)" + red + R"(, "id": "a", "start": 1, "duration": 1})"), "used twice"},
      {doc("{" + red + R"(, "start": 0, "duration": 2, "effects": [{"type": "blur", "radius": 0.01}, {"type": "blur", "radius": 0.02}]})"),
       "at most one blur"},
      {doc("{" + red + R"(, "start": 0, "duration": 4}, {"type": "transition", "kind": "push", "duration": 3}, {)" + red +
           R"(, "start": 1, "duration": 4})"),
       "longer than half"},
      {doc("{" + red + R"(, "start": 0, "duration": 2, "opacity": {"keys": [[0, 0, "bounce"]]}})"), "must be one of"},
      {doc("{" + red + R"(, "start": "1/0", "duration": 2})"), "num/den"},
      {doc(R"({"type": "audio", "src": "clip0", "start": 0, "duration": 2})"), "must be one of video, image"},
      {doc(""), "nothing to play"},
  };
  for (const Case& c : cases) {
    Scene scene;
    std::string error;
    Result r = parseScene(c.text, resolveClip, &scene, &error);
    bool ok = r == Result::InvalidArgument && error.find(c.reason) != std::string::npos;
    if (!ok) std::fprintf(stderr, "  expected '%s', got '%s'\n", c.reason.c_str(), error.c_str());
    CHECK(ok);
  }
}

TEST(scene_easing_and_keyframes) {
  CHECK(Easing{}.apply(0.25) == 0.25);
  CHECK(Easing{Easing::Kind::Hold}.apply(0.99) == 0);
  CHECK(std::abs(Easing::bezier(0.42f, 0, 0.58f, 1).apply(0.5) - 0.5) < 1e-6);  // easeInOut is symmetric
  CHECK(Easing::bezier(0.42f, 0, 1, 1).apply(0.5) < 0.4);                         // easeIn starts slow
  CHECK(std::abs(Easing::bezier(0, 0, 1, 1).apply(0.3) - 0.3) < 1e-6);
  Animatable a;
  a.keys = {{0, 0, std::nullopt}, {1000000, 10, Easing{Easing::Kind::Hold}}, {2000000, 20, std::nullopt}};
  CHECK(a.at(-5) == 0);
  CHECK(a.at(500000) == 5);
  CHECK(a.at(1500000) == 10);  // held until the next key
  CHECK(a.at(2000000) == 20);
  CHECK(a.at(9000000) == 20);
}

// --- Effect plugins (the core's side: types, parameters) ---------------------------------------

// Registered once per process: the registry never forgets a type.
static const EffectInfo& testEffect() {
  static const EffectInfo* info = [] {
    EffectInfo e;
    e.type = "test.fx";
    e.displayName = "Test";
    e.params = {{"amount", 0.5, 0, 1}, {"size", 0.1, 0, 0.5}};
    CHECK(registerEffect(e) == Result::Ok);
    return findEffect("test.fx");
  }();
  return *info;
}

TEST(effect_registry_checks_types_and_parameters) {
  CHECK(testEffect().param("size") == 1 && testEffect().param("nope") == -1);
  auto rejects = [](EffectInfo e, const std::string& reason) {
    std::string error;
    bool ok = registerEffect(std::move(e), &error) == Result::InvalidArgument && error.find(reason) != std::string::npos;
    if (!ok) std::fprintf(stderr, "  expected '%s', got '%s'\n", reason.c_str(), error.c_str());
    CHECK(ok);
  };
  rejects({"test.fx", "", {}}, "already registered");
  rejects({"blur", "", {}}, "built in");
  rejects({"9lives", "", {}}, "must be 1 to 64");
  rejects({"test.bad", "", {{"type", 0, 0, 1}}}, "bad parameter name");
  rejects({"test.bad", "", {{"a", 0, 0, 1}, {"a", 0, 0, 1}}}, "listed twice");
  rejects({"test.bad", "", {{"a", 2, 0, 1}}}, "min <= default <= max");
  CHECK(findEffect("test.bad") == nullptr);
  bool listed = false;
  for (const EffectInfo* e : registeredEffects()) listed |= e == &testEffect();
  CHECK(listed);
}

TEST(scene_plugin_effects_parse_validate_and_write_back) {
  testEffect();
  const std::string red = R"("type": "color", "color": "#ff0000", "start": 0, "duration": 2)";
  Scene s = sceneFrom(doc("{" + red + R"(, "effects": [{"type": "blur", "radius": 0.01}, {"type": "test.fx", "amount": {"keys": [[0, 0], [2, 1]]}}]})"));
  const SceneEffects& e = s.tracks[0].items[0].effects;
  CHECK(e.blur && e.plugins.size() == 1 && e.plugins[0].type == "test.fx");
  CHECK(e.plugins[0].params.size() == 2 && e.plugins[0].params[0].keys.size() == 2);
  CHECK(e.plugins[0].params[1].value == 0.1);  // not given: its default

  std::string text = serializeScene(s);
  CHECK(text.find("\"test.fx\"") != std::string::npos && text.find("\"amount\"") != std::string::npos);
  CHECK(text.find("\"size\"") == std::string::npos);  // at its default: left out
  Scene back;
  CHECK(reserialized(text, &back) == text);

  struct Case {
    std::string effects, reason;
  };
  for (const Case& c : std::vector<Case>{
           {R"({"type": "test.fx", "colour": 1})", "unknown field 'colour'"},
           {R"({"type": "test.fx", "amount": 2})", "effects.amount: must be between 0 and 1"},
           {R"({"type": "test.fx", "size": {"keys": [[0, 0], [3, 0.2]]}})", "within the item"},
           {R"({"type": "test.fx"}, {"type": "test.fx"})", "at most one test.fx"},
           {R"({"type": "nope"})", "'nope' is not loaded"},
       }) {
    Scene scene;
    std::string error;
    Result r = parseScene(doc("{" + red + R"(, "effects": [)" + c.effects + "]}"), resolveClip, &scene, &error);
    bool ok = r == Result::InvalidArgument && error.find(c.reason) != std::string::npos;
    if (!ok) std::fprintf(stderr, "  expected '%s', got '%s'\n", c.reason.c_str(), error.c_str());
    CHECK(ok);
  }

  // Scenes built in code are checked the same way.
  Scene coded = s;
  coded.tracks[0].items[0].effects.plugins[0].type = "nope";
  std::string error;
  CHECK(validateScene(coded, &error) == Result::InvalidArgument && error.find("no loaded effect plugin") != std::string::npos);
  coded = s;
  coded.tracks[0].items[0].effects.plugins[0].params.pop_back();
  CHECK(validateScene(coded, &error) == Result::InvalidArgument && error.find("needs 2 parameters") != std::string::npos);
}

TEST(player_composes_plugin_effects_and_edits_them_live) {
  testEffect();
  fake::Harness h;
  Scene scene;
  SceneTrack track;
  SceneItem red;
  red.type = ItemType::Color;
  red.color = {1, 0, 0, 1};
  red.durationUs = 2000000;
  ScenePluginEffect fx{"test.fx", {Animatable(0.25), Animatable(0.3)}};
  red.effects.plugins.push_back(fx);
  track.items.push_back(red);
  track.effects.plugins.push_back({"test.fx", {Animatable(1), Animatable(0)}});  // on the track's image too
  scene.tracks.push_back(track);
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(30);
  CHECK(h.player->state() == State::Ready);
  const ComposedFrame& f = h.lastComposed();
  CHECK_EQ(f.layers.size(), size_t(1));
  CHECK(f.layers[0].effects.plugins.size() == 1 && f.layers[0].effects.plugins[0].type == "test.fx");
  CHECK(f.layers[0].effects.plugins[0].params == std::vector<float>({0.25f, 0.3f}));
  CHECK(f.groups.size() == 1 && f.groups[0].effects.plugins.size() == 1 && f.groups[0].effects.plugins[0].params[0] == 1);

  size_t frames = h.platform.display->composed.size();
  scene.tracks[0].items[0].effects.plugins[0].params[0] = Animatable(0.75);
  CHECK(h.player->updateAppearance(scene) == Result::Ok);
  h.run(10);
  CHECK_EQ(h.platform.display->composed.size(), frames + 1);
  CHECK(h.lastComposed().layers[0].effects.plugins[0].params[0] == 0.75f);
}

TEST(scene_layout_assigns_lanes_by_overlap) {
  Scene stacked = sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "video", "items": [{"type": "video", "src": "clip0", "start": 0, "duration": 2}]},
      {"kind": "video", "items": [{"type": "video", "src": "clip1", "start": 0.5, "duration": 1}]},
      {"kind": "video", "items": [{"type": "video", "src": "clip2", "start": 1, "duration": 1}]},
      {"kind": "audio", "items": [{"type": "audio", "src": "clip0", "start": 5, "duration": 1}]}]})");
  SceneLayout layout;
  std::string error;
  CHECK(layout.build(stacked, &error));
  CHECK_EQ(layout.lanes(), 3);  // three videos at once; the later audio reuses a lane
  CHECK_EQ(layout.laneOf(3), 0);
  Scene apart = sceneFrom(doc(R"({"type": "video", "src": "clip0", "start": 0, "duration": 1},
                                  {"type": "video", "src": "clip1", "start": 3, "duration": 1},
                                  {"type": "video", "src": "clip2", "start": 6, "duration": 1})"));
  CHECK(layout.build(apart, &error));
  CHECK_EQ(layout.lanes(), 1);
  std::string tooMany = R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [)";
  for (int i = 0; i < 9; ++i) tooMany += std::string(i ? "," : "") + R"({"kind": "video", "items": [{"type": "video", "src": "clip0", "start": 0, "duration": 1}]})";
  CHECK(layout.build(sceneFrom(tooMany + "]}"), &error) == false);
  CHECK(error.find("more than 8") != std::string::npos);
}

TEST(scene_composites_three_stacked_videos) {
  fake::Harness h(clips(3, 2000000));
  Scene scene = sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "video", "items": [{"type": "video", "src": "clip0", "start": 0, "duration": 2}]},
      {"kind": "video", "items": [{"type": "video", "src": "clip1", "start": 0, "duration": 2,
        "transform": {"x": 0.8, "y": 0.2, "scale": 0.3}}]},
      {"kind": "video", "items": [{"type": "video", "src": "clip2", "start": 0, "duration": 2, "fit": "cover",
        "transform": {"x": 0.2, "scale": {"keys": [[0, 0.2], [2, 0.4]]}},
        "effects": [{"type": "colorAdjust", "saturation": 0}]}]}]})");
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(20);
  CHECK(h.player->state() == State::Ready);
  h.player->seek(1000000);
  h.run(50);
  const ComposedFrame& f = h.lastComposed();
  CHECK_EQ(f.width, 640);
  CHECK_EQ(f.layers.size(), size_t(3));
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(f.layers[i].item, i);  // bottom to top
    CHECK_EQ(f.layers[i].frame.ptsUs, 1000000);
  }
  CHECK(f.layers[1].x == 0.8f && f.layers[1].scale == 0.3f);
  CHECK(std::abs(f.layers[2].scale - 0.3f) < 1e-6 && f.layers[2].fit == Fit::Cover && f.layers[2].effects.saturation == 0);
  CHECK(f.groups.empty());  // one item per track, no track effects: drawn straight onto the canvas
  h.player->play();
  h.run(2500);
  CHECK_EQ(h.listener.ended, 1);
  MetricsReport m = h.player->metrics();
  CHECK_EQ(m.lateDrops, 0);
  CHECK_EQ(m.lateLayers, 0);
  for (const ComposedFrame& c : h.platform.display->composed) CHECK_EQ(c.layers.size(), size_t(3));
}

TEST(scene_draws_image_text_and_color_items) {
  fake::Harness h;
  Scene scene = sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30, "background": "#102030"}, "tracks": [
      {"kind": "video", "items": [{"type": "color", "color": "#ff000080", "start": 0, "duration": 2}]},
      {"kind": "video", "opacity": 0.5, "items": [{"type": "image", "src": "logo.png", "start": 0, "duration": 2,
        "opacity": {"keys": [[0, 0], [1, 1]]}, "blend": "screen"}]},
      {"kind": "video", "items": [{"type": "text", "text": "Hi", "start": 0.25, "duration": 1,
        "style": {"size": 0.1, "align": "left", "box": "#000000"}}]}]})");
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(20);
  h.player->seek(500000);
  h.run(50);
  const ComposedFrame& f = h.lastComposed();
  CHECK(f.background == (Color{16 / 255.f, 32 / 255.f, 48 / 255.f, 1}));
  CHECK_EQ(f.layers.size(), size_t(3));
  CHECK(f.layers[0].kind == ComposedLayer::Kind::Color && std::abs(f.layers[0].color.a - 128 / 255.f) < 1e-6);
  CHECK(f.layers[1].kind == ComposedLayer::Kind::Image && f.layers[1].frame.image && f.layers[1].blend == Blend::Screen);
  CHECK(std::abs(f.layers[1].opacity - 0.25f) < 1e-6);  // half-way up its fade, on a half-opacity track
  CHECK(f.layers[2].kind == ComposedLayer::Kind::Text && *f.layers[2].text == "Hi" && f.layers[2].fit == Fit::None);
  CHECK(f.layers[2].style.hasBox && f.layers[2].style.align == TextAlign::Left);
  h.player->seek(1500000);
  h.run(50);
  CHECK_EQ(h.lastComposed().layers.size(), size_t(2));  // the text has ended
  h.player->play();
  h.run(1000);
  CHECK_EQ(h.listener.ended, 1);  // no video or audio: the steady clock runs to the end
}

TEST(scene_transitions_crossfade_slide_and_wipe) {
  fake::Harness h;
  auto item = [](const char* color, double start) {
    return std::string(R"({"type": "color", "color": ")") + color + R"(", "start": )" + std::to_string(start) + R"(, "duration": 2})";
  };
  Scene scene = sceneFrom(doc(item("#ff0000", 0) + R"(, {"type": "transition", "kind": "crossfade", "duration": 1}, )" +
                              item("#00ff00", 1) + R"(, {"type": "transition", "kind": "slide", "direction": "up", "duration": 1}, )" +
                              item("#0000ff", 2) + R"(, {"type": "transition", "kind": "wipe", "direction": "right", "duration": 1}, )" +
                              item("#ffffff", 3)));
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(20);
  auto at = [&](int64_t t) -> const ComposedFrame& {
    h.player->seek(t);
    h.run(30);
    return h.lastComposed();
  };
  const ComposedFrame& fade = at(1250000);  // a quarter through: (1 − p)·A + p·B, summed in the track's own image
  CHECK_EQ(fade.layers.size(), size_t(2));
  CHECK_EQ(fade.groups.size(), size_t(1));
  CHECK(fade.layers[0].group == 0 && fade.layers[1].group == 0);
  CHECK(std::abs(fade.layers[0].opacity - 0.75f) < 1e-6 && std::abs(fade.layers[1].opacity - 0.25f) < 1e-6);
  CHECK(fade.layers[0].blend == Blend::Add && fade.layers[1].blend == Blend::Add);
  const ComposedFrame& slide = at(2500000);
  CHECK(slide.layers[0].offsetY == 0 && std::abs(slide.layers[1].offsetY - 0.5f) < 1e-6);  // coming up from below
  CHECK(slide.layers[0].opacity == 1 && slide.layers[1].blend == Blend::Normal && slide.layers[1].group == 0);  // B over A
  const ComposedFrame& wipe = at(3250000);
  CHECK(wipe.layers[1].clip[0] == 0 && std::abs(wipe.layers[1].clip[2] - 0.25f) < 1e-6);   // revealed from the left
}

// §5.1: a track's pair is combined on its own, then its effects, opacity and the top item's
// blend apply to the result. A lone item without track effects is drawn straight onto the canvas.
TEST(scene_groups_a_track_in_a_transition_or_with_effects) {
  fake::Harness h;
  Scene scene = sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "video", "items": [{"type": "color", "color": "#00ff00", "start": 0, "duration": 4}]},
      {"kind": "video", "opacity": 0.5, "items": [
        {"type": "color", "color": "#ff0000", "start": 0, "duration": 2, "opacity": 0.8},
        {"type": "transition", "kind": "push", "duration": 1},
        {"type": "color", "color": "#0000ff", "start": 1, "duration": 2, "blend": "screen"}]},
      {"kind": "video", "effects": [{"type": "colorAdjust", "brightness": {"keys": [[0, 0], [4, -0.4]]}},
                                    {"type": "crop", "right": 0.5}],
        "items": [{"type": "color", "color": "#ffffff", "start": 2, "duration": 2}]}]})");
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(20);
  auto at = [&](int64_t t) -> const ComposedFrame& {
    h.player->seek(t);
    h.run(30);
    return h.lastComposed();
  };
  const ComposedFrame& push = at(1500000);
  CHECK_EQ(push.layers.size(), size_t(3));
  CHECK_EQ(push.layers[0].group, -1);  // the bottom track
  CHECK_EQ(push.groups.size(), size_t(1));
  const ComposedGroup& pair = push.groups[0];
  CHECK(pair.track == 1 && pair.opacity == 0.5f && pair.blend == Blend::Screen);  // track opacity, B's blend
  CHECK(push.layers[1].group == 0 && push.layers[2].group == 0);
  CHECK(std::abs(push.layers[1].opacity - 0.8f) < 1e-6 && push.layers[2].opacity == 1);  // without the track's opacity
  CHECK(push.layers[1].blend == Blend::Normal && push.layers[2].blend == Blend::Normal);

  const ComposedFrame& alone = at(500000);  // before the push: one item, no track effects
  CHECK(alone.groups.empty() && alone.layers.size() == size_t(2));
  CHECK(std::abs(alone.layers[1].opacity - 0.4f) < 1e-6);  // 0.8 on a half-opacity track

  const ComposedFrame& effects = at(2500000);  // after the push; the top track has effects, keyed in scene time
  CHECK_EQ(effects.groups.size(), size_t(1));
  const ComposedGroup& top = effects.groups[0];
  CHECK_EQ(top.track, 2);
  CHECK(std::abs(top.effects.brightness - -0.25f) < 1e-6 && top.effects.crop[2] == 0.5f);
  CHECK(effects.layers[1].group == -1 && effects.layers[2].group == 0);
  CHECK(effects.layers[2].effects.brightness == 0);  // the track's effects aren't the item's

  std::string error;
  CHECK(parseScene(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "video", "effects": [{"type": "blur", "radius": 0.5}], "items": [{"type": "color", "color": "#ffffff", "start": 0, "duration": 1}]}]})",
                   resolveClip, &scene, &error) == Result::InvalidArgument);
  CHECK(error.find("tracks[0].effects.radius") != std::string::npos);
  CHECK(parseScene(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "audio", "effects": [{"type": "blur", "radius": 0.01}], "items": [{"type": "audio", "src": "clip0", "start": 0, "duration": 1}]}]})",
                   resolveClip, &scene, &error) == Result::InvalidArgument);
}

TEST(scene_speed_and_in_map_to_media_time) {
  fake::Harness h;
  CHECK(h.openScene(sceneFrom(doc(R"({"type": "video", "src": "clip0", "start": 0, "duration": 0.5, "in": 0.5, "speed": 2})"))) ==
        Result::Ok);
  h.run(20);
  h.player->seek(250000);
  h.run(50);
  CHECK_EQ(h.lastComposed().layers[0].frame.ptsUs, 1000000);  // 0.5 + 0.25 × 2
}

TEST(scene_mixes_resampled_audio_with_gain_and_pan) {
  std::vector<fake::Clip> c = clips(1, 2000000);
  c[0].sampleRate = 44100;  // mixed into 48 kHz
  fake::Harness h(c);
  Scene scene = sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30, "sampleRate": 48000}, "tracks": [
      {"kind": "audio", "items": [{"type": "audio", "src": "clip0", "start": 0, "duration": 1, "gain": 0.5, "pan": 1}]}]})");
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(20);
  h.player->play();
  h.run(1500);
  CHECK_EQ(h.listener.ended, 1);
  const std::vector<int16_t>& heard = h.platform.speaker->heard;
  int64_t right = 0;
  bool leftSilent = true, level = true;
  for (size_t k = 0; k + 1 < heard.size(); k += 2) {
    leftSilent &= heard[k] == 0;
    if (heard[k + 1] != 0) {
      ++right;
      level &= heard[k + 1] >= 49 && heard[k + 1] <= 51;
    }
  }
  CHECK(leftSilent);
  CHECK(level);                                    // no gaps or doubled samples from resampling
  CHECK(right >= 47500 && right <= 48500);         // one second
}

TEST(scene_equal_power_crossfade_keeps_loudness) {
  fake::Harness h(clips(2, 2000000));
  Scene scene = sceneFrom(R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [
      {"kind": "audio", "items": [{"type": "audio", "src": "clip0", "start": 0, "duration": 2},
        {"type": "transition", "kind": "crossfade", "duration": 1, "audio": "equalPower"},
        {"type": "audio", "src": "clip1", "start": 1, "duration": 2}]}]})");
  CHECK(h.openScene(scene) == Result::Ok);
  h.run(20);
  h.player->play();
  h.run(3500);
  CHECK_EQ(h.listener.ended, 1);
  const std::vector<int16_t>& heard = h.platform.speaker->heard;
  int16_t peak = *std::max_element(heard.begin(), heard.end());
  CHECK(peak >= 139 && peak <= 142);  // 100 × (cos + sin)(π/4) at the midpoint
}

TEST(scene_export_uses_the_output_size_and_rate) {
  fake::ExportHarness h(clips(1, 2000000));
  Scene scene = sceneFrom(doc(R"({"type": "video", "src": "clip0", "start": 0, "duration": 1})",
                              R"({"width": 640, "height": 360, "fps": "30000/1001"})"));
  CHECK(h.exporter->start(scene, ExportTarget{}) == Result::Ok);
  h.run(20000);
  CHECK_EQ(h.listener.completed, 1);
  fake::ExportSink& sink = *h.platform.exportSink;
  CHECK_EQ(sink.settings.width, 640);
  CHECK_EQ(sink.settings.fps, 30);
  CHECK_EQ(sink.video.size(), size_t(30));  // 1 s at 29.97 fps
  CHECK_EQ(sink.video[29].ptsUs, int64_t{29} * 1001 * 1000000 / 30000);
  CHECK_EQ(sink.video[0].height, 360);
}

TEST(export_scales_the_scene_to_the_frame_size) {
  fake::ExportHarness h(clips(1, 2000000));
  Scene scene = sceneFrom(doc(R"({"type": "video", "src": "clip0", "start": 0, "duration": 1})",
                              R"({"width": 640, "height": 360, "fps": 30})"));
  ExportSettings settings;
  settings.frameWidth = 320;
  settings.frameHeight = 180;
  CHECK(h.exporter->start(scene, ExportTarget{}, settings) == Result::Ok);
  h.run(20000);
  CHECK_EQ(h.listener.completed, 1);
  fake::ExportSink& sink = *h.platform.exportSink;
  CHECK_EQ(sink.settings.width, 320);  // the file's frames
  CHECK_EQ(sink.settings.height, 180);
  CHECK_EQ(sink.video[0].width, 640);  // composed at the scene's size, which the sink scales
  CHECK_EQ(sink.video[0].height, 360);

  fake::ExportHarness odd(clips(1, 2000000));
  settings.frameWidth = 321;  // frames need even sizes
  CHECK(odd.exporter->start(scene, ExportTarget{}, settings) == Result::InvalidArgument);
}

TEST(scene_open_reports_invalid_scenes_and_too_many_lanes) {
  fake::Harness h;
  Scene bad;
  std::string error;
  CHECK(h.openScene(bad, OutputDriver::Auto, &error) == Result::InvalidArgument);
  CHECK(!error.empty());
  std::string tooMany = R"({"version": 1, "output": {"width": 640, "height": 360, "fps": 30}, "tracks": [)";
  for (int i = 0; i < 9; ++i) tooMany += std::string(i ? "," : "") + R"({"kind": "video", "items": [{"type": "video", "src": "clip0", "start": 0, "duration": 1}]})";
  CHECK(h.openScene(sceneFrom(tooMany + "]}")) == Result::Ok);  // valid; the decoders are the limit
  h.run(20);
  CHECK(h.player->state() == State::Error);
  CHECK(h.listener.errors.size() == 1 && h.listener.errors[0] == Result::Unsupported);
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
