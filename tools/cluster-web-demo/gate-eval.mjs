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
];

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

// State the outcome in terms of what was OBSERVED, never a diagnosis the
// evidence does not carry.
export function verdictOf(R) {
  if (Object.keys(R.ovmx_founded || {}).length) {
    return `REGRESSION: an OVMX node founded a cluster (${Object.keys(R.ovmx_founded)}) ` +
           `-- the OVMX nodes must be NON-VOTING (rd vms-1a1)`;
  }
  if (Object.keys(R.bugchecks || {}).length) return `BUGCHECK: ${JSON.stringify(R.bugchecks)}`;
  if (R.cn3) return 'CN=3';
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
  if (Object.keys(R.lost || {}).length) why.push(`lost: ${JSON.stringify(R.lost)}`);
  if (Object.keys(R.lost_unnamed || {}).length) {
    why.push(`unnamed connection losses: ${JSON.stringify(R.lost_unnamed)}`);
  }
  const stalls = R.repaint_stalls || {};
  if (Object.keys(stalls).length) {
    why.push(`panels that only repainted once revealed: ${JSON.stringify(stalls)} (rd vms-0bc)`);
  }
  const silent = silentNeverStarts(R, ['OVMXB', 'VAXC'].filter((w) => w in (R.panel_started || {})));
  if (silent.length) {
    why.push(`SILENT never-start (no console, no watchdog): ${silent.join(',')} (rd vms-bfdd)`);
  }
  return `NOT CN=3: ${why.join('; ')}`;
}
