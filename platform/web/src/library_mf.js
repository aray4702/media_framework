// The browser side of the web platform (web_platform.cpp, web_api.cpp): the scheduler's timers,
// WebCodecs decoders, the WebGPU display, image decoding and the audio clock mapping. All of it runs
// on the player's thread (a worker), except the audio clock mapping, on the page's thread with the
// AudioContext; the AudioWorklet runs only C++. Times passed to C++ are absolute
// (performance.timeOrigin + performance.now()): each thread has its own time origin.

addToLibrary({
  $MF__postset: "",

  $MF: {
    live: new Set(),       // schedulers that may still be run
    queue: [],             // runs waiting for the next message
    channel: null,
    frames: new Map(),     // handle -> VideoFrame held by C++
    nextHandle: 1,
    images: new Map(),     // handle -> { bitmap: ImageBitmap, texture: GPUTexture once drawn }
    nextImage: 1,
    compositor: null,
    maxHeld: 0,            // most frames held by C++ at once
    maxDecoding: 0,        // most frames inside the decoders (queued, not yet output) at once
    vdecs: [],
    adecs: [],
    displays: new Set(),
    speakers: new Map(),   // speaker -> interval id
    gpu: null,
    drawn: 0,

    statsPtr: 0,
    rafIntervals: [],      // the display's animation-frame intervals (ms), the last 600
    lastRaf: 0,

    stats() {
      let decoding = 0;
      for (const st of MF.vdecs) if (st) decoding += st.decoder.decodeQueueSize + st.out.length;
      const r = [...MF.rafIntervals].sort((a, b) => a - b), pick = (q) => (r.length ? Math.round(r[Math.floor(q * (r.length - 1))] * 100) / 100 : 0);
      return { heldFrames: MF.frames.size, maxHeldFrames: MF.maxHeld, maxDecodingFrames: MF.maxDecoding, decodingNow: decoding,
               drawn: MF.drawn, rafP50Ms: pick(0.5), rafP99Ms: pick(0.99), rafMaxMs: pick(1) };
    },

    absolute: (t) => performance.timeOrigin + t,

    notify(onOutput) { _mf_web_output(onOutput); },

    addImage(bitmap) {
      const h = MF.nextImage++;
      MF.images.set(h, { bitmap });
      return h;
    },

    // The video decoders' frames in flight: queued for decoding plus output not yet taken.
    noteDecoding() {
      let n = 0;
      for (const st of MF.vdecs) if (st) n += st.decoder.decodeQueueSize + st.out.length;
      MF.maxDecoding = Math.max(MF.maxDecoding, n);
    },

    // A decode error closes a WebCodecs decoder. It is reported once as a corrupt frame (the core
    // then skips to the next keyframe, holding the last good frame, A14), and the decoder is made
    // again at that keyframe. Only a failed configure() is fatal.
    makeVideoDecoder(st) {
      st.decoder = new VideoDecoder({
        output: (frame) => { st.out.push(frame); MF.notify(st.onOutput); },
        error: (e) => MF.broke(st, 'VideoDecoder', e),
      });
      st.decoder.ondequeue = () => MF.notify(st.onOutput);  // room to queue again
    },

    broke(st, what, e) {
      console.warn(`${what}: ${e.message}; ${what === 'VideoDecoder' ? 'skipping to the next keyframe' : 'resuming at the next packet'}`);
      st.broken = st.corrupt = true;
      MF.notify(st.onOutput);
    },

    // Before decoding: a broken decoder is made again at a keyframe (every audio packet is one);
    // other packets are dropped until then. True when the packet should be decoded.
    revive(st, make, key) {
      if (!st.broken) return true;
      if (!key) return false;
      make(st);
      st.decoder.configure(st.config);
      st.broken = false;
      return true;
    },

    makeAudioDecoder(st) {
      st.decoder = new AudioDecoder({
        output: (data) => {
          const ch = data.numberOfChannels, n = data.numberOfFrames;
          const pcm = new Int16Array(n * ch), plane = new Float32Array(n);
          for (let c = 0; c < ch; ++c) {
            data.copyTo(plane, { planeIndex: c, format: 'f32-planar' });
            for (let i = 0; i < n; ++i) pcm[i * ch + c] = Math.max(-32768, Math.min(32767, Math.round(plane[i] * 32767)));
          }
          st.out.push({ pts: data.timestamp, frames: n, channels: ch, pcm });
          data.close();
          MF.notify(st.onOutput);
        },
        error: (e) => MF.broke(st, 'AudioDecoder', e),
      });
      st.decoder.ondequeue = () => MF.notify(st.onOutput);
    },
  },

  // --- Scheduler ---

  mf_js_request_run__deps: ['$MF', 'mf_web_run'],
  mf_js_request_run: (scheduler, delayMs) => {
    MF.live.add(scheduler);
    const run = () => { if (MF.live.has(scheduler)) _mf_web_run(scheduler); };
    if (delayMs > 0) {
      setTimeout(run, delayMs);
      return;
    }
    // As soon as possible after other events: a message, which unlike setTimeout(0) isn't clamped.
    if (!MF.channel) {
      MF.channel = new MessageChannel();
      MF.channel.port1.onmessage = () => { const q = MF.queue; MF.queue = []; q.forEach((f) => f()); };
    }
    MF.queue.push(run);
    if (MF.queue.length === 1) MF.channel.port2.postMessage(0);
  },
  mf_js_scheduler_gone__deps: ['$MF'],
  mf_js_scheduler_gone: (scheduler) => { MF.live.delete(scheduler); },

  // --- Video decoder (WebCodecs) ---

  mf_js_vdec_create__deps: ['$MF', 'mf_web_output'],
  mf_js_vdec_create: (onOutput) => {
    const st = { onOutput, out: [], eos: false, failed: false, config: null, gen: 0 };
    MF.makeVideoDecoder(st);
    MF.vdecs.push(st);
    return MF.vdecs.length;
  },
  mf_js_vdec_configure__deps: ['$MF'],
  mf_js_vdec_configure: (id, codec, description, length) => {
    const st = MF.vdecs[id - 1];
    st.config = { codec: UTF8ToString(codec), description: HEAPU8.slice(description, description + length), optimizeForLatency: true };
    st.out.forEach((f) => f.close());
    st.out = [];
    st.eos = st.failed = st.broken = st.corrupt = false;
    try {
      if (st.decoder.state === 'closed') MF.makeVideoDecoder(st);
      st.decoder.configure(st.config);
      return 1;
    } catch (e) {
      console.error('VideoDecoder.configure:', e.message);
      return 0;
    }
  },
  // 0 queued, 1 full (again after an output or a dequeue event), 2 failed.
  mf_js_vdec_decode__deps: ['$MF'],
  mf_js_vdec_decode: (id, data, length, ptsUs, key) => {
    const st = MF.vdecs[id - 1];
    if (st.failed) return 2;
    if (!MF.revive(st, MF.makeVideoDecoder, key)) return 0;
    if (st.decoder.decodeQueueSize + st.out.length >= 6) return 1;
    st.decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: ptsUs, data: HEAPU8.slice(data, data + length) }));
    MF.noteDecoding();
    return 0;
  },
  mf_js_vdec_flush_eos__deps: ['$MF'],
  // A broken decoder has nothing left to output: its end comes at once.
  mf_js_vdec_flush_eos: (id) => {
    const st = MF.vdecs[id - 1], gen = st.gen;
    const done = () => { if (st.gen === gen) { st.eos = true; MF.notify(st.onOutput); } };
    if (st.broken) done();
    else st.decoder.flush().then(done, done);
  },
  mf_js_vdec_reset__deps: ['$MF'],
  mf_js_vdec_reset: (id) => {
    const st = MF.vdecs[id - 1];
    ++st.gen;
    st.out.forEach((f) => f.close());
    st.out = [];
    st.eos = st.failed = st.broken = st.corrupt = false;
    if (st.decoder.state === 'closed') MF.makeVideoDecoder(st);
    else st.decoder.reset();
    if (st.config) st.decoder.configure(st.config);
  },
  // > 0 a frame handle (its time in *ptsUs), 0 again, -1 Eos, -2 failed, -3 corrupt (skip to a keyframe).
  mf_js_vdec_dequeue__deps: ['$MF'],
  mf_js_vdec_dequeue: (id, ptsUs) => {
    const st = MF.vdecs[id - 1];
    if (st.out.length) {
      const frame = st.out.shift(), h = MF.nextHandle++;
      MF.frames.set(h, frame);
      MF.maxHeld = Math.max(MF.maxHeld, MF.frames.size);
      HEAPF64[ptsUs >> 3] = frame.timestamp;
      return h;
    }
    if (st.failed) return -2;
    if (st.corrupt) { st.corrupt = false; return -3; }
    return st.eos ? -1 : 0;
  },
  mf_js_vdec_destroy__deps: ['$MF'],
  mf_js_vdec_destroy: (id) => {
    const st = MF.vdecs[id - 1];
    st.out.forEach((f) => f.close());
    if (st.decoder.state !== 'closed') st.decoder.close();
    MF.vdecs[id - 1] = null;
  },
  mf_js_frame_close__deps: ['$MF'],
  mf_js_frame_close: (h) => {
    const f = MF.frames.get(h);
    if (f) { f.close(); MF.frames.delete(h); }
  },

  // --- Audio decoder (WebCodecs) ---

  mf_js_adec_create__deps: ['$MF', 'mf_web_output'],
  mf_js_adec_create: (onOutput) => {
    const st = { onOutput, out: [], eos: false, failed: false, config: null, gen: 0 };
    MF.makeAudioDecoder(st);
    MF.adecs.push(st);
    return MF.adecs.length;
  },
  mf_js_adec_configure__deps: ['$MF'],
  mf_js_adec_configure: (id, codec, description, length, sampleRate, channels) => {
    const st = MF.adecs[id - 1];
    st.config = { codec: UTF8ToString(codec), sampleRate, numberOfChannels: channels };
    if (length > 0) st.config.description = HEAPU8.slice(description, description + length);
    st.out = [];
    st.eos = st.failed = st.broken = st.corrupt = false;
    try {
      if (st.decoder.state === 'closed') MF.makeAudioDecoder(st);
      st.decoder.configure(st.config);
      return 1;
    } catch (e) {
      console.error('AudioDecoder.configure:', e.message);
      return 0;
    }
  },
  mf_js_adec_decode__deps: ['$MF'],
  mf_js_adec_decode: (id, data, length, ptsUs) => {
    const st = MF.adecs[id - 1];
    if (st.failed) return 2;
    MF.revive(st, MF.makeAudioDecoder, true);
    if (st.decoder.decodeQueueSize + st.out.length >= 8) return 1;
    st.decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: ptsUs, data: HEAPU8.slice(data, data + length) }));
    return 0;
  },
  mf_js_adec_flush_eos__deps: ['$MF'],
  mf_js_adec_flush_eos: (id) => {
    const st = MF.adecs[id - 1], gen = st.gen;
    const done = () => { if (st.gen === gen) { st.eos = true; MF.notify(st.onOutput); } };
    if (st.broken) done();
    else st.decoder.flush().then(done, done);
  },
  mf_js_adec_reset__deps: ['$MF'],
  mf_js_adec_reset: (id) => {
    const st = MF.adecs[id - 1];
    ++st.gen;
    st.out = [];
    st.eos = st.failed = st.broken = st.corrupt = false;
    if (st.decoder.state === 'closed') MF.makeAudioDecoder(st);
    else st.decoder.reset();
    if (st.config) st.decoder.configure(st.config);
  },
  // > 0 frames per channel of the next output (its time and channels written), 0 again, -1 Eos,
  // -2 failed, -3 corrupt: audio was lost (the mixer fills the gap with silence).
  mf_js_adec_dequeue__deps: ['$MF'],
  mf_js_adec_dequeue: (id, ptsUs, channels) => {
    const st = MF.adecs[id - 1];
    if (st.out.length) {
      HEAPF64[ptsUs >> 3] = st.out[0].pts;
      HEAP32[channels >> 2] = st.out[0].channels;
      return st.out[0].frames;
    }
    if (st.failed) return -2;
    if (st.corrupt) { st.corrupt = false; return -3; }
    return st.eos ? -1 : 0;
  },
  mf_js_adec_take__deps: ['$MF'],
  mf_js_adec_take: (id, out) => { HEAP16.set(MF.adecs[id - 1].out.shift().pcm, out >> 1); },
  mf_js_adec_destroy__deps: ['$MF'],
  mf_js_adec_destroy: (id) => {
    const st = MF.adecs[id - 1];
    if (st.decoder.state !== 'closed') st.decoder.close();
    MF.adecs[id - 1] = null;
  },

  // --- Display (WebGPU) ---

  // Module.mfGpuDevice must hold a GPUDevice (the page requests it: that is asynchronous).
  // On the player's thread the canvas is the OffscreenCanvas transferred with it (by its id).
  mf_js_display_attach__deps: ['$MF', '$MFC', '$GL', 'mf_web_display_tick'],
  mf_js_display_attach: (selector, display) => {
    const name = UTF8ToString(selector), device = Module['mfGpuDevice'];
    const canvas = globalThis.document ? document.querySelector(name) : GL.offscreenCanvases[name.replace(/^#/, '')]?.offscreenCanvas;
    if (!canvas || !device) return 0;
    const context = canvas.getContext('webgpu'), format = navigator.gpu.getPreferredCanvasFormat();
    context.configure({ device, format, alphaMode: 'opaque' });
    MF.gpu = { device, context, canvas };
    MF.compositor = MFC.create(device, format);
    MF.displays.add(display);
    const loop = (t) => {
      if (!MF.displays.has(display)) return;
      if (MF.lastRaf) { MF.rafIntervals.push(t - MF.lastRaf); if (MF.rafIntervals.length > 600) MF.rafIntervals.shift(); }
      MF.lastRaf = t;
      _mf_web_display_tick(display, MF.absolute(t));
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
    return 1;
  },
  mf_js_display_detach__deps: ['$MF'],
  mf_js_display_detach: (display) => { MF.displays.delete(display); },
  mf_js_display_visible: () => (globalThis.document?.visibilityState === 'hidden' ? 0 : 1),  // a worker can't tell: visible
  // frameJson: a composed frame (web_platform.cpp's frameJson); the canvas takes its size.
  mf_js_display_draw__deps: ['$MF', '$MFC'],
  mf_js_display_draw: (frameJson) => {
    const g = MF.gpu;
    if (!g) return;
    const frame = JSON.parse(UTF8ToString(frameJson));
    if (g.canvas.width !== frame.width || g.canvas.height !== frame.height) { g.canvas.width = frame.width; g.canvas.height = frame.height; }
    MFC.encode(MF.compositor, frame, g.context.getCurrentTexture().createView(), g.canvas.width, g.canvas.height);
    ++MF.drawn;
  },

  // --- Player thread setup (web_api.cpp) ---

  // The WebGPU device, requested on the player's thread, where the canvas is.
  mf_js_gpu_init__deps: ['$MF', 'mf_web_prepared'],
  mf_js_gpu_init: (session) => {
    (async () => {
      const adapter = await navigator.gpu?.requestAdapter();
      if (adapter) Module['mfGpuDevice'] = await adapter.requestDevice();
      else console.error('no WebGPU adapter: nothing will be drawn');
    })().catch((e) => console.error('WebGPU:', e.message)).finally(() => _mf_web_prepared(session));
  },
  // Decodes an image file's bytes into an ImageBitmap the compositor draws (handle 0: failed).
  mf_js_decode_image__deps: ['$MF', 'mf_web_image_ready', '$stringToNewUTF8', 'free'],
  mf_js_decode_image: (session, name, bytes, length) => {
    const key = UTF8ToString(name), blob = new Blob([HEAPU8.slice(bytes, bytes + length)]);
    const done = (h, w, ht) => {
      const ptr = stringToNewUTF8(key);
      _mf_web_image_ready(session, ptr, h, w, ht);
      _free(ptr);
    };
    createImageBitmap(blob).then((b) => done(MF.addImage(b), b.width, b.height), (e) => {
      console.error(`image ${key}:`, e.message);
      done(0, 0, 0);
    });
  },
  // The frame statistics as JSON, for the report. Valid until the next call.
  mf_js_stats_json__deps: ['$MF', '$stringToNewUTF8', 'free'],
  mf_js_stats_json: () => {
    if (MF.statsPtr) _free(MF.statsPtr);
    MF.statsPtr = stringToNewUTF8(JSON.stringify(MF.stats()));
    return MF.statsPtr;
  },

  // --- Export (WebExportSink): the compositor into an export-sized canvas, WebCodecs encoders ---

  mf_js_export_open__deps: ['$MF', '$MFC', 'mf_web_export_chunk', 'malloc', 'free'],
  mf_js_export_open: (sink, width, height, fps, videoBitrate, sampleRate, channels, audioBitrate) => {
    const device = Module['mfGpuDevice'];
    if (!device) return 0;
    const canvas = new OffscreenCanvas(width, height), format = navigator.gpu.getPreferredCanvasFormat();
    const context = canvas.getContext('webgpu');
    context.configure({ device, format, alphaMode: 'opaque' });
    const ex = MF.export = { canvas, context, compositor: MFC.create(device, format), failed: false, configSent: [false, false],
                             sampleRate, channels };
    // An encoded chunk to the muxer, with the codec's description (avcC, AudioSpecificConfig) once.
    const deliver = (track, chunk, meta) => {
      const bytes = new Uint8Array(chunk.byteLength);
      chunk.copyTo(bytes);
      const ptr = _malloc(bytes.length);
      HEAPU8.set(bytes, ptr);
      let config = 0, configLength = 0;
      const desc = meta?.decoderConfig?.description;
      if (desc && !ex.configSent[track]) {
        const d = ArrayBuffer.isView(desc) ? new Uint8Array(desc.buffer, desc.byteOffset, desc.byteLength) : new Uint8Array(desc);
        config = _malloc(d.length);
        HEAPU8.set(d, config);
        configLength = d.length;
        ex.configSent[track] = true;
      }
      _mf_web_export_chunk(sink, track, ptr, bytes.length, chunk.timestamp, chunk.duration ?? 0, chunk.type === 'key' ? 1 : 0, config, configLength);
      _free(ptr);
      if (config) _free(config);
    };
    const failed = (what) => (e) => { console.error(`${what}: ${e.message}`); ex.failed = true; };
    try {
      ex.video = new VideoEncoder({ output: (c, m) => deliver(0, c, m), error: failed('VideoEncoder') });
      ex.video.configure({ codec: width * height <= 1920 * 1088 ? 'avc1.640028' : 'avc1.640033', width, height, bitrate: videoBitrate,
                           framerate: fps, avc: { format: 'avc' }, latencyMode: 'quality' });
      if (channels > 0) {
        ex.audio = new AudioEncoder({ output: (c, m) => deliver(1, c, m), error: failed('AudioEncoder') });
        ex.audio.configure({ codec: 'mp4a.40.2', sampleRate, numberOfChannels: channels, bitrate: audioBitrate });
      }
      return 1;
    } catch (e) {
      console.error('export:', e.message);
      return 0;
    }
  },
  // 0 encoding, 1 the encoder is busy (again later), 2 failed.
  mf_js_export_video__deps: ['$MF', '$MFC'],
  mf_js_export_video: (frameJson, ptsUs, durationUs, key) => {
    const ex = MF.export;
    if (!ex || ex.failed) return 2;
    if (ex.video.encodeQueueSize >= 4) return 1;
    MFC.encode(ex.compositor, JSON.parse(UTF8ToString(frameJson)), ex.context.getCurrentTexture().createView(), ex.canvas.width, ex.canvas.height);
    const bitmap = ex.canvas.transferToImageBitmap();
    const frame = new VideoFrame(bitmap, { timestamp: ptsUs, duration: durationUs });
    bitmap.close();
    ex.video.encode(frame, { keyFrame: !!key });
    frame.close();
    return 0;
  },
  mf_js_export_audio__deps: ['$MF'],
  mf_js_export_audio: (pcm, frames, ptsUs) => {
    const ex = MF.export;
    if (!ex || ex.failed) return 2;
    if (!ex.audio) return 0;
    if (ex.audio.encodeQueueSize >= 8) return 1;
    const data = HEAP16.slice(pcm >> 1, (pcm >> 1) + frames * ex.channels);
    const audio = new AudioData({ format: 's16', sampleRate: ex.sampleRate, numberOfFrames: frames, numberOfChannels: ex.channels,
                                  timestamp: ptsUs, data });
    ex.audio.encode(audio);
    audio.close();
    return 0;
  },
  mf_js_export_finish__deps: ['$MF', 'mf_web_export_finished'],
  mf_js_export_finish: (sink) => {
    const ex = MF.export;
    Promise.all([ex.video.flush(), ex.audio?.flush()])
      .then(() => _mf_web_export_finished(sink, ex.failed ? 0 : 1), (e) => { console.error('export:', e.message); _mf_web_export_finished(sink, 0); });
  },
  mf_js_export_close__deps: ['$MF'],
  mf_js_export_close: () => {
    const ex = MF.export;
    if (!ex) return;
    for (const e of [ex.video, ex.audio]) if (e && e.state !== 'closed') e.close();
    MF.export = null;
  },

  // --- Speaker clock ---

  // Keeps the worklet's map from AudioContext time to performance.now() time up to date.
  mf_js_speaker_track__deps: ['$MF', '$emscriptenGetAudioObject', 'mf_web_speaker_clock'],
  mf_js_speaker_track: (context, speaker) => {
    const ctx = emscriptenGetAudioObject(context);
    const update = () => {
      if (ctx.state === 'closed') return;
      const ts = ctx.getOutputTimestamp ? ctx.getOutputTimestamp() : null;
      if (ts && ts.contextTime > 0 && ts.performanceTime > 0) _mf_web_speaker_clock(speaker, ts.contextTime, MF.absolute(ts.performanceTime));
      else _mf_web_speaker_clock(speaker, ctx.currentTime, MF.absolute(performance.now() + ((ctx.baseLatency || 0) + (ctx.outputLatency || 0)) * 1000));
    };
    update();
    MF.speakers.set(speaker, setInterval(update, 20));
  },
  mf_js_speaker_untrack__deps: ['$MF'],
  mf_js_speaker_untrack: (speaker) => {
    clearInterval(MF.speakers.get(speaker));
    MF.speakers.delete(speaker);
  },
  mf_js_speaker_suspend__deps: ['$emscriptenGetAudioObject'],
  mf_js_speaker_suspend: (context) => { emscriptenGetAudioObject(context).suspend(); },
});
