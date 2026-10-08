// Step 1 spike: the core in WebAssembly playing a scene of 1 to 4 lanes in the browser, with its
// metrics. URL parameters: scene (single, stacked2, stacked4, sync), seconds (how long to play),
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
  stacked2: {
    clips: ['1080p30', '720p24'],
    tracks: [track(video('1080p30')), track(video('720p24', { transform: { x: 0.8, y: 0.2, scale: 0.3 }, audio: { mute: true } }))],
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
  const adapter = await navigator.gpu?.requestAdapter();
  if (!adapter) throw new Error('no WebGPU adapter');
  const device = await adapter.requestDevice();
  // What the run measured: a real GPU or a software one, and whether H.264 can decode in hardware.
  const environment = { gpu: `${adapter.info?.vendor || '?'} ${adapter.info?.architecture || ''}`.trim(),
    hardwareH264: (await VideoDecoder.isConfigSupported({ codec: 'avc1.640028', hardwareAcceleration: 'prefer-hardware' })).supported,
    userAgent: navigator.userAgent };
  const M = await createMediaFramework({ mfGpuDevice: device });
  const session = M._mf_create();
  const cstr = (s) => M.stringToNewUTF8(s);

  status.textContent = 'Loading clips…';
  for (const name of scene.clips) {
    const bytes = new Uint8Array(await (await fetch(`/clips/${name}.mp4`)).arrayBuffer());
    const ptr = M._malloc(bytes.length);
    M.HEAPU8.set(bytes, ptr);
    M._mf_add_source(session, cstr(name), ptr, bytes.length);
    M._free(ptr);
  }
  const doc = JSON.stringify({ version: 1, output, tracks: scene.tracks });
  const report = () => JSON.parse(M.UTF8ToString(M._mf_report(session)));
  const r = M._mf_open(session, cstr(doc), cstr('#view'), 0);
  if (r !== 0) throw new Error('open failed: ' + M.UTF8ToString(M._mf_error(session)));

  const openedAt = performance.now();
  while (report().state !== 'READY') {
    if (report().state === 'ERROR') throw new Error(report().error);
    if (performance.now() - openedAt > 10000) throw new Error('not ready after 10 s: ' + JSON.stringify(report()));
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
  playing = false;

  intervals.sort((a, b) => a - b);
  const result = { scene: sceneName, seconds, environment, ...report(), gpu: M.mfStats(),
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
