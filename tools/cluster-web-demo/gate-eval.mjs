// gate-eval.mjs — everything the visitor gate DECIDES, with no browser in it
// (rd vms-1a1).
//
// The gate's judgement is the part that has to be right: it has already once
// called a healthy CN=3 cluster a failure (it recomputed its observations from
// a capped scrollback, which SHRINKS), and once named a cause the evidence did
// not carry ("split brain") for a run that had in fact reached CN=3 and then
// lost the real VAX. Both are the kind of mistake that sends the next person
// hunting an executive bug that is not there.
//
// So the deciding lives here: pure functions over text and observations, no
// playwright, no filesystem, no clock -- unit-testable on a host with nothing
// but node (test/visitor-gate.test.mjs). visitor-gate.mjs drives the browser
// and hands its readings to these.

export const NODES = ['OVMXA', 'OVMXB', 'VAXC'];

// The matrix. Each row is a way a real person might use the page. The two that
// matter most are the FAST ones, and the ones that do not click Node C first:
// those are what the old grader never did, and what the field report came from.
// Throttled rows stand in for the laptop the visitor actually has.
export const MATRIX = [
  { label: 'page-order',             order: ['A', 'B', 'C'], gap: 5000, throttle: 1 },
  { label: 'all-at-once',            order: ['A', 'B', 'C'], gap: 500,  throttle: 1 },
  { label: 'page-order-throttled4',  order: ['A', 'B', 'C'], gap: 5000, throttle: 4 },
  { label: 'b-first',                order: ['B', 'A', 'C'], gap: 3000, throttle: 1 },
  { label: 'c-first-legacy',         order: ['C', 'A', 'B'], gap: 5000, throttle: 1 },
  { label: 'all-at-once-throttled2', order: ['A', 'B', 'C'], gap: 500,  throttle: 2 },
  // Node C boots AFTER an OVMX node is already up (rd vms-db1): a booting real
  // VAX then sends that node its system-disk closes, and a node that does not
  // answer them leaves the VAX hung in startup -- a member that never offers
  // Username:. Visitors choose the order, so the matrix does too. `after`
  // holds a click until the named node's own console has offered Username:
  // (Node A's page strikes its OPA0 RETURN itself; nothing is typed).
  { label: 'c-after-a-up',           order: ['A', 'C', 'B'], gap: 3000, throttle: 1, after: { C: 'OVMXA' } },
  { label: 'c-last-after-a-up',      order: ['A', 'B', 'C'], gap: 3000, throttle: 1, after: { C: 'OVMXA' } },
];

// How long a held click waits for its node before it is made anyway (and the
// wait recorded) -- a node that never comes up must not stall the matrix.
export const AFTER_WAIT_MS = 1200000;

// The three system ids the roster declares (mk_democonfig.py DEMO_ROSTER).
export const WANT = ['0x7c3', '0x7c4', '0x7c5'];

// What each node says on its own console.
export const MEMBER = /%CNXMAN,\s*this node is now a VAXcluster member/;
// Must NEVER appear on an OVMX node: the OVMX pair is non-voting and cannot
// found a cluster. If it does, the roster regressed (rd vms-1a1).
export const FOUNDED = /%CNXMAN,\s*this node has quorum by its own votes/;
export const ADDED = /%CNXMAN, system 0*([0-9a-f]+) was added to the cluster/g;
export const VADD = /proposing addition of system (\S+)|proposed addition of node (\S+)/g;
// Who a node says it lost, in the node's OWN words. The real VAX names the
// peer ("%CNXMAN,  lost connection to system OVMXB"); OVMX's connection manager
// often does not ("lost connection to a cluster member, reconnecting") and its
// executive names the system by id ("vms: SCS path lost to system 0:1988").
// Matching \S+ against the unnamed form yielded the peer name "a", which is how
// a report stops being evidence.
export const LOST = /lost connection to (?:system|node) ([A-Z][A-Z0-9$]{0,5})|SCS path lost to system (\d+:\d+)/g;
// A loss the node reports without naming anyone. Counted, never guessed at.
export const LOST_UNNAMED = /lost connection to a cluster member|lost the VMS\$VAXcluster connection before this node was admitted/g;
export const BUG = /\*\*\*\s*Fatal BUG CHECK|\*\*\*\s*FATAL BUGCHECK|CNXMGRERR|BUGCHECK CODE|Kernel panic|\bOops:/i;
// A KA655 power-on self-test banner. Seeing it twice in one run's transcript
// means the guest restarted -- a fact worth reporting, and not one to infer
// from a screenshot taken after the fact.
export const POWERON = /KA655-B V5\.3, VMB/g;

// NetBSD's own boot output on an OVMX/VAX console. rd vms-553 silenced the
// substrate AT THE SOURCE (an OVMX_QUIET kernel option plus a quiet secondary
// bootstrap), and the pcjs page no longer filters anything, so the RAW console
// is the claim: an OpenVMX/VAX node shows the VMS personality and nothing else.
//
// Each marker is a line the substrate used to print, taken from a console this
// repo already keeps -- tests/lab/captures/vms-e18e-cn3-live-v072-20260926/
// OVMXB.console.log, the V0.7-2 deploy:
//
//   >> NetBSD/vax boot [1.12 (Mon Dec 16 13:08:11 UTC 2024)] <<
//   [ 1.0000000] NetBSD 10.1 (OVMX) #9: Sat Aug 29 08:54:46 UTC 2026
//   [ 1.0000000] Copyright (c) 1996, ... The NetBSD Foundation, Inc. ...
//   [ 1.0000000] MicroVAX 3800/3900
//   [ 1.0000000] total memory = 16328 KB / avail memory = 11144 KB
//   [ 1.0000000] Detecting hardware...
//
// What is deliberately NOT here: OVMX's own substrate announcement (it NAMES
// the substrate -- "OVMX/NetBSD-vax -- SYSKRNL (NetBSD kernel)" -- and that is
// the VMS personality speaking, which is what we want on the console), the
// KA655 firmware banner (that is the machine's ROM, not NetBSD), and anything
// matching on the bracketed-uptime form alone (the executive's own operator
// lines print that way too).
export const NETBSD_NOISE = [
  { name: 'secondary bootstrap banner', re: />>\s*NetBSD\/vax boot\s*\[/g },
  { name: 'kernel version line',        re: /NetBSD \d+\.\d+[^\n]*#\d+/g },
  { name: 'NetBSD Foundation copyright', re: /The NetBSD Foundation, Inc\./g },
  { name: 'machine identification',     re: /MicroVAX \d{4}\/\d{4}/g },
  { name: 'memory sizing',              re: /(?:total|avail) memory = \d+ KB/g },
  { name: 'device autoconfiguration',   re: /Detecting hardware\.\.\./g },
  { name: 'root mount',                 re: /root on \w+\d+\w* dumps on|root file system type:/g },
];

// Which substrate markers a console shows, and how many times. {} is the claim
// rd vms-553 makes; anything else is the console telling on it.
export function netbsdNoise(text) {
  const t = String(text || '');
  const out = {};
  for (const { name, re } of NETBSD_NOISE) {
    const n = (t.match(new RegExp(re.source, 'g')) || []).length;
    if (n) out[name] = n;
  }
  return out;
}

// Every capture group match of a /g regex, de-duplicated, in first-seen order.
export function setOf(text, re) {
  const seen = new Set();
  const r = new RegExp(re.source, re.flags.includes('g') ? re.flags : re.flags + 'g');
  let m;
  while ((m = r.exec(text)) !== null) seen.add(m[1] || m[2]);
  return [...seen];
}

// How many times a guest restarted, per its own transcript.
export const restartsIn = (transcript) =>
  Math.max(0, (String(transcript || '').match(POWERON) || []).length - 1);

// Stitch a capped scrollback into a growing transcript.
//
// `cur` is the whole screen as it is NOW. These screens are capped -- pcjs
// trims its terminal to MAX_LINES, node.html keeps the last 40000 characters --
// so the part of `cur` we have already recorded is a suffix of `acc` and only
// the remainder is new. An overlap of zero means the guest cleared its screen,
// i.e. it restarted, and then the whole screen is new, which is exactly right.
//
// This is what makes an observation MONOTONIC: a node does not un-join because
// its console scrolled.
//
// THE CURSOR IS NOT OUTPUT. pcjs paints its cursor into the screen text as a
// block glyph at the write position -- the end of the screen, often on a line
// of its own -- and the next poll has printed where it stood. Left in, the
// recorded tail never matches the next screen's head, the WHOLE screen is
// appended again on every poll, and each copy re-reads the power-on banner as
// a restart. MEASURED, deploy V0.7-3 pass1 all-at-once: "OVMXB restarted 33x"
// was one boot whose screen was re-appended 33 times (the copies end at
// uptimes 98, 152, 206 ... 1858 s -- one clock, never reset).
const CURSOR_TAIL = /[\s\u2588]+$/u;
const tidyScreen = (s) => String(s || '').replace(CURSOR_TAIL, '');

// The longest prefix of `cur` that `acc` ends with, stitched; null if none.
function stitchOnto(acc, cur) {
  for (let k = Math.min(acc.length, cur.length); k > 0; k--) {
    if (acc.endsWith(cur.slice(0, k))) return acc + cur.slice(k);
  }
  return null;
}

export function joinScrollback(acc, cur) {
  acc = tidyScreen(acc);
  cur = tidyScreen(cur);
  if (!cur) return acc;
  if (!acc) return cur;
  const exact = stitchOnto(acc, cur);
  if (exact !== null) return exact;
  // THE CURSOR'S LINE IS PROVISIONAL. The terminal may still rewrite the line
  // it is on -- a countdown redrawn with CR ("autoboot 2" -> "autoboot 0"),
  // the cursor block painted over the character the next poll shows there --
  // so before calling it a cleared screen, match against the settled lines
  // only and let the new screen supply the last one.
  const cut = acc.lastIndexOf('\n');
  if (cut >= 0) {
    const settled = stitchOnto(acc.slice(0, cut + 1), cur);
    if (settled !== null) return settled;
  }
  // No overlap: the guest cleared its screen. The line break the tidy took off
  // the old tail goes back, so the two screens stay two lines.
  return acc + '\n' + cur;
}

// Fold one poll of one node's screen into the run's observations, in place.
// Observations only ever grow (see joinScrollback).
export function observe(R, who, screen) {
  R.transcript[who] = joinScrollback(R.transcript[who], screen);
  const t = R.transcript[who];
  R.restarts[who] = restartsIn(t);
  if (MEMBER.test(t)) R.member[who] = true;
  R.added[who] = setOf(t, ADDED).map((h) => '0x' + h.replace(/^0x/, ''));
  const lost = setOf(t, LOST);
  if (lost.length) R.lost[who] = lost;
  const unnamed = (t.match(LOST_UNNAMED) || []).length;
  if (unnamed) R.lost_unnamed[who] = unnamed;
  if (BUG.test(t)) R.bugchecks[who] = (t.match(BUG) || [''])[0];
  if (who !== 'VAXC' && FOUNDED.test(t)) R.ovmx_founded[who] = true;
  if (who === 'VAXC') R.vaxc_admitted = setOf(t, VADD);
  // The OVMX/VAX node's raw console must carry no NetBSD boot output (rd
  // vms-553). Node C is a real VAX/VMS machine and node A is Linux-substrate:
  // neither has a NetBSD substrate to be quiet about.
  if (who === 'OVMXB') {
    const noise = netbsdNoise(t);
    if (Object.keys(noise).length) R.substrate_noise[who] = noise;
  }
  return R;
}

// The pass bar: both OVMX nodes' own connection managers name all three
// systems, AND the real VAX admitted both of them in its own words. Nothing
// here is inferred from the other nodes' consoles.
export const isCN3 = (R) =>
  NODES.slice(0, 2).every((w) => WANT.every((id) => (R.added[w] || []).includes(id))) &&
  (R.vaxc_admitted || []).length >= 2;

// Did revealing a panel expose the page defect in rd vms-0bc?
//
// Not "the console moved while we revealed it": a booting guest prints all the
// time, and counting that called every healthy panel stalled. The signature is
// that the panel showed NOTHING NEW for a whole poll interval -- the text is
// byte-identical to the previous poll -- and then being scrolled into view
// produced output.
export const isRepaintStall = (lastSeen, before, after) =>
  lastSeen !== undefined && before === lastSeen && after.length > before.length + 8;

// Should the gate scroll a panel into view before reading it?
//
// It did, unconditionally, because an offscreen panel used to stop repainting
// (rd vms-0bc). The embed now repaints on a timer, so the scrolling buys
// nothing -- and it COSTS: scrolling three canvas-heavy cross-origin iframes
// into view every 15s starved node A's qemu-wasm worker badly enough to fail
// the run. MEASURED on the live V0.7-4 page, same pod, same hour: 6 of 12 runs
// failed with 'no SCA frames from OVMXA' with the scrolling on, and 6 of 6
// passed with it off. A grader must not be the reason a node does not boot.
//
// So it is opt-in now, for diagnosing a repaint regression, never for grading.
export const wantsReveal = (env = {}) => env.REVEAL === '1' && env.NO_REVEAL !== '1';

// A fresh, empty set of observations.
export const newObservations = () => ({
  transcript: {}, restarts: {}, member: {}, added: {}, lost: {},
  vaxc_admitted: [], ovmx_founded: {}, bugchecks: {}, sca: {}, cn3: false, lost_unnamed: {},
  // How many times a panel's console only advanced after it was scrolled into
  // view -- the page defect in rd vms-0bc, counted rather than hidden.
  repaint_stalls: {},
  // rd vms-bfdd: whether a pcjs node's own node-pcjs.html wrapper ever saw a
  // sign of life (__nodeState.started) and whether its boot watchdog fired
  // (an HONEST "this did not start" the visitor can see and retry, not a
  // silent hang). Populated by the caller from the node-pcjs.html frame, not
  // from the inner pcjs machine frame (consoleOf reads) -- the watchdog lives
  // one frame up, specifically so it survives the inner frame never loading.
  panel_started: {}, panel_watchdog_fired: {},
  // NetBSD boot output seen on the OVMX/VAX node's raw console (rd vms-553).
  substrate_noise: {},
  // rd vms-4ff: Node A is the qemu-wasm node, and it had NO counterpart to the
  // pcjs nodes' panel_started/panel_watchdog_fired above. Its own node.html
  // wrapper publishes worker-level state on window.__nodeState -- whether the
  // worker was spawned, whether the guest ever wrote to the console, whether
  // the qemu module halted or aborted, how many frames the guest actually put
  // on the wire -- and the gate read NONE of it, only consoleText. So a Node A
  // that stopped was recorded as "a console that ends at %OVMX-I-MOUNTED" and
  // nothing else: a worker that aborted, a worker that errored, and a guest
  // still grinding through the ACP staging phase were indistinguishable in the
  // evidence, which is why two V0.7-7 failures could not be root-caused from
  // what the run kept. Observed, monotonic, never inferred.
  nodeA_worker: {},
  // rd vms-db1: per node, when (seconds since the first click) it first offered
  // Username:, when the gate sent its login (the transcript length at that
  // moment), whether the login was refused, and when a DCL `$` followed.
  username_at: {}, login_sent: {}, login_failed: {}, dcl_at: {},
});

// A SILENT never-start: the raw console stayed empty (nothing to show a
// visitor) AND the node's own wrapper never reported a sign of life AND its
// watchdog never fired to say so. This is the exact defect rd vms-bfdd
// captured (0-byte console, zero hub frames, 586s, no message to the
// visitor) -- the one failure the watchdog exists to turn into an HONEST one.
// A node the matrix never clicked (VAXC in a 2-node check, say) also reads
// "not started", so this is only meaningful for nodes the caller actually
// knows were booted; the caller filters by its own click list.
export function silentNeverStarts(R, pcjsNodes) {
  return pcjsNodes.filter((w) =>
    !(R.transcript[w] || '').length && !R.panel_started[w] && !R.panel_watchdog_fired[w]);
}

// ---- Node A's own worker state (rd vms-4ff) -------------------------------
//
// The three groups of window.__nodeState fields node.html publishes for the
// qemu-wasm node (demo/cluster/node.html in openvmx-site). Named here so the
// gate records the node's OWN report of itself and not a guess from its
// console: a flag that latched, a counter that moved, a fault it declared.
export const NODEA_FLAGS = ['workerSpawned', 'pipeReady', 'firstOut', 'acpOk'];
// workerTicks is the worker's OWN event-loop heartbeat (1 Hz, node-worker.js),
// and it is a statement about the WORKER, not the guest: the qemu-wasm module
// can hold that thread between yields. Frozen ticks say this worker is not being
// scheduled; ticks advancing against a console that has not moved say it is, and
// the guest is not printing. Absent on a deploy older than rd vms-4ff, which
// reads as absent -- never as 0.
export const NODEA_COUNTERS = ['nicTxCount', 'nicRxCount', 'nicRxDeliverOk', 'workerTicks'];
export const NODEA_FAULTS = ['halt', 'workerError'];

// Fold one sample of Node A's __nodeState into the observations, in place.
// MONOTONIC for the same reason the transcript is: flags latch, counters only
// grow, and the FIRST fault seen is kept -- a frame that reloads (or a worker
// that stops answering) must not erase a halt it already reported.
export function observeNodeAWorker(R, st) {
  if (!st) return R;
  const W = (R.nodeA_worker = R.nodeA_worker || {});
  for (const k of NODEA_FLAGS) if (st[k]) W[k] = true;
  for (const k of NODEA_COUNTERS) {
    const n = Number(st[k]);
    if (Number.isFinite(n)) W[k] = Math.max(W[k] || 0, n);
  }
  for (const k of NODEA_FAULTS) if (st[k] && !W[k]) W[k] = String(st[k]).slice(0, 200);
  return R;
}

// The last line a console actually printed -- what a stalled node ends on.
// Trailing blank lines and the cursor's own line are not output (joinScrollback
// has the long version of why).
export function lastLineOf(text) {
  const lines = String(text || '').split('\n')
    .map((s) => s.replace(/[\s█]+$/u, '')).filter((s) => s.length);
  return lines.length ? lines[lines.length - 1] : '';
}

// Why Node A has nothing on the wire, IN ITS OWN REPORT OF ITSELF -- or null
// when it is talking (then there is nothing to explain).
//
// Every branch is a reading, not a diagnosis: the gate has twice named a cause
// its evidence did not carry (this file's header), and "Node A stalled" is
// exactly that kind of sentence. What it may say is which of the node's own
// facts held: the worker declared an error, the module halted, the console
// never produced a byte, or the console stopped at a named line with the
// worker reporting neither fault.
export function nodeAStallWhy(R) {
  const sca = (R.sca || {}).OVMXA || 0;
  if (sca > 0) return null;
  const W = R.nodeA_worker || {};
  const tail = lastLineOf((R.transcript || {}).OVMXA);
  if (W.workerError) return `Node A's worker declared an error: ${JSON.stringify(W.workerError)}`;
  if (W.halt) return `Node A's qemu module halted: ${JSON.stringify(W.halt)}`;
  if (!W.workerSpawned && !Object.keys(W).length) {
    return 'Node A\'s worker state was never readable (its node.html frame answered nothing)';
  }
  if (!W.firstOut) {
    return `Node A's worker was spawned but the guest never wrote to the console ` +
           `(pipeReady=${!!W.pipeReady})`;
  }
  const beat = Number.isFinite(W.workerTicks)
    ? `, worker event-loop ticks=${W.workerTicks}` : ', worker event-loop ticks not reported';
  return `Node A's console stops after ${JSON.stringify(tail)} with its worker reporting ` +
         `no halt and no error (nicTx=${W.nicTxCount || 0}, ACP mount seen=${!!W.acpOk}${beat})`;
}

// Is the OVMX/VAX node's raw console free of NetBSD boot output (rd vms-553)?
export const isQuiet = (R) => !Object.keys(R.substrate_noise || {}).length;

// ---- Every node reaches a logged-in DCL prompt (rd vms-db1 / vms-52d) -------
//
// CN=3 is not the end of a visitor's run. A real VAX can be admitted, so the
// cluster reads CN=3 on every console, and still hang in its own startup and
// never offer Username: -- measured when Node C boots after an OVMX node and
// asks it to serve its system-disk closes (rd vms-db1). A visitor sees a dead
// console. So the run passes only if EVERY node is usable: each OVMX node
// offers Username: and then, logged in with the login the site publishes, a
// DCL `$`; the real VAX offers Username: -- which is the proof its startup
// completed, exactly what vms-db1/vms-52d break. The gate never logs in to the
// real VAX: its password is not published and is not needed for that proof.
//
// The gate still types nothing until the cluster has formed (the membership
// claim stays "with nobody touching a console"); after CN=3 it does what a
// visitor does: strikes RETURN on a console that has not offered Username:,
// and logs in on the OVMX nodes where they have.
//
// The nodes the gate logs in to. The real VAX (VAXC) is not one of them.
export const LOGIN_NODES = ['OVMXA', 'OVMXB'];
export const USERNAME = /Username:/;
export const AUTH_FAIL = /User authorization failure/i;

// The OVMX login the site publishes to visitors -- the ONE source the gate
// reads it from, so there is nothing to supply by hand and nothing to drift.
// The homepage's figcaption reads: "Log in with username <b>SYSTEM</b>,
// password <b>MANAGER</b>". Returns {user, pass} or null when the page does
// not say (then the gate refuses to run rather than guess).
export function parseLoginHint(html) {
  const m = /Log in with username\s*<b>\s*([A-Za-z0-9$_]+)\s*<\/b>\s*,?\s*password\s*<b>\s*([^<\s]+)\s*<\/b>/i
    .exec(String(html || ''));
  return m ? { user: m[1], pass: m[2] } : null;
}

// The node's last printed line is a bare DCL prompt.
export const atDcl = (text) => lastLineOf(text).trim() === '$';

// Fold one node's login progress into the observations, in place, at elapsed
// time `el` (seconds since the first click). Monotonic like everything else:
// the first time a node offered Username: and the first time it showed a DCL
// prompt AFTER the gate sent its password are kept.
export function observeLogin(R, who, el) {
  const t = String((R.transcript || {})[who] || '');
  R.username_at = R.username_at || {};
  R.dcl_at = R.dcl_at || {};
  R.login_sent = R.login_sent || {};
  R.login_failed = R.login_failed || {};
  if (R.username_at[who] === undefined && USERNAME.test(t)) R.username_at[who] = el;
  const sent = R.login_sent[who];
  if (sent !== undefined && R.dcl_at[who] === undefined) {
    const after = t.slice(sent);
    if (AUTH_FAIL.test(after)) R.login_failed[who] = true;
    else if (after.length && atDcl(t)) R.dcl_at[who] = el;
  }
  return R;
}

// What the gate should do at a node's console now, or null. 'wake' = strike
// RETURN (a VMS console offers Username: only when asked), 'login' = type the
// user name and password. Nothing is typed before the cluster formed.
export function loginAction(R, who) {
  if (!R.cn3) return null;
  if (!LOGIN_NODES.includes(who)) {
    // The real VAX: wake its console until it offers Username:, then stop.
    return (R.username_at || {})[who] === undefined ? 'wake' : null;
  }
  if ((R.dcl_at || {})[who] !== undefined || (R.login_failed || {})[who]) return null;
  if ((R.login_sent || {})[who] !== undefined) return null;
  if ((R.username_at || {})[who] === undefined) return 'wake';
  return USERNAME.test(lastLineOf((R.transcript || {})[who])) ? 'login' : 'wake';
}

// When a node became usable (seconds since the first click), or undefined: an
// OVMX node at its logged-in DCL prompt, the real VAX at Username:.
export const readyAt = (R, w) =>
  LOGIN_NODES.includes(w) ? (R.dcl_at || {})[w] : (R.username_at || {})[w];

// Every node usable within the bound (Infinity = no bound beyond the run's own
// length).
export const allReady = (R, boundS = Infinity) =>
  NODES.every((w) => readyAt(R, w) !== undefined && readyAt(R, w) <= boundS);

// The run's pass bar: the cluster formed, the console a visitor reads shows the
// VMS personality only, AND every node is usable within the bound: each OVMX
// node at a logged-in DCL prompt, the real VAX at Username:. Membership alone has
// not been the bar since V0.7-6 (quiet), nor since V0.7-9 (rd vms-db1: a real
// VAX that is a member and never offers Username: is a dead demo).
export const isPass = (R, boundS = R.login_bound_s ?? Infinity) =>
  !!R.cn3 && isQuiet(R) && allReady(R, boundS);

// Why a clustered run did not have every node usable, in observed terms.
function loginWhy(R, boundS) {
  const why = [];
  for (const w of NODES) {
    const u = (R.username_at || {})[w], d = (R.dcl_at || {})[w];
    const login = LOGIN_NODES.includes(w);
    if ((R.login_failed || {})[w]) why.push(`${w} refused the login (User authorization failure)`);
    else if (u === undefined) why.push(`${w} never offered Username: (last line ${JSON.stringify(lastLineOf((R.transcript || {})[w]))})`);
    else if (login && d === undefined) why.push(`${w} offered Username: at t=${u}s but never showed a DCL $ after login`);
    else if (login && d > boundS) why.push(`${w} reached DCL at t=${d}s, past the ${boundS}s bound`);
    else if (!login && u > boundS) why.push(`${w} offered Username: at t=${u}s, past the ${boundS}s bound`);
  }
  return why.join('; ');
}

// How a console that is not quiet is said, once, in one place.
const noiseWhy = (R) =>
  `NetBSD substrate lines on the OVMX/VAX console: ${JSON.stringify(R.substrate_noise)} (rd vms-553)`;

// State the outcome in terms of what was OBSERVED, never a diagnosis the
// evidence does not carry.
export function verdictOf(R) {
  if (Object.keys(R.ovmx_founded || {}).length) {
    return `REGRESSION: an OVMX node founded a cluster (${Object.keys(R.ovmx_founded)}) ` +
           `-- the OVMX nodes must be NON-VOTING (rd vms-1a1)`;
  }
  if (Object.keys(R.bugchecks || {}).length) return `BUGCHECK: ${JSON.stringify(R.bugchecks)}`;
  if (R.cn3) {
    if (!isQuiet(R)) return `CN=3 but NOT QUIET: ${noiseWhy(R)}`;
    const boundS = R.login_bound_s ?? Infinity;
    return allReady(R, boundS) ? 'CN=3' : `CN=3 but NOT EVERY NODE USABLE: ${loginWhy(R, boundS)}`;
  }
  const why = [];
  for (const w of NODES.slice(0, 2)) {
    const miss = WANT.filter((id) => !(R.added[w] || []).includes(id));
    if (miss.length) why.push(`${w} never named ${miss.join(',')}`);
  }
  const adm = R.vaxc_admitted || [];
  if (adm.length < 2) why.push(`the real VAX admitted ${adm.length ? adm.join('+') : 'nobody'}`);
  for (const w of NODES) if ((R.restarts || {})[w]) why.push(`${w} restarted ${R.restarts[w]}x`);
  const quiet = NODES.filter((w) => !(R.sca || {})[w]);
  if (quiet.length) why.push(`no SCA frames from ${quiet.join(',')}`);
  // rd vms-4ff: "no SCA frames from OVMXA" is where this gate's reports used to
  // stop. Node A's own worker state says more, so it is said here.
  const aWhy = nodeAStallWhy(R);
  if (aWhy) why.push(aWhy);
  if (Object.keys(R.lost || {}).length) why.push(`lost: ${JSON.stringify(R.lost)}`);
  if (Object.keys(R.lost_unnamed || {}).length) {
    why.push(`unnamed connection losses: ${JSON.stringify(R.lost_unnamed)}`);
  }
  const stalls = R.repaint_stalls || {};
  if (Object.keys(stalls).length) {
    why.push(`panels that only repainted once revealed: ${JSON.stringify(stalls)} (rd vms-0bc)`);
  }
  if (!isQuiet(R)) why.push(noiseWhy(R));
  const silent = silentNeverStarts(R, ['OVMXB', 'VAXC'].filter((w) => w in (R.panel_started || {})));
  if (silent.length) {
    why.push(`SILENT never-start (no console, no watchdog): ${silent.join(',')} (rd vms-bfdd)`);
  }
  return `NOT CN=3: ${why.join('; ')}`;
}
