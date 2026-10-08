// The browser side of the web platform (web_platform.cpp): the scheduler's timers, WebCodecs
// decoders, the WebGPU display and the audio clock mapping. Everything here runs on the page's
// thread; the AudioWorklet runs only C++.

addToLibrary({
  $MF__postset: "Module['mfStats'] = () => MF.stats();",
  $MF: {
    live: new Set(),       // schedulers that may still be run
    queue: [],             // runs waiting for the next message
    channel: null,
    frames: new Map(),     // handle -> VideoFrame held by C++
    nextHandle: 1,
    maxHeld: 0,            // most frames held by C++ at once
    maxDecoding: 0,        // most frames inside the decoders (queued, not yet output) at once
    vdecs: [],
    adecs: [],
    displays: new Set(),
    speakers: new Map(),   // speaker -> interval id
    gpu: null,
    drawn: 0,

    stats() {
      let decoding = 0;
      for (const st of MF.vdecs) if (st) decoding += st.decoder.decodeQueueSize + st.out.length;
      return { heldFrames: MF.frames.size, maxHeldFrames: MF.maxHeld, maxDecodingFrames: MF.maxDecoding, decodingNow: decoding,
               drawn: MF.drawn };
    },

    notify(onOutput) { _mf_web_output(onOutput); },

    // The video decoders' frames in flight: queued for decoding plus output not yet taken.
    noteDecoding() {
      let n = 0;
      for (const st of MF.vdecs) if (st) n += st.decoder.decodeQueueSize + st.out.length;
      MF.maxDecoding = Math.max(MF.maxDecoding, n);
    },

    makeVideoDecoder(st) {
      st.decoder = new VideoDecoder({
        output: (frame) => { st.out.push(frame); MF.notify(st.onOutput); },
        error: (e) => { console.error('VideoDecoder:', e.message); st.failed = true; MF.notify(st.onOutput); },
      });
      st.decoder.ondequeue = () => MF.notify(st.onOutput);  // room to queue again
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
        error: (e) => { console.error('AudioDecoder:', e.message); st.failed = true; MF.notify(st.onOutput); },
      });
      st.decoder.ondequeue = () => MF.notify(st.onOutput);
    },

    initPipeline(device, format) {
      const module = device.createShaderModule({ code: `
        struct U { rect: vec4f, opacity: vec4f };
        @group(0) @binding(0) var samp: sampler;
        @group(0) @binding(1) var tex: texture_external;
        @group(0) @binding(2) var<uniform> u: U;
        struct V { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
        @vertex fn vs(@builtin(vertex_index) i: u32) -> V {
          var corners = array<vec2f, 6>(vec2f(0, 0), vec2f(1, 0), vec2f(0, 1), vec2f(0, 1), vec2f(1, 0), vec2f(1, 1));
          let c = corners[i];
          var o: V;
          o.pos = vec4f(mix(u.rect.x, u.rect.z, c.x), mix(u.rect.y, u.rect.w, c.y), 0.0, 1.0);
          o.uv = c;
          return o;
        }
        @fragment fn fs(v: V) -> @location(0) vec4f {
          let c = textureSampleBaseClampToEdge(tex, samp, v.uv);
          return vec4f(c.rgb * u.opacity.x, u.opacity.x);
        }` });
      const blend = { color: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' }, alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' } };
      return device.createRenderPipeline({
        layout: 'auto',
        vertex: { module, entryPoint: 'vs' },
        fragment: { module, entryPoint: 'fs', targets: [{ format, blend }] },
        primitive: { topology: 'triangle-list' },
      });
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
    st.eos = st.failed = false;
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
    if (st.failed || st.decoder.state !== 'configured') return 2;
    if (st.decoder.decodeQueueSize + st.out.length >= 6) return 1;
    st.decoder.decode(new EncodedVideoChunk({ type: key ? 'key' : 'delta', timestamp: ptsUs, data: HEAPU8.slice(data, data + length) }));
    MF.noteDecoding();
    return 0;
  },
  mf_js_vdec_flush_eos__deps: ['$MF'],
  mf_js_vdec_flush_eos: (id) => {
    const st = MF.vdecs[id - 1], gen = st.gen;
    st.decoder.flush().then(() => { if (st.gen === gen) { st.eos = true; MF.notify(st.onOutput); } }, () => {});
  },
  mf_js_vdec_reset__deps: ['$MF'],
  mf_js_vdec_reset: (id) => {
    const st = MF.vdecs[id - 1];
    ++st.gen;
    st.out.forEach((f) => f.close());
    st.out = [];
    st.eos = st.failed = false;
    if (st.decoder.state === 'closed') MF.makeVideoDecoder(st);
    else st.decoder.reset();
    if (st.config) st.decoder.configure(st.config);
  },
  // > 0 a frame handle (its time in *ptsUs), 0 again, -1 Eos, -2 failed.
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
    st.eos = st.failed = false;
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
    if (st.failed || st.decoder.state !== 'configured') return 2;
    if (st.decoder.decodeQueueSize + st.out.length >= 8) return 1;
    st.decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: ptsUs, data: HEAPU8.slice(data, data + length) }));
    return 0;
  },
  mf_js_adec_flush_eos__deps: ['$MF'],
  mf_js_adec_flush_eos: (id) => {
    const st = MF.adecs[id - 1], gen = st.gen;
    st.decoder.flush().then(() => { if (st.gen === gen) { st.eos = true; MF.notify(st.onOutput); } }, () => {});
  },
  mf_js_adec_reset__deps: ['$MF'],
  mf_js_adec_reset: (id) => {
    const st = MF.adecs[id - 1];
    ++st.gen;
    st.out = [];
    st.eos = st.failed = false;
    if (st.decoder.state === 'closed') MF.makeAudioDecoder(st);
    else st.decoder.reset();
    if (st.config) st.decoder.configure(st.config);
  },
  // > 0 frames per channel of the next output (its time and channels written), 0 again, -1 Eos, -2 failed.
  mf_js_adec_dequeue__deps: ['$MF'],
  mf_js_adec_dequeue: (id, ptsUs, channels) => {
    const st = MF.adecs[id - 1];
    if (st.out.length) {
      HEAPF64[ptsUs >> 3] = st.out[0].pts;
      HEAP32[channels >> 2] = st.out[0].channels;
      return st.out[0].frames;
    }
    if (st.failed) return -2;
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
  mf_js_display_attach__deps: ['$MF', 'mf_web_display_tick'],
  mf_js_display_attach: (selector, display) => {
    const canvas = document.querySelector(UTF8ToString(selector)), device = Module['mfGpuDevice'];
    if (!canvas || !device) return 0;
    const context = canvas.getContext('webgpu'), format = navigator.gpu.getPreferredCanvasFormat();
    context.configure({ device, format, alphaMode: 'opaque' });
    MF.gpu = { device, context, canvas, pipeline: MF.initPipeline(device, format), sampler: device.createSampler({ magFilter: 'linear', minFilter: 'linear' }),
               uniforms: device.createBuffer({ size: 256 * 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }) };
    MF.displays.add(display);
    const loop = (t) => {
      if (!MF.displays.has(display)) return;
      _mf_web_display_tick(display, t);
      requestAnimationFrame(loop);
    };
    requestAnimationFrame(loop);
    return 1;
  },
  mf_js_display_detach__deps: ['$MF'],
  mf_js_display_detach: (display) => { MF.displays.delete(display); },
  mf_js_display_visible: () => (document.visibilityState === 'visible' ? 1 : 0),
  // layers: per video layer [handle, fit, x, y, anchorX, anchorY, scale, offsetX, offsetY, opacity].
  mf_js_display_draw__deps: ['$MF'],
  mf_js_display_draw: (layersPtr, count, backgroundPtr, width, height) => {
    const g = MF.gpu;
    if (!g) return;
    if (g.canvas.width !== width || g.canvas.height !== height) { g.canvas.width = width; g.canvas.height = height; }
    const bg = HEAPF32.subarray(backgroundPtr >> 2, (backgroundPtr >> 2) + 4);
    const L = HEAPF32.slice(layersPtr >> 2, (layersPtr >> 2) + count * 10);
    const n = Math.min(count, 16), u = new Float32Array(64 * n), frames = [];
    for (let i = 0; i < n; ++i) {
      const [h, fit, x, y, ax, ay, scale, ox, oy, opacity] = L.subarray(i * 10, i * 10 + 10);
      const f = MF.frames.get(h);
      frames.push(f);
      if (!f) continue;
      const fw = f.displayWidth, fh = f.displayHeight;
      let sx = 1, sy = 1;
      if (fit === 0) sx = sy = Math.min(width / fw, height / fh);
      else if (fit === 1) sx = sy = Math.max(width / fw, height / fh);
      else if (fit === 2) { sx = width / fw; sy = height / fh; }
      const bw = fw * sx * scale, bh = fh * sy * scale;
      const left = x * width - ax * bw + ox * width, top = y * height - ay * bh + oy * height;
      u.set([left / width * 2 - 1, 1 - top / height * 2, (left + bw) / width * 2 - 1, 1 - (top + bh) / height * 2, opacity], i * 64);
    }
    g.device.queue.writeBuffer(g.uniforms, 0, u);
    const encoder = g.device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: g.context.getCurrentTexture().createView(),
      clearValue: { r: bg[0], g: bg[1], b: bg[2], a: 1 }, loadOp: 'clear', storeOp: 'store' }] });
    pass.setPipeline(g.pipeline);
    frames.forEach((f, i) => {
      if (!f) return;
      pass.setBindGroup(0, g.device.createBindGroup({ layout: g.pipeline.getBindGroupLayout(0), entries: [
        { binding: 0, resource: g.sampler },
        { binding: 1, resource: g.device.importExternalTexture({ source: f }) },
        { binding: 2, resource: { buffer: g.uniforms, offset: i * 256, size: 32 } }] }));
      pass.draw(6);
    });
    pass.end();
    g.device.queue.submit([encoder.finish()]);
    ++MF.drawn;
  },

  // --- Speaker clock ---

  // Keeps the worklet's map from AudioContext time to performance.now() time up to date.
  mf_js_speaker_track__deps: ['$MF', '$emscriptenGetAudioObject', 'mf_web_speaker_clock'],
  mf_js_speaker_track: (context, speaker) => {
    const ctx = emscriptenGetAudioObject(context);
    const update = () => {
      if (ctx.state === 'closed') return;
      const ts = ctx.getOutputTimestamp ? ctx.getOutputTimestamp() : null;
      if (ts && ts.contextTime > 0 && ts.performanceTime > 0) _mf_web_speaker_clock(speaker, ts.contextTime, ts.performanceTime);
      else _mf_web_speaker_clock(speaker, ctx.currentTime, performance.now() + ((ctx.baseLatency || 0) + (ctx.outputLatency || 0)) * 1000);
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
