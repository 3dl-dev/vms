// cn3-grade.js -- the CN=3 in-browser grader (rd vms-e18e).
//
// Boots the three demo nodes on ONE in-page L2 hub and PASSES only when EACH
// node's OWN executive says it is in a three-member cluster:
//
//   OVMXA  (OVMX/x86_64, qemu-wasm)  -- DCL  SHOW CLUSTER
//   OVMXB  (OVMX/VAX, pcjs KA655)    -- DCL  SHOW CLUSTER
//   VAXC   (real OpenVMS VAX V7.3)   -- SDA  ANALYZE/SYSTEM -> SHOW CLUSTER
//              (plain DCL SHOW CLUSTER needs a TERMTABLE entry this minimally
//               tailored volume does not carry -- %SMG-F-UNDTERNOS, measured
//               in rd vms-2570; SDA is the product-native command that works)
//
// Nothing here counts frames and calls it membership: the pass bar is three
// distinct node identities in a membership table read back off each guest's
// own console. Frame counters are reported as diagnostics only.
//
// Built on openvmx-site's committed demo/cluster/e2e/e2e-boot.js page-driving
// code (same frame-lookup rule, same snapshot shape) -- the "throwaway probe,
// not the repo" precedent of rd vms-2570's probe-run3 and rd vms-b34's
// cn2-grade.js. Kept with the capture so the run is reproducible.
//
// Env: PORT, INITRAMFS, SYSDISK, NODE_B, NODE_C, DEADLINE_MS, HOLD_MS,
//      BOOT_ORDER ("C,A,B" default; "A,B,C" for the OVMX-founds order),
//      STAGGER_MS, OUT_DIR.
const { chromium } = require('playwright');
const fs = require('fs');

const PORT = process.env.PORT || 8110;
const INITRAMFS = process.env.INITRAMFS || 'initramfs-ovmx-nodeA.cpio.gz';
const SYSDISK = process.env.SYSDISK || '';
const MAC = process.env.MAC || '52:54:00:00:00:0A';
const NODE_B = process.env.NODE_B || '';
const NODE_C = process.env.NODE_C || '';
const DEADLINE_MS = +(process.env.DEADLINE_MS || 2400000);
const HOLD_MS = +(process.env.HOLD_MS || 600000);       // the >=10 min stability bar
const STAGGER_MS = +(process.env.STAGGER_MS || 0);
const BOOT_ORDER = (process.env.BOOT_ORDER || 'C,A,B').split(',');
const OUT_DIR = process.env.OUT_DIR || __dirname;

const OVMX_USER = 'SYSTEM', OVMX_PASS = 'MANAGER';
const VAXC_USER = 'SYSTEM', VAXC_PASS = process.env.VAXC_PASS || 'OVMXCLUSTER1';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(`[${new Date().toISOString().slice(11, 19)}]`, ...a);

// ---------------------------------------------------------------- page glue

function demoUrl() {
  let u = `http://localhost:${PORT}/index.html?mac=${encodeURIComponent(MAC)}` +
          `&initramfs=${encodeURIComponent(INITRAMFS)}`;
  if (SYSDISK) u += `&sysdisk=${encodeURIComponent(SYSDISK)}`;
  if (NODE_B) u += `&nodeB=${encodeURIComponent(NODE_B)}`;
  if (NODE_C) u += `&nodeC=${encodeURIComponent(NODE_C)}`;
  return u;
}

// e2e-boot.js's rule, verbatim in intent: never the parent page, whose own URL
// carries the child's URL as an encoded query value and would substring-match.
function pcjsFrame(page, macSuffix) {
  return page.frames().find((f) => f !== page.mainFrame() &&
    f.url().split('?')[0].endsWith('/ovmx-cluster.html') && f.url().includes(macSuffix));
}
function nodeAFrame(page) {
  return page.frames().find((f) => f !== page.mainFrame() &&
    f.url().split('?')[0].endsWith('/node.html'));
}

// ------------------------------------------------------------- console reads

async function consoleOf(page, who) {
  if (who === 'OVMXA') {
    const f = nodeAFrame(page);
    if (!f) return '';
    return f.evaluate(() => (window.__nodeState && window.__nodeState.consoleText) || '')
            .catch(() => '');
  }
  const f = pcjsFrame(page, who === 'OVMXB' ? '0B' : '0C');
  if (!f) return '';
  return f.evaluate(() => { const e = document.getElementById('screen'); return e ? e.textContent : ''; })
          .catch(() => '');
}

// ------------------------------------------------------------ console writes
//
// Node A's page exposes its qemu-wasm worker (window.__nodeWorker) and takes
// input as {t:'in', d}. The pcjs machines take input through VaxTerminal's own
// keydown listener on #screen, so a key is delivered as the keydown the
// terminal is written to read -- the same path a visitor's keystroke takes.

async function typeInto(page, who, text) {
  if (who === 'OVMXA') {
    const f = nodeAFrame(page);
    if (!f) throw new Error('node A frame absent');
    return f.evaluate((d) => window.__nodeWorker.postMessage({ t: 'in', d }), text);
  }
  const f = pcjsFrame(page, who === 'OVMXB' ? '0B' : '0C');
  if (!f) throw new Error(`${who} machine frame absent`);
  return f.evaluate((s) => {
    const el = document.getElementById('screen');
    el.focus();
    for (const ch of s) {
      const key = ch === '\r' ? 'Enter' : ch;
      el.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true, cancelable: true }));
    }
  }, text);
}

async function sendLine(page, who, line) {
  await typeInto(page, who, line);
  await sleep(300);
  await typeInto(page, who, '\r');
}

/** Wait until `re` appears in `who`'s console, or `ms` elapses. */
async function waitFor(page, who, re, ms, label) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const c = await consoleOf(page, who);
    if (re.test(c)) { log(`  ${who}: saw ${label}`); return c; }
    await sleep(3000);
  }
  return null;
}

// -------------------------------------------------------------- the readback
//
// What counts as membership, per node, read off that node's own console.

/** OVMX DCL SHOW CLUSTER: count distinct MEMBER rows carrying a CSID. */
function ovmxMembers(text) {
  const tail = text.slice(-8000);
  const seen = new Set();
  // | NAME-or-SYSID | CSID | SOFTWARE | STATUS |
  const re = /\|\s*(\S+)\s*\|\s*([0-9A-Fa-f]{8})\s*\|[^|]*\|\s*(MEMBER|LOCAL)\s*\|/g;
  let m;
  while ((m = re.exec(tail)) !== null) seen.add(`${m[1]}=${m[2]}`);
  return [...seen];
}

/** Real VMS SDA SHOW CLUSTER: the CSB list's own rows (addr node csid votes ...). */
function sdaMembers(text) {
  const tail = text.slice(-8000);
  const seen = new Set();
  const re = /^\s*[0-9A-F]{8}\s+(\S+)\s+([0-9A-F]{8})\s+\d+\s/gm;
  let m;
  while ((m = re.exec(tail)) !== null) seen.add(`${m[1]}=${m[2]}`);
  return [...seen];
}

const BUGCHECK = /\*\*\*\s*FATAL BUGCHECK|BUGCHECK CODE|Kernel panic|\bOops:/i;

// ---------------------------------------------------------------- the phases

/* Every wait here is on something the GUEST printed, never a wall-clock sleep
 * that assumes a speed. These guests run under browser TCG/JS emulation, where
 * the interval between "Username:" and "Password:" is seconds to minutes
 * depending on what else the host is doing; a fixed sleep between them types
 * the password into the username field and calls the node broken. */

/** Log in and PROVE it by asking DCL to echo a sentinel. A prompt-shaped regex
 *  is not proof: this volume's own boot transcript contains `$!' comment lines
 *  that match one. */
async function login(page, who, user, pass, bootMs) {
  if (!await waitFor(page, who, /Username:/, bootMs, 'Username:')) {
    log(`  ${who}: never reached Username: in ${bootMs / 1000}s`);
    return false;
  }
  for (let attempt = 1; attempt <= 3; attempt++) {
    await sendLine(page, who, user);
    if (!await waitFor(page, who, /Password:/, 180000, 'Password:')) {
      log(`  ${who}: no Password: prompt (attempt ${attempt}); console tail:`);
      log('    ' + JSON.stringify((await consoleOf(page, who)).slice(-500)));
      await typeInto(page, who, '\r');
      continue;
    }
    await sendLine(page, who, pass);
    if (await sentinel(page, who, 180000)) { log(`  ${who}: at DCL (sentinel echoed)`); return true; }
    log(`  ${who}: logged in? sentinel did not echo (attempt ${attempt}); console tail:`);
    log('    ' + JSON.stringify((await consoleOf(page, who)).slice(-500)));
    await typeInto(page, who, '\r');
    await waitFor(page, who, /Username:/, 120000, 'Username: again');
  }
  return false;
}

/* The console buffers these guests expose are CAPPED scrollbacks (pcjs's
 * VaxTerminal trims to MAX_LINES), so "everything after byte N" silently breaks
 * the moment a long run trims. Each question is therefore preceded by a DCL
 * sentinel the guest ECHOES, and the answer is read out of the text after the
 * LAST occurrence of that sentinel -- immune to trimming, and it cannot match a
 * table printed before the question was asked. */
let markSeq = 0;
async function sentinel(page, who, ms) {
  const tag = `E18E-MARK-${++markSeq}`;
  await sendLine(page, who, `WRITE SYS$OUTPUT "${tag}"`);
  const t0 = Date.now();
  while (Date.now() - t0 < (ms || 120000)) {
    const c = await consoleOf(page, who);
    // the echo of the typed command contains the tag too; the guest's own
    // output line is the LAST one, which is what afterSentinel() takes.
    if ((c.match(new RegExp(tag, 'g')) || []).length >= 2) return tag;
    await sleep(2000);
  }
  log(`  ${who}: sentinel ${tag} never came back`);
  return null;
}
async function afterSentinel(page, who, tag) {
  const c = await consoleOf(page, who);
  const i = c.lastIndexOf(tag);
  return i < 0 ? '' : c.slice(i + tag.length);
}

/** Ask a node for its membership table and return the identities IT named, read
 *  only out of the text its console produced in answer to THIS question. */
async function readMembership(page, who) {
  const tag = await sentinel(page, who);
  if (!tag) return [];
  if (who === 'VAXC') {
    await sendLine(page, 'VAXC', 'ANALYZE/SYSTEM');
    await sleep(20000);
    await sendLine(page, 'VAXC', 'SHOW CLUSTER');
    await sleep(25000);
    const fresh = await afterSentinel(page, 'VAXC', tag);
    await sendLine(page, 'VAXC', 'EXIT');
    await sleep(5000);
    return sdaMembers(fresh);
  }
  await sendLine(page, who, 'SHOW CLUSTER');
  await sleep(15000);
  return ovmxMembers(await afterSentinel(page, who, tag));
}

const snap = () => {
  const hf = window.__hubframes || [];
  const scaByPort = {};
  for (const f of hf) if (f.ethertype === 0x6007) scaByPort[f.port] = (scaByPort[f.port] || 0) + 1;
  return { total: hf.length, scaByPort, roster: window.__roster || [] };
};

// ---------------------------------------------------------------------- main

(async () => {
  const result = { pass: false, boot_order: BOOT_ORDER.join(','), samples: [],
                   members: {}, bugchecks: {}, elapsed_s: 0, hold_s: 0 };
  const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
  const page = await browser.newPage();
  page.on('pageerror', (e) => log('[pageerr]', e.message));
  log('goto', demoUrl());
  await page.goto(demoUrl(), { waitUntil: 'load' });
  await page.waitForFunction(() => window.__demoReady === true, { timeout: 60000 });

  // Snapshot every console to disk continuously -- a run that is interrupted,
  // or one whose login never completes, must still leave the transcripts that
  // say WHY, not just a verdict.
  const snapshotter = setInterval(async () => {
    for (const who of ['OVMXA', 'OVMXB', 'VAXC']) {
      try { fs.writeFileSync(`${OUT_DIR}/${who}.console.log`, await consoleOf(page, who)); } catch (e) {}
    }
  }, 20000);

  const t0 = Date.now();
  const btn = { A: '#bootbtn-a', B: '#bootbtn-b', C: '#bootbtn-c' };
  for (const n of BOOT_ORDER) {
    log('boot', n);
    await page.click(btn[n.trim()]);
    if (STAGGER_MS) await sleep(STAGGER_MS);
  }

  // Phase 1 -- each node to a usable prompt. VAXC first: it is the founder in
  // the default order, and nothing can join a cluster that has not formed.
  const order = BOOT_ORDER.map((s) => s.trim());
  const loggedIn = {};
  // Boot budgets, measured not guessed: Node C (real V7.3 under pcjs) reached
  // Username: in ~60 s in this rig; Node A (qemu-wasm TCG) in ~2 min; Node B
  // (OVMX/VAX under pcjs, a 340 MB volume) is the slow one and gets the longest.
  if (order.includes('C')) loggedIn.VAXC  = await login(page, 'VAXC',  VAXC_USER, VAXC_PASS, 1200000);
  if (order.includes('A')) loggedIn.OVMXA = await login(page, 'OVMXA', OVMX_USER, OVMX_PASS, 900000);
  if (order.includes('B')) loggedIn.OVMXB = await login(page, 'OVMXB', OVMX_USER, OVMX_PASS, 1800000);
  log('logged in:', JSON.stringify(loggedIn));
  result.logged_in = loggedIn;

  // Phase 2 -- poll each node's own membership table until all three name three
  // identities, then HOLD for HOLD_MS re-asking, so "3 members" is a state the
  // cluster sustained and not a frame it passed through.
  const want = order.length;
  let firstAllAt = null;
  while (Date.now() - t0 < DEADLINE_MS) {
    const el = Math.round((Date.now() - t0) / 1000);
    const hub = await page.evaluate(snap).catch(() => ({ scaByPort: {}, total: 0 }));
    const members = {};
    for (const who of ['OVMXA', 'OVMXB', 'VAXC']) {
      if (!loggedIn[who]) continue;
      try { members[who] = await readMembership(page, who); }
      catch (e) { members[who] = ['ERR:' + e.message]; }
    }
    const counts = Object.fromEntries(Object.entries(members).map(([k, v]) => [k, v.length]));
    const allSee = Object.keys(members).length === want &&
                   Object.values(members).every((v) => v.length >= want);
    log(`t=${el}s sca=${JSON.stringify(hub.scaByPort)} members=${JSON.stringify(counts)} allSee=${allSee}`);
    for (const [k, v] of Object.entries(members)) log(`    ${k}: ${JSON.stringify(v)}`);
    result.samples.push({ t: el, sca: hub.scaByPort, members, allSee });
    result.members = members;

    for (const who of ['OVMXA', 'OVMXB', 'VAXC']) {
      const c = await consoleOf(page, who);
      if (BUGCHECK.test(c)) result.bugchecks[who] = (result.bugchecks[who] || 0) + 1;
    }

    if (allSee) {
      if (firstAllAt === null) { firstAllAt = Date.now(); log(`*** CN=${want} at t=${el}s -- holding ${HOLD_MS / 1000}s`); }
      if (Date.now() - firstAllAt >= HOLD_MS) {
        result.pass = Object.keys(result.bugchecks).length === 0;
        result.hold_s = Math.round((Date.now() - firstAllAt) / 1000);
        break;
      }
    } else if (firstAllAt !== null) {
      log('*** membership LOST after holding -- resetting the hold clock');
      firstAllAt = null;
    }
    await sleep(45000);
  }

  clearInterval(snapshotter);
  result.elapsed_s = Math.round((Date.now() - t0) / 1000);
  for (const who of ['OVMXA', 'OVMXB', 'VAXC']) {
    const c = await consoleOf(page, who);
    fs.writeFileSync(`${OUT_DIR}/${who}.console.log`, c);
  }
  await page.screenshot({ path: `${OUT_DIR}/cn3-final.png`, fullPage: true }).catch(() => {});
  fs.writeFileSync(`${OUT_DIR}/cn3-result.json`, JSON.stringify(result, null, 2) + '\n');
  log('CN3_RESULT=' + JSON.stringify({ pass: result.pass, hold_s: result.hold_s,
        elapsed_s: result.elapsed_s, bugchecks: result.bugchecks }));
  await browser.close();
  process.exit(result.pass ? 0 : 1);
})();
