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
//   * it TYPES NOTHING UNTIL THE CLUSTER HAS FORMED. Membership must form with
//     nobody touching a console. (A grader that logs in also keeps the main
//     threads hot, which is another thing a visitor does not do.) After CN=3 it
//     does what a visitor does next: strikes RETURN at a console that has not
//     offered Username:, and logs in on the OVMX nodes where they have (the
//     login the site publishes). It never logs in to the real VAX: Node C
//     offering Username: is the proof its startup completed.
//
// PASS BAR: every run in the matrix reaches CN=3 -- both OVMX nodes' own CNXMAN
// naming all three systems AND the real VAX admitting both in its own words --
// with no bugcheck, a quiet OVMX/VAX console, AND every node usable within
// LOGIN_BOUND_S of the first click -- each OVMX node at a logged-in DCL `$`,
// the real VAX offering Username: (rd vms-db1: a real VAX can be a member and
// still hang in its startup, a dead console to a visitor). The matrix includes
// runs that boot Node C only after an OVMX node is up, since visitors choose
// the order and that is the order that hung.
//
// Usage:
//   node visitor-gate.mjs                      # the full matrix
//   RUNS=1 node visitor-gate.mjs               # first case only (smoke)
//   CASE=c-first-legacy node visitor-gate.mjs  # one named case
//   REVEAL=1            node visitor-gate.mjs  # scroll panels in before reading
//                                              # (diagnosing rd vms-0bc only:
//                                              #  it starves node A)
//   DEMO_URL=... OUT_DIR=... node visitor-gate.mjs
//   [DEMO_LOGIN_HINT_URL=...] [LOGIN_BOUND_S=...]
//                                              # the OVMX login is read from the site's
//                                              # published hint; nothing to supply
//
// Needs: playwright, and a host that can carry three emulators (the browser
// runs qemu-wasm + two pcjs VAXen). Use k3s-worker, not a small dev host.
import { chromium } from 'playwright';
import { grabWorkerStacks } from './worker-stacks.mjs';
import fs from 'node:fs';
import { NODES, MATRIX, newObservations, observe, isCN3, verdictOf, isRepaintStall, wantsReveal,
         silentNeverStarts, isPass, isQuiet, observeNodeAWorker, nodeAStallWhy,
         observeLogin, loginAction, allReady, USERNAME, AFTER_WAIT_MS, parseLoginHint,
         productVersionOf, versionWhy, driveLoginStep } from './gate-eval.mjs';

const URL_ = process.env.DEMO_URL || 'https://openvmx.3dl.dev/demo/cluster/';
const OUT = process.env.OUT_DIR || '/out/visitor-gate';
const RUN_MS = +(process.env.RUN_MS || 2400000);
// rd vms-db1: every node must be usable within this many seconds of the first
// click (OVMX nodes at a logged-in DCL `$`, the real VAX at Username:).
const LOGIN_BOUND_S = +(process.env.LOGIN_BOUND_S || RUN_MS / 1000);
// The OVMX nodes' login is the one the site PUBLISHES to visitors (the
// homepage's "Log in with username ... password ..." hint), read from the page
// at start-up -- one source, nothing to supply by hand. The real VAX is never
// logged in to (gate-eval.mjs LOGIN_NODES): it passes on offering Username:.
const HINT_URL = process.env.DEMO_LOGIN_HINT_URL || new URL('/', URL_).href;
const CREDS = {};
// The OpenVMX version under test: OVMX_PRODUCT_VERSION of the vms tree this
// gate was checked out at (the one source -- the gate and the build it grades
// come from the same checkout), or OVMX_VERSION when grading a page against a
// different checkout. Every OVMX node in the page must announce it, and only it
// (gate-eval.mjs versionWhy).
const IDENTITY_H = new URL('../../src/libvms/include/ovmx_identity.h', import.meta.url);
let WANT_VERSION = process.env.OVMX_VERSION || null;
const userFor = (w) => (CREDS[w] || {}).user;
const passFor = (w) => (CREDS[w] || {}).pass || '';
// A console that has not offered Username: gets a RETURN at most this often.
const WAKE_EVERY_S = +(process.env.WAKE_EVERY_S || 20);
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
// How long to let a revealed panel catch up before reading it (rd vms-0bc).
const REVEAL_MS = +(process.env.REVEAL_MS || 1500);
// Scrolling a panel into view before reading it is OPT-IN (REVEAL=1) -- see
// wantsReveal() in gate-eval.mjs for what it cost when it was not.
const REVEAL = wantsReveal(process.env);

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
// rd vms-bfdd: the SAME-ORIGIN node-pcjs.html wrapper (one frame up from the pcjs
// machine) -- where the boot watchdog lives, specifically so it still answers even
// when the cross-origin machine frame above never loads at all.
const nodePcjsWrapper = (p, s) => p.frames().find((f) => f !== p.mainFrame() &&
  f.url().split('?')[0].endsWith('/node-pcjs.html') && f.url().includes(s));

// Poll each pcjs node's own wrapper for a sign of life / an honest watchdog fire.
// Monotonic (OR into whatever R already has) -- a frame that briefly disappears
// mid-navigation (the watchdog's own retry) must not un-report a start it already saw.
async function samplePanelHealth(page, R) {
  for (const [w, suffix] of [['OVMXB', '0B'], ['VAXC', '0C']]) {
    const f = nodePcjsWrapper(page, suffix);
    if (!f) continue;
    try {
      const st = await f.evaluate(() => window.__nodeState || null);
      if (st) {
        R.panel_started[w] = R.panel_started[w] || !!st.started;
        R.panel_watchdog_fired[w] = R.panel_watchdog_fired[w] || !!st.watchdogFired;
      }
    } catch (e) { /* mid-navigation (a watchdog retry reloaded the iframe) -- sample again next tick */ }
  }
}

// Poll Node A's own node.html wrapper for the worker-level state it publishes
// (rd vms-4ff). The pcjs nodes have samplePanelHealth above; Node A had no
// equivalent, so a Node A that stopped left behind a console and no answer to
// "was the worker still alive?". Same frame consoleText is read from, same
// monotonic discipline; a frame that answers nothing leaves the fold untouched.
async function sampleNodeAWorker(page, R) {
  const f = nodeA(page);
  if (!f) return;
  try {
    observeNodeAWorker(R, await f.evaluate(() => window.__nodeState || null));
  } catch (e) { /* mid-navigation; sample again next tick */ }
}

async function rawConsoleOf(page, who) {
  if (who === 'OVMXA') {
    const f = nodeA(page);
    return f ? f.evaluate(() => (window.__nodeState && window.__nodeState.consoleText) || '').catch(() => '') : '';
  }
  const f = pcjs(page, who === 'OVMXB' ? '0B' : '0C');
  return f ? f.evaluate(() => { const e = document.getElementById('screen'); return e ? e.textContent : ''; }).catch(() => '') : '';
}

// Type into a node's console as a visitor would: Node A through its own
// node.html worker input (what its terminal's onData does), a pcjs node by
// focusing its screen and using the keyboard.
async function typeTo(page, who, text) {
  if (who === 'OVMXA') {
    const f = nodeA(page);
    if (!f) return false;
    return f.evaluate((t) => { try { window.__nodeWorker.postMessage({ t: 'in', d: t }); return true; }
                               catch (e) { return false; } }, text).catch(() => false);
  }
  const f = pcjs(page, who === 'OVMXB' ? '0B' : '0C');
  if (!f) return false;
  await f.locator('#screen').click().catch(() => {});
  const body = text.replace(/\r$/, '');
  if (body) await page.keyboard.type(body, { delay: 40 }).catch(() => {});
  if (text.endsWith('\r')) await page.keyboard.press('Enter').catch(() => {});
  return true;
}

// One login step per node per poll (rd vms-db1). Nothing before CN=3.
async function driveLogins(page, R, el, lastWake) {
  // Every keystroke the gate sends is recorded per node (rd vms-25f): when,
  // what kind (a password is never recorded), whether typeTo reported it
  // delivered, and how much the console grew in the second after a bare RETURN.
  // That splits "the RETURN never reached the node" (delivered=false, or
  // grew=0 on a node that is otherwise alive) from "the node got it and
  // never restarted its login" -- the two are indistinguishable from the
  // console log alone. What the gate asserts is unchanged.
  const record = async (w, t) => {
    const wake = t === '\r';
    const before = wake ? (await rawConsoleOf(page, w)).length : 0;
    const delivered = await typeTo(page, w, t);
    let grew = null;
    if (wake) { await sleep(1000); grew = (await rawConsoleOf(page, w)).length - before; }
    (R.keystrokes = R.keystrokes || {});
    (R.keystrokes[w] = R.keystrokes[w] || []).push(
      { el, kind: wake ? 'RETURN' : `text(${t.replace(/\r$/, '').length})`, delivered, grew });
    if (wake) log(`  ${w}: wake RETURN at t=${el}s delivered=${delivered} console grew ${grew} chars`);
    return delivered;
  };
  const io = {
    type: record,
    read: (w) => consoleOf(page, w, R),
    sleep, log,
    user: userFor, pass: passFor,
  };
  for (const w of NODES) await driveLoginStep(R, w, el, lastWake, WAKE_EVERY_S, io);
}

// Bring a panel into the viewport. A pcjs panel below the fold STOPS
// REPAINTING -- Chromium throttles rendering in an offscreen cross-origin
// iframe and the terminal writes its DOM text on the render tick -- so its
// screen text goes stale while the guest runs on. Measured (rd vms-0bc): a
// panel frozen 180s at 211 characters of power-on self-test jumped to 6864
// characters, the whole VMS boot and its cluster transition, within 10s of a
// scrollIntoView -- with nothing typed and nothing clicked.
//
// Reading a throttled panel as if it were the guest's output is how a formed
// cluster gets reported as "the real VAX admitted nobody". So reveal a panel
// before reading it, and COUNT the times that was what made it move: that
// count is the page defect, measured, not a thing the gate hides.
async function reveal(page, who) {
  if (!REVEAL) return;
  const key = who === 'OVMXB' ? '0B' : who === 'VAXC' ? '0C' : null;
  if (!key) return;                                  // node A's console is a JS string
  await page.evaluate((k) => {
    const f = [...document.querySelectorAll('iframe')].find((x) => x.src.includes(k));
    if (f) f.scrollIntoView({ block: 'center' });
  }, key).catch(() => {});
  await sleep(REVEAL_MS);
}

// A STALL is not "the console moved while we revealed it" -- a booting guest
// prints all the time, so that counted a healthy panel. It is: the panel showed
// NOTHING NEW for a whole poll interval, and then revealing it produced output.
// That signature is the page defect (rd vms-0bc) and nothing else.
async function consoleOf(page, who, R) {
  const before = await rawConsoleOf(page, who);
  await reveal(page, who);
  const after = await rawConsoleOf(page, who);
  if (R) {
    if (isRepaintStall(R.lastSeen[who], before, after)) {
      R.repaint_stalls[who] = (R.repaint_stalls[who] || 0) + 1;
    }
    R.lastSeen[who] = after;
  }
  return after.length >= before.length ? after : before;
}

// The transcripts live in the .console.log files; keeping them out of the JSON
// keeps result.json readable.
const asJson = (R) => JSON.stringify(
  R, (k, v) => (k === 'transcript' || k === 'lastSeen' ? undefined : v), 1);

async function oneRun(spec, idx) {
  const dir = `${OUT}/${String(idx).padStart(2, '0')}-${spec.label}`;
  fs.mkdirSync(dir, { recursive: true });
  const R = { ...spec, order: spec.order.join(','), url: URL_, samples: [],
              verdict: null, lastSeen: {}, ...newObservations(), login_bound_s: LOGIN_BOUND_S,
              want_version: WANT_VERSION,
              after_waited: {} };
  const lastWake = {};
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
    for (const n of spec.order) {
      // `after`: hold this click until the named node has offered Username:
      // (rd vms-db1 -- Node C booting after an OVMX node is up). Observed from
      // that node's own console; nothing is typed.
      const dep = (spec.after || {})[n];
      if (dep) {
        const w0 = Date.now();
        while (Date.now() - w0 < AFTER_WAIT_MS) {
          observe(R, dep, await consoleOf(page, dep, R));
          if (USERNAME.test(R.transcript[dep] || '')) break;
          await sleep(10000);
        }
        R.after_waited[n] = { node: dep, s: Math.round((Date.now() - w0) / 1000),
                              ready: USERNAME.test(R.transcript[dep] || '') };
        log(`  clicking ${n} after ${dep}: ${JSON.stringify(R.after_waited[n])}`);
      }
      await page.click(btn[n]); await sleep(spec.gap);
    }

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
        observe(R, w, await consoleOf(page, w, R));
        observeLogin(R, w, el);
        try { fs.writeFileSync(`${dir}/${w}.console.log`, R.transcript[w] || ''); } catch (e) {}
      }
      await samplePanelHealth(page, R);
      await sampleNodeAWorker(page, R);
      // rd vms-e79 mode (b): heartbeat frozen 60 s -> grab the worker stacks (twice at most).
      {
        const tk = (R.nodeA_worker || {}).workerTicks;
        if (tk !== undefined && tk !== R._tkLast) { R._tkLast = tk; R._tkAt = Date.now(); }
        if (R._tkAt && Date.now() - R._tkAt > 60000 && (R._stackGrabs || 0) < 2) {
          R._stackGrabs = (R._stackGrabs || 0) + 1; R._tkAt = Date.now();
          const g = await grabWorkerStacks(ctx, page, dir, `nodeA workerTicks stuck at ${tk}`);
          R.nodeA_stack_grabs = (R.nodeA_stack_grabs || 0) + 1;
          log(`  Node A worker heartbeat stuck at ${tk}: grabbed ${g.workers.length} worker stack(s), ` +
              `paused=${g.workers.filter((w) => w.paused).length}`);
        }
      }
      const health = await page.evaluate(() => ({
        vis: document.visibilityState,
        drift: window.__probe ? window.__probe.worstDriftMs : null,
      })).catch(() => ({}));
      R.worstDriftMs = health.drift; R.vis = health.vis; R.sca = hub;
      if (isCN3(R)) R.cn3 = true;
      R.samples.push({ t: el, sca: hub, member: { ...R.member }, added: { ...R.added },
                       vaxc_admitted: [...R.vaxc_admitted], lost: { ...R.lost },
                       restarts: { ...R.restarts },
                       vis: health.vis, worstDriftMs: health.drift });
      // Node A's worker state is logged while it has nothing on the wire --
      // the window in which "is it dead or still booting?" is the question
      // (rd vms-4ff), and silent once it starts talking.
      const aState = hub.OVMXA ? '' : ` nodeA=${JSON.stringify(R.nodeA_worker)}`;
      log(`  t=${el}s vis=${health.vis} drift=${health.drift}ms sca=${JSON.stringify(hub)} ` +
          `added=${JSON.stringify(R.added)} vaxcAdm=${JSON.stringify(R.vaxc_admitted)} ` +
          `lost=${JSON.stringify(R.lost)} stalls=${JSON.stringify(R.repaint_stalls)}${aState}` +
          (R.cn3 ? ` username=${JSON.stringify(R.username_at)} dcl=${JSON.stringify(R.dcl_at)}` : ''));
      fs.writeFileSync(`${dir}/result.json`, asJson(R));
      if (R.cn3 && allReady(R)) break;
      if (Object.keys(R.bugchecks).length) break;
      if (Object.keys(R.login_failed).length) break;
      await driveLogins(page, R, el, lastWake);
      await sleep(15000);
    }

    const elEnd = Math.round((Date.now() - t0) / 1000);
    for (const w of NODES) {
      observe(R, w, await consoleOf(page, w, R));
      observeLogin(R, w, elEnd);
      fs.writeFileSync(`${dir}/${w}.console.log`, R.transcript[w] || '');
    }
    await samplePanelHealth(page, R);
    await sampleNodeAWorker(page, R);
    R.verdict = verdictOf(R);
    await page.screenshot({ path: `${dir}/final.png`, fullPage: true }).catch(() => {});
  } finally {
    fs.writeFileSync(`${dir}/result.json`, asJson(R));
    await browser.close();
  }
  return R;
}

await (async () => {
  // The pass bar logs in on every node (rd vms-db1): no credentials, no run.
  let hint = null;
  try { hint = parseLoginHint(await (await fetch(HINT_URL)).text()); } catch (e) { /* reported below */ }
  if (!hint) {
    console.error(`no published login hint at ${HINT_URL} ("Log in with username <b>U</b>, password <b>P</b>")`);
    process.exit(2);
  }
  CREDS.OVMXA = hint; CREDS.OVMXB = hint;
  if (!WANT_VERSION) {
    try { WANT_VERSION = productVersionOf(fs.readFileSync(IDENTITY_H, 'utf8')); } catch (e) { /* reported below */ }
  }
  if (!WANT_VERSION) {
    console.error(`no OpenVMX version under test: set OVMX_VERSION or run from a vms checkout (${IDENTITY_H.pathname})`);
    process.exit(2);
  }
  log(`build under test: OpenVMX ${WANT_VERSION} -- every OVMX node must announce it, and only it`);
  log(`logins: OVMX nodes as ${hint.user} (published at ${HINT_URL}); VAXC is not logged in to (Username: is its bar)`);
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
                   repaint_stalls: r.repaint_stalls, lost_unnamed: r.lost_unnamed,
                   substrate_noise: r.substrate_noise, quiet: isQuiet(r), pass: isPass(r),
                   worstDriftMs: r.worstDriftMs, froze: r.froze || false,
                   panel_started: r.panel_started, panel_watchdog_fired: r.panel_watchdog_fired,
                   silent_never_starts: silentNeverStarts(r, ['OVMXB', 'VAXC']),
                   nodeA_worker: r.nodeA_worker, nodeA_why: nodeAStallWhy(r),
                   username_at: r.username_at, dcl_at: r.dcl_at, login_failed: r.login_failed,
                   after_waited: r.after_waited,
                   want_version: r.want_version, ovmx_versions: r.ovmx_versions, version_why: versionWhy(r) });
    fs.writeFileSync(`${OUT}/summary.json`, JSON.stringify(results, null, 1));
  }
  const bad = results.filter((r) => !r.pass);
  const silentTotal = results.reduce((n, r) => n + (r.silent_never_starts || []).length, 0);
  console.log('');
  for (const r of results) console.log(`${r.pass ? 'PASS' : 'FAIL'}  ${r.label.padEnd(24)} ${r.verdict}`);
  console.log(`\nVISITOR_GATE=${JSON.stringify({ runs: results.length, failed: bad.length,
      pass: bad.length === 0, silent_never_starts: silentTotal })}`);
  process.exit(bad.length === 0 ? 0 : 1);
})();
