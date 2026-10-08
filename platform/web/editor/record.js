// Camera and microphone recordings for the editor (the macOS editor's camera window and
// voice-over): a MediaStream (getUserMedia) read frame by frame with MediaStreamTrackProcessor,
// encoded with WebCodecs (H.264, AAC) and muxed by the portable MP4 muxer (recorder_api.cpp).
// Times are capture times, counted from the first video frame (from the first audio when there is
// no video); audio captured before that is left out. Chrome stamps camera frames and microphone
// audio on different clocks, so each track's times are moved onto the page's (performance.now)
// by the offset its first sample showed on arrival. While the video encoder is behind, frames are
// dropped and counted rather than queued, so a slow encoder costs frames, not memory or latency.

// An H.264 encoder for a camera track, configured ahead of the recording: on macOS, configuring
// the hardware encoder holds up the camera's frames for over a second, which would cut the start
// of the recording. `ready` resolves once it is configured.
export function warmVideoEncoder(track) {
  const s = track.getSettings();
  const w = { width: (s.width || 1280) & ~1, height: (s.height || 720) & ~1, fps: Math.round(s.frameRate || 30), output: null, failed: null };
  w.encoder = new VideoEncoder({ output: (c, m) => w.output?.(c, m), error: (e) => { w.failed = e.message; } });
  w.encoder.configure({ codec: w.width * w.height <= 1920 * 1088 ? 'avc1.640028' : 'avc1.640033', width: w.width, height: w.height,
                        bitrate: 6000000, framerate: w.fps, latencyMode: 'realtime', avc: { format: 'avc' } });
  w.ready = w.encoder.flush().then(() => w, () => w);
  return w;
}

export class Recording {
  // `warm`: a warmVideoEncoder for the stream's camera track (else one is made now).
  constructor(M, stream, warm = null) {
    this.warm = warm;
    this.M = M;
    this.stream = stream;
    this.rec = M._rec_new();
    this.ids = [-1, -1];  // the muxer's video and audio tracks, made on each one's first chunk
    this.origin = null;   // the first frame's capture time (µs)
    this.frames = 0;
    this.dropped = 0;
    this.readers = [];
    this.failed = null;
    this.lastUs = 0;
    this.offsets = new Map();  // track kind -> page time minus capture time (µs)
  }

  // A capture time on the page's clock.
  pageUs(kind, timestamp) {
    if (!this.offsets.has(kind)) this.offsets.set(kind, performance.now() * 1000 - timestamp);
    return timestamp + this.offsets.get(kind);
  }

  start() {
    const video = this.stream.getVideoTracks()[0], audio = this.stream.getAudioTracks()[0];
    this.hasVideo = !!video;
    const pumps = [];
    if (video) {
      const w = this.warm || warmVideoEncoder(video);
      Object.assign(this, { width: w.width, height: w.height, fps: w.fps, videoEncoder: w.encoder });
      w.output = (c, m) => this.write(0, c, m);
      pumps.push(this.pump(video, (frame) => this.video(frame)));
    }
    if (audio) pumps.push(this.pump(audio, (data) => this.audio(data)));
    this.pumping = Promise.all(pumps);
  }

  async pump(track, take) {
    const reader = new MediaStreamTrackProcessor({ track }).readable.getReader();
    this.readers.push(reader);
    for (;;) {
      const { value, done } = await reader.read().catch(() => ({ done: true }));
      if (done) return;
      if (this.stopped) { value.close(); return; }
      take(value);
    }
  }

  video(frame) {
    const t = this.pageUs('video', frame.timestamp);
    if (this.origin === null) this.origin = t;
    if (this.videoEncoder.encodeQueueSize > 2) {  // behind: this frame goes
      ++this.dropped;
      frame.close();
      return;
    }
    const shifted = new VideoFrame(frame, { timestamp: t - this.origin });
    this.videoEncoder.encode(shifted, { keyFrame: this.frames % (2 * this.fps) === 0 });
    shifted.close();
    this.lastUs = Math.max(this.lastUs, t - this.origin);
    ++this.frames;
    frame.close();
  }

  audio(data) {
    const t = this.pageUs('audio', data.timestamp);
    if (this.origin === null && !this.hasVideo) this.origin = t;
    if (this.origin === null || t < this.origin) { data.close(); return; }  // before the start
    if (!this.audioEncoder) {
      this.sampleRate = data.sampleRate;
      this.channels = data.numberOfChannels;
      this.audioEncoder = new AudioEncoder({ output: (c, m) => this.write(1, c, m), error: (e) => { this.failed = e.message; } });
      this.audioEncoder.configure({ codec: 'mp4a.40.2', sampleRate: this.sampleRate, numberOfChannels: this.channels, bitrate: 128000 });
    }
    if (!this.hasVideo) this.lastUs = Math.max(this.lastUs, t + data.duration - this.origin);
    this.audioEncoder.encode(this.retimed(data, t - this.origin));
    data.close();
  }

  // The audio with a new timestamp (AudioData has no copy-with-timestamp: its samples are copied).
  retimed(data, timestamp) {
    const format = data.format;
    const planes = format.endsWith('-planar') ? data.numberOfChannels : 1;
    let size = 0;
    for (let p = 0; p < planes; ++p) size += data.allocationSize({ planeIndex: p, format });
    const buffer = new ArrayBuffer(size);
    for (let p = 0, at = 0; p < planes; ++p) {
      const n = data.allocationSize({ planeIndex: p, format });
      data.copyTo(new Uint8Array(buffer, at, n), { planeIndex: p, format });
      at += n;
    }
    return new AudioData({ format, sampleRate: data.sampleRate, numberOfFrames: data.numberOfFrames, numberOfChannels: data.numberOfChannels, timestamp, data: buffer });
  }

  // An encoded chunk to the muxer; a track is added with its first chunk's codec description.
  write(track, chunk, meta) {
    const M = this.M;
    if (this.ids[track] < 0) {
      const desc = meta?.decoderConfig?.description;
      if (!desc) { this.failed = 'the encoder gave no codec description'; return; }
      const d = ArrayBuffer.isView(desc) ? new Uint8Array(desc.buffer, desc.byteOffset, desc.byteLength) : new Uint8Array(desc);
      const p = M._malloc(d.length);
      M.HEAPU8.set(d, p);
      this.ids[track] = M._rec_add_track(this.rec, track === 0 ? 1 : 0, this.width || 0, this.height || 0, this.sampleRate || 0, this.channels || 0, p, d.length);
      M._free(p);
    }
    const bytes = new Uint8Array(chunk.byteLength);
    chunk.copyTo(bytes);
    const p = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, p);
    M._rec_write(this.rec, this.ids[track], p, bytes.length, Math.max(0, chunk.timestamp), chunk.duration ?? 0, chunk.type === 'key' ? 1 : 0);
    M._free(p);
  }

  get durationUs() { return this.lastUs; }

  // Stops the capture and returns the file: { bytes, durationUs, frames, dropped }, or throws.
  async stop() {
    this.stopped = true;
    for (const r of this.readers) r.cancel().catch(() => {});
    this.stream.getTracks().forEach((t) => t.stop());
    await this.pumping;
    await Promise.all([this.videoEncoder?.flush(), this.audioEncoder?.flush()].filter(Boolean));
    const M = this.M;
    try {
      this.failed ||= this.warm?.failed;
      if (this.failed) throw new Error(this.failed);
      if (this.ids[0] < 0 && this.ids[1] < 0) throw new Error('nothing was recorded');
      if (!M._rec_finish(this.rec)) throw new Error('the recording could not be written');
      const data = M._rec_data(this.rec);
      return { bytes: M.HEAPU8.slice(data, data + M._rec_size(this.rec)), durationUs: this.lastUs, frames: this.frames, dropped: this.dropped };
    } finally {
      this.videoEncoder?.close();
      this.audioEncoder?.close();
      M._rec_delete(this.rec);
    }
  }
}
