#!/usr/bin/env node
// visitor-gate.mjs — the pre-deploy gate for the 3-node browser cluster demo
// (rd vms-1a1). RUN THIS AGAINST THE LIVE PAGE BEFORE AND AFTER EVERY REDEPLOY.
//
// WHY THIS EXISTS
//
// Two deploys were verified green by a grader that, without anyone noticing,
// always clicked Node C FIRST and waited FIVE MINUTES between nodes. That gave
// the only node able to found a cluster -- the real VAX -- a head start, so it
// was always the founder. A visitor clicks the page top to bottom (its own order
// is A, B, C) within seconds, on a laptop. Then the two OVMX nodes, which boot
// far faster than a real VAX under pcjs, got there first; with a vote each their
// COMBINED votes satisfied quorum, so they founded a cluster of their own and the
// real VAX formed a separate one-member cluster it never admitted anybody into.
// Reported from the field as "vax vms never joined".
//
// The roster fix (both OVMX nodes NON-VOTING) makes that impossible. This gate
// makes sure nothing puts it back, by testing the thing the old grader assumed
// away: CLICK ORDER and SPEED.
//
// WHAT IT DOES NOT DO, ON PURPOSE
//
//   * no query parameters -- no ?initramfs=/?sysdisk=/?nodeB=/?nodeC=/?mac=.
//     Whatever the page ships is what boots.
//   * a fresh browser context per run: new profile, COLD cache, so the 58 MB
//     Node B and 30 MB Node C images are fetched over the wire while the guests
//     are running, as they are for a first-time visitor.
//   * it reports each run's worst main-thread drift, because every inter-node
//     frame in this demo crosses FOUR main threads (node worker -> node iframe ->
//     PARENT page's L2 hub -> node iframe -> worker), so main-thread starvation
//     is latency on every frame, and an SCS virtual circuit dies on latency.
//   * it TYPES NOTHING. Membership must form with nobody touching a console.
//     (A grader that logs in also keeps the main threads hot, which is another
//     thing a visitor does not do.)
//
// PASS BAR: every run in the matrix reaches CN=3 -- both OVMX nodes' own CNXMAN
// naming all three systems AND the real VAX admitting both in its own words --
// with no bugcheck and no lost connection.
//
// Usage:
//   node visitor-gate.mjs                      # the full matrix
//   RUNS=1 node visitor-gate.mjs               # first case only (smoke)
//   CASE=c-first-legacy node visitor-gate.mjs  # one named case
//   DEMO_URL=... OUT_DIR=... node visitor-gate.mjs
//
// Needs: playwright, and a host that can carry three emulators (the browser
// runs qemu-wasm + two pcjs VAXen). Use k3s-worker, not a small dev host.
import { chromium } from 'playwright';
import fs from 'node:fs';
import { NODES, MATRIX, newObservations, observe, isCN3, verdictOf }
  from './gate-eval.mjs';

const URL_ = process.env.DEMO_URL || 'https://openvmx.3dl.dev/demo/cluster/';
const OUT = process.env.OUT_DIR || '/out/visitor-gate';
const RUN_MS = +(process.env.RUN_MS || 1200000);
const ONLY = process.env.RUNS ? +process.env.RUNS : 0;
// CASE=<label>[,<label>] re-runs named rows of the matrix (to re-examine one
// failure without paying for the other five).
const CASE = (process.env.CASE || '').split(',').map((x) => x.trim()).filter(Boolean);
// FREEZE_MS > 0 suspends the page mid-formation (CDP Page.setWebLifecycleState
// 'frozen' -- the state Chrome really puts an aggressively backgrounded tab in)
// and then thaws it, which is what a visitor does by switching tabs. It is OFF
// by default: while the page is frozen the L2 hub stops relaying, every virtual
// circuit goes quiet, and the real VAX's connection manager reacts to that --
// which is rd vms-8c54's territory (the executive side), not this gate's. Turn
// it on to REPRODUCE 8c54, not to gate a deploy on it.
const FREEZE_MS = +(process.env.FREEZE_MS || 0);
const FREEZE_AT_MS = +(process.env.FREEZE_AT_MS || 60000);

// Injected before any page script: let the page report its own main-thread
// health. A starved or throttled document shows up here as a large drift, which
// is the condition an SCS virtual circuit cannot survive.
const PROBE = `(() => {
  window.__probe = { worstDriftMs: 0, ticks: 0 };
  let last = performance.now();
  setInterval(() => {
    const now = performance.now();
    const d = (now - last) - 250;
    if (d > window.__probe.worstDriftMs) window.__probe.worstDriftMs = Math.round(d);
    window.__probe.ticks++; last = now;
  }, 250);
})()`;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(`[${new Date().toISOString().slice(11, 19)}]`, ...a);

const pcjs = (p, s) => p.frames().find((f) => f !== p.mainFrame() &&
  f.url().split('?')[0].endsWith('/ovmx-cluster.html') && f.url().includes(s));
const nodeA = (p) => p.frames().find((f) => f !== p.mainFrame() &&
  f.url().split('?')[0].endsWith('/node.html'));

async function consoleOf(page, who) {
  if (who === 'OVMXA') {
    const f = nodeA(page);
    return f ? f.evaluate(() => (window.__nodeState && window.__nodeState.consoleText) || '').catch(() => '') : '';
  }
  const f = pcjs(page, who === 'OVMXB' ? '0B' : '0C');
  return f ? f.evaluate(() => { const e = document.getElementById('screen'); return e ? e.textContent : ''; }).catch(() => '') : '';
}

// The transcripts live in the .console.log files; keeping them out of the JSON
// keeps result.json readable.
const asJson = (R) => JSON.stringify(R, (k, v) => (k === 'transcript' ? undefined : v), 1);

async function oneRun(spec, idx) {
  const dir = `${OUT}/${String(idx).padStart(2, '0')}-${spec.label}`;
  fs.mkdirSync(dir, { recursive: true });
  const R = { ...spec, order: spec.order.join(','), url: URL_, samples: [],
              verdict: null, ...newObservations() };
  const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
  const ctx = await browser.newContext();                 // fresh profile, cold cache
  const page = await ctx.newPage();
  await page.addInitScript(PROBE);
  page.on('pageerror', (e) => log('  [pageerr]', String(e.message).slice(0, 120)));

  try {
    if (spec.throttle > 1) {
      const cdp = await ctx.newCDPSession(page);
      await cdp.send('Emulation.setCPUThrottlingRate', { rate: spec.throttle });
    }
    await page.goto(URL_, { waitUntil: 'load' });
    await page.waitForFunction(() => window.__demoReady === true, { timeout: 120000 });

    const btn = { A: '#bootbtn-a', B: '#bootbtn-b', C: '#bootbtn-c' };
    const t0 = Date.now();
    for (const n of spec.order) { await page.click(btn[n]); await sleep(spec.gap); }

    if (FREEZE_MS > 0) {
      setTimeout(async () => {
        try {
          const cdp = await ctx.newCDPSession(page);
          log(`  freezing the page for ${FREEZE_MS}ms (a visitor switched tabs)`);
          await cdp.send('Page.setWebLifecycleState', { state: 'frozen' });
          R.froze = true;
          await sleep(FREEZE_MS);
          await cdp.send('Page.setWebLifecycleState', { state: 'active' });
          log('  page thawed');
        } catch (e) { R.freeze_error = String(e.message).slice(0, 140); }
      }, FREEZE_AT_MS);
    }

    while (Date.now() - t0 < RUN_MS) {
      const el = Math.round((Date.now() - t0) / 1000);
      const hub = await page.evaluate(() => {
        const h = window.__hubframes || [];
        const s = {}; for (const f of h) if (f.ethertype === 0x6007) s[f.port] = (s[f.port] || 0) + 1;
        return s;
      }).catch(() => ({}));
      for (const w of NODES) {
        // The evidence file is the TRANSCRIPT, not the screen. A guest that
        // restarts clears its own terminal, and a screenshot taken afterwards
        // of a KA655 power-on self-test is then the only surviving trace of
        // twenty minutes of cluster messages. Keep everything ever seen, and
        // judge from that.
        observe(R, w, await consoleOf(page, w));
        try { fs.writeFileSync(`${dir}/${w}.console.log`, R.transcript[w] || ''); } catch (e) {}
      }
      const health = await page.evaluate(() => ({
        vis: document.visibilityState,
        drift: window.__probe ? window.__probe.worstDriftMs : null,
      })).catch(() => ({}));
      R.worstDriftMs = health.drift; R.vis = health.vis; R.sca = hub;
      const ok = isCN3(R);
      R.samples.push({ t: el, sca: hub, member: { ...R.member }, added: { ...R.added },
                       vaxc_admitted: [...R.vaxc_admitted], lost: { ...R.lost },
                       restarts: { ...R.restarts },
                       vis: health.vis, worstDriftMs: health.drift });
      log(`  t=${el}s vis=${health.vis} drift=${health.drift}ms sca=${JSON.stringify(hub)} ` +
          `added=${JSON.stringify(R.added)} vaxcAdm=${JSON.stringify(R.vaxc_admitted)} ` +
          `lost=${JSON.stringify(R.lost)}`);
      fs.writeFileSync(`${dir}/result.json`, asJson(R));
      if (ok) { R.cn3 = true; break; }
      if (Object.keys(R.bugchecks).length) break;
      await sleep(15000);
    }

    for (const w of NODES) {
      observe(R, w, await consoleOf(page, w));
      fs.writeFileSync(`${dir}/${w}.console.log`, R.transcript[w] || '');
    }
    R.verdict = verdictOf(R);
    await page.screenshot({ path: `${dir}/final.png`, fullPage: true }).catch(() => {});
  } finally {
    fs.writeFileSync(`${dir}/result.json`, asJson(R));
    await browser.close();
  }
  return R;
}

await (async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const rows = CASE.length ? MATRIX.filter((m) => CASE.includes(m.label))
                           : (ONLY ? MATRIX.slice(0, ONLY) : MATRIX);
  if (!rows.length) { console.error(`no such case: ${CASE}`); process.exit(2); }
  const results = [];
  for (let i = 0; i < rows.length; i++) {
    log(`=== run ${i + 1}/${rows.length}: ${rows[i].label} ` +
        `(order ${rows[i].order.join(',')}, gap ${rows[i].gap}ms, cpu x${rows[i].throttle}) ===`);
    const r = await oneRun(rows[i], i + 1);
    log(`--> ${rows[i].label}: ${r.verdict}`);
    results.push({ label: rows[i].label, order: r.order, gap: rows[i].gap,
                   throttle: rows[i].throttle, verdict: r.verdict, cn3: r.cn3,
                   added: r.added, vaxc_admitted: r.vaxc_admitted, restarts: r.restarts,
                   ovmx_founded: r.ovmx_founded, bugchecks: r.bugchecks, lost: r.lost,
                   worstDriftMs: r.worstDriftMs, froze: r.froze || false });
    fs.writeFileSync(`${OUT}/summary.json`, JSON.stringify(results, null, 1));
  }
  const bad = results.filter((r) => !r.cn3);
  console.log('');
  for (const r of results) console.log(`${r.cn3 ? 'PASS' : 'FAIL'}  ${r.label.padEnd(24)} ${r.verdict}`);
  console.log(`\nVISITOR_GATE=${JSON.stringify({ runs: results.length, failed: bad.length,
      pass: bad.length === 0 })}`);
  process.exit(bad.length === 0 ? 0 : 1);
})();
