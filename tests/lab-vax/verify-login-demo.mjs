// verify-login-demo.mjs — swap-day verification: drive the OVMX/VAX embed all the
// way to a live DCL `$` in real headless Chrome, against a given disk.
//
// Unlike the wiring test (which expects auth-FAILURE on the V0.5-2 disk), THIS asserts
// SUCCESS: boot -> Username: -> SYSTEM -> Password: -> MANAGER -> `$`. Run it against a
// V0.5-5 drive-to-$ image before deploying live.
//
// USAGE (serve the login-drive ovmx.html locally first, e.g. pcjs on :8188):
//   PW=/home/baron/.npm/_npx/db89d7302a373f10/node_modules/playwright-core \
//   SITE_BASE=http://localhost:8188 \
//   DISK_URL=https://do6xafl18swnu.cloudfront.net/vaxdisks/ovmx-vax-demo-v0.5-5.img \
//   node verify-login-demo.mjs
//
// Exit 0 iff the `$` prompt is reached; non-zero otherwise.

const PW   = process.env.PW || '/home/baron/.npm/_npx/db89d7302a373f10/node_modules/playwright-core';
const BASE = process.env.SITE_BASE || 'http://localhost:8188';
const ROM  = process.env.ROM_URL  || 'https://vax.3dl.network/disks/vaxdisks/ka655x.bin';
// AWS-free default: DISK_GZ_URL → ?diskgz= (one plain GET + in-browser gunzip, GitHub-Pages-hosted).
// DISK_URL → ?image= (ranged GET) is the legacy CloudFront path, kept for back-compat.
const DISK_GZ = process.env.DISK_GZ_URL || null;
const DISK    = process.env.DISK_URL || null;
if (!DISK_GZ && !DISK) throw new Error('set DISK_GZ_URL (?diskgz=, AWS-free) or DISK_URL (?image=)');
const diskParam = DISK_GZ ? `diskgz=${encodeURIComponent(DISK_GZ)}` : `image=${encodeURIComponent(DISK)}`;
const CEIL = parseInt(process.env.CEILING_S || '340', 10);

const { chromium } = (await import(PW + '/index.js')).default;
const URL = `${BASE}/machines/dec/vax/browser/ovmx.html`
  + `?drive=login&rom=${encodeURIComponent(ROM)}&${diskParam}&mem=32`;

const browser = await chromium.launch({ headless: true });
const page = await (await browser.newContext()).newPage();
page.on('pageerror', (e) => console.log('PAGEERROR:', String(e).slice(0, 200)));
await page.goto(URL, { waitUntil: 'load' });

const t0 = Date.now(); const seen = {};
const mark = (k) => { if (!seen[k]) { seen[k] = true; console.log(`  [${(Date.now()-t0)/1000|0}s] ${k}`); } };
let ok = false, lastPhase = '';

for (let i = 0; i < CEIL / 2; i++) {
  await page.waitForTimeout(2000);
  const s = await page.evaluate(() => ({
    phase: window.vaxStatus?.() || '?', user: window.ovmxReachedLogin?.() || false,
    dollar: window.ovmxReachedDollar?.() || false,
    screen: (document.getElementById('screen')||{}).textContent || '', err: window.vaxError || null,
  }));
  if (s.phase !== lastPhase) { console.log(`  [${(Date.now()-t0)/1000|0}s] phase="${s.phase}"`); lastPhase = s.phase; }
  if (s.user) mark('Username:');
  if (/Username:\s*SYSTEM/i.test(s.screen)) mark('SYSTEM typed');
  if (/Password:/i.test(s.screen)) mark('Password:');
  if (/authorization failure|invalid|not authorized/i.test(s.screen)) { console.log('  AUTH FAILED — wrong creds or non-auth disk'); break; }
  if (s.dollar) { mark('$ REACHED — logged in'); ok = true; break; }
  if (s.err) { console.log('  ERROR:', s.err); break; }
}

const tail = await page.evaluate(() => ((document.getElementById('screen')||{}).textContent||'').split('\n').filter(l=>l.trim()).slice(-8));
console.log('\n--- screen tail ---'); tail.forEach(l => console.log('   ', l.trim()));
console.log(ok ? '\nPASS: reached DCL $ ✓' : '\nFAIL: did not reach $ ✗');
await browser.close();
process.exit(ok ? 0 : 1);
