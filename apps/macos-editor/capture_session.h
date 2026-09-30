#pragma once

// A camera recording (the camera window): segments recorded one after another, and what goes over
// them. Platform-free: the window records the files and plays the music; this keeps the model.
//
// `doc` is the preview's scene, at the project's size and rate. Track 0 holds the camera: one
// video item standing for the live picture, with the current effects and mirror (flipX), filling
// the frame. The tracks above it hold the overlays (stickers, emojis, text), one per track, which
// cover the whole recording. Pressing Record copies the current effects and mirror into the new
// segment; changing them later affects the preview and the segments after that only.
//
// Done inserts the recording into the project (insertInto): the segments back to back on a new
// video track, each with its own effects; each overlay on a new track above, over the recording's
// length; the music on an audio track, from where recording started it.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "document.h"

namespace editor {

class CaptureSession {
 public:
  static constexpr int64_t kMinSegmentUs = 300000;       // shorter segments are dropped
  static constexpr int64_t kUnlimited = 0;               // no maximum length
  static constexpr int64_t kPreviewUs = 3600LL * 1000000;  // the preview scene's items: longer than any recording
  static constexpr int64_t kMaxChoicesUs[] = {15000000, 30000000, 60000000, 180000000, kUnlimited};

  struct Segment {
    std::string file;
    int64_t durationUs = 0;
    mf::SceneEffects effects;  // as they were when it started
    bool mirror = false;
  };

  // `project`: the project's output (size, rate, background). `mirror`: the camera's default
  // (on for a front camera).
  CaptureSession(const mf::SceneOutput& project, int64_t maxDurationUs, bool mirror);

  Document doc;
  static constexpr int kCameraTrack = 0;
  mf::SceneItem& camera() { return doc.item(kCameraTrack, 0); }
  const mf::SceneItem& camera() const { return doc.scene.tracks[kCameraTrack].items[0]; }
  mf::SceneEffects& effects() { return camera().effects; }  // the current effects: the preview's, and the next segment's
  bool mirror() const { return camera().transform.flipX; }
  void setMirror(bool on) { camera().transform.flipX = on; }

  // Length.
  int64_t maxDurationUs() const { return maxUs_; }
  void setMaxDurationUs(int64_t us) { maxUs_ = us; }
  int64_t totalUs() const;      // recorded, in all segments
  int64_t remainingUs() const;  // until the maximum; INT64_MAX when unlimited
  bool full() const { return remainingUs() <= 0; }

  // Recording. Effects can't change while recording: the window locks them.
  bool recording() const { return recording_; }
  bool canRecord() const { return !recording_ && !full(); }
  // Starts a segment with the current effects and mirror. False when it can't record.
  bool startSegment();
  // Ends the segment being recorded, with its file and length, cut to the room left. Returns false
  // when it's dropped (shorter than kMinSegmentUs): the caller deletes its file.
  bool stopSegment(const std::string& file, int64_t durationUs);
  void cancelSegment();  // a segment that failed to record: as if never started
  // Removes the last segment and returns it (the caller deletes its file); none while recording
  // or when there's none.
  std::optional<Segment> removeLast();
  const std::vector<Segment>& segments() const { return segments_; }

  // Music: plays while recording, from where the last segment left it.
  bool hasMusic() const { return !musicSrc_.empty(); }
  const std::string& musicSrc() const { return musicSrc_; }
  void setMusic(const std::string& src, int64_t lengthUs, int64_t inUs = 0);
  void clearMusic();
  int64_t musicPositionUs() const;  // in the file: where the next segment's music starts

  // The microphone: on by default, and off by default with music (the speakers would be heard).
  // Once set explicitly, music no longer changes it.
  bool recordMicrophone() const { return microphone_; }
  void setRecordMicrophone(bool on) {
    microphone_ = on;
    microphoneChosen_ = true;
  }

  // Adds a sticker, emoji or text over the whole recording, on its own track on top. Returns its
  // track, or -1 when the scene already has 16 tracks.
  int addOverlay(mf::SceneItem item);

  // Inserts the recording into `project` at atUs (see the top). `resolve` gives each file its
  // MediaSource. Returns the segments' track, or -1 with `error` (nothing recorded, or too many
  // tracks), leaving the project as it was.
  int insertInto(Document& project, int64_t atUs, const mf::SourceResolver& resolve, std::string* error) const;

  // Recovery: the session as a scene (the camera on a disabled track, the segments, overlays and
  // music on tracks of their own), to write with serializeScene after each segment; and back.
  // It's a valid scene once something is recorded (or overlaid). restore() fails when it isn't a
  // session's scene.
  mf::Scene journal() const;
  bool restore(const mf::Scene& journal);

 private:
  int64_t maxUs_;
  std::vector<Segment> segments_;
  bool recording_ = false;
  std::string musicSrc_;
  int64_t musicLengthUs_ = 0, musicInUs_ = 0;
  bool microphone_ = true, microphoneChosen_ = false;
};

}  // namespace editor
