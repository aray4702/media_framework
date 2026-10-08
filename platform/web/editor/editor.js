// The web editor: the macOS editor's Document (C++, editor_api.cpp) on this page's thread, and the
// player on its own (web_api.cpp). Every edit goes through the Document, which keeps every track
// valid; the scene then goes to the player (mf_apply), which redraws the frame when only the look
// changed, else reopens the scene at the playhead.

import { Inspector } from './inspector.js';
import { Recording, warmVideoEncoder } from './record.js';
import { Timeline } from './timeline.js';

const M = await createMediaFramework();

// C strings and byte buffers for the C API, freed after the call (it copies what it keeps).
const withStr = (s, f) => { const p = M.stringToNewUTF8(s); try { return f(p); } finally { M._free(p); } };
const withBytes = (bytes, f) => { const p = M._malloc(bytes.length); M.HEAPU8.set(bytes, p); try { return f(p); } finally { M._free(p); } };
const us = (seconds) => Math.round((Number(seconds) || 0) * 1e6);
const kStillUs = 5000000;  // new image, text and color items, as on macOS

const app = {
  session: withStr('#preview', (p) => M._mf_create(p)),
  doc: M._ed_new(),
  scene: null,
  sel: { track: -1, item: -1, transition: false },
  playheadUs: 0,
  state: 'START',
  media: new Map(),  // name -> { kind: 'video' | 'audio' | 'image', lengthUs, width, height }
  zoom: 80,          // pixels per second
  ed: M,             // the C API, for the views
  withStr,
};

const status = document.getElementById('status');
function say(text, error = false) {
  status.textContent = text;
  status.classList.toggle('error', error);
}
app.say = say;
app.us = us;
app.fmt = (t) => fmt(t);

// --- The scene ---

// The scene as a document, for the player and saving (app.sceneJson), and for the views
// (app.scene): in a document a track's transitions sit among its items, so there each track's
// items are the items only, and its transitions say which item they follow (`from`), as the
// Document's item indices count them.
function readScene() {
  app.sceneJson = M.UTF8ToString(M._ed_scene(app.doc));
  app.scene = JSON.parse(app.sceneJson);
  for (const track of app.scene.tracks) {
    const items = [], transitions = [];
    for (const entry of track.items) {
      if (entry.type === 'transition') transitions.push({ ...entry, kind: entry.kind || 'crossfade', from: items.length - 1 });
      else items.push(entry);
    }
    track.items = items;
    track.transitions = transitions;
  }
}
app.durationUs = () => {
  let end = 0;
  for (const t of app.scene.tracks) if (t.enabled !== false) for (const i of t.items) end = Math.max(end, us(i.start) + us(i.duration));
  return end;
};
app.item = (t, k) => app.scene.tracks[t]?.items[k];

let applyTimer = 0;
// After an edit: the views follow, and the player a moment after the last of a burst of edits.
const preview = document.getElementById('preview');
app.commit = () => {
  readScene();
  const o = app.scene.output || {};
  preview.style.aspectRatio = `${o.width || 1920} / ${o.height || 1080}`;  // the canvas's pixels are the player's
  if (app.sel.track >= app.scene.tracks.length) app.sel = { track: -1, item: -1, transition: false };
  timeline.render();
  inspector.render();
  updateTime();
  clearTimeout(applyTimer);
  applyTimer = setTimeout(() => withStr(app.sceneJson, (p) => M._mf_apply(app.session, p, app.playheadUs)), 60);
};

app.select = (sel) => {
  app.sel = { track: -1, item: -1, transition: false, ...sel };
  timeline.render();
  inspector.render();
};

// --- Media ---

function uniqueName(name) {
  if (!app.media.has(name)) return name;
  const dot = name.lastIndexOf('.');
  for (let n = 2; ; ++n) {
    const candidate = dot > 0 ? `${name.slice(0, dot)} ${n}${name.slice(dot)}` : `${name} ${n}`;
    if (!app.media.has(candidate)) return candidate;
  }
}

// Files picked or dropped: media is listed (and sent to the player); a .json opens as the project,
// its media matched by file name among the files given with it.
async function importFiles(files) {
  let project = null;
  for (const f of files) {
    if (f.name.toLowerCase().endsWith('.json')) { project = f; continue; }
    await importBytes(f.name, new Uint8Array(await f.arrayBuffer()), f.type);
  }
  sidebar.render();
  if (project) await openProject(project);
  else if (files.length) say(`${app.media.size} file${app.media.size === 1 ? '' : 's'} in the project`);
}

// A media file's bytes into the project, under a name not yet taken (returned; null: not media).
async function importBytes(fileName, bytes, type) {
  const name = uniqueName(fileName);
  if (type.startsWith('image/')) {
    try {
      const bitmap = await createImageBitmap(new Blob([bytes], { type }));
      app.media.set(name, { kind: 'image', width: bitmap.width, height: bitmap.height });
      bitmap.close();
      withBytes(bytes, (ptr) => withStr(name, (n) => M._mf_add_image(app.session, n, ptr, bytes.length)));
      return name;
    } catch {
      say(`${fileName}: not an image this browser decodes`, true);
      return null;
    }
  }
  const probe = JSON.parse(withBytes(bytes, (ptr) => M.UTF8ToString(M._ed_probe(ptr, bytes.length))));
  if (probe.error || (!probe.video && !probe.audio)) {
    say(`${fileName}: not an MP4 with H.264 video or AAC audio (${probe.error || 'no usable track'})`, true);
    return null;
  }
  app.media.set(name, { kind: probe.video ? 'video' : 'audio', lengthUs: probe.durationUs, width: probe.video?.width, height: probe.video?.height, audio: probe.audio });
  withBytes(bytes, (ptr) => withStr(name, (n) => M._mf_add_source(app.session, n, ptr, bytes.length)));
  return name;
}

async function openProject(file) {
  const ok = withStr(await file.text(), (p) => M._ed_load(app.doc, p));
  if (!ok) return say(`${file.name}: ${M.UTF8ToString(M._ed_error())}`, true);
  readScene();
  const missing = new Set();
  for (const t of app.scene.tracks) {
    for (const i of t.items) {
      if (!i.src) continue;
      const m = app.media.get(i.src);
      if (!m) missing.add(i.src);
      else if (m.lengthUs) withStr(i.id, (id) => M._ed_set_length(app.doc, id, m.lengthUs));
    }
  }
  app.playheadUs = us(app.scene.metadata?.editor?.playhead);  // where the editor that saved it was
  app.select({});
  app.commit();
  say(missing.size ? `Opened ${file.name}; add these files too: ${[...missing].join(', ')}` : `Opened ${file.name}`, missing.size > 0);
}

// --- Adding items ---

// A track for an item from startUs to endUs: a video at the lowest free video track; anything
// else visual above whatever plays then (a free video track above it, or a new one); audio on the
// lowest free audio track, or a new one.
function trackFor(item, startUs, endUs) {
  const tracks = app.scene.tracks;
  const busy = (t) => t.items.some((i) => us(i.start) < endUs && us(i.start) + us(i.duration) > startUs);
  if (item.type === 'audio') {
    const t = M._ed_free_track(app.doc, 0, startUs, endUs);
    return t >= 0 ? t : M._ed_add_track(app.doc, 0);
  }
  if (item.type === 'video') {
    const t = M._ed_free_track(app.doc, 1, startUs, endUs);
    return t >= 0 ? t : M._ed_add_track(app.doc, 1);
  }
  let top = -1;
  tracks.forEach((t, i) => { if (t.kind === 'video' && busy(t)) top = i; });
  for (let i = top + 1; i < tracks.length; ++i) if (tracks[i].kind === 'video' && !busy(tracks[i])) return i;
  return M._ed_add_track(app.doc, 1);
}

// Adds an item (as in a document) at the playhead (or atUs) and selects it.
app.addItem = (item, lengthUs = 0, atUs = app.playheadUs) => {
  readScene();
  const startUs = atUs, endUs = startUs + (item.duration ? us(item.duration) : lengthUs);
  const t = trackFor(item, startUs, endUs);
  if (t < 0) return say('There are already 16 tracks', true);
  const k = withStr(JSON.stringify({ start: 0, ...item }), (p) => M._ed_insert_item(app.doc, t, p, startUs, lengthUs));  // insertItem places it
  if (k < 0) return say(M.UTF8ToString(M._ed_error()), true);
  app.sel = { track: t, item: k, transition: false };
  app.commit();
};

app.addMedia = (name, atUs = app.playheadUs) => {
  const m = app.media.get(name);
  if (m.kind === 'image') return app.addItem({ type: 'image', src: name, duration: kStillUs / 1e6 }, 0, atUs);
  app.addItem({ type: m.kind, src: name, duration: m.lengthUs / 1e6 }, m.lengthUs, atUs);
};

// --- Playback ---

app.seek = (atUs) => {
  app.playheadUs = Math.max(0, Math.min(atUs, app.durationUs()));
  if (app.state === 'PLAY') M._mf_pause(app.session);
  if (app.state === 'PLAY' || app.state === 'READY') M._mf_seek(app.session, app.playheadUs);
  timeline.setPlayhead();
  updateTime();
};

function togglePlay() {
  if (app.state === 'PLAY') return M._mf_pause(app.session);
  if (app.state !== 'READY') return;
  if (app.playheadUs >= app.durationUs() - 50000) app.seek(0);
  M._mf_play(app.session);
}

const fmt = (t) => { const s = t / 1e6; return `${Math.floor(s / 60)}:${(s % 60).toFixed(2).padStart(5, '0')}`; };
function updateTime() {
  document.getElementById('time').textContent = `${fmt(app.playheadUs)} / ${fmt(app.durationUs())}`;
  document.getElementById('play').textContent = app.state === 'PLAY' ? '❚❚ Pause' : '▶︎ Play';
}

// The player's report: its state, and the playhead while it plays.
let seenEvents = 0;
const listeners = [];
app.onEvent = (f) => listeners.push(f);
setInterval(() => {
  const r = JSON.parse(M.UTF8ToString(M._mf_report(app.session)));
  app.report = r;
  app.state = r.state;
  if (r.state === 'PLAY' && r.positionUs >= 0) {
    app.playheadUs = r.positionUs;
    timeline.setPlayhead();
  }
  for (const e of r.events.slice(seenEvents)) {
    if (e === 'error') say(r.error, true);
    listeners.forEach((f) => f(e, r));
  }
  seenEvents = r.events.length;
  updateTime();
}, 50);

// --- Saving ---

function download(blob, name) {
  const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(blob), download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function save() {
  readScene();
  const doc = JSON.parse(app.sceneJson);
  doc.metadata = { ...(doc.metadata || {}), editor: { playhead: app.playheadUs / 1e6 } };
  download(new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' }), 'project.json');
  say('Saved project.json (its media files are referenced by name)');
}

// --- Sidebar ---

const sidebar = {
  tab: 'Media',
  tabs: ['Media', 'Text', 'Emoji', 'Colors', 'Record', 'Export'],
  render() {
    const bar = document.getElementById('tabs'), body = document.getElementById('tab-body');
    bar.replaceChildren(...this.tabs.map((name) => {
      const b = Object.assign(document.createElement('button'), { textContent: name, role: 'tab' });
      b.setAttribute('aria-selected', String(name === this.tab));
      b.onclick = () => { this.tab = name; this.render(); };
      return b;
    }));
    body.replaceChildren();
    const hint = (text) => body.append(Object.assign(document.createElement('div'), { className: 'hint', textContent: text }));
    const button = (text, onclick, style = '') => {
      const b = Object.assign(document.createElement('button'), { textContent: text, onclick });
      b.style.cssText = style;
      body.append(b);
      return b;
    };
    if (this.tab === 'Media') {
      if (!app.media.size) hint('Add MP4 videos, M4A audio or images with Add media…, or drop them here. Click one to add it at the playhead.');
      for (const [name, m] of app.media) {
        const row = document.createElement('div');
        row.className = 'media';
        row.innerHTML = `<span></span><small></small>`;
        row.firstChild.textContent = name;
        row.lastChild.textContent = m.kind === 'image' ? `${m.width}×${m.height}` : `${m.kind} ${fmt(m.lengthUs)}`;
        row.onclick = () => app.addMedia(name);
        body.append(row);
      }
    } else if (this.tab === 'Text') {
      const presets = [['Heading', 'system-bold', 0.1, 'font-size:20px;font-weight:700'], ['Subheading', 'system-bold', 0.065, 'font-size:16px;font-weight:700'],
                       ['Body text', 'system', 0.045, 'font-size:13px'], ['Caption', 'system', 0.045, 'font-size:12px']];
      for (const [text, font, size, css] of presets) {
        button(`Add ${text.toLowerCase()}`, () => {
          const item = { type: 'text', text, duration: kStillUs / 1e6, style: { font, size } };
          if (text === 'Caption') Object.assign(item, { style: { font, size, box: '#0000008c' }, transform: { y: 0.94, anchor: [0.5, 1] } });
          app.addItem(item);
        }, `text-align:left;${css}`);
      }
      hint('Text goes over the video at the playhead. Change its words and look in the properties.');
    } else if (this.tab === 'Emoji') {
      const row = Object.assign(document.createElement('div'), { className: 'emojis' });
      for (const e of ['😀', '😂', '😍', '🔥', '🎉', '👍', '❤️', '⭐️', '🚀', '🌈', '🎬', '📍']) {
        row.append(Object.assign(document.createElement('button'), { textContent: e,
          onclick: () => app.addItem({ type: 'text', text: e, duration: kStillUs / 1e6, style: { size: 0.16 } }) }));
      }
      body.append(row);
    } else if (this.tab === 'Colors') {
      const row = Object.assign(document.createElement('div'), { className: 'swatches' });
      for (const c of ['#000000', '#ffffff', '#e54d42', '#f2a33a', '#f5d63d', '#4caf6d', '#3e8ed0', '#7a5cd6']) {
        const b = Object.assign(document.createElement('button'), { title: c, onclick: () => app.addItem({ type: 'color', color: c, duration: kStillUs / 1e6 }) });
        b.style.background = c;
        row.append(b);
      }
      body.append(row);
      hint('A color fills the output: put it under other layers as a background.');
    } else if (this.tab === 'Record') {
      recordPanel(body, hint);
    } else if (this.tab === 'Export') {
      exportPanel(body, hint);
    }
    if (this.tab !== 'Record') recorder.closePreview();
  },
};

// The Export tab: a size, then the MP4 (H.264 + AAC) made on the player's thread and downloaded.
function exportPanel(body, hint) {
  const o = app.scene.output || {};
  const select = document.createElement('select');
  const W = o.width || 1920, H = o.height || 1080;
  for (const [label, w, h] of [[`Project (${W}×${H})`, 0, 0], ['720p', Math.round(W * 720 / H / 2) * 2, 720], ['1080p', Math.round(W * 1080 / H / 2) * 2, 1080]]) {
    select.append(new Option(label, `${w}x${h}`));
  }
  const progress = Object.assign(document.createElement('progress'), { max: 1, value: 0 });
  progress.style.width = '100%';
  const go = Object.assign(document.createElement('button'), { className: 'primary', textContent: 'Export MP4' });
  go.onclick = () => {
    if (!app.durationUs()) return say('Nothing to export: add something to the timeline', true);
    const [w, h] = select.value.split('x').map(Number);
    go.disabled = true;
    withStr(app.sceneJson, (p) => M._mf_export(app.session, p, w, h, 8000000));
    const started = performance.now();
    const tick = setInterval(() => {
      progress.value = app.report?.export?.progress || 0;
    }, 100);
    const done = (e, r) => {
      if (e !== 'exported' && !e.startsWith('exportFailed')) return;
      clearInterval(tick);
      go.disabled = false;
      listeners.splice(listeners.indexOf(done), 1);
      if (e !== 'exported') return say(`Export failed: ${r.error}`, true);
      progress.value = 1;
      const size = M._mf_export_size(app.session), data = M._mf_export_data(app.session);
      download(new Blob([M.HEAPU8.slice(data, data + size)], { type: 'video/mp4' }), 'export.mp4');
      say(`Exported ${(size / 1e6).toFixed(1)} MB in ${((performance.now() - started) / 1000).toFixed(1)} s`);
    };
    app.onEvent(done);
  };
  body.append(select, go, progress);
  hint('Exports run alongside the preview, faster than real time.');
}

// --- Recording ---

// The Record tab, as the macOS editor's camera window and voice-over: a camera recording (video and
// sound) goes in at the playhead where it started; a voice-over plays the timeline from the
// playhead while the microphone records, and goes on an audio track from there. Recordings are
// MP4s made in the page (record.js) and join the project's media like imported files.
const recorder = {
  stream: null,     // the camera preview's stream, while the Record tab shows it
  recording: null,  // the Recording under way
  mode: null,       // 'camera' | 'voice'
  atUs: 0,          // where the recording goes on the timeline
  count: { camera: 0, voice: 0 },

  warm: null,       // the next camera recording's video encoder, configured while the preview shows

  async openPreview() {
    if (!this.stream) this.stream = await navigator.mediaDevices.getUserMedia({ video: { width: 1280, height: 720 }, audio: true });
    if (!this.warm && 'VideoEncoder' in window) this.warm = warmVideoEncoder(this.stream.getVideoTracks()[0]);
    return this.stream;
  },
  closePreview() {  // a camera recording has its own clone of the stream, so it goes on
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = null;
    if (this.warm?.encoder.state !== 'closed') this.warm?.encoder.close();
    this.warm = null;
  },

  async start(mode) {
    if (this.recording || this.starting) return;
    this.starting = true;
    try {
      await this.begin(mode);
    } finally {
      this.starting = false;
    }
  },

  async begin(mode) {
    if (!('MediaStreamTrackProcessor' in window)) return say('Recording needs Chrome or Edge (MediaStreamTrackProcessor)', true);
    let stream;
    try {
      stream = mode === 'camera' ? (await this.openPreview()).clone()
                                 : await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true } });
    } catch (e) {
      return say(`No ${mode === 'camera' ? 'camera' : 'microphone'}: ${e.message}`, true);
    }
    this.mode = mode;
    this.atUs = app.playheadUs;
    let warm = null;
    if (mode === 'camera') {
      say('Getting the camera ready…');
      warm = await this.warm.ready;
      this.warm = null;
    }
    this.recording = new Recording(M, stream, warm);
    this.recording.start();
    this.startedAt = performance.now();
    if (mode === 'voice' && app.state === 'READY') {
      if (app.playheadUs >= app.durationUs() - 50000) {
        app.seek(0);
        this.atUs = 0;
      }
      M._mf_play(app.session);
    }
    say(mode === 'camera' ? 'Recording the camera…' : 'Recording a voice-over…');
    sidebar.render();
  },

  async stop() {
    const r = this.recording;
    if (!r) return;
    if (this.mode === 'voice' && app.state === 'PLAY') M._mf_pause(app.session);
    let file;
    try {
      file = await r.stop();
    } catch (e) {
      this.recording = null;
      sidebar.render();
      return say(`The recording failed: ${e.message}`, true);
    }
    this.recording = null;
    if (this.stream && this.mode === 'camera') this.openPreview();  // the next recording's encoder
    const kind = this.mode;
    const n = ++this.count[kind];
    const name = await importBytes(kind === 'camera' ? `Camera ${n}.mp4` : `Voice-over ${n}.m4a`, file.bytes, kind === 'camera' ? 'video/mp4' : 'audio/mp4');
    app.lastRecording = { name, ...file };  // for tests
    if (name) {
      app.addMedia(name, this.atUs);
      const dropped = file.dropped ? `, ${file.dropped} frame${file.dropped === 1 ? '' : 's'} dropped` : '';
      say(`Recorded ${name} (${fmt(app.media.get(name).lengthUs)}${dropped})`);
    }
    sidebar.render();
  },
};
app.recorder = recorder;

function recordPanel(body, hint) {
  const busy = recorder.recording;
  const video = Object.assign(document.createElement('video'), { muted: true, autoplay: true, playsInline: true });
  video.className = 'camera';
  const go = (text, onclick, primary) => {
    const b = Object.assign(document.createElement('button'), { textContent: text, onclick, disabled: busy && !primary });
    if (primary) b.className = 'primary';
    return b;
  };
  if (busy) {
    const elapsed = Object.assign(document.createElement('div'), { className: 'hint' });
    const tick = setInterval(() => {
      if (!elapsed.isConnected) return clearInterval(tick);
      elapsed.textContent = `● ${fmt((performance.now() - recorder.startedAt) * 1000)}${busy.dropped ? ` · ${busy.dropped} dropped` : ''}`;
    }, 200);
    if (recorder.mode === 'camera') { video.srcObject = recorder.stream; body.append(video); }
    body.append(go('■ Stop recording', () => recorder.stop(), true), elapsed);
    return;
  }
  body.append(video);
  recorder.openPreview().then((s) => { if (video.isConnected) video.srcObject = s; }, (e) => { video.remove(); hint(`No camera: ${e.message}`); });
  body.append(go('● Record camera', () => recorder.start('camera')), go('● Record voice-over', () => recorder.start('voice')));
  hint('A camera recording goes in at the playhead. A voice-over plays the timeline from the playhead while it records the microphone. Needs Chrome or Edge.');
}

// --- Wiring ---

const timeline = new Timeline(document.getElementById('timeline'), app);
const inspector = new Inspector(document.getElementById('props'), app);
app.timeline = timeline;
app.inspector = inspector;

const picker = document.getElementById('files');
document.getElementById('add-media').onclick = () => { picker.accept = 'video/mp4,audio/mp4,audio/x-m4a,.m4a,image/*'; picker.click(); };
document.getElementById('open').onclick = () => { picker.accept = '.json,video/mp4,audio/mp4,.m4a,image/*'; picker.click(); };
picker.onchange = () => { importFiles([...picker.files]); picker.value = ''; };
document.getElementById('save').onclick = save;
document.getElementById('new').onclick = () => {
  M._ed_delete(app.doc);
  app.doc = M._ed_new();
  app.playheadUs = 0;
  app.select({});
  app.commit();
};
document.getElementById('play').onclick = togglePlay;
document.getElementById('add-video-track').onclick = () => { if (M._ed_add_track(app.doc, 1) < 0) say('There are already 16 tracks', true); app.commit(); };
document.getElementById('add-audio-track').onclick = () => { if (M._ed_add_track(app.doc, 0) < 0) say('There are already 16 tracks', true); app.commit(); };
document.getElementById('zoom').oninput = (e) => { app.zoom = Number(e.target.value); timeline.render(); };

addEventListener('dragover', (e) => { e.preventDefault(); document.body.classList.add('dropping'); });
addEventListener('dragleave', (e) => { if (!e.relatedTarget) document.body.classList.remove('dropping'); });
addEventListener('drop', (e) => { e.preventDefault(); document.body.classList.remove('dropping'); importFiles([...e.dataTransfer.files]); });

addEventListener('keydown', (e) => {
  if (e.target.closest('input, textarea, select')) return;
  if (e.key === ' ') { e.preventDefault(); togglePlay(); }
  if (e.key === 'Delete' || e.key === 'Backspace') { e.preventDefault(); inspector.deleteSelection(); }
});

readScene();
sidebar.render();
app.commit();
say('Add media to start');
window.editor = app;  // for tests and the console
