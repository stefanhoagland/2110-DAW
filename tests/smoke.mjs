// Browser smoke test using Chromium's fake microphone: enable audio,
// record two seconds, stop, and check the take landed on the timeline.
import { chromium } from 'playwright';
import { serve } from './serve.mjs';

// Set BASE_URL to test a running copy instead, e.g. the Docker container.
const base = process.env.BASE_URL || 'http://localhost:8111/';
const server = process.env.BASE_URL ? null : await serve(8111);
const browser = await chromium.launch({
  args: ['--use-fake-ui-for-media-stream', '--use-fake-device-for-media-stream', '--autoplay-policy=no-user-gesture-required'],
});
let failed = false;
try {
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 }, ignoreHTTPSErrors: true });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  await page.goto(base);
  await page.click('#btn-enable');
  await page.waitForFunction(() => window.daw.state.open, null, { timeout: 10000 });
  const info = await page.textContent('#device-info');
  await page.waitForTimeout(500);
  await page.keyboard.press('r');
  await page.waitForTimeout(2000);
  await page.click('#btn-stop');
  await page.waitForFunction(() => window.daw.tracks[0].buffer, null, { timeout: 10000 });
  const result = await page.evaluate(() => {
    const t = window.daw.tracks;
    return {
      take: window.daw.state.take,
      duration: t[0].buffer.duration,
      maxPeak: Math.max(...t[0].peaks),
      recordedTracks: t.filter((x) => x.buffer).length,
      exportEnabled: !document.getElementById('btn-export').disabled,
    };
  });
  await page.click('#btn-play');
  await page.waitForTimeout(700);
  const playing = await page.evaluate(() => window.daw.engine.playing && window.daw.engine.playPosition > 0.2);
  await page.screenshot({ path: process.env.SCREENSHOT || 'tests/smoke.png' });
  console.log({ info, ...result, playing, errors });
  const ok = result.take === 1 && result.duration > 1.5 && result.duration < 3 && result.maxPeak > 0.01
    && result.recordedTracks === 16 && result.exportEnabled && playing && errors.length === 0;
  if (!ok) { failed = true; console.error('SMOKE TEST FAILED'); } else console.log('SMOKE TEST PASSED');
} finally {
  await browser.close();
  server?.close();
}
process.exit(failed ? 1 : 0);
