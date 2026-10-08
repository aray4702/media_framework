// The C API the page calls (see app/spike.js). The player runs on its own thread (a pthread, so a
// worker), which owns the canvas (transferred to it), the WebGPU device and the decoders; the page
// keeps only the AudioContext, which browsers allow only there. Calls return at once: they are
// carried out on the player thread, in order. Their results, the player's state and its metrics
// appear in mf_report(), a snapshot the player thread refreshes.

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>

#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <algorithm>
#include <string>
#include <vector>

#include "mf/exporter.h"
#include "mf/player.h"
#include "mf/scene.h"
#include "mf/web.h"
#include "mp4_demuxer.h"
#include "mp4_muxer.h"

extern "C" {
// library_mf.js, on the player thread.
void mf_js_gpu_init(void* session);
void mf_js_decode_image(void* session, const char* name, const uint8_t* bytes, int length);
const char* mf_js_stats_json();
}

namespace {

struct Session;

// An export's events, into the session's.
struct ExportEvents : mf::ExportListener {
  Session* s = nullptr;
  void onCompleted() override;
  void onError(mf::Result, const std::string& reason) override;
};

struct Session : mf::PlayerListener {
  pthread_t thread{};
  std::string canvas;  // the transferred canvas's selector

  // On the player thread only.
  std::unique_ptr<mf::PlatformFactory> platform;
  std::unique_ptr<mf::Player> player;
  std::map<std::string, std::shared_ptr<mf::web::ByteSource>> sources;
  int pending = 0;                      // the WebGPU device and images still being prepared
  std::vector<std::function<void()>> whenReady;  // open() and export() calls waiting for them, in order
  std::string error, events;
  std::unique_ptr<mf::Exporter> exporter;
  ExportEvents exportEvents;
  std::shared_ptr<mf::web::MemoryWriter> exportFile;  // the exported MP4, complete once "exported"
  double exportStartedMs = 0, exportMs = 0;

  // The snapshot the page reads.
  std::mutex mu;
  std::string report, reportCopy;

  bool playWhenReady = false;  // mf_apply reopened a playing scene: play again once it's ready

  void addEvent(const std::string& e) { events += std::string(events.empty() ? "" : ",") + "\"" + e + "\""; }
  void onStateChanged(mf::State state) override;
  void onError(mf::Result r, const std::string& reason) override {
    error = std::string(mf::toString(r)) + ": " + reason;
    addEvent("error");
  }
  void onEnded() override { addEvent("ended"); }
  void onSeekCompleted(int64_t) override { addEvent("seeked"); }
};

// Runs f on the player thread, after the calls posted before it.
void post(Session* s, std::function<void()> f);

void Session::onStateChanged(mf::State state) {
  addEvent(std::string("state:") + mf::toString(state));
  if (state == mf::State::Ready && playWhenReady) {
    playWhenReady = false;
    post(this, [this] { player->play(); });  // not from inside the player's callback
  }
}

void ExportEvents::onCompleted() {
  s->exportMs = emscripten_get_now() - s->exportStartedMs;
  s->addEvent("exported");
}
void ExportEvents::onError(mf::Result r, const std::string& reason) {
  s->error = std::string(mf::toString(r)) + ": " + reason;
  s->addEvent("exportFailed");
}

std::string quoted(const std::string& s) {
  std::string out = "\"";
  for (char c : s) out += c == '"' ? std::string("\\\"") : std::string(1, c);
  return out + "\"";
}

// On the player thread: the state, events, metrics and frame statistics, as JSON.
void snapshot(Session* s) {
  std::string body = "{\"state\":" + quoted(s->player ? mf::toString(s->player->state()) : "Start") + ",\"error\":" + quoted(s->error) +
                     ",\"events\":[" + s->events + "]";
  if (s->player) {
    mf::MetricsReport m = s->player->metrics();
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  ",\"positionUs\":%lld,\"metrics\":{\"presented\":%lld,\"lateDrops\":%lld,\"rateCapDrops\":%lld,\"lateLayers\":%lld,"
                  "\"droppedRate\":%.4f,\"janks\":%lld,\"intervals\":%lld,\"jankRate\":%.4f,\"avSamples\":%lld,\"avMeanMs\":%.2f,"
                  "\"avMeanAbsMs\":%.2f,\"avP95AbsMs\":%.1f,\"ttffMs\":%.1f,\"decodeSkips\":%lld,\"decodeStepDowns\":%lld,"
                  "\"decodeStepUps\":%lld,\"holdExpiries\":%lld,\"corruptSkips\":%lld}",
                  (long long)s->player->positionUs(), (long long)m.presented, (long long)m.lateDrops, (long long)m.rateCapDrops,
                  (long long)m.lateLayers, m.droppedRate, (long long)m.janks, (long long)m.intervals, m.jankRate, (long long)m.avSamples,
                  m.avMeanMs, m.avMeanAbsMs, m.avP95AbsMs, m.ttffMs, (long long)m.decodeSkips, (long long)m.decodeStepDowns,
                  (long long)m.decodeStepUps, (long long)m.holdExpiries, (long long)m.corruptSkips);
    body += buf;
  }
  if (s->exporter) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), ",\"export\":{\"progress\":%.4f,\"bytes\":%llu,\"ms\":%.0f}", s->exporter->progress(),
                  (unsigned long long)(s->exportFile ? s->exportFile->bytes.size() : 0), s->exportMs);
    body += buf;
  }
  body += std::string(",\"frames\":") + mf_js_stats_json() + "}";
  std::lock_guard<std::mutex> lock(s->mu);
  s->report = std::move(body);
}

void post(Session* s, std::function<void()> f) {
  auto* fn = new std::function<void()>(std::move(f));
  emscripten_proxy_async(emscripten_proxy_get_system_queue(), s->thread, [](void* p) {
    auto* fn = static_cast<std::function<void()>*>(p);
    (*fn)();
    delete fn;
  }, fn);
}

void* playerThread(void* p) {
  auto* s = static_cast<Session*>(p);
  s->platform = mf::web::createPlatform();
  s->player = mf::Player::create(*s->platform, s);
  ++s->pending;
  mf_js_gpu_init(s);  // mf_web_prepared() once the device is ready
  emscripten_set_interval([](void* p) { snapshot(static_cast<Session*>(p)); }, 50, s);
  snapshot(s);
  emscripten_unwind_to_js_event_loop();  // the thread lives on, serving its event loop
  return nullptr;
}

}  // namespace

extern "C" {

// The canvas (a CSS selector, "#view") is transferred to the player thread: the page can no longer
// draw into it or resize its pixels.
EMSCRIPTEN_KEEPALIVE Session* mf_create(const char* canvasSelector) {
  auto* s = new Session;
  s->exportEvents.s = s;
  s->canvas = canvasSelector;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  emscripten_pthread_attr_settransferredcanvases(&attr, s->canvas.c_str());
  if (pthread_create(&s->thread, &attr, playerThread, s) != 0) {
    delete s;
    return nullptr;
  }
  return s;
}

// A preparation step (the WebGPU device, an image) finished; a waiting open() runs once all have.
EMSCRIPTEN_KEEPALIVE void mf_web_prepared(Session* s) {
  if (--s->pending > 0) return;
  auto waiting = std::move(s->whenReady);
  s->whenReady.clear();
  for (auto& f : waiting) f();
}

// Called by library_mf.js on the player thread once an image is decoded.
EMSCRIPTEN_KEEPALIVE void mf_web_image_ready(Session* s, const char* name, int handle, int width, int height) {
  if (handle > 0) s->sources[name] = std::make_shared<mf::web::ImageSource>(handle, width, height);
  mf_web_prepared(s);
}

// The file's bytes (copied); a scene's "src": name refers to them.
EMSCRIPTEN_KEEPALIVE void mf_add_source(Session* s, const char* name, const uint8_t* bytes, int length) {
  auto source = mf::web::memorySource(std::vector<uint8_t>(bytes, bytes + length));
  post(s, [s, name = std::string(name), source] { s->sources[name] = source; });
}

// An image file's bytes (PNG, JPEG...): decoded on the player thread before the next open().
EMSCRIPTEN_KEEPALIVE void mf_add_image(Session* s, const char* name, const uint8_t* bytes, int length) {
  auto data = std::make_shared<std::vector<uint8_t>>(bytes, bytes + length);
  post(s, [s, name = std::string(name), data] {
    ++s->pending;
    mf_js_decode_image(s, name.c_str(), data->data(), int(data->size()));
  });
}

// driver: 0 Auto, 1 LeadingClip, 2 Vsync. Opens once the device and images are ready; a failure
// shows in mf_report()'s error.
EMSCRIPTEN_KEEPALIVE void mf_open(Session* s, const char* sceneJson, int driver) {
  post(s, [s, json = std::string(sceneJson), driver] {
    auto open = [s, json, driver] {
      auto resolve = [s](const std::string& src) {
        auto it = s->sources.find(src);
        return it == s->sources.end() ? mf::MediaSource{} : mf::web::mediaSource(it->second);
      };
      mf::Scene scene;
      s->error.clear();
      mf::Result r = mf::parseScene(json, resolve, &scene, &s->error);
      if (r == mf::Result::Ok) {
        r = s->player->open(scene, mf::RenderTarget{const_cast<char*>(s->canvas.c_str())}, mf::OutputDriver(driver), &s->error);
      }
      if (r != mf::Result::Ok) s->addEvent(std::string("openFailed:") + mf::toString(r));
      snapshot(s);
    };
    if (s->pending > 0) {
      s->whenReady.push_back(open);
    } else {
      open();
    }
  });
}

// Exports a scene to MP4 (H.264 + AAC) at width x height (0: the scene's size), alongside the
// player, once the device and images are ready. Its progress is in mf_report()'s "export"; on the
// "exported" event, mf_export_data() / mf_export_size() hold the file.
EMSCRIPTEN_KEEPALIVE void mf_export(Session* s, const char* sceneJson, int width, int height, int videoBitrate) {
  post(s, [s, json = std::string(sceneJson), width, height, videoBitrate] {
    auto start = [s, json, width, height, videoBitrate] {
      auto resolve = [s](const std::string& src) {
        auto it = s->sources.find(src);
        return it == s->sources.end() ? mf::MediaSource{} : mf::web::mediaSource(it->second);
      };
      mf::Scene scene;
      s->error.clear();
      mf::Result r = mf::parseScene(json, resolve, &scene, &s->error);
      if (r == mf::Result::Ok) {
        if (s->exporter) s->exporter->shutdown();
        s->exportFile = std::make_shared<mf::web::MemoryWriter>();
        s->exporter = mf::Exporter::create(*s->platform, &s->exportEvents);
        mf::ExportSettings settings;
        settings.frameWidth = width;
        settings.frameHeight = height;
        if (videoBitrate > 0) settings.videoBitrate = videoBitrate;
        s->exportStartedMs = emscripten_get_now();
        r = s->exporter->start(scene, mf::ExportTarget{s->exportFile}, settings, &s->error);
      }
      if (r != mf::Result::Ok) s->addEvent(std::string("exportFailed:") + mf::toString(r));
      snapshot(s);
    };
    if (s->pending > 0) {
      s->whenReady.push_back(start);
    } else {
      start();
    }
  });
}

// The exported file, once mf_report() has the "exported" event (the player's thread no longer
// changes it then). Read it before the next export.
EMSCRIPTEN_KEEPALIVE const uint8_t* mf_export_data(Session* s) { return s->exportFile ? s->exportFile->bytes.data() : nullptr; }
EMSCRIPTEN_KEEPALIVE int mf_export_size(Session* s) { return s->exportFile ? int(s->exportFile->bytes.size()) : 0; }

// The editor's scene after an edit. Only its look changed: the frame on screen is redrawn
// (Player::updateAppearance). Else the scene is opened again at `atUs`, playing again if it was
// (as the macOS editor reloads its player). An empty scene leaves nothing open.
EMSCRIPTEN_KEEPALIVE void mf_apply(Session* s, const char* sceneJson, double atUs) {
  post(s, [s, json = std::string(sceneJson), atUs] {
    auto apply = [s, json, atUs] {
      auto resolve = [s](const std::string& src) {
        auto it = s->sources.find(src);
        return it == s->sources.end() ? mf::MediaSource{} : mf::web::mediaSource(it->second);
      };
      mf::Scene scene;
      s->error.clear();
      if (mf::parseScene(json, resolve, &scene, &s->error) != mf::Result::Ok) return snapshot(s);
      mf::State state = s->player->state();
      if ((state == mf::State::Ready || state == mf::State::Play) && s->player->updateAppearance(scene) == mf::Result::Ok) return snapshot(s);
      bool playing = state == mf::State::Play;
      s->player->shutdown();
      s->player = mf::Player::create(*s->platform, s);
      int64_t duration = scene.durationUs();
      if (duration > 0) {
        int64_t at = std::min<int64_t>(std::max<int64_t>(0, int64_t(atUs)), duration - 1);
        mf::Result r = s->player->open(scene, mf::RenderTarget{const_cast<char*>(s->canvas.c_str())}, mf::OutputDriver::Vsync, &s->error, at);
        if (r != mf::Result::Ok) s->addEvent(std::string("openFailed:") + mf::toString(r));
        s->playWhenReady = playing && r == mf::Result::Ok;
      }
      snapshot(s);
    };
    if (s->pending > 0) {
      s->whenReady.push_back(apply);
    } else {
      apply();
    }
  });
}

EMSCRIPTEN_KEEPALIVE void mf_play(Session* s) { post(s, [s] { s->player->play(); }); }
EMSCRIPTEN_KEEPALIVE void mf_pause(Session* s) { post(s, [s] { s->player->pause(); }); }
EMSCRIPTEN_KEEPALIVE void mf_seek(Session* s, double us) { post(s, [s, us] { s->player->seek(int64_t(us)); }); }

// The latest snapshot (at most 50 ms old), as JSON. Valid until the next call.
EMSCRIPTEN_KEEPALIVE const char* mf_report(Session* s) {
  std::lock_guard<std::mutex> lock(s->mu);
  s->reportCopy = s->report.empty() ? "{\"state\":\"Start\",\"events\":[]}" : s->report;
  return s->reportCopy.c_str();
}

}  // extern "C"
