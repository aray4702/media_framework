// Records with Chrome's fake camera and microphone in the web editor: a camera recording at the
// playhead, then a voice-over over the timeline, checking each lands on the timeline as a playable
// item of about the recorded length. RECORDINGS=dir saves the files. Needs tools/serve.py on port
// 8000 and the clips (scripts/make_clips.sh).
import { writeFileSync } from 'node:fs';
import puppeteer from 'puppeteer-core';

const chrome = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const clips = new URL('../../../clips/', import.meta.url).pathname;
const browser = await puppeteer.launch({ executablePath: chrome, headless: process.env.HEADLESS !== '0',
  args: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', '--window-size=1400,900',
         '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'],
  defaultViewport: { width: 1400, height: 900 } });
const page = await browser.newPage();
const logs = [];
page.on('console', (m) => { if (m.type() === 'error' || m.type() === 'warn') logs.push(`${m.type()}: ${m.text()}`); });
page.on('pageerror', (e) => logs.push(`pageerror: ${e.message}`));

let failures = 0;
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) ++failures; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const scene = () => page.evaluate(() => window.editor.scene);
const report = () => page.evaluate(() => window.editor.report);
const waitState = async (states, ms = 8000) => {
  const end = Date.now() + ms;
  while (Date.now() < end) { const r = await report(); if (r && states.includes(r.state)) return r.state; await sleep(50); }
  return (await report())?.state;
};
const clickButton = (text) => page.evaluate((t) => [...document.querySelectorAll('#tab-body button')].find((b) => b.textContent.includes(t)).click(), text);
const items = async () => (await scene()).tracks.flatMap((t, track) => t.items.map((i) => ({ ...i, track, kind: t.kind })));
const save = async (name) => {
  if (!process.env.RECORDINGS) return;
  const b64 = await page.evaluate(() => {
    const bytes = window.editor.lastRecording.bytes;
    let str = '';
    for (let i = 0; i < bytes.length; i += 0x8000) str += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(str);
  });
  writeFileSync(`${process.env.RECORDINGS}/${name}`, Buffer.from(b64, 'base64'));
};
const lastRecording = () => page.evaluate(() => ({ ...window.editor.lastRecording, bytes: window.editor.lastRecording.bytes.length }));

await page.goto('http://127.0.0.1:8000/platform/web/editor/');
await page.waitForFunction(() => window.editor, { timeout: 15000 });

// 1. A clip to record over.
await (await page.$('#files')).uploadFile(`${clips}1080p30.mp4`);
await page.waitForFunction(() => window.editor.media.size === 1, { timeout: 15000 });
await page.evaluate(() => [...document.querySelectorAll('.media')][0].click());
check(await waitState(['READY']) === 'READY', 'a clip on the timeline');

// 2. The camera, 3 s, at 2 s.
await page.evaluate(() => window.editor.seek(2e6));
await page.evaluate(() => [...document.querySelectorAll('#tabs button')].find((b) => b.textContent === 'Record').click());
await page.waitForFunction(() => document.querySelector('video.camera')?.videoWidth > 0, { timeout: 10000 });
check(true, 'the camera preview shows');
await clickButton('Record camera');
await sleep(3000);
await clickButton('Stop recording');
await page.waitForFunction(() => window.editor.media.has('Camera 1.mp4'), { timeout: 10000 });
let rec = await lastRecording();
await save('camera.mp4');
let m = await page.evaluate(() => window.editor.media.get('Camera 1.mp4'));
console.log(`     camera: ${rec.frames} frames, ${rec.dropped} dropped, ${(rec.bytes / 1e6).toFixed(2)} MB, ${m.width}×${m.height}, ${(m.lengthUs / 1e6).toFixed(2)} s`);
check(m.kind === 'video' && Math.abs(m.lengthUs - 3e6) < 0.4e6, 'Camera 1.mp4 is a video of about 3 s'); check(m.audio, 'with sound');
let cam = (await items()).find((i) => i.src === 'Camera 1.mp4');
check(cam && Math.abs(cam.start - 2) < 1e-6 && cam.kind === 'video', 'it is on a video track at 2 s');
check(rec.dropped === 0, `no frames dropped (${rec.dropped} of ${rec.frames + rec.dropped})`);

// 3. A voice-over, 2 s, from 1 s: the timeline plays meanwhile.
check(await waitState(['READY']) === 'READY', 'the player reopened with the recording');
await page.evaluate(() => window.editor.seek(1e6));
await clickButton('Record voice-over');
check(await waitState(['PLAY'], 3000) === 'PLAY', 'the timeline plays during the voice-over');
await sleep(2000);
await clickButton('Stop recording');
await page.waitForFunction(() => window.editor.media.has('Voice-over 1.m4a'), { timeout: 10000 });
rec = await lastRecording();
await save('voice-over.m4a');
m = await page.evaluate(() => window.editor.media.get('Voice-over 1.m4a'));
console.log(`     voice-over: ${(rec.bytes / 1e3).toFixed(0)} kB, ${(m.lengthUs / 1e6).toFixed(2)} s`);
check(m.kind === 'audio' && Math.abs(m.lengthUs - 2e6) < 0.4e6, 'Voice-over 1.m4a is audio of about 2 s');
const vo = (await items()).find((i) => i.src === 'Voice-over 1.m4a');
check(vo && Math.abs(vo.start - 1) < 1e-6 && vo.kind === 'audio', 'it is on an audio track at 1 s');
check(await waitState(['READY'], 2000) === 'READY', 'the timeline stopped with the recording');

// 4. Play across both recordings.
check(await waitState(['READY']) === 'READY', 'the player reopened with the voice-over');
await page.evaluate(() => window.editor.seek(0.5e6));
await sleep(300);
await page.click('#play');
check(await waitState(['PLAY']) === 'PLAY', 'playing');
await sleep(4500);
await page.click('#play');
const r = await report();
check(r.positionUs > 4.5e6, `the playhead moved on (${(r.positionUs / 1e6).toFixed(2)} s)`);
check(r.metrics.lateDrops === 0 && r.metrics.janks === 0 && !r.error, `no late frames, jank or errors (late ${r.metrics.lateDrops}, janks ${r.metrics.janks})`);

console.log(failures ? `${failures} FAILED` : 'OK');
if (logs.length) console.log('console:', logs.slice(0, 10));
await browser.close();
process.exit(failures ? 1 : 0);
