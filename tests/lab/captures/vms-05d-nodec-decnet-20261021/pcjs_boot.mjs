// rd vms-05d: boot the Node C DECnet disk in pcjs's ovmx-cluster.html (headless chromium),
// log in, and capture the console: proves pcjs bootability + the at-boot STARTNET failure.
import { createRequire } from 'node:module';
import { writeFileSync } from 'node:fs';
const require = createRequire('/home/baron/projects/openvmx-site/package.json');
const { chromium } = require('playwright');
const url = process.argv[2], out = process.argv[3];
const browser = await chromium.launch();
const page = await browser.newPage();
page.on('pageerror', (e) => console.log('[pageerror]', e.message));
page.on('console', (m) => { if (m.type() === 'error') console.log('[console.error]', m.text()); });
await page.goto(url);
const screen = () => page.evaluate(() => document.getElementById('screen').textContent);
const lines = []; const seenSet = new Set();
async function text() {
  const cur = await screen();
  for (const l of cur.split('\n')) { const k = l.trimEnd(); if (k && !seenSet.has(k)) { seenSet.add(k); lines.push(k); } }
  return cur;
}
const all = () => lines.join('\n');
const t0 = Date.now();
async function waitFor(re, secs) {
  while (Date.now() - t0 < secs * 1000) {
    const s = await text();
    if (re.test(s) || (!re.multiline && re.test(all()))) return s;
    await page.waitForTimeout(1000);
    if (lines.length % 50 === 0 && lines.length) console.log('[progress]', lines.length, 'lines, t=', (Date.now() - t0) / 1000);
  }
  throw new Error('timeout ' + re);
}
async function type(s) { await page.focus('#screen'); await page.keyboard.type(s, { delay: 250 }); await page.keyboard.press('Enter'); }
let s = '';
try {
  s = await waitFor(/%SYSTEM-F-NOLICENSE|DECnet not started/, 1500);
  console.log('STARTNET outcome seen at', (Date.now() - t0) / 1000, 's');
  await page.waitForTimeout(20000);
  await type('');
  await waitFor(/Username: *$/m, 300); await type('SYSTEM');
  await waitFor(/Password: *$/m, 120); await type('OVMXCLUSTER1');
  await waitFor(/\$ *$/m, 300);
  await type('MCR NCP LIST KNOWN NODES');
  await page.waitForTimeout(30000);
  await type('SHOW LICENSE');
  await page.waitForTimeout(20000);
} finally {
  await text();
  writeFileSync(out, all() + '\n');
  console.log(all().slice(-3500));
  await browser.close();
}
