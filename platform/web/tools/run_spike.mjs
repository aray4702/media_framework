// Runs the web platform's test page in Chrome and prints each scene's result as JSON.
// Usage: node run_spike.mjs [scene...] (default: single sync stacked2 stacked4), with
// tools/serve.py running on port 8000. CHROME=path overrides the browser; HEADLESS=0 shows it.
// EXPORT=dir exports each scene instead of playing it, saving <dir>/<scene>.mp4.
import { writeFileSync } from 'node:fs';
import puppeteer from 'puppeteer-core';

const chrome = process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const scenes = process.argv.slice(2).length ? process.argv.slice(2) : ['single', 'sync', 'stacked2', 'stacked4'];
const seconds = Number(process.env.SECONDS || 8);

const browser = await puppeteer.launch({
  executablePath: chrome,
  headless: process.env.HEADLESS !== '0',
  args: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', '--window-size=1100,900'],
});
for (const scene of scenes) {
  const page = await browser.newPage();
  const logs = [];
  page.on('console', (m) => logs.push(`${m.type()}: ${m.text()}`));
  page.on('pageerror', (e) => logs.push(`pageerror: ${e.message}`));
  page.on('response', (r) => { if (r.status() >= 400) logs.push(`http ${r.status()}: ${r.url()}`); });
  const mode = process.env.EXPORT ? '&export=1' : '';
  await page.goto(`http://127.0.0.1:8000/platform/web/app/?scene=${scene}&seconds=${seconds}&autoplay=1${mode}`);
  // SCREENSHOT=dir saves <dir>/<scene>-<t>.png at each of SHOTS (seconds into playback; default
  // halfway through).
  if (process.env.SCREENSHOT) {
    await page.waitForFunction(() => document.getElementById('status').textContent === 'Playing', { timeout: 30000 }).catch(() => {});
    const start = Date.now();
    for (const t of (process.env.SHOTS || String(seconds / 2)).split(',').map(Number)) {
      await new Promise((r) => setTimeout(r, Math.max(0, start + t * 1000 - Date.now())));
      await page.screenshot({ path: `${process.env.SCREENSHOT}/${scene}-${t}.png`, clip: { x: 0, y: 0, width: 1000, height: 560 } });
    }
  }
  await page.waitForFunction(() => window.spikeResult, { timeout: (seconds + 30) * 1000 }).catch(() => {});
  const result = await page.evaluate(() => window.spikeResult || null);
  if (process.env.EXPORT && result && !result.failed) {
    const b64 = await page.evaluate(() => {
      let s = '';
      const bytes = window.exportBytes;
      for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
      return btoa(s);
    });
    writeFileSync(`${process.env.EXPORT}/${scene}.mp4`, Buffer.from(b64, 'base64'));
    // The scene itself, its sources as the clips' paths, for export_reference (the macOS export).
    const doc = JSON.parse(await page.evaluate(() => window.sceneDoc));
    const clips = new URL('../../../clips/', import.meta.url).pathname;
    for (const t of doc.tracks) for (const i of t.items) if (i.src) i.src = `${clips}${i.src}.${i.type === 'image' ? 'png' : 'mp4'}`;
    writeFileSync(`${process.env.EXPORT}/${scene}.json`, JSON.stringify(doc, null, 1));
  }
  console.log(JSON.stringify({ ...(result || { scene, failed: 'no result' }), logs: logs.slice(0, 20) }));
  await page.close();
}
await browser.close();
