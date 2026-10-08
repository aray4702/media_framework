// The C API the page calls (see app/player.js): a Player on the web platform, its media given as
// bytes under the names a scene's "src" uses.

#include <emscripten.h>

#include <cstdio>
#include <map>
#include <string>

#include "mf/player.h"
#include "mf/scene.h"
#include "mf/web.h"
#include "mp4_demuxer.h"

namespace {

struct Session : mf::PlayerListener {
  std::unique_ptr<mf::PlatformFactory> platform;
  std::unique_ptr<mf::Player> player;
  std::map<std::string, std::shared_ptr<mf::web::ByteSource>> sources;
  std::string canvas;  // the RenderTarget points at it
  std::string error, events, report;

  void onStateChanged(mf::State s) override { events += std::string(events.empty() ? "" : ",") + "\"state:" + mf::toString(s) + "\""; }
  void onError(mf::Result r, const std::string& reason) override {
    error = std::string(mf::toString(r)) + ": " + reason;
    events += std::string(events.empty() ? "" : ",") + "\"error\"";
  }
  void onEnded() override { events += std::string(events.empty() ? "" : ",") + "\"ended\""; }
  void onSeekCompleted(int64_t) override { events += std::string(events.empty() ? "" : ",") + "\"seeked\""; }
};

// JSON string contents: the reasons we write have no control characters, only quotes to escape.
std::string quoted(const std::string& s) {
  std::string out = "\"";
  for (char c : s) out += c == '"' ? std::string("\\\"") : std::string(1, c);
  return out + "\"";
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE Session* mf_create() {
  auto* s = new Session;
  s->platform = mf::web::createPlatform();
  s->player = mf::Player::create(*s->platform, s);
  return s;
}

// Copies the file's bytes; a scene's "src": name refers to them.
EMSCRIPTEN_KEEPALIVE void mf_add_source(Session* s, const char* name, const uint8_t* bytes, int length) {
  s->sources[name] = mf::web::memorySource(std::vector<uint8_t>(bytes, bytes + length));
}

// driver: 0 Auto, 1 LeadingClip, 2 Vsync. Returns a mf::Result; mf_error() says why.
EMSCRIPTEN_KEEPALIVE int mf_open(Session* s, const char* sceneJson, const char* canvasSelector, int driver) {
  auto resolve = [s](const std::string& src) {
    auto it = s->sources.find(src);
    return it == s->sources.end() ? mf::MediaSource{} : mf::web::mediaSource(it->second);
  };
  mf::Scene scene;
  s->error.clear();
  mf::Result r = mf::parseScene(sceneJson, resolve, &scene, &s->error);
  if (r != mf::Result::Ok) return int(r);
  s->canvas = canvasSelector;
  return int(s->player->open(scene, mf::RenderTarget{const_cast<char*>(s->canvas.c_str())}, mf::OutputDriver(driver), &s->error));
}

EMSCRIPTEN_KEEPALIVE int mf_play(Session* s) { return int(s->player->play()); }
EMSCRIPTEN_KEEPALIVE int mf_pause(Session* s) { return int(s->player->pause()); }
EMSCRIPTEN_KEEPALIVE int mf_seek(Session* s, double us) { return int(s->player->seek(int64_t(us))); }
EMSCRIPTEN_KEEPALIVE const char* mf_error(Session* s) { return s->error.c_str(); }

// The player's state, position, events so far and metrics, as JSON.
EMSCRIPTEN_KEEPALIVE const char* mf_report(Session* s) {
  mf::MetricsReport m = s->player->metrics();
  char buf[1024];
  std::snprintf(buf, sizeof(buf),
                "\"presented\":%lld,\"lateDrops\":%lld,\"rateCapDrops\":%lld,\"lateLayers\":%lld,\"droppedRate\":%.4f,"
                "\"janks\":%lld,\"intervals\":%lld,\"jankRate\":%.4f,\"avSamples\":%lld,\"avMeanMs\":%.2f,\"avMeanAbsMs\":%.2f,"
                "\"avP95AbsMs\":%.1f,\"ttffMs\":%.1f,\"decodeSkips\":%lld,\"decodeStepDowns\":%lld,\"decodeStepUps\":%lld,"
                "\"holdExpiries\":%lld,\"corruptSkips\":%lld",
                (long long)m.presented, (long long)m.lateDrops, (long long)m.rateCapDrops, (long long)m.lateLayers, m.droppedRate,
                (long long)m.janks, (long long)m.intervals, m.jankRate, (long long)m.avSamples, m.avMeanMs, m.avMeanAbsMs,
                m.avP95AbsMs, m.ttffMs, (long long)m.decodeSkips, (long long)m.decodeStepDowns, (long long)m.decodeStepUps,
                (long long)m.holdExpiries, (long long)m.corruptSkips);
  s->report = "{\"state\":" + quoted(mf::toString(s->player->state())) + ",\"positionUs\":" +
              std::to_string(s->player->positionUs()) + ",\"error\":" + quoted(s->error) + ",\"events\":[" + s->events +
              "],\"metrics\":{" + buf + "}}";
  return s->report.c_str();
}

EMSCRIPTEN_KEEPALIVE void mf_destroy(Session* s) {
  s->player->shutdown();
  delete s;
}

}  // extern "C"
