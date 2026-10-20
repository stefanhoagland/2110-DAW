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
  await page.waitForFunction(() => !window.daw.state.saving && window.daw.tracks[0].buffer, null, { timeout: 10000 });
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
  await page.click('#btn-stop');

  // Select the clip on track 1 and press Delete.
  let box = await page.locator('#timeline-canvas').boundingBox();
  await page.mouse.click(box.x + 20, box.y + 28 + 32);
  await page.keyboard.press('Delete');
  const afterDelete = await page.evaluate(() => window.daw.tracks.filter((x) => x.buffer).length);
  const firstGone = await page.evaluate(() => !window.daw.tracks[0].buffer);

  // Record a second take into a folder (the browser's private file system
  // stands in for a real folder), then delete a clip and check its WAV moved
  // into the Deleted subfolder.
  const disk = await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    window.daw.state.folder = root;
    return true;
  });
  await page.keyboard.press('r');
  await page.waitForTimeout(1200);
  await page.click('#btn-stop');
  await page.waitForFunction(() => window.daw.state.take === 2 && !window.daw.state.saving && window.daw.tracks[0].buffer, null, { timeout: 10000 });
  box = await page.locator('#timeline-canvas').boundingBox(); // the notice bar may have closed
  await page.mouse.click(box.x + 20, box.y + 28 + 64 + 32);
  await page.keyboard.press('Delete');
  await page.waitForFunction(() => !window.daw.tracks[1].buffer, null, { timeout: 5000 });
  const files = await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const list = async (d) => { const out = []; for await (const [n, h] of d.entries()) out.push(h.kind === 'directory' ? `${n}/` : n); return out.sort(); };
    const deleted = await list(await root.getDirectoryHandle('Deleted'));
    return { root: await list(root), deleted };
  });
  const diskOk = disk && files.deleted.length === 1 && files.deleted[0].startsWith('T02_02_')
    && files.root.filter((n) => n.startsWith('T02_')).length === 31;

  // Tab from one channel name to the next while typing.
  await page.locator('.track-header .name').nth(3).click(); // track 4
  await page.keyboard.type('Bass');
  await page.keyboard.press('Tab');
  await page.keyboard.type('Keys');
  await page.keyboard.press('Shift+Tab');
  const tabOk = await page.evaluate(() => [window.daw.tracks[3].name, window.daw.tracks[4].name, document.activeElement === window.daw.tracks[3].el.querySelector('.name')]);

  // Volume and pan tooltips show values.
  const tips = await page.evaluate(() => {
    const el = window.daw.tracks[0].el;
    const vol = el.querySelector('.vol');
    vol.value = 0.5; vol.dispatchEvent(new Event('input'));
    const pan = el.querySelector('.pan');
    pan.value = -0.3; pan.dispatchEvent(new Event('input'));
    return [vol.title, pan.title];
  });
  const tipsOk = tips[0].startsWith('Playback volume -6.0 dB') && tips[1].startsWith('Pan L30');

  // Import channel names from CSV, then export them again.
  await page.setInputFiles('#names-file', { name: 'names.csv', mimeType: 'text/csv', buffer: Buffer.from('Channel,Name\n1,Kick\n3,"Snare, top"\n') });
  await page.waitForFunction(() => window.daw.tracks[2].name === 'Snare, top', null, { timeout: 5000 });
  const [download] = await Promise.all([page.waitForEvent('download'), page.click('#btn-names-export')]);
  const exported = (await (await download.createReadStream()).toArray()).join('');
  const namesOk = exported.startsWith('Channel,Name\r\n1,Kick\r\n2,Ch 2\r\n3,"Snare, top"');

  await page.screenshot({ path: process.env.SCREENSHOT || 'tests/smoke.png' });
  console.log({ info, ...result, playing, afterDelete, namesOk, tips, tabOk, errors });
  const ok = result.take === 1 && result.duration > 1.5 && result.duration < 3 && result.maxPeak > 0.01
    && result.recordedTracks === 32 && result.exportEnabled && playing && afterDelete === 31 && firstGone && namesOk && tipsOk && tabOk.join() === 'Bass,Keys,true' && diskOk
    && errors.length === 0;
  if (!ok) { failed = true; console.error('SMOKE TEST FAILED'); } else console.log('SMOKE TEST PASSED');
} finally {
  await browser.close();
  server?.close();
}
process.exit(failed ? 1 : 0);
