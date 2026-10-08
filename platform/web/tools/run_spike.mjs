// Runs the step 1 spike in Chrome and prints each scene's result as JSON.
// Usage: node run_spike.mjs [scene...] (default: single sync stacked2 stacked4), with
// tools/serve.py running on port 8000. CHROME=path overrides the browser; HEADLESS=0 shows it.
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
  await page.goto(`http://127.0.0.1:8000/platform/web/app/?scene=${scene}&seconds=${seconds}&autoplay=1`);
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
  console.log(JSON.stringify({ ...(result || { scene, failed: 'no result' }), logs: logs.slice(0, 20) }));
  await page.close();
}
await browser.close();
