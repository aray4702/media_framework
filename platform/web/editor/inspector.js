// The properties of the selection, as in the macOS editor: an item (timing; transform, opacity,
// fit, blend and effects for visual items; words and style for text; sound for video and audio),
// the join of two items (its transition), a track (on or off, opacity or gain, order), or with
// nothing selected the project (output size, frame rate, background). Timing changes go through
// the Document's edits; the rest replaces the item's look (ed_set_item). Values are constants:
// keyframes made elsewhere are replaced when a value is edited here.

const kKinds = ['none', 'cut', 'crossfade', 'push', 'slide', 'wipe'];  // index - 1 = SceneTransitionKind
const kDirections = ['left', 'right', 'up', 'down'];
const value = (v, fallback) => (typeof v === 'number' ? v : v && typeof v === 'object' && v.keys ? v.keys[0][1] : fallback);

export class Inspector {
  constructor(el, app) {
    this.el = el;
    this.app = app;
  }

  render() {
    const { app } = this, sel = app.sel;
    this.el.replaceChildren();
    if (sel.track < 0) return this.project();
    if (sel.transition) return this.transition(sel.track, sel.item);
    if (sel.item < 0) return this.track(sel.track);
    this.item(sel.track, sel.item);
  }

  // --- Building blocks ---

  title(text) { this.el.append(Object.assign(document.createElement('h2'), { textContent: text })); }
  section(text) { this.el.append(Object.assign(document.createElement('h3'), { textContent: text })); }

  field(label, input) {
    const row = Object.assign(document.createElement('label'), { className: 'field' });
    row.append(Object.assign(document.createElement('span'), { textContent: label }), input);
    this.el.append(row);
    return input;
  }

  // A slider with its value: changes apply as it moves (the player redraws the frame).
  slider(label, v, min, max, step, onChange) {
    const input = Object.assign(document.createElement('input'), { type: 'range', min, max, step, value: v });
    const shown = Object.assign(document.createElement('output'), { value: Number(v).toFixed(step < 0.01 ? 3 : 2) });
    shown.style.cssText = 'min-width: 42px; text-align: right; color: var(--muted); font-variant-numeric: tabular-nums';
    input.oninput = () => { shown.value = Number(input.value).toFixed(step < 0.01 ? 3 : 2); onChange(Number(input.value)); };
    const box = document.createElement('span');
    box.style.cssText = 'display: flex; gap: 6px; align-items: center';
    box.append(input, shown);
    this.field(label, box);
    return input;
  }

  number(label, v, step, onChange, min = 0) {
    const input = Object.assign(document.createElement('input'), { type: 'number', step, min, value: Math.round(v * 1000) / 1000 });
    input.onchange = () => onChange(Number(input.value));
    return this.field(label, input);
  }

  choice(label, options, v, onChange) {
    const input = document.createElement('select');
    for (const o of options) input.append(new Option(o, o));
    input.value = v;
    input.onchange = () => onChange(input.value);
    return this.field(label, input);
  }

  check(label, v, onChange) {
    const input = Object.assign(document.createElement('input'), { type: 'checkbox', checked: !!v });
    input.onchange = () => onChange(input.checked);
    return this.field(label, input);
  }

  color(label, v, onChange) {
    const input = Object.assign(document.createElement('input'), { type: 'color', value: (v || '#000000').slice(0, 7) });
    input.oninput = () => onChange(input.value + (v && v.length === 9 ? v.slice(7) : ''));
    return this.field(label, input);
  }

  actions(...buttons) {
    const row = Object.assign(document.createElement('div'), { className: 'actions' });
    for (const [text, onclick] of buttons) row.append(Object.assign(document.createElement('button'), { textContent: text, onclick }));
    this.el.append(row);
  }

  // Replaces item k of track t's look and sound with `change` applied to a copy of it.
  setItem(t, k, change) {
    const { app } = this, item = JSON.parse(JSON.stringify(app.item(t, k)));
    change(item);
    const ok = app.withStr(JSON.stringify(item), (p) => app.ed._ed_set_item(app.doc, t, k, p));
    if (!ok) return app.say(app.ed.UTF8ToString(app.ed._ed_error()), true);
    app.commit();
  }

  // The item's effect of that type (created with `defaults` when absent).
  static effect(item, type, defaults) {
    item.effects = item.effects || [];
    let e = item.effects.find((x) => x.type === type);
    if (!e) item.effects.push((e = { type, ...defaults }));
    return e;
  }

  deleteSelection() {
    const { app } = this, { track: t, item: k, transition } = app.sel, M = app.ed;
    if (t < 0) return;
    if (transition) M._ed_set_transition(app.doc, t, k, -1, 0, 0);
    else if (k >= 0) M._ed_remove_item(app.doc, t, k);
    else M._ed_remove_track(app.doc, t);
    app.sel = transition ? { track: t, item: k, transition: true } : { track: -1, item: -1, transition: false };
    app.commit();
  }

  // --- The selection ---

  item(t, k) {
    const { app } = this, item = app.item(t, k), us = app.us, M = app.ed;
    const visual = item.type !== 'audio', media = item.type === 'video' || item.type === 'audio';
    this.title(item.type === 'text' ? `Text: ${item.text}` : item.type === 'color' ? 'Color' : `${item.type[0].toUpperCase()}${item.type.slice(1)}: ${item.src}`);

    this.section('Timing');
    this.number('Start (s)', us(item.start) / 1e6, 0.1, (v) => { M._ed_move_item(app.doc, t, k, v * 1e6); app.commit(); });
    this.number('Duration (s)', us(item.duration) / 1e6, 0.1, (v) => { M._ed_set_duration(app.doc, t, k, v * 1e6); app.commit(); });
    if (media) {
      this.number('In (s)', us(item.in) / 1e6, 0.1, (v) => { M._ed_set_in(app.doc, t, k, v * 1e6); app.commit(); });
      this.number('Speed', item.speed ?? 1, 0.25, (v) => { M._ed_set_speed(app.doc, t, k, Math.max(0.01, v)); app.commit(); }, 0.01);
    }

    if (item.type === 'text') {
      this.section('Text');
      const words = document.createElement('textarea');
      words.value = item.text;
      words.rows = 2;
      words.onchange = () => this.setItem(t, k, (i) => { i.text = words.value || ' '; });
      this.field('Words', words);
      const style = item.style || {};
      this.choice('Font', ['system', 'system-bold'], style.font || 'system', (v) => this.setItem(t, k, (i) => { i.style = { ...i.style, font: v }; }));
      this.slider('Size', style.size ?? 0.05, 0.02, 0.3, 0.005, (v) => this.setItem(t, k, (i) => { i.style = { ...i.style, size: v }; }));
      this.color('Color', style.color || '#ffffff', (v) => this.setItem(t, k, (i) => { i.style = { ...i.style, color: v }; }));
      this.check('Box', !!style.box, (v) => this.setItem(t, k, (i) => {
        i.style = { ...i.style };
        if (v) i.style.box = '#0000008c'; else delete i.style.box;
      }));
    }
    if (item.type === 'color') this.color('Color', item.color, (v) => this.setItem(t, k, (i) => { i.color = v; }));

    if (visual) {
      this.section('Position');
      const tr = item.transform || {};
      const transform = (key, v) => this.setItem(t, k, (i) => { i.transform = { ...i.transform, [key]: v }; });
      this.slider('X', value(tr.x, 0.5), 0, 1, 0.005, (v) => transform('x', v));
      this.slider('Y', value(tr.y, 0.5), 0, 1, 0.005, (v) => transform('y', v));
      this.slider('Scale', value(tr.scale, 1), 0.05, 3, 0.01, (v) => transform('scale', v));
      this.slider('Rotation', value(tr.rotation, 0), -180, 180, 1, (v) => transform('rotation', v));
      this.slider('Opacity', value(item.opacity, 1), 0, 1, 0.01, (v) => this.setItem(t, k, (i) => { i.opacity = v; }));
      if (item.type !== 'text') this.choice('Fit', ['contain', 'cover', 'fill', 'none'], item.fit || 'contain', (v) => this.setItem(t, k, (i) => { i.fit = v; }));
      this.choice('Blend', ['normal', 'add', 'multiply', 'screen'], item.blend || 'normal', (v) => this.setItem(t, k, (i) => { i.blend = v; }));

      if (item.type !== 'text') {
        this.section('Effects');
        const fx = (type) => (item.effects || []).find((e) => e.type === type);
        const set = (type, defaults, key, v) => this.setItem(t, k, (i) => { Inspector.effect(i, type, defaults)[key] = v; });
        const toggle = (type, defaults, on) => this.setItem(t, k, (i) => {
          if (on) Inspector.effect(i, type, defaults);
          else i.effects = (i.effects || []).filter((e) => e.type !== type);
        });
        const ca = fx('colorAdjust');
        this.check('Color adjust', ca, (v) => toggle('colorAdjust', {}, v));
        if (ca) {
          this.slider('Brightness', value(ca.brightness, 0), -1, 1, 0.01, (v) => set('colorAdjust', {}, 'brightness', v));
          this.slider('Contrast', value(ca.contrast, 1), 0, 2, 0.01, (v) => set('colorAdjust', {}, 'contrast', v));
          this.slider('Saturation', value(ca.saturation, 1), 0, 2, 0.01, (v) => set('colorAdjust', {}, 'saturation', v));
        }
        const blur = fx('blur');
        this.check('Blur', blur, (v) => toggle('blur', { radius: 0.005 }, v));
        if (blur) this.slider('Radius', value(blur.radius, 0.005), 0, 0.05, 0.001, (v) => set('blur', {}, 'radius', v));
        const crop = fx('crop');
        this.check('Crop', crop, (v) => toggle('crop', {}, v));
        if (crop) for (const side of ['left', 'top', 'right', 'bottom']) this.slider(side[0].toUpperCase() + side.slice(1), value(crop[side], 0), 0, 0.45, 0.005, (v) => set('crop', {}, side, v));
        const key = fx('chromaKey');
        this.check('Chroma key', key, (v) => toggle('chromaKey', { color: '#00ff00' }, v));
        if (key) {
          this.color('Key color', key.color || '#00ff00', (v) => set('chromaKey', {}, 'color', v));
          this.slider('Tolerance', key.tolerance ?? 0.15, 0, 0.5, 0.01, (v) => set('chromaKey', {}, 'tolerance', v));
        }
      }
    }

    if (media) {
      this.section('Sound');
      const audio = item.type === 'video' ? item.audio || {} : item;
      const sound = (key, v) => this.setItem(t, k, (i) => {
        if (i.type === 'video') i.audio = { ...i.audio, [key]: v }; else i[key] = v;
      });
      if (item.type === 'video') this.check('Mute', audio.mute, (v) => sound('mute', v));
      this.slider('Gain', value(audio.gain, 1), 0, 4, 0.05, (v) => sound('gain', v));
      this.slider('Pan', value(audio.pan, 0), -1, 1, 0.05, (v) => sound('pan', v));
    }

    const buttons = [['Delete', () => this.deleteSelection()]];
    if (item.type === 'video' && !item.audio?.mute) {
      buttons.unshift(['Detach audio', () => {
        const r = M._ed_detach_audio(app.doc, t, k);
        if (r < 0) return app.say('Can\'t detach: there are already 16 tracks', true);
        app.sel = { track: r >> 16, item: r & 0xffff, transition: false };
        app.commit();
      }]);
    }
    this.actions(...buttons);
  }

  transition(t, k) {
    const { app } = this, track = app.scene.tracks[t], M = app.ed;
    const x = app.timeline.transitionInto(track, k);
    this.title('Transition');
    const kind = x ? x.kind || 'crossfade' : 'none';
    const apply = (kindName, direction, duration) => {
      M._ed_set_transition(app.doc, t, k, kKinds.indexOf(kindName) - 1, kDirections.indexOf(direction), duration * 1e6);
      app.commit();
    };
    const direction = x?.direction || 'left', duration = x ? app.us(x.duration) / 1e6 : 1;
    this.choice('Kind', kKinds, kind, (v) => apply(v, direction, duration));
    if (x && kind !== 'cut') {
      if (['push', 'slide', 'wipe'].includes(kind)) this.choice('Direction', kDirections, direction, (v) => apply(kind, v, duration));
      this.number('Duration (s)', duration, 0.1, (v) => apply(kind, direction, Math.max(0.1, v)));
    }
    if (x) this.actions(['Remove', () => this.deleteSelection()]);
  }

  track(t) {
    const { app } = this, track = app.scene.tracks[t], M = app.ed;
    this.title(`${track.kind === 'video' ? 'Video' : 'Audio'} track ${t + 1}`);
    const set = (change) => {
      const next = { kind: track.kind, enabled: track.enabled !== false, items: [], ...(track.kind === 'video' ? { opacity: track.opacity ?? 1, effects: track.effects || [] } : { gain: track.gain ?? 1 }) };
      change(next);
      if (!app.withStr(JSON.stringify(next), (p) => M._ed_set_track(app.doc, t, p))) return app.say(M.UTF8ToString(M._ed_error()), true);
      app.commit();
    };
    this.check('On', track.enabled !== false, (v) => set((x) => { x.enabled = v; }));
    if (track.kind === 'video') this.slider('Opacity', track.opacity ?? 1, 0, 1, 0.01, (v) => set((x) => { x.opacity = v; }));
    else this.slider('Gain', track.gain ?? 1, 0, 4, 0.05, (v) => set((x) => { x.gain = v; }));
    const move = (delta) => { app.sel = { track: M._ed_move_track(app.doc, t, delta), item: -1, transition: false }; app.commit(); };
    this.actions(['Move up', () => move(1)], ['Move down', () => move(-1)], ['Delete track', () => this.deleteSelection()]);
  }

  project() {
    const { app } = this, o = app.scene.output || {}, M = app.ed;
    this.title('Project');
    const set = (change) => {
      const next = { width: o.width || 1920, height: o.height || 1080, fps: o.fps || 30, background: o.background || '#000000' };
      change(next);
      if (!app.withStr(JSON.stringify(next), (p) => M._ed_set_output(app.doc, p))) return app.say(M.UTF8ToString(M._ed_error()), true);
      app.commit();
    };
    this.choice('Size', ['1920×1080', '1280×720', '1080×1920', '1080×1080'], `${o.width || 1920}×${o.height || 1080}`,
                (v) => set((x) => { [x.width, x.height] = v.split('×').map(Number); }));
    this.choice('Frame rate', ['24', '25', '30', '60'], String(o.fps || 30), (v) => set((x) => { x.fps = Number(v); }));
    this.color('Background', o.background || '#000000', (v) => set((x) => { x.background = v; }));
    const hint = Object.assign(document.createElement('p'), { className: 'hint',
      textContent: 'Select an item, a track or a diamond between two items (a transition) to see its properties.' });
    hint.style.color = 'var(--muted)';
    this.el.append(hint);
  }
}
