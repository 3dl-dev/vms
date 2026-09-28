#!/usr/bin/env node
// visitor-gate.js — the pre-deploy gate for the 3-node browser cluster demo
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
//   node visitor-gate.js                      # the full matrix
//   RUNS=1 node visitor-gate.js               # first case only (smoke)
//   DEMO_URL=... OUT_DIR=... node visitor-gate.js
//
// Needs: playwright, and a host that can carry three emulators (the browser
// runs qemu-wasm + two pcjs VAXen). Use k3s-worker, not a small dev host.
'use strict';
const { chromium } = require('playwright');
const fs = require('fs');

const URL_ = process.env.DEMO_URL || 'https://openvmx.3dl.dev/demo/cluster/';
const OUT = process.env.OUT_DIR || '/out/visitor-gate';
const RUN_MS = +(process.env.RUN_MS || 1200000);
const ONLY = process.env.RUNS ? +process.env.RUNS : 0;
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

// The matrix. Each row is a way a real person might use the page. The two that
// matter most are the fast ones: they are what the old grader never did.
const MATRIX = [
  { label: 'page-order',            order: ['A', 'B', 'C'], gap: 5000, throttle: 1 },
  { label: 'all-at-once',           order: ['A', 'B', 'C'], gap: 500,  throttle: 1 },
  { label: 'page-order-throttled4', order: ['A', 'B', 'C'], gap: 5000, throttle: 4 },
  { label: 'b-first',               order: ['B', 'A', 'C'], gap: 3000, throttle: 1 },
  { label: 'c-first-legacy',        order: ['C', 'A', 'B'], gap: 5000, throttle: 1 },
  { label: 'all-at-once-throttled2', order: ['A', 'B', 'C'], gap: 500, throttle: 2 },
];

const NODES = ['OVMXA', 'OVMXB', 'VAXC'];
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

const MEMBER = /%CNXMAN,\s*this node is now a VAXcluster member/;
const FOUNDED = /%CNXMAN,\s*this node has quorum by its own votes/;   // must NEVER appear on an OVMX node
const ADDED = /%CNXMAN, system 0*([0-9a-f]+) was added to the cluster/g;
const VADD = /proposing addition of system (\S+)|proposed addition of node (\S+)/g;
const LOST = /lost connection to (?:system )?(\S+)/g;
const BUG = /\*\*\*\s*Fatal BUG CHECK|\*\*\*\s*FATAL BUGCHECK|CNXMGRERR|BUGCHECK CODE|Kernel panic|\bOops:/i;
const setOf = (t, re) => { const s = new Set(); const r = new RegExp(re); let m;
  while ((m = r.exec(t)) !== null) s.add(m[1] || m[2]); return [...s]; };

// The three system ids the roster declares (mk_democonfig.py DEMO_ROSTER).
const WANT = ['0x7c3', '0x7c4', '0x7c5'];

async function oneRun(spec, idx) {
  const dir = `${OUT}/${String(idx).padStart(2, '0')}-${spec.label}`;
  fs.mkdirSync(dir, { recursive: true });
  const R = { ...spec, order: spec.order.join(','), url: URL_, samples: [],
              member: {}, added: {}, lost: {}, vaxc_admitted: [], ovmx_founded: {},
              bugchecks: {}, cn3: false, verdict: null };
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
        const c = await consoleOf(page, w);
        try { fs.writeFileSync(`${dir}/${w}.console.log`, c); } catch (e) {}
        // ACCUMULATE, never recompute. These consoles are CAPPED scrollbacks
        // (pcjs's VaxTerminal trims to MAX_LINES, node.html keeps the last
        // 40000 chars), so a "was added to the cluster" line scrolls away on a
        // long run -- and a set recomputed from the current text SHRINKS, which
        // fails a perfectly healthy cluster. Observed once is true forever: a
        // node does not un-join because its console scrolled.
        if (MEMBER.test(c)) R.member[w] = true;
        const seen = new Set(R.added[w] || []);
        for (const h of setOf(c, ADDED)) seen.add('0x' + h.replace(/^0x/, ''));
        R.added[w] = [...seen];
        const l = setOf(c, LOST); if (l.length) R.lost[w] = [...new Set([...(R.lost[w] || []), ...l])];
        if (BUG.test(c)) R.bugchecks[w] = (c.match(BUG) || [''])[0];
        if (w !== 'VAXC' && FOUNDED.test(c)) R.ovmx_founded[w] = true;
        if (w === 'VAXC') R.vaxc_admitted = [...new Set([...R.vaxc_admitted, ...setOf(c, VADD)])];
      }
      const health = await page.evaluate(() => ({
        vis: document.visibilityState,
        drift: window.__probe ? window.__probe.worstDriftMs : null,
      })).catch(() => ({}));
      R.worstDriftMs = health.drift; R.vis = health.vis;
      const ok = NODES.slice(0, 2).every((w) => WANT.every((id) => (R.added[w] || []).includes(id))) &&
                 R.vaxc_admitted.length >= 2;
      R.samples.push({ t: el, sca: hub, member: { ...R.member }, added: { ...R.added },
                       vaxc_admitted: R.vaxc_admitted, lost: { ...R.lost },
                       vis: health.vis, worstDriftMs: health.drift });
      log(`  t=${el}s vis=${health.vis} drift=${health.drift}ms sca=${JSON.stringify(hub)} ` +
          `added=${JSON.stringify(R.added)} vaxcAdm=${JSON.stringify(R.vaxc_admitted)} ` +
          `lost=${JSON.stringify(R.lost)}`);
      fs.writeFileSync(`${dir}/result.json`, JSON.stringify(R, null, 1));
      if (ok) { R.cn3 = true; break; }
      if (Object.keys(R.bugchecks).length) break;
      await sleep(15000);
    }

    // A single OVMX node founding is the regression this gate exists for: it
    // means the pair can form a cluster without the real VAX.
    if (Object.keys(R.ovmx_founded).length) {
      R.verdict = `REGRESSION: an OVMX node founded a cluster (${Object.keys(R.ovmx_founded)}) ` +
                  `-- the OVMX nodes must be NON-VOTING (rd vms-1a1)`;
    } else if (Object.keys(R.bugchecks).length) {
      R.verdict = `BUGCHECK: ${JSON.stringify(R.bugchecks)}`;
    } else if (R.cn3) {
      R.verdict = 'CN=3';
    } else if (R.member.OVMXA && R.member.OVMXB && R.vaxc_admitted.length === 0) {
      R.verdict = 'SPLIT BRAIN: the OVMX pair clustered and the real VAX admitted nobody';
    } else {
      R.verdict = 'NOT CN=3';
    }
    for (const w of NODES) fs.writeFileSync(`${dir}/${w}.console.log`, await consoleOf(page, w));
    await page.screenshot({ path: `${dir}/final.png`, fullPage: true }).catch(() => {});
  } finally {
    fs.writeFileSync(`${dir}/result.json`, JSON.stringify(R, null, 1));
    await browser.close();
  }
  return R;
}

(async () => {
  fs.mkdirSync(OUT, { recursive: true });
  const rows = ONLY ? MATRIX.slice(0, ONLY) : MATRIX;
  const results = [];
  for (let i = 0; i < rows.length; i++) {
    log(`=== run ${i + 1}/${rows.length}: ${rows[i].label} ` +
        `(order ${rows[i].order.join(',')}, gap ${rows[i].gap}ms, cpu x${rows[i].throttle}) ===`);
    const r = await oneRun(rows[i], i + 1);
    log(`--> ${rows[i].label}: ${r.verdict}`);
    results.push({ label: rows[i].label, order: r.order, gap: rows[i].gap,
                   throttle: rows[i].throttle, verdict: r.verdict, cn3: r.cn3,
                   added: r.added, vaxc_admitted: r.vaxc_admitted,
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
