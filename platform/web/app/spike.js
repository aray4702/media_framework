// The web platform's test page: the core in WebAssembly playing a scene in the browser, on its own
// thread (the canvas is transferred to it), with its metrics. URL parameters: scene (single, stacked2, stacked4, sync, features), seconds (how long to play),
// autoplay=1 (start without a click: needs Chrome's --autoplay-policy=no-user-gesture-required).
// The result is shown on the page and left in window.spikeResult for tools/run_spike.mjs.

const params = new URLSearchParams(location.search);
const sceneName = params.get('scene') || 'stacked2';
const seconds = Number(params.get('seconds') || 8);
const status = document.getElementById('status');

// Scenes: 1280x720 output, the clips from scripts/make_clips.sh.
const output = { width: 1280, height: 720, fps: 30, sampleRate: 48000, channels: 2 };
const video = (src, extra = {}) => ({ type: 'video', src, start: 0, duration: seconds, ...extra });
const track = (item) => ({ kind: 'video', items: [item] });
const quarter = (src, x, y) => video(src, { transform: { x, y, scale: 0.5 }, audio: { mute: src !== '1080p30' } });
const scenes = {
  single: { clips: ['1080p30'], tracks: [track(video('1080p30'))] },
  sync: { clips: ['sync_flash_beep'], tracks: [track(video('sync_flash_beep'))] },
  corrupt: { clips: ['corrupt_mdat'], tracks: [track(video('corrupt_mdat'))] },  // 200 bytes damaged mid-file
  stacked2: {
    clips: ['1080p30', '720p24'],
    tracks: [track(video('1080p30')), track(video('720p24', { transform: { x: 0.8, y: 0.2, scale: 0.3 }, audio: { mute: true } }))],
  },
  // The compositor's features: a crossfade and a cropped, desaturated clip under an image (rotated,
  // blurred, screen blend), a chroma-keyed green square that should vanish, a wipe between two
  // colors, and a caption on a box.
  features: {
    clips: ['1080p30', '720p24'],
    images: ['image'],
    tracks: [
      { kind: 'video', items: [
        video('1080p30', { duration: 3.5, effects: [{ type: 'colorAdjust', saturation: 0.2 }] }),
        { type: 'transition', kind: 'crossfade', duration: 1 },
        video('720p24', { start: 2.5, duration: 3.5, effects: [{ type: 'crop', left: 0.1, right: 0.1 }], audio: { mute: true } })] },
      track({ type: 'image', src: 'image', start: 0, duration: 6, transform: { x: 0.82, y: 0.25, scale: 0.3, rotation: 15 },
              blend: 'screen', effects: [{ type: 'blur', radius: 0.004 }] }),
      track({ type: 'color', color: '#00ff00', start: 0, duration: 6, transform: { x: 0.15, y: 0.2, scale: 0.2 },
              effects: [{ type: 'chromaKey', color: '#00ff00' }] }),
      { kind: 'video', items: [
        { type: 'color', color: '#3060ff', start: 0, duration: 2, transform: { x: 0.15, y: 0.75, scale: 0.25 } },
        { type: 'transition', kind: 'wipe', direction: 'right', duration: 1 },
        { type: 'color', color: '#ff8000', start: 1, duration: 5, transform: { x: 0.15, y: 0.75, scale: 0.25 } }] },
      track({ type: 'text', text: 'Hello from WebGPU', start: 0, duration: 6, style: { box: '#0000008c', size: 0.07 },
              transform: { y: 0.95, anchor: [0.5, 1] } }),
    ],
  },
  stacked4: {
    clips: ['1080p30', '720p24', '1080p60', 'gop4s'],
    tracks: [track(quarter('1080p30', 0.25, 0.25)), track(quarter('720p24', 0.75, 0.25)),
             track(quarter('1080p60', 0.25, 0.75)), track(quarter('gop4s', 0.75, 0.75))],
  },
};
const scene = scenes[sceneName];

async function main() {
  if (!crossOriginIsolated) throw new Error('not cross-origin isolated: serve with tools/serve.py (COOP/COEP headers)');
  // What the run measured: a real GPU or a software one, and whether H.264 can decode in hardware.
  // (The player's thread requests its own device.)
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) throw new Error('no WebGPU adapter');
  const environment = { gpu: `${adapter.info?.vendor || '?'} ${adapter.info?.architecture || ''}`.trim(),
    hardwareH264: (await VideoDecoder.isConfigSupported({ codec: 'avc1.640028', hardwareAcceleration: 'prefer-hardware' })).supported,
    userAgent: navigator.userAgent };
  const M = await createMediaFramework();
  const cstr = (s) => M.stringToNewUTF8(s);
  const session = M._mf_create(cstr('#view'));
  if (!session) throw new Error('could not start the player thread');

  status.textContent = 'Loading clips…';
  for (const name of scene.clips) {
    const bytes = new Uint8Array(await (await fetch(`/clips/${name}.mp4`)).arrayBuffer());
    const ptr = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, ptr);
    M._mf_add_source(session, cstr(name), ptr, bytes.length);
    M._free(ptr);
  }
  for (const name of scene.images || []) {  // decoded on the player's thread before the scene opens
    const bytes = new Uint8Array(await (await fetch(`/clips/${name}.png`)).arrayBuffer());
    const ptr = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, ptr);
    M._mf_add_image(session, cstr(name), ptr, bytes.length);
    M._free(ptr);
  }
  const doc = JSON.stringify({ version: 1, output, tracks: scene.tracks });
  const report = () => JSON.parse(M.UTF8ToString(M._mf_report(session)));
  M._mf_open(session, cstr(doc), 0);

  const openedAt = performance.now();
  for (;;) {
    const r = report();
    if (r.state === 'READY') break;
    if (r.state === 'ERROR' || r.events.some((e) => e.startsWith('openFailed'))) throw new Error('open failed: ' + r.error);
    if (performance.now() - openedAt > 10000) throw new Error('not ready after 10 s: ' + JSON.stringify(r));
    await new Promise((res) => setTimeout(res, 20));
  }
  status.textContent = 'Ready';
  if (params.get('autoplay') !== '1') await new Promise((res) => document.getElementById('play').addEventListener('click', res, { once: true }));

  // The page's own refresh intervals while playing, to tell the player's jank from the page's.
  const intervals = [];
  let last = 0, playing = true;
  const tick = (t) => { if (last) intervals.push(t - last); last = t; if (playing) requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
  M._mf_play(session);
  status.textContent = 'Playing';
  await new Promise((res) => setTimeout(res, (seconds + 1.5) * 1000));
  await new Promise((res) => setTimeout(res, 100));  // the report is refreshed every 50 ms
  playing = false;

  intervals.sort((a, b) => a - b);
  const final = report();
  const result = { scene: sceneName, seconds, environment, ...final, gpu: final.frames,
                   rafIntervalP50Ms: intervals[intervals.length >> 1], rafIntervalP99Ms: intervals[Math.floor(intervals.length * 0.99)] };
  document.getElementById('report').textContent = JSON.stringify(result, null, 2);
  status.textContent = result.state;
  window.spikeResult = result;
}

main().catch((e) => {
  status.textContent = 'Failed';
  document.getElementById('report').textContent = String(e.stack || e);
  window.spikeResult = { scene: sceneName, failed: String(e.message || e) };
});
