// The WebGPU compositor: draws a composed frame (as web_platform.cpp's frameJson writes it) the
// way platform/macos/src/metal_compositor.mm does. Per layer: crop, fit, scale, rotation and flip
// about the anchor, the transition offset and the wipe's clip; in one shader, chroma key, color
// adjust (brightness, contrast, saturation), the global filter and opacity; a blur in two passes
// on an offscreen copy; blend modes onto the canvas. A track drawn on its own (a transition, or
// track effects) is combined into a texture first and drawn as one layer. Plugin effects are
// macOS only so far: a layer's plugin effects are skipped here.

addToLibrary({
  $MFC__deps: ['$MF'],
  $MFC: {
    // Every fragment shader returns premultiplied color; `shade` follows the Metal one exactly.
    shaders: `
      struct U {
        p0: vec4f, p1: vec4f,  // the corners TL, TR | BL, BR, in normalized device coordinates
        uv: vec4f,             // u0, v0, u1, v1
        color: vec4f,
        fx0: vec4f,            // brightness, contrast, saturation, opacity
        fx1: vec4f,            // global brightness, global contrast, key on, tolerance
        fx2: vec4f,            // softness, key Cb, key Cr, premultiplied (the texture's color is)
        blur: vec4f,           // step x, step y, sigma, radius
      };
      @group(0) @binding(0) var samp: sampler;
      @group(0) @binding(1) var ext: texture_external;
      @group(0) @binding(2) var<uniform> u: U;
      @group(0) @binding(3) var tex: texture_2d<f32>;
      struct V { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
      @vertex fn vs(@builtin(vertex_index) i: u32) -> V {
        var p = array<vec2f, 4>(u.p0.xy, u.p0.zw, u.p1.xy, u.p1.zw);
        let c = vec2f(f32(i & 1u), f32((i >> 1u) & 1u));
        var o: V;
        o.pos = vec4f(p[i], 0.0, 1.0);
        o.uv = mix(u.uv.xy, u.uv.zw, c);
        return o;
      }
      const kLuma = vec3f(0.2126, 0.7152, 0.0722);
      fn shade(rgb: vec3f, alpha: f32) -> vec4f {
        var a = alpha;
        if (u.fx1.z > 0.5) {
          let Y = dot(rgb, kLuma);
          let cc = vec2f((rgb.b - Y) / 1.8556, (rgb.r - Y) / 1.5748);
          a *= smoothstep(u.fx1.w, u.fx1.w + max(u.fx2.x, 0.0001), distance(cc, u.fx2.yz));
        }
        var c = (rgb - 0.5) * u.fx0.y + 0.5 + u.fx0.x;
        let L = dot(c, kLuma);
        c = L + (c - L) * u.fx0.z;
        c = (saturate(c) - 0.5) * u.fx1.y + 0.5 + u.fx1.x;
        a *= u.fx0.w;
        return vec4f(saturate(c) * a, a);
      }
      @fragment fn fs_ext(v: V) -> @location(0) vec4f {
        return shade(textureSampleBaseClampToEdge(ext, samp, v.uv).rgb, 1.0);
      }
      @fragment fn fs_tex(v: V) -> @location(0) vec4f {
        let p = textureSampleLevel(tex, samp, v.uv, 0.0);
        if (u.fx2.w > 0.5) { return shade(select(vec3f(0.0), p.rgb / p.a, p.a > 0.0), p.a); }
        return shade(p.rgb, p.a);
      }
      @fragment fn fs_color(v: V) -> @location(0) vec4f { return shade(u.color.rgb, u.color.a); }
      @fragment fn fs_blur(v: V) -> @location(0) vec4f {
        var sum = vec4f(0.0);
        var total = 0.0;
        let r = i32(u.blur.w);
        for (var i = -r; i <= r; i++) {
          let x = f32(i);
          let w = exp(-x * x / (2.0 * u.blur.z * u.blur.z));
          sum += w * textureSampleLevel(tex, samp, v.uv + u.blur.xy * x, 0.0);
          total += w;
        }
        return sum / total;
      }`,

    kOffscreen: 'rgba16float',
    kFullQuad: { pos: [-1, 1, 1, 1, -1, -1, 1, -1], uv: [0, 0, 1, 1] },
    kMaxTexts: 16,

    create(device, format) {
      const c = { device, format, module: device.createShaderModule({ code: MFC.shaders }), pipes: new Map(),
                  sampler: device.createSampler({ magFilter: 'linear', minFilter: 'linear' }),
                  uniforms: null, slots: 0, data: null, groupTextures: [], scratch: [], scratchUsed: 0, texts: [] };
      // Compile the pipelines most frames use now, off the frame path: a pipeline compiled on first
      // use stalls that frame.
      const warm = [['fs_ext', format, 0], ['fs_tex', format, 0], ['fs_color', format, 0], ['fs_tex', MFC.kOffscreen, -1],
                    ['fs_ext', MFC.kOffscreen, 0], ['fs_ext', MFC.kOffscreen, 1, true], ['fs_blur', MFC.kOffscreen, -1]];
      for (const [fs, f, blend, plus = false] of warm) {
        const key = `${fs}|${f}|${blend}|${plus}`;
        c.device.createRenderPipelineAsync(MFC.pipelineDescriptor(c, fs, f, blend, plus))
          .then((p) => { if (!c.pipes.has(key)) c.pipes.set(key, p); }, () => {});
      }
      return c;
    },

    // Blend factors as on macOS: premultiplied source over an opaque canvas. `plus`: alpha adds too
    // (a crossfade pair sums to (1 - p)·A + p·B in a group).
    blendState(blend, plus) {
      const alpha = { srcFactor: 'one', dstFactor: plus ? 'one' : 'one-minus-src-alpha' };
      const color = [
        { srcFactor: 'one', dstFactor: 'one-minus-src-alpha' },  // normal
        { srcFactor: 'one', dstFactor: 'one' },                  // add
        { srcFactor: 'dst', dstFactor: 'one-minus-src-alpha' },  // multiply
        { srcFactor: 'one', dstFactor: 'one-minus-src' },        // screen
      ][blend];
      return { color, alpha };
    },

    // fs: 'fs_ext' | 'fs_tex' | 'fs_color' | 'fs_blur'; blend: -1 none, else 0..3; plus: see above.
    pipelineDescriptor(c, fs, format, blend, plus) {
      const target = { format };
      if (blend >= 0) target.blend = MFC.blendState(blend, plus);
      return { layout: 'auto', vertex: { module: c.module, entryPoint: 'vs' },
               fragment: { module: c.module, entryPoint: fs, targets: [target] }, primitive: { topology: 'triangle-strip' } };
    },
    pipeline(c, fs, format, blend, plus = false) {
      const key = `${fs}|${format}|${blend}|${plus}`;
      let p = c.pipes.get(key);
      if (!p) {
        p = c.device.createRenderPipeline(MFC.pipelineDescriptor(c, fs, format, blend, plus));
        c.pipes.set(key, p);
      }
      return p;
    },

    // One draw's uniforms; returns its slot. Written to the GPU once per frame, before submit.
    slot(c, vert, fx, blur) {
      if (c.slots === c.data.length / 64) {
        const grown = new Float32Array(c.data.length * 2);
        grown.set(c.data);
        c.data = grown;
      }
      const d = c.data, o = c.slots * 64;
      d.set(vert.pos, o);
      d.set(vert.uv, o + 8);
      d.set(fx, o + 12);
      if (blur) d.set(blur, o + 28);
      return c.slots++;
    },

    // A layer's (or a track's) effects as shader parameters: color, fx0..fx2.
    effectsOf(l, opacity) {
      const e = l.fx, col = l.color || [0, 0, 0, 1];
      const fx = [col[0], col[1], col[2], col[3], e.brightness, e.contrast, e.saturation, opacity, 0, 1, 0, 0, 0, 0, 0, 0];
      if (e.key) {
        const k = e.key.color, Y = 0.2126 * k[0] + 0.7152 * k[1] + 0.0722 * k[2];
        fx[10] = 1;
        fx[11] = e.key.tolerance;
        fx[12] = e.key.softness;
        fx[13] = (k[2] - Y) / 1.8556;
        fx[14] = (k[0] - Y) / 1.5748;
      }
      return fx;
    },

    // Text (§4.5) on a 2D canvas: wrapped at maxWidthPx, aligned, in the style's color, on its box.
    textTexture(c, text, style, maxWidthPx, lineHeightPx) {
      const key = JSON.stringify([text, style, maxWidthPx, lineHeightPx]);
      const hit = c.texts.findIndex((t) => t.key === key);
      if (hit >= 0) {
        const [t] = c.texts.splice(hit, 1);
        c.texts.unshift(t);
        return t.texture;
      }
      const points = lineHeightPx / 1.2;
      const family = style.font === 'system' || style.font === 'system-bold' ? 'system-ui, sans-serif' : `"${style.font}", sans-serif`;
      const font = `${style.font === 'system-bold' ? '600 ' : ''}${points}px ${family}`;
      const pad = style.hasBox ? lineHeightPx * 0.3 : 2;
      const measure = new OffscreenCanvas(1, 1).getContext('2d');
      measure.font = font;
      const limit = Math.max(1, maxWidthPx - 2 * pad), lines = [];
      for (const paragraph of text.split('\n')) {
        let line = '';
        for (const word of paragraph.split(' ')) {
          const next = line ? `${line} ${word}` : word;
          if (line && measure.measureText(next).width > limit) { lines.push(line); line = word; } else line = next;
        }
        lines.push(line);
      }
      const width = Math.ceil(Math.min(limit, Math.max(...lines.map((l) => measure.measureText(l).width))) + 2 * pad);
      const height = Math.ceil(lines.length * lineHeightPx + 2 * pad);
      const canvas = new OffscreenCanvas(width, height), g = canvas.getContext('2d');
      const rgba = (k) => `rgba(${k[0] * 255}, ${k[1] * 255}, ${k[2] * 255}, ${k[3]})`;
      if (style.hasBox) {
        g.fillStyle = rgba(style.box);
        g.beginPath();
        g.roundRect(0, 0, width, height, pad);
        g.fill();
      }
      g.font = font;
      g.fillStyle = rgba(style.color);
      g.textBaseline = 'middle';
      g.textAlign = ['left', 'center', 'right'][style.align];
      const x = [pad, width / 2, width - pad][style.align];
      lines.forEach((l, i) => g.fillText(l, x, pad + (i + 0.5) * lineHeightPx));
      const texture = c.device.createTexture({ size: [width, height], format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
      c.device.queue.copyExternalImageToTexture({ source: canvas }, { texture, premultipliedAlpha: false }, [width, height]);
      c.texts.unshift({ key, texture });
      if (c.texts.length > MFC.kMaxTexts) c.texts.pop().texture.destroy();
      return texture;
    },

    // An image item's texture, made once from its ImageBitmap.
    imageTexture(c, h) {
      const image = MF.images.get(h);
      if (!image) return null;
      if (!image.texture) {
        const b = image.bitmap;
        image.texture = c.device.createTexture({ size: [b.width, b.height], format: 'rgba8unorm',
          usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.RENDER_ATTACHMENT });
        c.device.queue.copyExternalImageToTexture({ source: b }, { texture: image.texture, premultipliedAlpha: false }, [b.width, b.height]);
      }
      return image.texture;
    },

    // The layer's source, then its geometry (§4.1): crop the natural size, fit it to the canvas,
    // then scale and rotate about the anchor placed at (x, y) plus the transition offset.
    prepare(c, l, W, H, scale) {
      const p = { fs: 'fs_color', ext: null, tex: null, premultiplied: false, pos: new Array(8), uv: [0, 0, 1, 1], boxW: 0, boxH: 0, effectsDone: false };
      let nw = W, nh = H;
      if (l.kind === 0) {  // video
        const f = MF.frames.get(l.h);
        if (!f) return null;
        p.fs = 'fs_ext';
        p.ext = f;
        nw = f.displayWidth;
        nh = f.displayHeight;
      } else if (l.kind === 1) {  // image
        p.tex = MFC.imageTexture(c, l.h);
        if (!p.tex) return null;
        p.fs = 'fs_tex';
        nw = p.tex.width;
        nh = p.tex.height;
      } else if (l.kind === 2) {  // text
        const lineHeight = Math.max(4, Math.round(l.style.size * H * scale));
        const maxWidth = Math.max(lineHeight, Math.round(l.style.maxWidth * W * scale));
        p.tex = MFC.textTexture(c, l.text, l.style, maxWidth, lineHeight);
        p.fs = 'fs_tex';
        nw = p.tex.width / scale;
        nh = p.tex.height / scale;
      }
      const crop = l.fx.crop, u0 = crop[0], v0 = crop[1], u1 = 1 - crop[2], v1 = 1 - crop[3];
      if (u1 <= u0 || v1 <= v0) return null;
      const cw = nw * (u1 - u0), ch = nh * (v1 - v0);
      let bw = cw, bh = ch;
      if (l.fit === 0 || l.fit === 1) {
        const k = l.fit === 0 ? Math.min(W / cw, H / ch) : Math.max(W / cw, H / ch);
        bw = cw * k;
        bh = ch * k;
      } else if (l.fit === 2) {
        bw = W;
        bh = H;
      }
      p.boxW = bw * l.scale * scale;
      p.boxH = bh * l.scale * scale;
      const px = (l.x + l.offsetX) * W, py = (l.y + l.offsetY) * H;
      const a = l.rotation * Math.PI / 180, cs = Math.cos(a), sn = Math.sin(a);
      for (let k = 0; k < 4; ++k) {
        const u = k & 1, v = (k >> 1) & 1;
        const dx = (u - l.anchorX) * bw * l.scale, dy = (v - l.anchorY) * bh * l.scale;
        const x = px + dx * cs - dy * sn, y = py + dx * sn + dy * cs;  // clockwise on screen (y down)
        p.pos[2 * k] = 2 * x / W - 1;
        p.pos[2 * k + 1] = 1 - 2 * y / H;
      }
      p.uv = [l.flipX ? u1 : u0, v0, l.flipX ? u0 : u1, v1];
      return p;
    },

    scratch(c, w, h) {
      if (c.scratchUsed === c.scratch.length) c.scratch.push(null);
      let t = c.scratch[c.scratchUsed];
      if (!t || t.width !== w || t.height !== h) {
        if (t) t.destroy();
        t = c.device.createTexture({ size: [w, h], format: MFC.kOffscreen, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
        c.scratch[c.scratchUsed] = t;
      }
      ++c.scratchUsed;
      return t;
    },

    bind(c, pipeline, p, slot) {
      const entries = [{ binding: 2, resource: { buffer: c.uniforms, offset: slot * 256, size: 128 } }];
      if (p.fs !== 'fs_color') entries.push({ binding: 0, resource: c.sampler });
      if (p.fs === 'fs_ext') entries.push({ binding: 1, resource: c.device.importExternalTexture({ source: p.ext }) });
      if (p.fs === 'fs_tex' || p.fs === 'fs_blur') entries.push({ binding: 3, resource: p.tex.createView() });
      return c.device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries });
    },

    // Draws the layer's source with its shader effects into a texture of its size on screen, then
    // blurs it in two passes (horizontal, vertical). The layer then draws that texture. Passes are
    // recorded in `passes` (run before the canvas pass) as [target, pipeline, prepared, slot].
    effectsInto(c, p, l, passes, pixelsPerUnit) {
      const w = Math.min(4096, Math.max(1, Math.round(p.boxW))), h = Math.min(4096, Math.max(1, Math.round(p.boxH)));
      let a = MFC.scratch(c, w, h);
      const b = MFC.scratch(c, w, h);
      const src = { ...p, pos: MFC.kFullQuad.pos }, fx = MFC.effectsOf(l, 1);
      fx[15] = p.premultiplied ? 1 : 0;  // a track's combined image is premultiplied
      passes.push([a, MFC.pipeline(c, p.fs, MFC.kOffscreen, -1), src, MFC.slot(c, { pos: MFC.kFullQuad.pos, uv: p.uv }, fx)]);
      if (l.fx.blur > 0) {
        // Wide blurs sample every few pixels, so a pass takes at most 64 taps each side.
        const sigmaPx = l.fx.blur * pixelsPerUnit, reach = 3 * sigmaPx, stride = Math.max(1, reach / 64);
        const sigma = Math.max(0.5, sigmaPx / stride), radius = Math.max(1, Math.ceil(reach / stride));
        const none = [0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0];
        for (const axis of [0, 1]) {
          const from = axis === 0 ? a : b, to = axis === 0 ? b : a;
          const blur = [axis === 0 ? stride / w : 0, axis === 1 ? stride / h : 0, sigma, radius];
          passes.push([to, MFC.pipeline(c, 'fs_blur', MFC.kOffscreen, -1), { fs: 'fs_blur', tex: from },
                       MFC.slot(c, MFC.kFullQuad, none, blur)]);
        }
      }
      Object.assign(p, { fs: 'fs_tex', ext: null, tex: a, premultiplied: true, uv: [0, 0, 1, 1], effectsDone: true });
    },

    // The draw's shader parameters, as Metal's draw(): without its own effects once they were
    // drawn offscreen; the global filter on video and image layers only.
    drawFx(l, p, filter, effects) {
      const fx = MFC.effectsOf(l, l.opacity);
      if (!effects) { fx[4] = 0; fx[5] = fx[6] = 1; fx[10] = 0; }
      if (filter && (l.kind === 0 || l.kind === 1)) { fx[8] = filter[0]; fx[9] = filter[1]; }
      fx[15] = p.premultiplied ? 1 : 0;
      return fx;
    },

    encode(c, frame, view, tw, th) {
      const W = frame.width || tw, H = frame.height || th;
      const scale = Math.min(tw / W, th / H), vw = W * scale, vh = H * scale, vx = (tw - vw) / 2, vy = (th - vh) / 2;
      c.slots = 0;
      c.scratchUsed = 0;
      if (!c.data) c.data = new Float32Array(64 * 64);
      const pre = [], groups = [];  // offscreen passes, then group passes: [target, pipeline, prepared, slot, clip]
      const offscreen = (e) => e.blur > 0;
      const prepared = frame.layers.map((l) => {
        if (!(l.opacity > 0)) return null;
        const p = MFC.prepare(c, l, W, H, scale);
        if (p && offscreen(l.fx)) MFC.effectsInto(c, p, l, pre, H * scale);
        return p;
      });
      // Each track drawn on its own: its layers into a target-sized texture, B over A or summed for
      // a crossfade, then drawn as one layer, less the track's crop, with the track's effects.
      const groupPrepared = frame.groups.map((g, gi) => {
        const crop = g.fx.crop, x0 = crop[0], y0 = crop[1], x1 = 1 - crop[2], y1 = 1 - crop[3];
        if (!(g.opacity > 0) || x1 <= x0 || y1 <= y0) return null;
        let t = c.groupTextures[gi];
        if (!t || t.width !== tw || t.height !== th) {
          if (t) t.destroy();
          t = c.groupTextures[gi] = c.device.createTexture({ size: [tw, th], format: MFC.kOffscreen,
            usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
        }
        const draws = [];
        frame.layers.forEach((l, i) => {
          if (l.group !== gi || !prepared[i]) return;
          const p = prepared[i];
          draws.push([MFC.pipeline(c, p.fs, MFC.kOffscreen, l.blend === 1 ? 1 : 0, l.blend === 1), p,
                      MFC.slot(c, p, MFC.drawFx(l, p, frame.filter, !p.effectsDone)), l.clip]);
        });
        groups.push([t, draws]);
        const gl = { kind: 1, opacity: g.opacity, blend: g.blend, fx: g.fx };
        const p = { fs: 'fs_tex', tex: t, premultiplied: true, effectsDone: false,
                    pos: [2 * x0 - 1, 1 - 2 * y0, 2 * x1 - 1, 1 - 2 * y0, 2 * x0 - 1, 1 - 2 * y1, 2 * x1 - 1, 1 - 2 * y1],
                    uv: [(vx + x0 * vw) / tw, (vy + y0 * vh) / th, (vx + x1 * vw) / tw, (vy + y1 * vh) / th],
                    boxW: (x1 - x0) * vw, boxH: (y1 - y0) * vh, layer: gl };
        if (offscreen(g.fx)) MFC.effectsInto(c, p, gl, pre, H * scale);
        return p;
      });

      // The canvas: black bands, the background, then the layers and track images in order.
      const main = [];
      const bg = { kind: 3, color: frame.background, opacity: 1, fx: { crop: [0, 0, 0, 0], brightness: 0, contrast: 1, saturation: 1 } };
      main.push([MFC.pipeline(c, 'fs_color', c.format, 0), { fs: 'fs_color', pos: MFC.kFullQuad.pos, uv: [0, 0, 1, 1] },
                 MFC.slot(c, MFC.kFullQuad, MFC.drawFx(bg, {}, null, false)), null]);
      const drawn = new Set();
      frame.layers.forEach((l, i) => {
        if (l.group >= 0) {
          if (drawn.has(l.group) || !groupPrepared[l.group]) return;
          drawn.add(l.group);
          const p = groupPrepared[l.group], gl = p.layer;
          main.push([MFC.pipeline(c, 'fs_tex', c.format, gl.blend), p, MFC.slot(c, p, MFC.drawFx(gl, p, null, !p.effectsDone)), null]);
          return;
        }
        const p = prepared[i];
        if (p) main.push([MFC.pipeline(c, p.fs, c.format, l.blend), p, MFC.slot(c, p, MFC.drawFx(l, p, frame.filter, !p.effectsDone)), l.clip]);
      });

      const bytes = c.slots * 256;
      if (!c.uniforms || c.uniforms.size < bytes) {
        if (c.uniforms) c.uniforms.destroy();
        c.uniforms = c.device.createBuffer({ size: Math.max(bytes, 256 * 64), usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      }
      for (let s = 0; s < c.slots; ++s) c.device.queue.writeBuffer(c.uniforms, s * 256, c.data, s * 64, 32);

      const encoder = c.device.createCommandEncoder();
      const pass = (target, clear, load) => encoder.beginRenderPass({ colorAttachments: [{ view: target,
        clearValue: clear, loadOp: load ? 'load' : 'clear', storeOp: 'store' }] });
      for (const [target, pipeline, p, slot] of pre) {
        const enc = pass(target.createView(), { r: 0, g: 0, b: 0, a: 0 });
        enc.setPipeline(pipeline);
        enc.setBindGroup(0, MFC.bind(c, pipeline, p, slot));
        enc.draw(4);
        enc.end();
      }
      // Clipped draws (wipes) are limited to their part of the canvas.
      const run = (enc, draws, tw, th) => {
        for (const [pipeline, p, slot, clip] of draws) {
          const clipped = clip && (clip[0] > 0 || clip[1] > 0 || clip[2] < 1 || clip[3] < 1);
          if (clipped) {
            const sx = Math.max(0, Math.floor(vx + clip[0] * vw)), sy = Math.max(0, Math.floor(vy + clip[1] * vh));
            const ex = Math.min(tw, Math.ceil(vx + clip[2] * vw)), ey = Math.min(th, Math.ceil(vy + clip[3] * vh));
            if (ex <= sx || ey <= sy) continue;
            enc.setScissorRect(sx, sy, ex - sx, ey - sy);
          }
          enc.setPipeline(pipeline);
          enc.setBindGroup(0, MFC.bind(c, pipeline, p, slot));
          enc.draw(4);
          if (clipped) enc.setScissorRect(0, 0, tw, th);
        }
      };
      for (const [t, draws] of groups) {
        const enc = pass(t.createView(), { r: 0, g: 0, b: 0, a: 0 });
        enc.setViewport(vx, vy, vw, vh, 0, 1);
        run(enc, draws, tw, th);
        enc.end();
      }
      const enc = pass(view, { r: 0, g: 0, b: 0, a: 1 });
      enc.setViewport(vx, vy, vw, vh, 0, 1);
      run(enc, main, tw, th);
      enc.end();
      c.device.queue.submit([encoder.finish()]);
    },
  },
});
