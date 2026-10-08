// The timeline: a ruler, then one row per track with the top layer first (audio rows tinted), each
// item a block at its start and duration, a diamond where two items meet that a transition can
// join, and the playhead. As in the macOS editor: click an item to select it, the empty part of a
// row to select the track and move the playhead there, a diamond to select that join. Drag an item
// to move it, along its track or onto another that takes it; drag its ends to trim it (the start
// trims into the file). Dragging in the ruler scrubs.

const kHead = 120;  // the row headers' width

export class Timeline {
  constructor(el, app) {
    this.el = el;
    this.app = app;
  }

  x(us) { return (us / 1e6) * this.app.zoom; }
  usAt(clientX) {
    const lane = this.el.querySelector('.ruler').getBoundingClientRect();
    return Math.max(0, Math.round(((clientX - lane.left) / this.app.zoom) * 1e6));
  }

  render() {
    const { app } = this, scene = app.scene, us = app.us;
    const width = Math.max(this.el.clientWidth - kHead, this.x(app.durationUs()) + 400);
    this.el.replaceChildren();

    const ruler = Object.assign(document.createElement('div'), { className: 'ruler' });
    ruler.style.width = `${width}px`;
    const step = app.zoom >= 160 ? 0.5 : app.zoom >= 60 ? 1 : app.zoom >= 30 ? 2 : 5;
    for (let t = 0; this.x(t * 1e6) < width; t += step) {
      const mark = Object.assign(document.createElement('span'), { textContent: app.fmt(t * 1e6).replace(/\.\d+$/, '') });
      mark.style.left = `${this.x(t * 1e6)}px`;
      ruler.append(mark);
    }
    this.scrub(ruler);
    this.el.append(ruler);

    // Top layer first: video tracks from the last, then the audio tracks.
    const order = scene.tracks.map((t, i) => i);
    order.sort((a, b) => (scene.tracks[a].kind === scene.tracks[b].kind ? 0 : scene.tracks[a].kind === 'video' ? -1 : 1) || b - a);
    for (const t of order) {
      const track = scene.tracks[t];
      const row = Object.assign(document.createElement('div'), { className: `row ${track.kind}` });
      row.dataset.track = t;
      row.classList.toggle('selected', app.sel.track === t && app.sel.item < 0);
      const head = Object.assign(document.createElement('div'), { className: 'head' });
      const label = Object.assign(document.createElement('span'), {
        textContent: `${track.kind === 'video' ? 'Video' : 'Audio'} ${t + 1}${track.enabled === false ? ' (off)' : ''}` });
      head.append(label);
      head.onclick = () => app.select({ track: t });
      const lane = Object.assign(document.createElement('div'), { className: 'lane' });
      lane.style.width = `${width}px`;
      lane.onpointerdown = (e) => {
        if (e.target !== lane) return;
        app.select({ track: t });
        app.seek(this.usAt(e.clientX));
      };
      track.items.forEach((item, k) => lane.append(this.block(t, k, item)));
      track.items.forEach((item, k) => {
        if (k === 0 || !app.ed._ed_junction(app.doc, t, k)) return;
        const join = Object.assign(document.createElement('button'), { className: 'join', title: 'Transition' });
        join.classList.toggle('has', this.transitionInto(track, k) !== null);
        join.classList.toggle('selected', app.sel.transition && app.sel.track === t && app.sel.item === k);
        join.style.left = `${this.x(us(item.start))}px`;
        join.onclick = (e) => { e.stopPropagation(); app.select({ track: t, item: k, transition: true }); };
        lane.append(join);
      });
      row.append(head, lane);
      this.el.append(row);
    }
    this.playhead = Object.assign(document.createElement('div'), { className: 'playhead' });
    this.el.append(this.playhead);
    this.setPlayhead();
  }

  // The transition into item k (editor.js's readScene gives each one the item it follows).
  transitionInto(track, k) {
    return track.transitions.find((x) => x.from === k - 1) || null;
  }

  setPlayhead() {
    if (!this.playhead) return;
    this.playhead.style.left = `${kHead + this.x(this.app.playheadUs)}px`;
    this.playhead.style.height = `${this.el.scrollHeight}px`;
  }

  scrub(ruler) {
    ruler.onpointerdown = (e) => {
      ruler.setPointerCapture(e.pointerId);
      this.app.seek(this.usAt(e.clientX));
      ruler.onpointermove = (m) => this.app.seek(this.usAt(m.clientX));
      ruler.onpointerup = () => { ruler.onpointermove = ruler.onpointerup = null; };
    };
  }

  block(t, k, item) {
    const { app } = this, us = app.us;
    const kind = item.type === 'audio' ? 'audio' : item.type === 'video' ? 'video' : 'visual';
    const el = Object.assign(document.createElement('div'), { className: `item ${kind}` });
    el.classList.toggle('selected', !app.sel.transition && app.sel.track === t && app.sel.item === k);
    const startUs = us(item.start), durUs = us(item.duration);
    el.style.left = `${this.x(startUs)}px`;
    el.style.width = `${Math.max(4, this.x(durUs))}px`;
    el.textContent = item.type === 'text' ? item.text : item.type === 'color' ? item.color : item.src;
    el.title = `${item.type}: ${el.textContent}`;
    const start = Object.assign(document.createElement('div'), { className: 'trim start' });
    const end = Object.assign(document.createElement('div'), { className: 'trim end' });
    el.append(start, end);

    el.onpointerdown = (e) => {
      e.stopPropagation();
      const mode = e.target === start ? 'start' : e.target === end ? 'end' : 'move';
      const target = el;
      target.setPointerCapture(e.pointerId);
      if (!(app.sel.track === t && app.sel.item === k && !app.sel.transition)) {  // selected, without redrawing the item being dragged
        app.sel = { track: t, item: k, transition: false };
        this.el.querySelectorAll('.item.selected').forEach((i) => i.classList.remove('selected'));
        el.classList.add('selected');
        app.inspector.render();
      }
      const x0 = e.clientX;
      let dx = 0, to = t;
      target.onpointermove = (m) => {
        dx = m.clientX - x0;
        const d = (dx / app.zoom) * 1e6;
        if (mode === 'move') {
          target.style.left = `${this.x(Math.max(0, startUs + d))}px`;
          const row = document.elementFromPoint(m.clientX, m.clientY)?.closest('.row');
          to = row ? Number(row.dataset.track) : t;
          target.style.opacity = to !== t ? '0.6' : '';
        } else if (mode === 'end') {
          target.style.width = `${Math.max(4, this.x(Math.max(100000, durUs + d)))}px`;
        } else {
          const nd = Math.max(100000, durUs - d);
          target.style.left = `${this.x(startUs + durUs - nd)}px`;
          target.style.width = `${Math.max(4, this.x(nd))}px`;
        }
      };
      target.onpointerup = () => {
        target.onpointermove = target.onpointerup = null;
        if (Math.abs(dx) < 3 && to === t) return app.timeline.render();  // a click: selected
        const d = Math.round((dx / app.zoom) * 1e6), M = app.ed;
        if (mode === 'move' && to !== t) {
          const r = M._ed_move_to_track(app.doc, t, k, to, Math.max(0, startUs + d));
          if (r < 0) app.say('That track doesn\'t take this item', true);
          else app.sel = { track: r >> 16, item: r & 0xffff, transition: false };
        } else if (mode === 'move') {
          M._ed_move_item(app.doc, t, k, Math.max(0, startUs + d));
          app.sel = { track: t, item: this.indexOf(t, item.id), transition: false };
        } else if (mode === 'end') {
          M._ed_set_duration(app.doc, t, k, Math.max(100000, durUs + d));
        } else {
          M._ed_trim_start(app.doc, t, k, Math.max(100000, durUs - d));
        }
        app.commit();
      };
    };
    return el;
  }

  // After a move, the item's index on its track by its id (items stay in start order).
  indexOf(t, id) {
    const s = JSON.parse(this.app.ed.UTF8ToString(this.app.ed._ed_scene(this.app.doc)));
    return s.tracks[t].items.findIndex((i) => i.id === id);
  }
}
