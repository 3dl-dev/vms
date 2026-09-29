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
  isRepaintStall,
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
