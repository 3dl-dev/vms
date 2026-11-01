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

// What each node says on its own console -- in the words VMS itself prints
// there (rd vms-c56). A joining VMS node announces its own membership, naming
// itself: "%CNXMAN,  now a VAXcluster member -- system VAX2" (two spaces;
// tests/lab/captures/vms-52d-0b01-oracle-20261013/armA-vax2-console.log, and
// VAXC's own console in the demo). It prints nothing on its console when a
// LATER node joins -- that is OPCOM's, from the coordinator -- so who else each
// OVMX node holds as a member is read from its own SHOW CLUSTER (clusterViewOf
// below), the executive's table, after the gate has logged in.
export const MEMBER = /%CNXMAN,\s+now a VAXcluster member -- system ([A-Z0-9$_]{1,6})/g;
// Must NEVER appear on an OVMX node: the OVMX pair is non-voting and cannot
// found a cluster. If it does, the roster regressed (rd vms-1a1). VMS's console
// form for founding is "proposing formation of a VAXcluster"; the older OVMX
// wording is still caught.
export const FOUNDED = /%CNXMAN,\s+proposing formation of a VAXcluster|%CNXMAN,\s*this node has quorum by its own votes|%CNXMAN,\s*the systems this node can see have quorum/;
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

// ---- Every OVMX node runs the build under test ------------------------------
//
// The page has two OVMX nodes, and for V0.7-8..V0.7-10 only ONE of them was
// rebuilt per release: Node B stayed a V0.7-7 image built by hand, so every
// executive fix the gate was graded on never reached it, and the gate passed or
// failed a mixed cluster without saying so. An OVMX console says what it is
// ("    OpenVMX V0.7-10 - OpenVMS-compatible" at boot, "Welcome to OpenVMX
// V0.7-10" at login), so the gate reads it there: each OVMX node must announce
// the version under test and no other. The version comes from the vms tree the
// gate itself was checked out at (ovmx_identity.h OVMX_PRODUCT_VERSION, the one
// source), not from anything typed in.
export const OVMX_VERSION = /OpenVMX (V\d+\.\d+-\d+)\b/g;

// OVMX_PRODUCT_VERSION out of ovmx_identity.h's text, or null.
export function productVersionOf(identityHeader) {
  const m = /^#define\s+OVMX_PRODUCT_VERSION\s+"([^"]+)"/m.exec(String(identityHeader || ''));
  return m ? m[1] : null;
}

// Why the OVMX nodes are not all the build under test, or null when they are.
// A node that never announced any version is not "the build under test" either:
// absence is not evidence of the right image.
export function versionWhy(R) {
  const want = R.want_version;
  if (!want) return 'the gate was not told which OpenVMX version is under test';
  const why = [];
  for (const w of LOGIN_NODES) {
    const seen = (R.ovmx_versions || {})[w] || [];
    if (!seen.length) why.push(`${w} never announced an OpenVMX version`);
    else if (seen.some((v) => v !== want)) why.push(`${w} announced ${seen.join('+')}`);
  }
  return why.length ? `${why.join('; ')} -- the build under test is ${want}` : null;
}

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
  // A member in its OWN words: the line must name this node, not another.
  if (setOf(t, MEMBER).includes(who)) R.member[who] = true;
  if (LOGIN_NODES.includes(who)) {
    const view = clusterViewOf(t);
    if (view) R.cluster_view[who] = view;
    // rd vms-c56: the console carries only what a VMS console would.
    const chat = consoleChatter(t);
    if (chat.length) R.chatter[who] = chat.slice(0, 20); else delete R.chatter[who];
  }
  const lost = setOf(t, LOST);
  if (lost.length) R.lost[who] = lost;
  const unnamed = (t.match(LOST_UNNAMED) || []).length;
  if (unnamed) R.lost_unnamed[who] = unnamed;
  if (BUG.test(t)) R.bugchecks[who] = (t.match(BUG) || [''])[0];
  if (who !== 'VAXC' && FOUNDED.test(t)) R.ovmx_founded[who] = true;
  if (who === 'VAXC') R.vaxc_admitted = setOf(t, VADD);
  if (LOGIN_NODES.includes(who)) {
    R.ovmx_versions = R.ovmx_versions || {};
    const had = R.ovmx_versions[who] || [];
    R.ovmx_versions[who] = [...new Set([...had, ...setOf(t, OVMX_VERSION)])];
  }
  // The OVMX/VAX node's raw console must carry no NetBSD boot output (rd
  // vms-553). Node C is a real VAX/VMS machine and node A is Linux-substrate:
  // neither has a NetBSD substrate to be quiet about.
  if (who === 'OVMXB') {
    const noise = netbsdNoise(t);
    if (Object.keys(noise).length) R.substrate_noise[who] = noise;
  }
  return R;
}

// The membership bar, as each node says it in VMS's own words (rd vms-c56):
// both OVMX nodes announce that THEY are now VAXcluster members, AND the real
// VAX proposed the addition of each of them by name. Nothing here is inferred
// from another node's console. That every OVMX node holds ALL THREE systems as
// members is the executive's own table -- its SHOW CLUSTER -- read once the
// gate is logged in (clusterViewWhy, part of isPass).
export const isCN3 = (R) =>
  LOGIN_NODES.every((w) => !!(R.member || {})[w]) &&
  LOGIN_NODES.every((w) => (R.vaxc_admitted || []).includes(w));

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
  transcript: {}, restarts: {}, member: {}, lost: {},
  // rd vms-c56: the MEMBER rows of each OVMX node's own SHOW CLUSTER, and the
  // console lines a real VAX would not print, per OVMX node.
  cluster_view: {}, chatter: {},
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
  // Every OpenVMX version each OVMX node announced on its own console, and the
  // one under test (set by the caller from ovmx_identity.h). See versionWhy().
  ovmx_versions: {}, want_version: null,
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
  // NUL is the fill character VMS writes before a prompt (the keystroke
  // oracle's real-VAX login: "<CR><NUL>$"); a terminal displays nothing for it,
  // so it is not part of what the line printed.
  const lines = String(text || '').split('\n')
    .map((s) => s.replace(/\u0000/gu, '').replace(/[\s█]+$/u, '')).filter((s) => s.length);
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
    else if (LOGIN_EXPIRED.test(after)) forgetLogin(R, who, 'the login prompt timed out after the login was typed');
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

// An OVMX console's login prompt does not wait for ever: Username: is abandoned
// after 20 s of guest time ("Error reading command input / Timeout period
// expired"), the line goes quiet, and the NEXT RETURN brings a fresh banner and
// prompt. A gate that strikes RETURN and then reads the console again a whole
// poll later (15 s of sleep plus three panel reveals) sees the prompt only after
// it has timed out again, and never types the user name (rd vms-124: OVMXB
// offered Username: in 8 of 8 cases and was never logged in to). So after a
// wake the gate looks again every WAKE_FOLLOWUP_MS and logs in the moment the
// prompt is the last line. What it asserts is unchanged.
export const WAKE_FOLLOWUP_POLLS = 12;
export const WAKE_FOLLOWUP_MS = 1000;

// A visitor who types a user name and finds the console answering with a FRESH
// "Username:" (the first prompt had already expired when the keys landed), or
// with "Timeout period expired", presses RETURN and starts over: that is what
// VMS does and what OVMX reproduces faithfully (LGI_RETRY_TMO, 20 s -- rd
// vms-29e). The gate used to type the user name, sleep, type the password and
// never look again, so ONE late landing -- on a guest that is slow under CPU
// throttling -- left the password to be taken as a user name and the node
// stranded at "Username: MANAGER / Password: / Timeout" with nobody typing
// any more (rd vms-25f: not a wedged console -- the executive's terminal driver
// was bound, idle and healthy; the gate had stopped sending anything).
// Now the password is sent only once the console says "Password:", and a login
// that expires or lands out of step is retried -- at most LOGIN_RETRIES times
// per node, each COUNTED in R.login_retries and logged, so slowness stays
// visible. What the gate ASSERTS is unchanged: a DCL "$" within the bound.
export const LOGIN_RETRIES = 3;
export const PASSWORD_WAIT_POLLS = 12;
export const PASSWORD_WAIT_MS = 1000;
export const PASSWORD_PROMPT = /Password:/;
export const LOGIN_EXPIRED = /Timeout period expired/;

async function typeLogin(R, who, el, io) {
  const sentAt = (R.transcript[who] || '').length;
  R.login_sent[who] = sentAt;
  io.log(`  ${who}: offered Username: -- logging in as ${io.user(who)}`);
  await io.type(who, io.user(who) + '\r');
  let ready = false;
  for (let i = 0; i < PASSWORD_WAIT_POLLS; i++) {
    await io.sleep(PASSWORD_WAIT_MS);
    observe(R, who, await io.read(who));
    const after = String((R.transcript[who] || '')).slice(sentAt);
    if (PASSWORD_PROMPT.test(after)) { ready = true; break; }
    if (LOGIN_EXPIRED.test(after) || AUTH_FAIL.test(after)) break;
    // a FRESH prompt: the typed name was taken as the wake keystroke of a new session
    if (/Username:\s*$/.test(lastLineOf(after)) && /Username:/.test(after)) break;
  }
  if (!ready) return retryLogin(R, who, io, 'no Password: prompt after the user name');
  await io.type(who, io.pass(who) + '\r');
  return 'login';
}

// Forget a login attempt that did not land so loginAction wakes the console and
// logs in again. Bounded; every retry is counted and said (R.login_retry_log).
function retryLogin(R, who, io, why) {
  if (!forgetLogin(R, who, why)) return 'login';       // out of retries: fail honestly
  const n = R.login_retries[who];
  if (io) io.log(`  ${who}: login did not land (${why}); starting over like a visitor (retry ${n}/${LOGIN_RETRIES})`);
  return 'retry';
}

export function forgetLogin(R, who, why) {
  R.login_retries = R.login_retries || {};
  R.login_retry_log = R.login_retry_log || [];
  const n = R.login_retries[who] || 0;
  if (n >= LOGIN_RETRIES) return false;
  R.login_retries[who] = n + 1;
  R.login_retry_log.push(`${who}: ${why} (retry ${n + 1}/${LOGIN_RETRIES})`);
  delete R.login_sent[who];
  return true;
}

// One login step for one node. `io` is the transport: { type(who, text),
// read(who) -> the node's screen, sleep(ms), user(who), pass(who), log(msg) }.
export async function driveLoginStep(R, who, el, lastWake, wakeEveryS, io) {
  let act = loginAction(R, who);
  if (act === 'wake' && el - (lastWake[who] ?? -1e9) >= wakeEveryS) {
    lastWake[who] = el;
    await io.type(who, '\r');
    act = null;
    if (LOGIN_NODES.includes(who)) {
      for (let i = 0; i < WAKE_FOLLOWUP_POLLS && act !== 'login'; i++) {
        await io.sleep(WAKE_FOLLOWUP_MS);
        observe(R, who, await io.read(who));
        observeLogin(R, who, el);
        act = loginAction(R, who);
        if (act !== 'login' && act !== 'wake') break;     // logged in, or nothing to do
      }
    }
  }
  if (act === 'login') {
    const r = await typeLogin(R, who, el, io);
    if (r === 'retry') {
      // A login that did not land is started over at once (a visitor presses
      // RETURN again), not a whole poll later. Each retry spends one of
      // LOGIN_RETRIES, so the recursion is bounded.
      lastWake[who] = -1e9;
      observe(R, who, await io.read(who));
      observeLogin(R, who, el);
      return driveLoginStep(R, who, el, lastWake, wakeEveryS, io);
    }
    return r;
  }
  return act;
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
  !!R.cn3 && isQuiet(R) && allReady(R, boundS) && versionWhy(R) === null &&
  clusterViewWhy(R) === null && chatterWhy(R) === null;

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
  if (Object.keys(R.bugchecks || {}).length) {
    // A bugcheck seen beside a node that is not the build under test says
    // nothing about the build under test, and must say so where a reader of
    // the verdict will see it (rd vms-4f4: case-filtered hunts ran a V0.7-7
    // Node B, and their BUGCHECK lines read as findings about the new build).
    const stale = versionWhy(R);
    return `BUGCHECK: ${JSON.stringify(R.bugchecks)}` +
           (stale ? ` -- and NOT THE BUILD UNDER TEST: ${stale}` : '');
  }

  if (R.cn3) {
    if (!isQuiet(R)) return `CN=3 but NOT QUIET: ${noiseWhy(R)}`;
    const stale = versionWhy(R);
    if (stale) return `CN=3 but NOT THE BUILD UNDER TEST: ${stale}`;
    const boundS = R.login_bound_s ?? Infinity;
    if (!allReady(R, boundS)) return `CN=3 but NOT EVERY NODE USABLE: ${loginWhy(R, boundS)}`;
    const view = clusterViewWhy(R);
    if (view) return `CN=3 but NOT EVERY MEMBER IN SHOW CLUSTER: ${view}`;
    const chat = chatterWhy(R);
    return chat ? `CN=3 but CONSOLE CHATTER: ${chat}` : 'CN=3';
  }
  const why = [];
  for (const w of LOGIN_NODES) {
    if (!(R.member || {})[w]) why.push(`${w} never said "now a VAXcluster member -- system ${w}"`);
  }
  const adm = R.vaxc_admitted || [];
  const unadmitted = LOGIN_NODES.filter((w) => !adm.includes(w));
  if (unadmitted.length) {
    why.push(`the real VAX admitted ${adm.length ? adm.join('+') : 'nobody'}` +
             (adm.length ? ` (not ${unadmitted.join('+')})` : ''));
  }
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
  const chat = chatterWhy(R);
  if (chat) why.push(`console chatter: ${chat}`);
  const stale = versionWhy(R);
  if (stale) why.push(`not the build under test: ${stale}`);
  const silent = silentNeverStarts(R, ['OVMXB', 'VAXC'].filter((w) => w in (R.panel_started || {})));
  if (silent.length) {
    why.push(`SILENT never-start (no console, no watchdog): ${silent.join(',')} (rd vms-bfdd)`);
  }
  return `NOT CN=3: ${why.join('; ')}`;
}

// ---- rd vms-c56: an OVMX console carries only what a VMS console carries ----
//
// MEASURED on the live V0.7-13 demo (tests/lab/captures/
// vms-c56-console-chatter-20261021/): both OVMX consoles carried the
// executive's own diagnostics -- "[   37.218759] %CNXMAN, no coordinator CSID
// seen yet ...", "%DLM, refusing a lock message (cat 0x02 op 0x01) ...",
// "%PEA0, channel verified" -- on the Username: line and at the DCL prompt,
// while the real VAX beside them printed three short %CNXMAN lines. The fix is
// at the source (the executive routes diagnostics to OPERATOR.LOG,
// src/kernel-core/vms_cluster_opa0.h); this is the assertion that keeps it so.
//
// A console line FAILS when it carries
//   (a) a substrate printk uptime stamp, "[   37.218759]" -- VMS has none; or
//   (b) a message identifier ("%FAC," or "%FAC-S-IDENT,") that a real VMS
//       console never showed: not in VMS_CONSOLE_IDS (every one of which a
//       real VAX printed -- the unit test re-reads the captures to prove it)
//       and not OVMX's own boot narration (OVMX_BOOT_IDS -- EMPTY since rd
//       vms-158d, kept only so the rule reads the same if it is ever needed);
//       an identifier with no leading '%' ("DECNETD-E-NOSOCKET, ...") is still
//       an identifier, and is judged the same way; or
//   (c) a cluster-facility line (%CNXMAN, %PEA0, %SYSINIT -- facilities VMS
//       prints with no severity) that is not one of the FORMS a real VAX
//       printed, word for word, two spaces and all.
// The firmware / page prelude before the OS's first line (SeaBIOS, iPXE, the
// KA655 self-test, the page's own NIC line) is not the OS's console and is not
// judged.

// Message identifiers seen on a real OpenVMS VAX V7.3 console in this repo --
// the demo's own Node C (C.console.txt above) and the VAX lab captures
// (tests/lab/captures/**/VAXC*.log, *vax<N>*.log). The unit test re-reads those
// captures: every identifier here must be on one -- the list is the captures',
// not anybody's opinion. An identifier a real VAX printed that is not here yet
// fails CLOSED (a gate run names it); add it with the capture that shows it.
export const VMS_CONSOLE_IDS = [
  // cluster, boot, startup
  'CNXMAN', 'PEA0', 'SYSINIT', 'VAXcluster-I-LOADSECDB', 'MSCPLOAD-I-LOADMSCP',
  'MSCPLOAD-I-CONFIGSCAN', 'STDRV-I-STARTUP', 'STARTUP-I-AUDITCONTINUE', 'CSP-I-DIFSWVER',
  'SET-I-NEWAUDSRV', 'SET-I-INTSET', 'SET-W-NOTSET', 'RUN-S-PROC_ID', 'DCL-S-SPAWNED',
  'INSTALL-E-OPENIN', 'MOUNT-I-MOUNTED', 'MOUNT-I-REBUILD',
  'MOUNT-W-RMTMNTFAIL', 'SMG-W-GBLNOTCRE', 'DECW-W-NODEVICE', 'JBC-E-OPENERR',
  'LICENSE-F-EMTLDB', 'LICENSE-E-NOAUTH', 'LICENSE-W-NOLOAD', 'LICENSE-F-EXCEEDED',
  'LICENSE-I-NOLICENSE',
  'SECSRV-E-NOPROXYDB', 'SECSRV-I-SERVERSTARTINGU', 'SECSRV-I-CIASTARTINGUP',
  'SECSRV-I-CIACRECLUDB', 'SECSRV-I-CIAEXISTCLU', 'SECSRV-I-PROXYSHUTDOWN',
  'SECSRV-I-CIASHUTDOWN', 'SECSRV-I-SERVERSHUTDOWN',
  'TCPIP-S-STARTDONE', 'TCPIP-I-NORMAL', 'TCPIP-I-INFO', 'TCPIP-I-STARTNAME',
  'TCPIP-I-STARTCOMM', 'TCPIP-I-SETPROTP', 'TCPIP-I-SERVLOADED', 'TCPIP-I-LOADSERV',
  'TCPIP-I-DEFINTE', 'TCPIP-I-SETLOCAL', 'TCPIP-E-PROXYERROR', 'TCPIP-E-STARTFAIL',
  'TCPIP-E-STARTERROR', 'TCPIP-E-CONFIGERROR', 'NCP-I-NOINFO', 'NCP-W-UNRCMP', 'NCP-W-OPEFAI',
  // shutdown
  'SHUTDOWN-I-STOPUSER', 'SHUTDOWN-I-STOPSMISRV', 'SHUTDOWN-I-STOPSECSRV',
  'SHUTDOWN-I-STOPQUEUES', 'SHUTDOWN-I-STOPAUDIT', 'SHUTDOWN-I-SHUTNET', 'SHUTDOWN-I-REMOVE',
  'SHUTDOWN-I-OPERATOR', 'SHUTDOWN-I-DISMOUNT', 'SHUTDOWN-I-DISLOGINS', 'DISM-W-RMTDSMFAIL',
  'DISM-W-CANNOTDMT', 'DISM-W-DSMCLUFAIL', 'DISM-W-USERFILES',
  // login and DCL
  'LOGIN-F-CMDINPUT', 'LOGIN-F-NOSUCHUSER', 'LOGIN-F-DISUSER', 'LOGIN-F-INVPWD',
  'LOGIN-S-LOGOPRCON', 'LOGIN-I-NOVAXCLUSTER', 'REM-S-END',
  'DCL-W-IVVERB', 'DCL-W-PARMDEL', 'DCL-I-SUPERSEDE', 'DCL-W-NOVALU', 'DCL-W-IVQUAL',
  'DCL-W-IVCHAR', 'DCL-W-DIRECT', 'DCL-E-OPENIN', 'DIRECT-W-NOFILES', 'SHOW-S-NOTRAN',
  'SEARCH-W-OPENIN', 'SEARCH-E-NOFILE', 'DELETE-W-SEARCHFAIL', 'DELETE-W-FILNOTDEL',
  'CREATE-E-DIRNOTCRE', 'DIRECT-E-OPENIN', 'RENAME-E-OPENIN', 'RENAME-E-SEARCHFAIL',
  'TYPE-W-OPENIN', 'TYPE-W-SEARCHFAIL', 'UAF-I-RDBADDMSGU', 'UAF-I-MDFYMSG', 'UAF-I-DONEMSG', 'UAF-I-ADDMSG',
  'UAF-I-RDBNOMODS', 'UAF-I-RDBDONEMSG', 'SDA-E-NORSBNAM', 'SDA-F-OPENIN', 'SYSMAN-I-ENV',
  'CLI-W-IVKEYW',
  'RMS-E-FNF', 'SYSTEM-F-DUPLNAM', 'SYSTEM-F-IVADDR', 'SYSTEM-F-NOLOGTAB',
  'SYSTEM-F-NOLOGNAM', 'SYSTEM-F-DEVNOTMOUNT',
];

// OVMX's own boot narration allowed on the console, which no VMS console
// prints. EMPTY (rd vms-158d): the executive-attached, system-disk mount,
// node-name and system-identity lines now go to OPERATOR.LOG, and STARTNET
// prints only what RUN prints; NETACP reports "DECnet starting" to OPCOM. An
// entry here would be debt with an rd item, never a convenience.
export const OVMX_BOOT_IDS = [];

// The forms the cluster facilities take on a real VAX console (C.console.txt;
// vms-52d-0b01-oracle-20261013, vms-ci6-evac-20261009, vms-fcb-lockdirwt-
// 20261004 and vms-dfe-blackout-recovery-20260925 VAX consoles).
const NODE_NAME = '[A-Z0-9$_]{1,6}';
// "%CNXMAN,  <phrase> <NODE>"
export const CNXMAN_NAMED_FORMS = [
  'sending VAXcluster membership request to system', 'received VAXcluster membership request from system',
  'proposing addition of system', 'now a VAXcluster member -- system', 'lost connection to system',
  'timed-out lost connection to system', 'removed from VAXcluster system',
  're-established connection to system', 'established connection to system',
  'have connection to system', 'discovered system',
];
// "%CNXMAN,  <phrase>"
export const CNXMAN_PLAIN_FORMS = [
  'proposing formation of a VAXcluster', 'completing VAXcluster state transition',
  'proposing reconfiguration of the VAXcluster', 'aborting VAXcluster state transition',
  'quorum lost, blocking activity', 'quorum regained, resuming activity',
  'proposing modification of quorum or quorum disk membership',
];
const reEsc = (x) => x.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
export const CLUSTER_FORMS = [
  new RegExp(`^%CNXMAN,  (?:${CNXMAN_NAMED_FORMS.map(reEsc).join('|')}) ${NODE_NAME}$`),
  new RegExp(`^%CNXMAN,  (?:${CNXMAN_PLAIN_FORMS.map(reEsc).join('|')})$`),
  new RegExp(`^%PEA0, (?:Port has Closed Virtual Circuit - REMOTE NODE ${NODE_NAME}|` +
    `Excessive packet losses on LAN Path from \\S+ to \\S+ on REMOTE NODE ${NODE_NAME})$`),
  /^%SYSINIT, waiting to form or join a VMScluster system$/,
];
const FORM_FACILITIES = ['CNXMAN', 'PEA0', 'SYSINIT'];

export const PRINTK_STAMP = /\[\s*\d+\.\d+\]/;
const MSG_ID = /%([A-Za-z][A-Za-z0-9$_]*(?:-[A-Z]-[A-Z0-9$_]+)?),/g;
// A message identifier written WITHOUT its '%' at the start of a line
// ("DECNETD-E-NOSOCKET, bpf datalink open ... failed", V0.7-13 Node B). A DCL
// continuation line starts with '-' ("-RMS-E-FNF, ...") and is not this.
const BARE_ID = /^\s*([A-Z][A-Z0-9$_]*-[A-Z]-[A-Z0-9$_]+),/;
// Where the OS's own console starts: the substrate banner, the product banner,
// or (older images) the executive's first line. Before it is firmware and the
// page.
const OS_START = /OVMX\/\S+ -- SYSKRNL|%OVMX-I-|OpenVMX V\d+\.\d+/;

// The console lines a real VMS console would not carry, each as
// {line, why}, in order, once each. [] is the claim.
export function consoleChatter(text) {
  let t = String(text || '').replace(/\r/g, '').replace(/\u0000/g, '');
  const start = t.search(OS_START);
  if (start > 0) t = t.slice(t.lastIndexOf('\n', start) + 1);
  const allowed = new Set([...VMS_CONSOLE_IDS, ...OVMX_BOOT_IDS]);
  const out = [];
  const seen = new Set();
  const flag = (line, why) => {
    const key = `${why}\u0000${line}`;
    if (!seen.has(key)) { seen.add(key); out.push({ line, why }); }
  };
  for (const raw of t.split('\n')) {
    const line = raw.replace(/[\s█]+$/u, '');
    if (!line) continue;
    if (PRINTK_STAMP.test(line)) flag(line, 'substrate printk timestamp');
    const bare = BARE_ID.exec(line);
    if (bare && !allowed.has(bare[1])) flag(line, `${bare[1]} is not a message a VMS console prints`);
    for (const m of line.matchAll(MSG_ID)) {
      const id = m[1];
      if (!allowed.has(id)) { flag(line, `%${id} is not a message a VMS console prints`); continue; }
      if (FORM_FACILITIES.includes(id)) {
        const msg = line.slice(m.index).replace(/\s+$/, '');
        if (!CLUSTER_FORMS.some((re) => re.test(msg))) flag(line, `not a form VMS prints for %${id}`);
      }
    }
  }
  return out;
}

// Why the OVMX consoles are not VMS-faithful, or null when they are.
export function chatterWhy(R) {
  const parts = [];
  for (const w of LOGIN_NODES) {
    const c = (R.chatter || {})[w] || [];
    if (c.length) parts.push(`${w} ${c.length}+ line(s), first ${JSON.stringify(c[0].line)} (${c[0].why})`);
  }
  return parts.length ? `${parts.join('; ')} (rd vms-c56)` : null;
}

// The MEMBER rows of the LAST SHOW CLUSTER table a console printed, as node
// names, or null if it printed none. The executive's own table -- the CSBs it
// holds (src/vmsdcl/dcl_cmd_show.c) -- not a log line:
//   | NODE   | CSID     | SOFTWARE        | STATUS           |
//   | OVMXB  | 00010003 | VMX V0.7        | MEMBER           |
export const SHOW_CLUSTER_HEAD = 'View of Cluster from system ID';
const SHOW_CLUSTER_ROW = /^\|\s*([A-Z0-9$_]{1,6})\s*\|\s*[0-9A-Fa-f]{8}\s*\|[^|\n]*\|\s*MEMBER\s*\|/gm;
export function clusterViewOf(text) {
  const t = String(text || '');
  const at = t.lastIndexOf(SHOW_CLUSTER_HEAD);
  if (at < 0) return null;
  const tail = t.slice(at);
  const end = tail.search(/\n\$ /);
  const table = end > 0 ? tail.slice(0, end) : tail;
  return [...new Set([...table.matchAll(SHOW_CLUSTER_ROW)].map((m) => m[1]))];
}

// Why some OVMX node's own SHOW CLUSTER does not hold all three systems as
// members, or null when every one does. This is the bar the old console-line
// count ("system 7c4 was added") stood for, now read from the executive.
export function clusterViewWhy(R) {
  const why = [];
  for (const w of LOGIN_NODES) {
    const view = (R.cluster_view || {})[w];
    if (!view) { why.push(`${w} showed no SHOW CLUSTER`); continue; }
    const miss = NODES.filter((n) => !view.includes(n));
    if (miss.length) why.push(`${w}'s SHOW CLUSTER lacks ${miss.join(',')} as MEMBER`);
  }
  return why.length ? why.join('; ') : null;
}

// One OVMX node's SHOW CLUSTER, typed at its logged-in DCL prompt the way a
// visitor would, once the cluster has formed and the node is usable. Waits for
// the table and the prompt after it; tries at most VIEW_TRIES times. `io` is
// the same transport driveLoginStep takes.
export const VIEW_TRIES = 3;
export const VIEW_WAIT_POLLS = 20;
export const VIEW_WAIT_MS = 1000;
export async function driveClusterView(R, who, io) {
  for (let n = 0; n < VIEW_TRIES; n++) {
    const from = String((R.transcript || {})[who] || '').length;
    io.log(`  ${who}: SHOW CLUSTER`);
    await io.type(who, 'SHOW CLUSTER\r');
    for (let i = 0; i < VIEW_WAIT_POLLS; i++) {
      await io.sleep(VIEW_WAIT_MS);
      observe(R, who, await io.read(who));
      const after = String(R.transcript[who] || '').slice(from);
      if (after.includes(SHOW_CLUSTER_HEAD) && atDcl(after)) return clusterViewOf(after);
    }
  }
  return null;
}
