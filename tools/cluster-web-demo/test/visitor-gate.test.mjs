// visitor-gate.test.mjs — the gate's judgement, tested without a browser
// (rd vms-1a1).
//
// Both of the gate's historical mistakes are regression-tested here:
//   1. it recomputed its observations from a CAPPED scrollback, so a healthy
//      CN=3 cluster "un-joined" as its console lines scrolled away, and the run
//      was reported FAIL;
//   2. it reported "SPLIT BRAIN: the OVMX pair clustered and the real VAX
//      admitted nobody" for a run whose evidence said no such thing.
// A grader that lies in either direction is worse than no grader.

import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import * as GATE from '../gate-eval.mjs';
const {
  joinScrollback, observe, isCN3, verdictOf, newObservations, restartsIn, setOf, ADDED, MATRIX,
  isRepaintStall, silentNeverStarts,
} = GATE;

const CN3_LINES = (self, a, b) => [
  '%CNXMAN, this node is now a VAXcluster member',
  `%CNXMAN, system 00000000000007${self} was added to the cluster`,
  `%CNXMAN, system 00000000000007${a} was added to the cluster`,
  `%CNXMAN, system 00000000000007${b} was added to the cluster`,
].join('\n') + '\n';

const VAXC_ADMITS = 'proposing addition of system OVMXA\nproposing addition of system OVMXB\n';

function cn3Observations() {
  const R = newObservations();
  observe(R, 'OVMXA', CN3_LINES('c3', 'c4', 'c5'));
  observe(R, 'OVMXB', CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', VAXC_ADMITS);
  R.sca = { OVMXA: 100, OVMXB: 120, VAXC: 90 };
  return R;
}

test('a scrollback that scrolls does not un-join a node', () => {
  const R = newObservations();
  observe(R, 'OVMXA', CN3_LINES('c3', 'c4', 'c5'));
  assert.deepEqual(new Set(R.added.OVMXA), new Set(['0x7c3', '0x7c4', '0x7c5']));

  // The terminal trims: the cluster lines are gone from the SCREEN, and 1200
  // lines of DCL have taken their place. The observation must not shrink.
  observe(R, 'OVMXA', '$ SHOW CLUSTER\n'.repeat(40));
  assert.deepEqual(new Set(R.added.OVMXA), new Set(['0x7c3', '0x7c4', '0x7c5']),
    'a node does not un-join because its console scrolled');
});

test('joinScrollback appends only the new tail', () => {
  assert.equal(joinScrollback('hello world', 'lo world\nnext'), 'hello world\nnext');
  assert.equal(joinScrollback('', 'first'), 'first');
  assert.equal(joinScrollback('abc', ''), 'abc');
  assert.equal(joinScrollback('abc', 'abc'), 'abc', 'an unchanged screen adds nothing');
});

test('the cursor glyph at the write position is not output (V0.7-3 "33 restarts")', () => {
  // The real shape: pcjs paints the cursor as a block on a line of its own at
  // the end of the screen, and the next poll has printed where it stood.
  const banner = 'KA655-B V5.3, VMB 2.7\n';
  const poll1 = banner + '[  98.02] %CNXMAN, waiting to form or join\n\u2588\n\n';
  const poll2 = banner + '[  98.02] %CNXMAN, waiting to form or join\n' +
                '[ 113.20] %PEA0, channel verified\n\u2588\n\n';
  const R = newObservations();
  observe(R, 'OVMXB', poll1);
  observe(R, 'OVMXB', poll2);
  observe(R, 'OVMXB', poll2);
  assert.equal(R.restarts.OVMXB, 0, 'one boot, however many polls');
  assert.equal(R.transcript.OVMXB,
    banner + '[  98.02] %CNXMAN, waiting to form or join\n[ 113.20] %PEA0, channel verified');
  // ...and a cursor that stood where the next line's first character lands.
  assert.equal(joinScrollback('line one\n\u2588', 'line one\n%CNXMAN, next'),
    'line one\n%CNXMAN, next');
  // The cursor's line is provisional: painted over a character (VAXC's
  // "\u2588CNXMAN, completing") or redrawn in place (a boot countdown).
  assert.equal(joinScrollback('db\n\u2588MSCPLOAD, loading ', 'db\n%MSCPLOAD, loading done\nnext'),
    'db\n%MSCPLOAD, loading done\nnext');
  assert.equal(joinScrollback('<<\n>> abort autoboot 2', '<<\n>> abort autoboot 0\nnfs_open'),
    '<<\n>> abort autoboot 0\nnfs_open');
  // A cleared screen is still a restart.
  const C = newObservations();
  observe(C, 'VAXC', 'KA655-B V5.3, VMB 2.7\nline\n\u2588');
  observe(C, 'VAXC', 'KA655-B V5.3, VMB 2.7\nline\nmore\n\u2588');
  assert.equal(C.restarts.VAXC, 0);
  observe(C, 'VAXC', 'KA655-B V5.3, VMB 2.7\nPerforming normal system tests.');
  assert.equal(C.restarts.VAXC, 1);
});

test('a guest that clears its screen is a restart, not an erasure', () => {
  const R = newObservations();
  observe(R, 'VAXC', 'KA655-B V5.3, VMB 2.7\nboot\n' + VAXC_ADMITS);
  assert.equal(R.restarts.VAXC, 0);
  // It reset: the terminal is cleared and the self-test starts over. Nothing we
  // already saw may be lost, and the restart itself must be visible.
  observe(R, 'VAXC', 'KA655-B V5.3, VMB 2.7\nPerforming normal system tests.\n');
  assert.equal(R.restarts.VAXC, 1);
  assert.deepEqual(R.vaxc_admitted, ['OVMXA', 'OVMXB'],
    'the admissions it made before it reset still happened');
});

test('restartsIn counts power-ons after the first', () => {
  assert.equal(restartsIn(''), 0);
  assert.equal(restartsIn('KA655-B V5.3, VMB 2.7'), 0);
  assert.equal(restartsIn('KA655-B V5.3, VMB 2.7\nx\nKA655-B V5.3, VMB 2.7'), 1);
});

test('CN=3 needs BOTH OVMX nodes naming all three AND the real VAX admitting both', () => {
  const R = cn3Observations();
  assert.equal(isCN3(R), true);
  R.cn3 = true;
  assert.equal(verdictOf(R), 'CN=3');

  const noVaxc = cn3Observations();
  noVaxc.vaxc_admitted = [];
  assert.equal(isCN3(noVaxc), false, 'the real VAX admitting nobody is not CN=3');

  const oneShort = cn3Observations();
  oneShort.added.OVMXA = ['0x7c3', '0x7c4'];
  assert.equal(isCN3(oneShort), false, 'a node that never saw the third system is not CN=3');
});

test('an OVMX node founding a cluster is a REGRESSION, named as one', () => {
  const R = cn3Observations();
  observe(R, 'OVMXA', '%CNXMAN, this node has quorum by its own votes\n');
  assert.match(verdictOf(R), /^REGRESSION: an OVMX node founded a cluster \(OVMXA\)/);
});

test('a bugcheck outranks every other reading', () => {
  const R = cn3Observations();
  R.cn3 = true;
  observe(R, 'VAXC', '**** FATAL BUGCHECK CNXMGRERR ****\n');
  assert.match(verdictOf(R), /^BUGCHECK:/);
});

test('a failure is reported as what was SEEN, never as a diagnosis', () => {
  // The real run this comes from: both OVMX nodes were members, the real VAX
  // transmitted SCA frames the whole time and then restarted, and node A never
  // learned of the third system. Calling that "the OVMX pair clustered and the
  // real VAX admitted nobody" asserted a cause the evidence did not carry.
  const R = newObservations();
  observe(R, 'OVMXA', CN3_LINES('c3', 'c4', 'c4'));
  observe(R, 'OVMXB', CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', 'KA655-B V5.3, VMB 2.7\nx\nKA655-B V5.3, VMB 2.7\n');
  R.sca = { OVMXA: 10, OVMXB: 20, VAXC: 30 };

  const v = verdictOf(R);
  assert.match(v, /^NOT CN=3:/);
  assert.match(v, /OVMXA never named 0x7c5/);
  assert.match(v, /the real VAX admitted nobody/);
  assert.match(v, /VAXC restarted 1x/);
  assert.doesNotMatch(v, /SPLIT BRAIN/i, 'the gate must not diagnose, only report');
});

test('a node the hub never heard from is named', () => {
  const R = newObservations();
  R.sca = { OVMXA: 5, OVMXB: 5 };
  assert.match(verdictOf(R), /no SCA frames from VAXC/);
});

test('the ADDED pattern reads a CNXMAN system id', () => {
  assert.deepEqual(setOf('%CNXMAN, system 00000000000007c3 was added to the cluster', ADDED),
                   ['7c3']);
});

// visitor-gate.mjs cannot be imported here -- it pulls in playwright and drives
// a browser -- so check its seam the cheap way. `node --check` only parses; an
// import of a name the module does not export fails at RUN time, which on a
// gate means 20 minutes into a deploy window. This caught exactly that.
test('every name visitor-gate.mjs imports from gate-eval.mjs exists', () => {
  const src = fs.readFileSync(new URL('../visitor-gate.mjs', import.meta.url), 'utf8');
  const m = src.match(/import\s*\{([^}]*)\}\s*from\s*'\.\/gate-eval\.mjs'/);
  assert.ok(m, 'visitor-gate.mjs must take its judgement from gate-eval.mjs');
  const names = m[1].split(',').map((x) => x.trim().split(/\s+as\s+/)[0]).filter(Boolean);
  assert.ok(names.length >= 4);
  for (const n of names) assert.ok(n in GATE, `gate-eval.mjs exports no '${n}'`);
});

test('the matrix tests click ORDER and SPEED, not just the easy path', () => {
  const labels = MATRIX.map((r) => r.label);
  assert.ok(MATRIX.some((r) => r.order.join('') === 'ABC' && r.gap <= 1000),
    'a visitor clicks top-to-bottom within seconds');
  assert.ok(MATRIX.some((r) => r.throttle >= 4), 'and does it on a slow laptop');
  assert.ok(MATRIX.some((r) => r.order[0] !== 'C'),
    'the old grader always gave the real VAX a head start; this one must not');
  assert.equal(new Set(labels).size, labels.length, 'labels select rows, so they must be unique');
});

test('a panel that only repaints when revealed is named in the verdict', () => {
  // Not a cluster fact -- a page defect (rd vms-0bc) -- but the run that hit it
  // must say so, because the alternative is someone re-reading a throttled
  // screen as "the real VAX never printed anything".
  const R = newObservations();
  R.sca = { OVMXA: 1, OVMXB: 1, VAXC: 1 };
  R.repaint_stalls = { VAXC: 4 };
  assert.match(verdictOf(R), /panels that only repainted once revealed: \{"VAXC":4\} \(rd vms-0bc\)/);
});

test('a lost connection is reported in the node\'s own words, or not at all', () => {
  // Real lines from the all-at-once run in
  // tests/lab/captures/vms-1a1-visitor-20260928/.
  const R = newObservations();
  observe(R, 'VAXC', [
    '%CNXMAN,  lost connection to system OVMXB',
    '%CNXMAN,  timed-out lost connection to system OVMXB',
    '%%%%% OPCOM  Node VAXC (csid 00010001) lost connection to node OVMXB',
  ].join('\n'));
  assert.deepEqual(R.lost.VAXC, ['OVMXB'], 'the VAX names the peer; say which one');

  observe(R, 'OVMXA', [
    '%CNXMAN, lost connection to a cluster member, reconnecting',
    '%CNXMAN, lost the VMS$VAXcluster connection before this node was admitted: waiting',
    'vms: SCS path lost to system 0:1988 -> SS$ 2692',
  ].join('\n'));
  assert.deepEqual(R.lost.OVMXA, ['0:1988'], 'OVMX names the system by id, so use the id');
  assert.equal(R.lost_unnamed.OVMXA, 2, 'and the unnamed losses are counted, not invented');
  assert.match(verdictOf(R), /unnamed connection losses: \{"OVMXA":2\}/);
});

test('an MSCP disk-client message is not a lost cluster connection', () => {
  const R = newObservations();
  observe(R, 'OVMXB', '%CNXMAN, lost the MSCP$DISK disk-client connection: this node ' +
                      'enumerates none of that member\'s units, and the join goes on\n');
  assert.deepEqual(R.lost, {}, 'a benign join step must not read as a dropped circuit');
  assert.deepEqual(R.lost_unnamed, {});
});

test('a panel that keeps printing is not a repaint stall', () => {
  // The first cut of this counter incremented whenever the console grew across
  // the reveal, which a booting guest does constantly -- so it reported stalls
  // on a perfectly live page and the number meant nothing.
  const t1 = 'KA655-B V5.3, VMB 2.7\n';
  const t2 = t1 + 'Performing normal system tests.\n';
  const t3 = t2 + '%SYSINIT, waiting to form or join a VMScluster system\n';
  assert.equal(isRepaintStall(t1, t2, t3), false, 'it moved on its own since the last poll');
  assert.equal(isRepaintStall(undefined, t1, t2), false, 'the first sample cannot be a stall');
  assert.equal(isRepaintStall(t1, t1, t1), false, 'stuck and still stuck is not a stall either');
  assert.equal(isRepaintStall(t1, t1, t3), true,
    'byte-identical for a whole poll, then output the moment it was revealed');
});

test('rd vms-bfdd: a silent never-start is flagged only when the panel said nothing at all', () => {
  // The captured defect: Node B's console stayed at 0 bytes for 586s, with
  // nothing (no error, no retry affordance) for a visitor to act on. The
  // watchdog (node-pcjs.html) is the fix -- it either sees a sign of life
  // (__nodeState.started) or says so (__nodeState.watchdogFired). A SILENT
  // never-start is the one combination that must never happen again: empty
  // console AND no started signal AND no watchdog fire.
  const R = newObservations();
  R.panel_started = { OVMXB: false, VAXC: true };
  R.panel_watchdog_fired = { OVMXB: false, VAXC: false };
  assert.deepEqual(silentNeverStarts(R, ['OVMXB', 'VAXC']), ['OVMXB'],
    'VAXC started; OVMXB neither started nor said anything -- that one is silent');

  // The honest-failure path: the panel never started, but its watchdog DID
  // fire -- the visitor saw a message and a retry button, so this is not the
  // silent defect, even though the console is still empty.
  const R2 = newObservations();
  R2.panel_started = { OVMXB: false };
  R2.panel_watchdog_fired = { OVMXB: true };
  assert.deepEqual(silentNeverStarts(R2, ['OVMXB']), [],
    'a fired watchdog is an HONEST failure, not a silent one');
});

test('verdictOf names a silent never-start distinctly from the usual NOT CN=3 reasons', () => {
  const R = newObservations();
  observe(R, 'OVMXA', CN3_LINES('c3', 'c4', 'c5'));
  R.sca = { OVMXA: 10 };
  R.panel_started = { OVMXB: false, VAXC: false };
  R.panel_watchdog_fired = { OVMXB: false, VAXC: false };
  const v = verdictOf(R);
  assert.match(v, /SILENT never-start/);
  assert.match(v, /OVMXB/);
  assert.match(v, /VAXC/);
});

test('the gate does not scroll panels while grading', () => {
  // Scrolling three canvas-heavy cross-origin iframes into view every 15s
  // starved node A's worker: on the live V0.7-4 page, 6 of 12 runs failed with
  // "no SCA frames from OVMXA" with it on, 6 of 6 passed with it off. The
  // instrument must not be the reason a node does not boot, so it is opt-in.
  assert.equal(GATE.wantsReveal({}), false, 'off by default');
  assert.equal(GATE.wantsReveal({ REVEAL: '1' }), true, 'opt in to diagnose a repaint regression');
  assert.equal(GATE.wantsReveal({ REVEAL: '1', NO_REVEAL: '1' }), false, 'NO_REVEAL still wins');
});

// --- rd vms-553: the OVMX/VAX console shows the VMS personality, nothing else --

test('the substrate markers fire on the console that actually had them', () => {
  // Positive control from this repo's own history: the V0.7-2 deploy's Node B
  // console, before the substrate was silenced at the source.
  const noisy = fs.readFileSync(new URL(
    '../../../tests/lab/captures/vms-e18e-cn3-live-v072-20260926/OVMXB.console.log',
    import.meta.url), 'utf8');
  const seen = GATE.netbsdNoise(noisy);
  assert.ok(Object.keys(seen).length >= 5, `expected several markers, got ${JSON.stringify(seen)}`);
  assert.ok(seen['secondary bootstrap banner'], 'the >> NetBSD/vax boot [ banner');
  assert.ok(seen['memory sizing'], 'total/avail memory = N KB');
});

test('OVMX output is not mistaken for substrate noise', () => {
  // The executive naming its own substrate IS the VMS personality speaking, the
  // KA655 banner is the machine's ROM, and the executive's operator lines carry
  // the same bracketed uptime the kernel's did -- none of them is NetBSD's boot.
  const ovmx = [
    'OVMX/NetBSD-vax -- SYSKRNL (NetBSD kernel)',
    '%OVMX-I-EXEC, VMS executive attached on /dev/vms',
    'KA655-B V5.3, VMB 2.7',
    '[   45.269670] %PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)',
    '[   48.036579] %CNXMAN, this node is a member of the cluster',
  ].join('\n');
  assert.deepEqual(GATE.netbsdNoise(ovmx), {});
});

test('a noisy console fails the run even when the cluster formed', () => {
  const R = newObservations();
  observe(R, 'OVMXA', CN3_LINES('c3', 'c4', 'c5'));
  observe(R, 'OVMXB', CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', VAXC_ADMITS);
  R.sca = { OVMXA: 1, OVMXB: 1, VAXC: 1 };
  R.cn3 = true;
  assert.equal(GATE.isPass(R), true, 'quiet and clustered passes');

  observe(R, 'OVMXB', '\n[   1.0000000] total memory = 16328 KB\n');
  assert.equal(GATE.isQuiet(R), false);
  assert.equal(GATE.isPass(R), false, 'membership alone is not the bar any more');
  assert.match(verdictOf(R), /^CN=3 but NOT QUIET: NetBSD substrate lines/);
});

test('only the OVMX/VAX node is held to the quiet claim', () => {
  // Node C is a real VAX/VMS machine and node A runs on Linux: neither has a
  // NetBSD substrate to be quiet about, so their text must not be scanned.
  const R = newObservations();
  observe(R, 'VAXC', '>> NetBSD/vax boot [1.12] <<\ntotal memory = 16328 KB\n');
  observe(R, 'OVMXA', '>> NetBSD/vax boot [1.12] <<\n');
  assert.deepEqual(R.substrate_noise, {});
  assert.equal(GATE.isQuiet(R), true);
});

// ---- Node A's own worker state (rd vms-4ff) --------------------------------
//
// The two V0.7-7 live failures were both "Node A's console ends right after
// %OVMX-I-MOUNTED", and that was ALL the run kept -- the gate read Node A's
// consoleText and none of the worker-level state its node.html wrapper
// publishes beside it, so a worker that aborted could not be told from a guest
// still grinding through the ACP staging phase. These tests hold the readings
// honest: what the node said about itself, never a diagnosis.

const MOUNTED_CONSOLE = [
  'OVMX/Linux -- SYSKRNL (Linux kernel)',
  '%OVMX-I-EXEC, VMS executive attached on /dev/vms',
  '%OVMX-I-SYSDISK, mounting system disk VDA0:',
  '%OVMX-I-MOUNTED, system disk VDA0: mounted',
].join('\n') + '\n';

test('Node A worker state is folded monotonically: flags latch, counters grow', () => {
  const R = newObservations();
  GATE.observeNodeAWorker(R, { workerSpawned: true, pipeReady: true, firstOut: false,
                               nicTxCount: 0, nicRxCount: 3 });
  GATE.observeNodeAWorker(R, { workerSpawned: true, firstOut: true, nicTxCount: 7,
                               nicRxCount: 1, acpOk: true });
  // A later poll that reads NOTHING (frame mid-navigation) erases nothing.
  GATE.observeNodeAWorker(R, null);
  assert.equal(R.nodeA_worker.firstOut, true);
  assert.equal(R.nodeA_worker.acpOk, true);
  assert.equal(R.nodeA_worker.nicTxCount, 7);
  assert.equal(R.nodeA_worker.nicRxCount, 3, 'a counter that went backwards keeps its high water');
});

test('a halt Node A reported once is not erased by a later silent poll', () => {
  const R = newObservations();
  GATE.observeNodeAWorker(R, { workerSpawned: true, halt: 'OpenVMX stopped unexpectedly.' });
  GATE.observeNodeAWorker(R, { workerSpawned: true });
  assert.equal(R.nodeA_worker.halt, 'OpenVMX stopped unexpectedly.');
});

test('a declared fault outranks the console tail in the reading', () => {
  const R = newObservations();
  observe(R, 'OVMXA', MOUNTED_CONSOLE);
  GATE.observeNodeAWorker(R, { workerSpawned: true, pipeReady: true, firstOut: true,
                               workerError: 'RuntimeError: memory access out of bounds' });
  assert.match(GATE.nodeAStallWhy(R), /worker declared an error.*memory access out of bounds/);
  assert.match(verdictOf(R), /no SCA frames from OVMXA.*worker declared an error/s);
});

test('a worker alive with a console that stopped is reported as exactly that', () => {
  const R = newObservations();
  observe(R, 'OVMXA', MOUNTED_CONSOLE);
  GATE.observeNodeAWorker(R, { workerSpawned: true, pipeReady: true, firstOut: true,
                               acpOk: true, nicTxCount: 0 });
  const why = GATE.nodeAStallWhy(R);
  assert.match(why, /console stops after "%OVMX-I-MOUNTED, system disk VDA0: mounted"/);
  assert.match(why, /no halt and no error/);
  assert.match(why, /nicTx=0/);
  // No invented cause: the word the old reports reached for is not in it.
  assert.doesNotMatch(why, /panic|timer|IO-APIC|wedged|hung/i);
});

test('a spawned worker whose guest never printed is distinguished from a stopped one', () => {
  const R = newObservations();
  GATE.observeNodeAWorker(R, { workerSpawned: true, pipeReady: true, firstOut: false });
  assert.match(GATE.nodeAStallWhy(R), /never wrote to the console \(pipeReady=true\)/);
});

test('an unreadable Node A frame is said to be unreadable, not called dead', () => {
  const R = newObservations();
  assert.match(GATE.nodeAStallWhy(R), /never readable/);
});

test('a Node A that is talking has nothing to explain', () => {
  const R = newObservations();
  observe(R, 'OVMXA', MOUNTED_CONSOLE);
  GATE.observeNodeAWorker(R, { workerSpawned: true, firstOut: true, nicTxCount: 91 });
  R.sca = { OVMXA: 91 };
  assert.equal(GATE.nodeAStallWhy(R), null);
  assert.ok(!/Node A/.test(verdictOf(R)), 'a working node is not narrated');
});

test('lastLineOf ignores trailing blanks and the cursor glyph', () => {
  assert.equal(GATE.lastLineOf('a\nb\n\n'), 'b');
  assert.equal(GATE.lastLineOf('%OVMX-I-MOUNTED, system disk VDA0: mounted\n█'),
               '%OVMX-I-MOUNTED, system disk VDA0: mounted');
  assert.equal(GATE.lastLineOf(''), '');
});

test('the Node A reading never fabricates a field the node did not publish', () => {
  // INV-6: a node that published nothing but a spawn flag must not read as
  // "0 frames sent, ACP seen" -- the counters are absent, not zero-claimed.
  const R = newObservations();
  GATE.observeNodeAWorker(R, { workerSpawned: true });
  assert.deepEqual(R.nodeA_worker, { workerSpawned: true });
});

test('the worker heartbeat is reported as the WORKER\'s, and absent when not sent', () => {
  const R = newObservations();
  observe(R, 'OVMXA', MOUNTED_CONSOLE);
  GATE.observeNodeAWorker(R, { workerSpawned: true, pipeReady: true, firstOut: true });
  assert.match(GATE.nodeAStallWhy(R), /worker event-loop ticks not reported/,
               'a pre-vms-4ff deploy sends no heartbeat, and that is said, not zeroed');

  GATE.observeNodeAWorker(R, { workerTicks: 41 });
  GATE.observeNodeAWorker(R, { workerTicks: 95 });
  assert.equal(R.nodeA_worker.workerTicks, 95);
  assert.match(GATE.nodeAStallWhy(R), /worker event-loop ticks=95/);
});
