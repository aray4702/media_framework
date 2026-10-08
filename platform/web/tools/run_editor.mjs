// Drives the web editor in Chrome the way a person would, checking the Document's scene and the
// player at each step: import media, add items, edit properties, drag an item, add a transition,
// play, export, save. Prints each step and a summary; SCREENSHOT=dir saves pictures along the way.
// Needs tools/serve.py on port 8000 and the clips (scripts/make_clips.sh).
import { writeFileSync } from 'node:fs';
import puppeteer from 'puppeteer-core';

const chrome = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const clips = new URL('../../../clips/', import.meta.url).pathname;
const shots = process.env.SCREENSHOT;
const browser = await puppeteer.launch({ executablePath: chrome, headless: process.env.HEADLESS !== '0',
  args: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', '--window-size=1400,900'],
  defaultViewport: { width: 1400, height: 900 } });
const page = await browser.newPage();
const logs = [];
page.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warn') logs.push(`${m.type()}: ${m.text()}`); });
page.on('pageerror', (e) => logs.push(`pageerror: ${e.message}`));

let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) ++failures; };
const scene = () => page.evaluate(() => window.editor.scene);
const report = () => page.evaluate(() => window.editor.report);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const shot = async (name) => { if (shots) await page.screenshot({ path: `${shots}/editor-${name}.png` }); };
const waitState = async (states, ms = 8000) => {
  const end = Date.now() + ms;
  while (Date.now() < end) { const r = await report(); if (r && states.includes(r.state)) return r.state; await sleep(50); }
  return (await report())?.state;
};

await page.goto('http://127.0.0.1:8000/platform/web/editor/');
await page.waitForFunction(() => window.editor, { timeout: 15000 });

// 1. Import two clips and an image.
const input = await page.$('#files');
await input.uploadFile(`${clips}1080p30.mp4`, `${clips}720p24.mp4`, `${clips}image.png`);
await page.waitForFunction(() => window.editor.media.size === 3, { timeout: 15000 });
check(true, 'imported 1080p30.mp4, 720p24.mp4 and image.png');

// 2. Add both clips: the second goes right after the first, on the same track.
const clickMedia = (name) => page.evaluate((n) => [...document.querySelectorAll('.media')].find((m) => m.textContent.includes(n)).click(), name);
await clickMedia('1080p30');
await page.evaluate(() => window.editor.seek(10e6));
await clickMedia('720p24');
let s = await scene();
const videoTrack = s.tracks.findIndex((t) => t.items.some((i) => i.src === '1080p30.mp4'));
check(s.tracks[videoTrack].items.map((i) => i.src).join(',') === '1080p30.mp4,720p24.mp4', 'both clips on one video track, in order');
check(await waitState(['READY']) === 'READY', 'the player opened the scene');

// 3. A caption and the image over the video, at 2 s.
await page.evaluate(() => window.editor.seek(2e6));
await page.evaluate(() => { document.querySelector('#tabs button:nth-child(2)').click(); });
await page.evaluate(() => [...document.querySelectorAll('#tab-body button')].find((b) => b.textContent.includes('caption')).click());
await page.evaluate(() => { document.querySelector('#tabs button:nth-child(1)').click(); });
await clickMedia('image.png');
s = await scene();
const caption = s.tracks.flatMap((t) => t.items).find((i) => i.type === 'text');
const image = s.tracks.flatMap((t) => t.items).find((i) => i.type === 'image');
check(caption && Math.abs(caption.start - 2) < 1e-6, 'caption added at the playhead');
check(image && s.tracks.findIndex((t) => t.items.includes(image)) > videoTrack, 'image added on a layer above the video');

// 4. Properties: the image smaller, rotated, to the top right (look only: no reopen).
const imageTrack = s.tracks.findIndex((t) => t.items.some((i) => i.type === 'image'));
await page.evaluate((t) => window.editor.select({ track: t, item: 0 }), imageTrack);
const setSlider = (label, v) => page.evaluate((l, val) => {
  const row = [...document.querySelectorAll('#props .field')].find((f) => f.firstChild.textContent === l);
  const input = row.querySelector('input[type=range]');
  input.value = val;
  input.dispatchEvent(new Event('input'));
}, label, v);
await setSlider('Scale', 0.3);
await setSlider('X', 0.8);
await setSlider('Y', 0.25);
await setSlider('Rotation', 12);
s = await scene();
const tr = s.tracks[imageTrack].items[0].transform;
check(tr && tr.scale === 0.3 && tr.x === 0.8 && tr.y === 0.25 && tr.rotation === 12, 'image transform set from the properties');
await sleep(300);
await shot('properties');

// 5. Drag the second clip later by 100 px on the timeline: its start moves by 100 px / zoom.
const zoom = await page.evaluate(() => window.editor.zoom);
const box = await page.evaluate((t) => {
  const r = document.querySelector(`.row[data-track="${t}"] .item:nth-of-type(2)`).getBoundingClientRect();
  return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
}, videoTrack);
const before = (await scene()).tracks[videoTrack].items[1].start;
await page.mouse.move(box.x, box.y);
await page.mouse.down();
await page.mouse.move(box.x + 50, box.y, { steps: 4 });
await page.mouse.move(box.x + 100, box.y, { steps: 4 });
await page.mouse.up();
s = await scene();
const moved = s.tracks[videoTrack].items[1].start - before;
check(Math.abs(moved - 100 / zoom) < 0.05, `dragging the clip 100 px moved it ${moved.toFixed(2)} s (expected ${(100 / zoom).toFixed(2)})`);

// 6. Put it back against the first, then a crossfade between them, from the join's diamond.
await page.evaluate((t) => { const e = window.editor; e.ed._ed_move_item(e.doc, t, 1, 10e6); e.commit(); }, videoTrack);
await page.waitForSelector('.join');
await page.click('.join');
await page.select('#props select', 'crossfade');
s = await scene();
const x = (s.tracks[videoTrack].transitions || [])[0];
check(x && x.kind === 'crossfade' && Math.abs(x.duration - 1) < 1e-6, 'a 1 s crossfade joins the clips');
check(Math.abs(s.tracks[videoTrack].items[1].start - 9) < 1e-6, 'the second clip now starts 1 s earlier, overlapping the first');

// 7. Play across the crossfade, from 8 s.
await page.evaluate(() => window.editor.select({}));
check(await waitState(['READY']) === 'READY', 'the player reopened the edited scene');
await page.evaluate(() => window.editor.seek(8e6));
await sleep(500);
await page.click('#play');
check(await waitState(['PLAY']) === 'PLAY', 'playing');
await sleep(1200);
await shot('crossfade');
await sleep(1000);
await page.click('#play');
const r = await report();
check(r.positionUs > 9.5e6, `the playhead moved on while playing (${(r.positionUs / 1e6).toFixed(2)} s)`);
check(r.metrics.lateDrops === 0 && r.metrics.janks === 0, `no late frames or jank (late ${r.metrics.lateDrops}, janks ${r.metrics.janks})`);

// 8. Export, at the project size.
await page.evaluate(() => { [...document.querySelectorAll('#tabs button')].find((b) => b.textContent === 'Export').click(); });
const exported = page.evaluate(() => new Promise((res) => window.editor.onEvent((e, rep) => {
  if (e === 'exported' || e.startsWith('exportFailed')) res({ e, bytes: rep.export?.bytes, ms: rep.export?.ms, error: rep.error });
})));
await page.evaluate(() => [...document.querySelectorAll('#tab-body button')].find((b) => b.textContent === 'Export MP4').click());
const ex = await exported;
check(ex.e === 'exported' && ex.bytes > 100000, `exported ${(ex.bytes / 1e6).toFixed(1)} MB in ${(ex.ms / 1000).toFixed(1)} s`);
if (process.env.EXPORT) {
  const b64 = await page.evaluate(() => {
    const M = window.editor.ed, size = M._mf_export_size(window.editor.session), data = M._mf_export_data(window.editor.session);
    const bytes = M.HEAPU8.slice(data, data + size);
    let str = '';
    for (let i = 0; i < bytes.length; i += 0x8000) str += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(str);
  });
  writeFileSync(`${process.env.EXPORT}/editor.mp4`, Buffer.from(b64, 'base64'));
  writeFileSync(`${process.env.EXPORT}/editor.json`, await page.evaluate(() => window.editor.sceneJson));  // the document as the editor has it
}
await shot('export');

// 9. Save, start a new project, and open the saved one with its media: the same scene comes back.
const saved = await page.evaluate(() => window.editor.sceneJson);
const savedPath = `${process.env.TMPDIR || '/tmp'}/mf-editor-project.json`;
writeFileSync(savedPath, saved);
await page.click('#new');
check((await scene()).tracks.every((t) => t.items.length === 0), 'New empties the timeline');
await input.uploadFile(savedPath, `${clips}1080p30.mp4`, `${clips}720p24.mp4`, `${clips}image.png`);
await page.waitForFunction((n) => window.editor.sceneJson && JSON.parse(window.editor.sceneJson).tracks.some((t) => t.items.length), { timeout: 15000 });
const reopened = JSON.parse(await page.evaluate(() => window.editor.sceneJson));
const withoutMeta = (d) => { const c = JSON.parse(JSON.stringify(d)); delete c.metadata; return JSON.stringify(c); };
check(withoutMeta(reopened) === withoutMeta(JSON.parse(saved)), 'the saved project opens as it was');
check(await waitState(['READY']) === 'READY', 'the player opened the reopened project');

console.log(failures ? `${failures} FAILED` : 'OK');
if (logs.length) console.log('console:', logs.slice(0, 10));
await browser.close();
process.exit(failures ? 1 : 0);
