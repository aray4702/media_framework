// Editing on the preview, as the macOS editor's overlay (apps/macos-editor/preview_overlay.mm): a
// click selects what's under it, top first; the selected item shows its box with handles to move
// it (drag inside), scale it (a corner), crop it (a side) and turn it (the knob above; Shift: 15°
// steps); a double-click on text edits its words in place. Values set here are constants, as
// there. Every change replaces the item's look (ed_set_item), which the player redraws at once.

const kHandle = 8;         // corner and side handles, in CSS pixels
const kRotateOffset = 26;  // the rotate handle, beyond the top edge
const kHit = 8;            // how near a handle a press takes it
const kMaxCrop = 0.45;     // a side: never the whole image
const kSides = ['left', 'top', 'right', 'bottom'];

// An animatable value at `local` µs into its item: a number, or keys [[seconds, value], ...]
// (eased keys are taken as linear: close enough for a box to grab).
function at(v, local, fallback) {
  if (typeof v === 'number') return v;
  const keys = v?.keys;
  if (!keys?.length) return fallback;
  const s = local / 1e6;
  if (s <= keys[0][0]) return keys[0][1];
  for (let i = 1; i < keys.length; ++i) {
    const [t0, v0] = keys[i - 1], [t1, v1] = keys[i];
    if (s <= t1) return t1 > t0 ? v0 + (v1 - v0) * (s - t0) / (t1 - t0) : v1;
  }
  return keys[keys.length - 1][1];
}

const distance = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);

// An item as drawn: its box after crop, fit and scale, rotated about its anchor, in canvas pixels.
class Box {
  constructor(o) { Object.assign(this, o); }
  at(u, v) {  // the box's point at fractions (u, v)
    const dx = (u - this.ax) * this.w, dy = (v - this.ay) * this.h, c = Math.cos(this.rad), s = Math.sin(this.rad);
    return { x: this.anchor.x + dx * c - dy * s, y: this.anchor.y + dx * s + dy * c };
  }
  contains(p) {
    const dx = p.x - this.anchor.x, dy = p.y - this.anchor.y, c = Math.cos(this.rad), s = Math.sin(this.rad);
    const u = this.ax + (dx * c + dy * s) / this.w, v = this.ay + (-dx * s + dy * c) / this.h;
    return this.w > 0 && this.h > 0 && u >= 0 && u <= 1 && v >= 0 && v <= 1;
  }
}

// The text's rasterized size in canvas pixels, as the compositor makes it (library_mf_compositor.js).
const measure = new OffscreenCanvas(1, 1).getContext('2d');
function textSize(item, W, H) {
  const style = item.style || {}, font = style.font || 'system', size = style.size ?? 0.05;
  const lineHeight = Math.max(4, Math.round(size * H));
  const maxWidth = Math.max(lineHeight, Math.round((style.maxWidth ?? 0.9) * W));
  const family = font === 'system' || font === 'system-bold' ? 'system-ui, sans-serif' : `"${font}", sans-serif`;
  measure.font = `${font === 'system-bold' ? '600 ' : ''}${lineHeight / 1.2}px ${family}`;
  const pad = style.box ? lineHeight * 0.3 : 2, limit = Math.max(1, maxWidth - 2 * pad), lines = [];
  for (const paragraph of item.text.split('\n')) {
    let line = '';
    for (const word of paragraph.split(' ')) {
      const next = line ? `${line} ${word}` : word;
      if (line && measure.measureText(next).width > limit) { lines.push(line); line = word; } else line = next;
    }
    lines.push(line);
  }
  return { w: Math.ceil(Math.min(limit, Math.max(...lines.map((l) => measure.measureText(l).width))) + 2 * pad),
           h: Math.ceil(lines.length * lineHeight + 2 * pad) };
}

export class Overlay {
  constructor(stage, preview, app) {
    this.stage = stage;
    this.preview = preview;
    this.app = app;
    // The canvas shows only while it has something to draw: over the player's, it would otherwise
    // cost the page's compositing every frame of playback. The stage takes the pointer.
    this.canvas = Object.assign(document.createElement('canvas'), { className: 'overlay', hidden: true });
    stage.append(this.canvas);
    this.drag = null;    // 'move' | 'scale' | 'rotate' | 'crop-left' ... while a press lasts
    this.editor = null;  // the field over a text item while its words are edited
    stage.addEventListener('pointerdown', (e) => this.down(e));
    stage.addEventListener('pointermove', (e) => this.moved(e));
    stage.addEventListener('pointerup', (e) => this.up(e));
    stage.addEventListener('pointercancel', (e) => this.up(e));
    stage.addEventListener('dblclick', (e) => this.doubleClick(e));
    new ResizeObserver(() => this.render()).observe(stage);
  }

  // --- Geometry ---

  get W() { return this.app.scene.output?.width || 1920; }
  get H() { return this.app.scene.output?.height || 1080; }

  // The canvas letterboxed into the preview element, as the player draws it, in the stage's
  // coordinates: its scale and top-left corner.
  frame() {
    const s = this.stage.getBoundingClientRect(), r = this.preview.getBoundingClientRect();
    const k = Math.min(r.width / this.W, r.height / this.H);
    return { k, x: r.left - s.left + (r.width - this.W * k) / 2, y: r.top - s.top + (r.height - this.H * k) / 2 };
  }
  toView(c, f = this.frame()) { return { x: f.x + c.x * f.k, y: f.y + c.y * f.k }; }
  toCanvas(p, f = this.frame()) { return { x: (p.x - f.x) / f.k, y: (p.y - f.y) / f.k }; }
  point(e) { const s = this.stage.getBoundingClientRect(); return { x: e.clientX - s.left, y: e.clientY - s.top }; }

  box(t, k) {
    const { app, W, H } = this, it = app.item(t, k), local = app.playheadUs - app.us(it.start);
    let natural = { w: W, h: H };  // a color fills the output
    if (it.type === 'video' || it.type === 'image') {
      const m = app.media.get(it.src);
      natural = { w: m?.width || 0, h: m?.height || 0 };
    } else if (it.type === 'text') {
      natural = textSize(it, W, H);
    } else if (it.type === 'audio') {
      return null;
    }
    if (natural.w <= 0 || natural.h <= 0) return null;
    const crop = (it.effects || []).find((e) => e.type === 'crop');
    const side = (s) => (crop ? at(crop[s], local, 0) : 0);
    const u0 = side('left'), v0 = side('top'), u1 = 1 - side('right'), v1 = 1 - side('bottom');
    if (u1 <= u0 || v1 <= v0) return null;
    const cw = natural.w * (u1 - u0), ch = natural.h * (v1 - v0);
    let bw = cw, bh = ch;
    const fit = it.type === 'text' ? 'none' : it.fit || 'contain';
    if (fit === 'contain' || fit === 'cover') {
      const f = fit === 'contain' ? Math.min(W / cw, H / ch) : Math.max(W / cw, H / ch);
      bw = cw * f;
      bh = ch * f;
    } else if (fit === 'fill') {
      bw = W;
      bh = H;
    }
    const tr = it.transform || {}, scale = at(tr.scale, local, 1), [ax, ay] = tr.anchor || [0.5, 0.5];
    return new Box({ anchor: { x: at(tr.x, local, 0.5) * W, y: at(tr.y, local, 0.5) * H }, w: bw * scale, h: bh * scale, ax, ay,
                     rad: at(tr.rotation, local, 0) * Math.PI / 180, visibleU: u1 - u0, visibleV: v1 - v0 });
  }

  visible(t, k) {
    const { app } = this, track = app.scene.tracks[t], it = track.items[k], start = app.us(it.start);
    return track.kind === 'video' && track.enabled !== false && it.type !== 'audio' && app.playheadUs >= start && app.playheadUs < start + app.us(it.duration);
  }

  // The selected item's box, when it's visible at the playhead.
  selectedBox() {
    const { track: t, item: k, transition } = this.app.sel;
    return t >= 0 && k >= 0 && !transition && this.app.item(t, k) && this.visible(t, k) ? this.box(t, k) : null;
  }

  // A box's handles, in the stage's coordinates: the corners (scale), the sides' middles (crop,
  // left, top, right, bottom), and the rotate handle, beyond the top edge's middle.
  handles(b, f = this.frame()) {
    const corners = [[0, 0], [1, 0], [1, 1], [0, 1]].map(([u, v]) => this.toView(b.at(u, v), f));
    const sides = [[0, 0.5], [0.5, 0], [1, 0.5], [0.5, 1]].map(([u, v]) => this.toView(b.at(u, v), f));
    const center = this.toView(b.at(0.5, 0.5), f), top = sides[1], len = Math.max(1, distance(top, center));
    return { corners, sides, top, rotate: { x: top.x + (top.x - center.x) / len * kRotateOffset, y: top.y + (top.y - center.y) / len * kRotateOffset } };
  }

  // What a press at p on the selected item would do.
  dragAt(p) {
    const b = this.selectedBox();
    if (!b) return null;
    const f = this.frame(), h = this.handles(b, f);
    if (distance(p, h.rotate) <= kHit + 2) return 'rotate';
    if (h.corners.some((c) => distance(p, c) <= kHit)) return 'scale';
    const side = h.sides.findIndex((s) => distance(p, s) <= kHit);
    if (side >= 0) return `crop-${kSides[side]}`;
    return b.contains(this.toCanvas(p, f)) ? 'move' : null;
  }

  // --- Drawing ---

  render() {
    if (this.editor && !this.isEdited()) this.stopEditing();
    const b = this.app.scene && this.selectedBox();
    this.canvas.hidden = !b;
    if (!b) return;
    const c = this.canvas, dpr = devicePixelRatio || 1, w = this.stage.clientWidth, h = this.stage.clientHeight;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr);
      c.height = Math.round(h * dpr);
    }
    const g = c.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, w, h);
    if (this.editor) this.placeEditor(b);
    const hs = this.handles(b), accent = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#2f6fdf';
    const path = (points) => { g.beginPath(); points.forEach((p, i) => (i ? g.lineTo(p.x, p.y) : g.moveTo(p.x, p.y))); g.closePath(); };
    path(hs.corners);
    g.strokeStyle = 'rgba(0, 0, 0, 0.35)';  // a dark edge, so the outline shows on white too
    g.lineWidth = 3.5;
    g.stroke();
    g.strokeStyle = accent;
    g.lineWidth = 1.5;
    g.stroke();
    g.beginPath();
    g.moveTo(hs.top.x, hs.top.y);
    g.lineTo(hs.rotate.x, hs.rotate.y);
    g.stroke();
    const knob = (shape) => {
      g.save();
      shape();
      g.fillStyle = '#fff';
      g.fill();
      g.strokeStyle = accent;
      g.stroke();
      g.restore();
    };
    for (const p of hs.corners) knob(() => { g.beginPath(); g.rect(p.x - kHandle / 2, p.y - kHandle / 2, kHandle, kHandle); });
    hs.sides.forEach((p, i) => knob(() => {  // crop: a bar along its side, as the item turns
      g.translate(p.x, p.y);
      g.rotate(b.rad);
      g.beginPath();
      if (i % 2 === 0) g.roundRect(-2.5, -8, 5, 16, 2.5); else g.roundRect(-8, -2.5, 16, 5, 2.5);
    }));
    knob(() => { g.beginPath(); g.arc(hs.rotate.x, hs.rotate.y, 6, 0, 2 * Math.PI); });
    g.beginPath();
    g.arc(hs.rotate.x, hs.rotate.y, 3, -Math.PI / 2, Math.PI);
    g.stroke();
  }

  // --- Changing the item ---

  // Replaces the selected item's look with `change` applied to a copy of it.
  change(change) {
    const { app } = this, { track: t, item: k } = app.sel, item = JSON.parse(JSON.stringify(app.item(t, k)));
    change(item);
    if (!app.withStr(JSON.stringify(item), (p) => app.ed._ed_set_item(app.doc, t, k, p))) return app.say(app.ed.UTF8ToString(app.ed._ed_error()), true);
    app.commit(true);
  }

  down(e) {
    if (e.button !== 0 || e.target === this.editor) return;
    this.stopEditing();
    const { app } = this, p = this.point(e);
    this.drag = this.dragAt(p);
    if (!this.drag) {  // select what's under the press, top first
      const c = this.toCanvas(p);
      let sel = {};
      for (let t = app.scene.tracks.length - 1; t >= 0 && sel.track === undefined; --t) {
        for (let i = app.scene.tracks[t].items.length - 1; i >= 0; --i) {
          if (this.visible(t, i) && this.box(t, i)?.contains(c)) { sel = { track: t, item: i }; break; }
        }
      }
      const was = app.sel;
      if (sel.track !== was.track || sel.item !== was.item || was.transition) app.select(sel);
      if (sel.track === undefined) return;
      this.drag = 'move';
    }
    // Where the drag starts from.
    const it = app.item(app.sel.track, app.sel.item), local = app.playheadUs - app.us(it.start), tr = it.transform || {};
    const crop = (it.effects || []).find((x) => x.type === 'crop');
    this.start = { p, box: this.selectedBox(), x: at(tr.x, local, 0.5), y: at(tr.y, local, 0.5), scale: at(tr.scale, local, 1),
                   rotation: at(tr.rotation, local, 0), crop: this.drag.startsWith('crop-') && crop ? at(crop[this.drag.slice(5)], local, 0) : 0 };
    this.stage.setPointerCapture(e.pointerId);
    this.cursor(this.drag === 'move' ? 'grabbing' : null);
  }

  moved(e) {
    const p = this.point(e);
    if (!this.drag) return this.cursor(this.dragAt(p));
    const { start } = this, f = this.frame(), { W, H } = this;
    if (!start.box) return;
    const dx = (p.x - start.p.x) / f.k, dy = (p.y - start.p.y) / f.k;  // canvas pixels
    const anchor = this.toView(start.box.anchor, f);
    const drag = this.drag;
    this.change((item) => {
      const tr = item.transform = { ...item.transform };
      if (drag === 'move') {
        tr.x = start.x + dx / W;
        tr.y = start.y + dy / H;
      } else if (drag === 'scale') {
        tr.scale = Math.min(100, Math.max(0.02, start.scale * distance(p, anchor) / Math.max(1, distance(start.p, anchor))));
      } else if (drag === 'rotate') {
        const turn = Math.atan2(p.y - anchor.y, p.x - anchor.x) - Math.atan2(start.p.y - anchor.y, start.p.x - anchor.x);
        let degrees = start.rotation + turn * 180 / Math.PI;
        if (e.shiftKey) degrees = Math.round(degrees / 15) * 15;
        tr.rotation = degrees;
      } else {  // crop: the drag along the box's own axes, as a fraction of the source
        const b = start.box, c = Math.cos(b.rad), s = Math.sin(b.rad), lx = dx * c + dy * s, ly = -dx * s + dy * c;
        item.effects = item.effects || [];
        let crop = item.effects.find((x) => x.type === 'crop');
        if (!crop) item.effects.push((crop = { type: 'crop', left: 0, top: 0, right: 0, bottom: 0 }));
        const side = drag.slice(5);
        const inward = { left: lx, right: -lx, top: ly, bottom: -ly }[side];
        const length = side === 'left' || side === 'right' ? b.w : b.h, visible = side === 'left' || side === 'right' ? b.visibleU : b.visibleV;
        crop[side] = Math.min(kMaxCrop, Math.max(0, start.crop + inward / Math.max(1, length) * visible));
      }
    });
  }

  up(e) {
    if (!this.drag) return;
    this.drag = null;
    if (this.stage.hasPointerCapture(e.pointerId)) this.stage.releasePointerCapture(e.pointerId);
    this.cursor(this.dragAt(this.point(e)));
  }

  cursor(drag) {
    this.stage.style.cursor = { move: 'grab', grabbing: 'grabbing', scale: 'crosshair', rotate: 'crosshair', 'crop-left': 'ew-resize',
                                 'crop-right': 'ew-resize', 'crop-top': 'ns-resize', 'crop-bottom': 'ns-resize' }[drag] || 'default';
  }

  // --- Editing text ---

  doubleClick(e) {
    const { app } = this, { track: t, item: k } = app.sel;
    if (this.editor || t < 0 || k < 0 || app.item(t, k)?.type !== 'text' || this.dragAt(this.point(e)) !== 'move') return;
    this.editText();
  }

  isEdited() {
    const { sel } = this.app;
    return this.edited && sel.track === this.edited.track && sel.item === this.edited.item && !sel.transition;
  }

  // A field over the selected text item, upright at its center, in its font, color and alignment
  // at the preview's scale.
  editText() {
    const b = this.selectedBox();
    if (!b) return;
    const { app } = this, it = app.item(app.sel.track, app.sel.item), style = it.style || {}, font = style.font || 'system';
    const points = Math.max(6, (style.size ?? 0.05) * this.H * this.frame().k / 1.2);  // a line is about 1.2 × the font size
    const field = this.editor = Object.assign(document.createElement('textarea'), { className: 'text-edit', value: it.text, spellcheck: false });
    const family = font === 'system' || font === 'system-bold' ? 'system-ui, sans-serif' : `"${font}", sans-serif`;
    Object.assign(field.style, { font: `${font === 'system-bold' ? '600 ' : ''}${points}px/1.2 ${family}`, textAlign: style.align || 'center',
                                 color: (style.color || '#ffffff').slice(0, 7) });
    this.edited = { ...app.sel };
    this.before = it.text;
    field.oninput = () => {  // the words as typed become the item's (not while empty: a text item needs some)
      if (field.value && this.isEdited()) this.change((item) => { item.text = field.value; });
    };
    field.onkeydown = (e) => {
      if (e.key === 'Enter' && !e.shiftKey && !e.altKey) { e.preventDefault(); this.stopEditing(); }  // Shift- or Option-Return: a new line
      if (e.key === 'Escape') {  // the text as it was
        e.preventDefault();
        if (this.isEdited()) this.change((item) => { item.text = this.before; });
        this.stopEditing();
      }
    };
    field.onblur = () => this.stopEditing();
    this.stage.append(field);
    this.placeEditor(b);
    field.focus();
    field.select();
  }

  // Over the item's box as it is now (it grows with the text), centered on it, a little larger so
  // the last characters typed don't wrap early.
  placeEditor(b) {
    const f = this.frame(), center = this.toView(b.at(0.5, 0.5), f), points = parseFloat(this.editor.style.fontSize) || 12;
    const w = Math.max(80, b.w * f.k + 16), h = Math.max(points * 1.4, b.h * f.k + 6);
    Object.assign(this.editor.style, { left: `${center.x - w / 2}px`, top: `${center.y - h / 2}px`, width: `${w}px`, height: `${h}px` });
  }

  stopEditing() {
    const field = this.editor;
    if (!field) return;
    this.editor = null;  // first: removing the field blurs it, which calls back here
    this.edited = null;
    field.remove();
    this.render();
  }
}
