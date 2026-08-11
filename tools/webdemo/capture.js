// capture.js — boot the OVMX guest under our qemu-wasm in headless Chromium,
// savevm the pre-booted 'ovmx' snapshot into the qcow2, and write it out. This
// is the CI equivalent of the by-hand capture; it is what makes the shipped
// snapshot resume in a few seconds instead of a ~70s cold boot.
//
//   node capture.js <base-url> <out-qcow2>
// e.g. node capture.js http://localhost:8099 ./sysdisk.qcow2
const { chromium } = require('playwright');

const BASE = process.argv[2] || 'http://localhost:8099';
const OUT = process.argv[3] || './sysdisk.qcow2';
const LOGIN_TIMEOUT_S = +(process.env.LOGIN_TIMEOUT_S || 240);

const readScreen = (p) => p.evaluate(() => {
  const t = window.__ovmxTerm; if (!t) return '';
  const b = t.buffer.active; let s = '';
  for (let j = 0; j < b.length; j++) { const l = b.getLine(j); if (l) s += l.translateToString(true) + '\n'; }
  return s;
});

(async () => {
  const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
  const ctx = await browser.newContext({ acceptDownloads: true });
  const page = await ctx.newPage();
  page.on('pageerror', (e) => console.error('PAGEERR', e.message));
  await page.goto(BASE + '/', { waitUntil: 'load', timeout: 20000 });

  // 1) boot to the login prompt
  let up = false;
  for (let i = 0; i < LOGIN_TIMEOUT_S; i++) {
    if (/Username:/.test(await readScreen(page))) { console.log('login reached at ~' + i + 's'); up = true; break; }
    await new Promise((r) => setTimeout(r, 1000));
  }
  if (!up) { console.error('FAIL: no login prompt within ' + LOGIN_TIMEOUT_S + 's'); process.exit(2); }

  // 2) drop to the QEMU monitor (Ctrl-A c) and savevm
  await page.click('#terminal');
  await page.keyboard.down('Control'); await page.keyboard.press('KeyA'); await page.keyboard.up('Control');
  await new Promise((r) => setTimeout(r, 500));
  await page.keyboard.press('KeyC');
  await new Promise((r) => setTimeout(r, 1200));
  if (!/\(qemu\)/.test(await readScreen(page))) { console.error('FAIL: QEMU monitor not reached'); process.exit(3); }
  await page.keyboard.type('savevm ovmx'); await page.keyboard.press('Enter');

  // 3) wait for the qcow2 in the emscripten FS to stop growing (savevm done)
  let last = -1, stable = 0, size = 0;
  for (let i = 0; i < 60; i++) {
    size = await page.evaluate(() => { try { return window.__ovmxMod.FS.stat('/pack-disk/sysdisk.qcow2').size; } catch (e) { return -1; } });
    if (size === last) { if (++stable >= 3) break; } else { stable = 0; last = size; }
    await new Promise((r) => setTimeout(r, 1000));
  }
  const err = (await readScreen(page)).split('savevm').pop() || '';
  if (/Error|failed|No space/i.test(err)) { console.error('FAIL: savevm error:', err.slice(0, 160)); process.exit(4); }
  console.log('savevm complete, qcow2 = ' + size + ' bytes');

  // 4) export the qcow2 out of the emscripten FS via a blob download
  const [dl] = await Promise.all([
    page.waitForEvent('download', { timeout: 60000 }),
    page.evaluate(() => {
      const d = window.__ovmxMod.FS.readFile('/pack-disk/sysdisk.qcow2');
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([d]));
      a.download = 'sysdisk.qcow2'; document.body.appendChild(a); a.click();
    }),
  ]);
  await dl.saveAs(OUT);
  console.log('SAVED ' + OUT);
  await browser.close();
})().catch((e) => { console.error('FATAL', e.message); process.exit(1); });
