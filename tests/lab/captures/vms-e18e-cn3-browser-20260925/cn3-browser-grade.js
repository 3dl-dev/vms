// cn3-browser.js -- the in-browser CN=3 grader (rd vms-e18e, after #1302/#1303/#1306).
//
// Three machines on ONE in-page L2 hub, and the bar is each node's OWN
// membership table:
//
//   OVMXA  OVMX/x86_64 (qemu-wasm)   -- DCL SHOW CLUSTER
//   OVMXB  OVMX/VAX (pcjs KA655)     -- DCL SHOW CLUSTER
//   VAXC   real OpenVMS VAX V7.3     -- its own CNXMAN/OPCOM 'proposed addition
//                                       of system X' + 'completing VAXcluster
//                                       state transition' for BOTH OVMX nodes,
//                                       and SHOW CLUSTER too when its console
//                                       can be driven (this minimally tailored
//                                       volume has no TERMTABLE entry, so plain
//                                       SHOW CLUSTER gives %SMG-F-UNDTERNOS and
//                                       SDA is the product-native fallback).
//
// ORDERING. Nothing is typed until the cluster has announced itself: rd
// vms-e18e measured that driving OPA0:'s login while a node is retrying
// admission gets %LOGIN-F-CMDINPUT, because the OPCOM broadcast storm the
// retries generate breaks LOGINOUT's read. Read first, type second.
//
// LIVENESS. Every machine's OWN NIC transmit counter is sampled every 15 s. The
// real VAX must still be transmitting at the end: rd vms-e18e's pre-fix runs
// showed it stop dead when a third participant appeared, and a membership table
// that was true ten minutes ago proves nothing about now.
//
// Env: PORT, INITRAMFS, SYSDISK, NODE_B, NODE_C, BOOT_ORDER, STAGGER_MS,
//      HOLD_MS, DEADLINE_MS, OUT_DIR.
const { chromium } = require('playwright');
const fs = require('fs');

const PORT = process.env.PORT || 8110;
const INITRAMFS = process.env.INITRAMFS || 'initramfs-ovmx-nodeA.cpio.gz';
const SYSDISK = process.env.SYSDISK || 'sysdisk-nodeA.qcow2.gz';
const MAC = process.env.MAC || '52:54:00:00:00:0A';
const NODE_B = process.env.NODE_B || '';
const NODE_C = process.env.NODE_C || '';
const BOOT_ORDER = (process.env.BOOT_ORDER || 'C,A,B').split(',').map((s) => s.trim());
const STAGGER_MS = +(process.env.STAGGER_MS || 180000);
const HOLD_MS = +(process.env.HOLD_MS || 660000);        // the >=10 min bar, +1 min
const DEADLINE_MS = +(process.env.DEADLINE_MS || 3600000);
const OUT_DIR = process.env.OUT_DIR || '/out/cn3';

const OVMX_USER = 'SYSTEM', OVMX_PASS = 'MANAGER';
const VAXC_USER = 'SYSTEM', VAXC_PASS = process.env.VAXC_PASS || 'OVMXCLUSTER1';
const NODES = ['OVMXA', 'OVMXB', 'VAXC'];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(`[${new Date().toISOString().slice(11, 19)}]`, ...a);

// ------------------------------------------------------------- frames + reads

// e2e-boot.js's rule: never the parent page, whose own URL carries the child's
// URL as an encoded query value and would substring-match.
const pcjsFrame = (page, suffix) => page.frames().find((f) => f !== page.mainFrame() &&
  f.url().split('?')[0].endsWith('/ovmx-cluster.html') && f.url().includes(suffix));
const nodeAFrame = (page) => page.frames().find((f) => f !== page.mainFrame() &&
  f.url().split('?')[0].endsWith('/node.html'));

async function consoleOf(page, who) {
  if (who === 'OVMXA') {
    const f = nodeAFrame(page);
    return f ? f.evaluate(() => (window.__nodeState && window.__nodeState.consoleText) || '').catch(() => '') : '';
  }
  const f = pcjsFrame(page, who === 'OVMXB' ? '0B' : '0C');
  return f ? f.evaluate(() => { const e = document.getElementById('screen'); return e ? e.textContent : ''; }).catch(() => '') : '';
}

async function nicTxOf(page, who) {
  if (who === 'OVMXA') {
    const f = nodeAFrame(page);
    return f ? f.evaluate(() => (window.__nodeState || {}).nicTxCount ?? null).catch(() => null) : null;
  }
  const f = pcjsFrame(page, who === 'OVMXB' ? '0B' : '0C');
  return f ? f.evaluate(() => (window.__clusterNode || {}).nicTxCount ?? null).catch(() => null) : null;
}

// ------------------------------------------------------------------- writes

async function typeInto(page, who, text) {
  if (who === 'OVMXA') {
    const f = nodeAFrame(page);
    if (!f) throw new Error('node A frame absent');
    return f.evaluate((d) => window.__nodeWorker.postMessage({ t: 'in', d }), text);
  }
  const f = pcjsFrame(page, who === 'OVMXB' ? '0B' : '0C');
  if (!f) throw new Error(`${who} machine frame absent`);
  // VaxTerminal reads keydown on #screen -- the same path a visitor's key takes.
  return f.evaluate((s) => {
    const el = document.getElementById('screen');
    el.focus();
    for (const ch of s) {
      el.dispatchEvent(new KeyboardEvent('keydown',
        { key: ch === '\r' ? 'Enter' : ch, bubbles: true, cancelable: true }));
    }
  }, text);
}

// ONE write, line and terminator together. Measured (rd vms-e18e): sending the
// text and then the RETURN as two writes 300 ms apart makes OVMX's LOGINOUT
// abandon the username and respawn -- the console shows "SYSTEM", then the
// welcome banner and a fresh "Username:", never a Password: prompt. A single
// write is also what a paste looks like, which is the path the demo's own
// console wake already uses.
async function sendLine(page, who, line) {
  await typeInto(page, who, line + '\r');
}

async function waitFor(page, who, re, ms, label) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (re.test(await consoleOf(page, who))) { log(`  ${who}: ${label}`); return true; }
    await sleep(3000);
  }
  log(`  ${who}: TIMED OUT waiting for ${label}`);
  return false;
}

// The consoles are CAPPED scrollbacks (pcjs trims to MAX_LINES), so "everything
// after byte N" breaks the moment a long run trims. Each question is preceded by
// a sentinel the guest ECHOES, and the answer is read after its LAST occurrence.
let markSeq = 0;
async function sentinel(page, who, ms) {
  const tag = `E18EB-${++markSeq}`;
  await sendLine(page, who, `WRITE SYS$OUTPUT "${tag}"`);
  const t0 = Date.now();
  while (Date.now() - t0 < (ms || 150000)) {
    const c = await consoleOf(page, who);
    if ((c.match(new RegExp(tag, 'g')) || []).length >= 2) return tag;
    await sleep(2000);
  }
  return null;
}
async function afterSentinel(page, who, tag) {
  const c = await consoleOf(page, who);
  const i = c.lastIndexOf(tag);
  return i < 0 ? '' : c.slice(i + tag.length);
}

/** Log in and PROVE it with an echoed sentinel -- a prompt-shaped regex is not
 *  proof: this V7.3 volume's own boot transcript carries `$!' comment lines. */
/* Re-offer `line` until `want` shows up. MEASURED (rd vms-e18e): a single
 * well-formed attempt is not enough on a clustered node, because the executive
 * keeps writing operator lines to the SAME console LOGINOUT is reading --
 * "%DLM, refusing a lock message from a system that has not proved it runs this
 * implementation" every time the real VAX sends one -- and an attempt that
 * lands while one of those is being written is abandoned: the console shows the
 * username echoed, then the login banner and a fresh "Username:", never a
 * "Password:". Real VMS redisplays the prompt after a broadcast instead of
 * dropping the read (the same asymmetry cost this harness VAXC's login
 * yesterday, there via OPCOM). So: re-offer on a short beat until the prompt
 * this line is supposed to produce appears, and count how many it took, so the
 * transcript records the cost rather than hiding it. */
async function offerUntil(page, who, line, want, ms, label) {
  const t0 = Date.now();
  let tries = 0;
  while (Date.now() - t0 < ms) {
    tries++;
    await sendLine(page, who, line);
    for (let i = 0; i < 4; i++) {           // ~8 s of listening per offer
      await sleep(2000);
      if (want.test(await consoleOf(page, who))) {
        log(`  ${who}: ${label} (after ${tries} offer${tries === 1 ? '' : 's'})`);
        return tries;
      }
    }
  }
  log(`  ${who}: TIMED OUT waiting for ${label} (${tries} offers)`);
  log(`    ${who} tail: ` + JSON.stringify((await consoleOf(page, who)).slice(-300)));
  return 0;
}

async function login(page, who, user, pass, bootMs) {
  if (!await waitFor(page, who, /Username:/, bootMs, 'reached Username:')) return false;
  const uTries = await offerUntil(page, who, user, /Password:/, 300000, 'reached Password:');
  if (!uTries) return false;
  await sendLine(page, who, pass);
  for (let attempt = 1; attempt <= 4; attempt++) {
    if (await sentinel(page, who, 60000)) { log(`  ${who}: at DCL`); return true; }
    log(`  ${who}: sentinel did not echo (attempt ${attempt}); re-offering credentials`);
    if (!await waitFor(page, who, /Username:/, 60000, 'Username: again')) continue;
    if (!await offerUntil(page, who, user, /Password:/, 180000, 'reached Password:')) return false;
    await sendLine(page, who, pass);
  }
  return false;
}

// ------------------------------------------------------------- the readbacks

/** OVMX DCL SHOW CLUSTER: the distinct rows carrying a CSID. */
function ovmxMembers(text) {
  const seen = new Set();
  const re = /\|\s*(\S+)\s*\|\s*([0-9A-Fa-f]{8})\s*\|[^|]*\|\s*(MEMBER|LOCAL)\s*\|/g;
  let m;
  while ((m = re.exec(text.slice(-8000))) !== null) seen.add(`${m[1]}=${m[2]}`);
  return [...seen];
}
/** Real VMS SDA SHOW CLUSTER: the CSB list's own rows. */
function sdaMembers(text) {
  const seen = new Set();
  const re = /^\s*[0-9A-F]{8}\s+(\S+)\s+([0-9A-F]{8})\s+\d+\s/gm;
  let m;
  while ((m = re.exec(text.slice(-8000))) !== null) seen.add(`${m[1]}=${m[2]}`);
  return [...seen];
}

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
    await sleep(4000);
    return sdaMembers(fresh);
  }
  await sendLine(page, who, 'SHOW CLUSTER');
  await sleep(15000);
  return ovmxMembers(await afterSentinel(page, who, tag));
}

const BUGCHECK = /\*\*\*\s*FATAL BUGCHECK|Fatal BUG CHECK|BUGCHECK CODE|Kernel panic|\bOops:/i;
const LOSS = /lost connection|quorum lost|was removed from the cluster/i;

// ------------------------------------------------------------------- the run


/** Did the real VAX say, in its own words, that it admitted each OVMX node?
 *  Accumulated across polls (once true it stays true -- the console can trim it
 *  away, but it was said). */
async function refreshVaxcOracle(page, R) {
  const c = await consoleOf(page, 'VAXC');
  for (const n of ['OVMXA', 'OVMXB']) {
    if (!R.vaxc_oracle[n]) {
      R.vaxc_oracle[n] = new RegExp(
        `proposing addition of system ${n}|proposed addition of node ${n}`).test(c);
    }
  }
  return R.vaxc_oracle;
}

function demoUrl() {
  let u = `http://localhost:${PORT}/index.html?mac=${encodeURIComponent(MAC)}` +
          `&initramfs=${encodeURIComponent(INITRAMFS)}&sysdisk=${encodeURIComponent(SYSDISK)}`;
  if (NODE_B) u += `&nodeB=${encodeURIComponent(NODE_B)}`;
  if (NODE_C) u += `&nodeC=${encodeURIComponent(NODE_C)}`;
  return u;
}

(async () => {
  fs.mkdirSync(OUT_DIR, { recursive: true });
  const R = { pass: false, boot_order: BOOT_ORDER.join(','), announced: {}, vaxc_oracle: {},
              members: {}, samples: [], bugchecks: {}, losses: {}, vaxc_tx: {}, hold_s: 0, elapsed_s: 0 };
  const browser = await chromium.launch({ headless: true, args: ['--no-sandbox'] });
  const page = await browser.newPage();
  page.on('pageerror', (e) => log('[pageerr]', e.message));
  log('goto', demoUrl());
  await page.goto(demoUrl(), { waitUntil: 'load' });
  await page.waitForFunction(() => window.__demoReady === true, { timeout: 60000 });

  const t0 = Date.now();
  const snapshotter = setInterval(async () => {
    for (const w of NODES) {
      try { fs.writeFileSync(`${OUT_DIR}/${w}.console.log`, await consoleOf(page, w)); } catch (e) {}
    }
  }, 20000);

  const btn = { A: '#bootbtn-a', B: '#bootbtn-b', C: '#bootbtn-c' };
  for (const n of BOOT_ORDER) {
    log('boot', n);
    await page.click(btn[n]).catch((e) => log('click fail', e.message));
    if (STAGGER_MS) await sleep(STAGGER_MS);
  }

  // --- phase 1: wait for the cluster to announce itself. Type NOTHING yet. ---
  log('phase 1: waiting for each executive to announce membership (no input)');
  R.announced.OVMXA = await waitFor(page, 'OVMXA',
    /%CNXMAN, this node is now a VAXcluster member/, 1500000, 'announced MEMBER');
  R.announced.OVMXB = await waitFor(page, 'OVMXB',
    /%CNXMAN, this node is now a VAXcluster member/, 1800000, 'announced MEMBER');
  // The VAXC oracle is re-read every poll, never once: its pcjs console is a
  // CAPPED scrollback and LOGINOUT clears it, so an admission line that was
  // there at t0 can be gone at t1 -- and the SDA table below is the stronger
  // form of the same question anyway.
  await refreshVaxcOracle(page, R);

  // --- phase 2: now drive the consoles and read the tables back ---------------
  log('phase 2: logging in and reading each membership table');
  // Slowest console first. OVMXB is an OVMX/VAX guest under pcjs: it is a
  // cluster MEMBER minutes before its startup reaches Username:, so giving it
  // the tail of the budget is how a run ends with two tables instead of three.
  const li = {};
  li.OVMXB = await login(page, 'OVMXB', OVMX_USER, OVMX_PASS, 1800000);
  li.OVMXA = await login(page, 'OVMXA', OVMX_USER, OVMX_PASS, 600000);
  li.VAXC  = await login(page, 'VAXC',  VAXC_USER, VAXC_PASS, 600000);
  R.logged_in = li;
  log('logged in:', JSON.stringify(li));

  // --- phase 3: hold, re-asking, and watch the VAX keep talking --------------
  let firstAllAt = null;
  while (Date.now() - t0 < DEADLINE_MS) {
    const el = Math.round((Date.now() - t0) / 1000);
    const tx = {}; for (const w of NODES) tx[w] = await nicTxOf(page, w);
    R.vaxc_tx[el] = tx.VAXC;

    // A console that was not ready during phase 2 gets another chance here --
    // never give up on a node that is demonstrably a cluster member.
    for (const w of NODES) {
      if (!li[w]) {
        li[w] = await login(page, w, w === 'VAXC' ? VAXC_USER : OVMX_USER,
                                  w === 'VAXC' ? VAXC_PASS : OVMX_PASS, 120000);
        if (li[w]) { R.logged_in = li; log(`  ${w}: logged in on a later pass`); }
      }
    }
    const members = {};
    for (const w of NODES) { if (li[w]) { try { members[w] = await readMembership(page, w); } catch (e) { members[w] = ['ERR:' + e.message]; } } }
    R.members = members;
    const counts = Object.fromEntries(Object.entries(members).map(([k, v]) => [k, v.length]));

    for (const w of NODES) {
      const c = await consoleOf(page, w);
      if (BUGCHECK.test(c)) R.bugchecks[w] = true;
      if (LOSS.test(c)) R.losses[w] = (c.match(LOSS) || [''])[0];
    }

    // The bar: the two OVMX nodes each name three systems, the real VAX proposed
    // and completed both admissions in its own words, and it is STILL talking.
    const ovmxOk = ['OVMXA', 'OVMXB'].every((w) => (members[w] || []).length >= 3);
    await refreshVaxcOracle(page, R);
    const saidIt = R.vaxc_oracle.OVMXA && R.vaxc_oracle.OVMXB;
    const sdaOk = (members.VAXC || []).length >= 3;   // VAXC's own CSB list
    const oracleOk = saidIt || sdaOk;
    R.vaxc_oracle.sda_rows = (members.VAXC || []).length;
    const vaxAlive = tx.VAXC !== null && tx.VAXC > (R.vaxc_tx[Math.max(0, el - 120)] ?? -1);
    const allSee = ovmxOk && oracleOk;

    log(`t=${el}s members=${JSON.stringify(counts)} vaxcTx=${tx.VAXC} alive=${vaxAlive} allSee=${allSee}`);
    for (const [k, v] of Object.entries(members)) log(`    ${k}: ${JSON.stringify(v)}`);
    R.samples.push({ t: el, members, tx, allSee, vaxAlive });
    fs.writeFileSync(`${OUT_DIR}/cn3-result.json`, JSON.stringify(R, null, 2) + '\n');

    if (allSee) {
      if (firstAllAt === null) { firstAllAt = Date.now(); log(`*** CN=3 at t=${el}s -- holding ${HOLD_MS / 1000}s`); }
      if (Date.now() - firstAllAt >= HOLD_MS) {
        R.hold_s = Math.round((Date.now() - firstAllAt) / 1000);
        R.pass = Object.keys(R.bugchecks).length === 0 && Object.keys(R.losses).length === 0 && vaxAlive;
        break;
      }
    } else if (firstAllAt !== null) {
      log('*** membership LOST while holding -- resetting the hold clock');
      firstAllAt = null;
    }
    await sleep(45000);
  }

  clearInterval(snapshotter);
  R.elapsed_s = Math.round((Date.now() - t0) / 1000);
  for (const w of NODES) fs.writeFileSync(`${OUT_DIR}/${w}.console.log`, await consoleOf(page, w));
  await page.screenshot({ path: `${OUT_DIR}/cn3-final.png`, fullPage: true }).catch(() => {});
  fs.writeFileSync(`${OUT_DIR}/cn3-result.json`, JSON.stringify(R, null, 2) + '\n');
  log('CN3_BROWSER_RESULT=' + JSON.stringify({ pass: R.pass, hold_s: R.hold_s, elapsed_s: R.elapsed_s,
      announced: R.announced, oracle: R.vaxc_oracle, bugchecks: R.bugchecks, losses: R.losses }));
  await browser.close();
  process.exit(R.pass ? 0 : 1);
})();
