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
  joinScrollback, observe, isCN3, verdictOf, newObservations, restartsIn, setOf, MEMBER, MATRIX,
  isRepaintStall, silentNeverStarts,
} = GATE;

// What a joining OVMX node prints on its own console, in VMS's words (rd
// vms-c56) -- the roster's system ids map to the demo's node names.
const NAME_OF = { c3: 'OVMXA', c4: 'OVMXB', c5: 'VAXC' };
const CN3_LINES = (self) => [
  '%SYSINIT, waiting to form or join a VMScluster system',
  '%CNXMAN,  sending VAXcluster membership request to system VAXC',
  `%CNXMAN,  now a VAXcluster member -- system ${NAME_OF[self]}`,
].join('\n') + '\n';

// An OVMX node's own SHOW CLUSTER, as DCL prints it (the V0.7-13 Node B
// console, tests/lab/captures/vms-c56-console-chatter-20261021/B.console.txt).
const SHOW_CLUSTER = (self, rows = ['OVMXB', 'OVMXA', 'VAXC']) => [
  '$ SHOW CLUSTER',
  `View of Cluster from system ID 1988 node: ${self}    01-JAN-2010 12:12:25`,
  '',
  '+---------------------------------------------------------------+',
  '|                 SYSTEMS                |      MEMBERS         |',
  '|----------------------------------------+----------------------|',
  '| NODE   | CSID     | SOFTWARE        | STATUS           |',
  '|--------+----------+-----------------+------------------|',
  ...rows.map((n, i) => `| ${n.padEnd(6)} | 0001000${3 - i} | ${n === self ? 'VMX V0.7       ' : '               '} | MEMBER           |`),
  '+---------------------------------------------------------------+',
  '$ ',
].join('\n');

const VAXC_ADMITS = 'proposing addition of system OVMXA\nproposing addition of system OVMXB\n';

// Each OVMX node's boot banner, as its console prints it.
const UNDER_TEST = 'V0.7-10';
const BANNER = (v = UNDER_TEST) => `    OpenVMX ${v} - OpenVMS-compatible\n`;

function cn3Observations() {
  const R = newObservations();
  R.want_version = UNDER_TEST;
  observe(R, 'OVMXA', BANNER() + CN3_LINES('c3', 'c4', 'c5'));
  observe(R, 'OVMXB', BANNER() + CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', VAXC_ADMITS);
  R.sca = { OVMXA: 100, OVMXB: 120, VAXC: 90 };
  return R;
}

// Every node offers Username:, the gate logs in, and DCL answers (rd vms-db1).
function loggedInEverywhere(R, at = 600) {
  for (const w of GATE.NODES) {
    observe(R, w, 'Username: ');
    GATE.observeLogin(R, w, at - 60);
    R.login_sent[w] = R.transcript[w].length;
    observe(R, w, 'SYSTEM\nPassword:\n Welcome to OpenVMS\n$ ');
    GATE.observeLogin(R, w, at);
    // rd vms-c56: then the gate reads the node's own SHOW CLUSTER.
    if (GATE.LOGIN_NODES.includes(w)) observe(R, w, '\n' + SHOW_CLUSTER(w));
  }
  return sessionsPassed(R);
}

// A real SHOW SYSTEM table as an OVMX node prints it (V0.7-13, DECnet on).
const OVMX_SHOW_SYSTEM = [
  'OpenVMX V0.7-13 - OpenVMS-compatible  on node OVMXA  21-OCT-2026 14:17:46.84  Uptime  0 00:02:59',
  '  Pid    Process Name          CPU         Page flts   Pages',
  '10000004 JOB_CONTROL        0 00:00:00.02         89     363',
  '10000001                    0 00:00:01.06         81     357',
  '10000003 NETACP             0 00:00:00.15       1311    1683',
  '10000005 SYSTEM             0 00:00:00.47       1098     876',
  ' $ ',
].join('\n');

// The session checks (rd vms-5843/vms-6512) as they look when they passed.
function sessionsPassed(R) {
  R.session_checked = true;
  R.show_system = {};
  for (const w of GATE.LOGIN_NODES) {
    const rows = GATE.parseShowSystem(OVMX_SHOW_SYSTEM);
    R.show_system[w] = { rows, ok: true, why: GATE.showSystemWhy(rows) };
  }
  R.sethost = {};
  for (const [a, b] of GATE.SETHOST_LEGS) R.sethost[`${a}->${b}`] = { ok: true, step: 'done', remote_node: b };
  return R;
}

test('a scrollback that scrolls does not un-join a node', () => {
  const R = newObservations();
  observe(R, 'OVMXA', CN3_LINES('c3', 'c4', 'c5'));
  assert.equal(R.member.OVMXA, true);

  // The terminal trims: the cluster lines are gone from the SCREEN, and 1200
  // lines of DCL have taken their place. The observation must not shrink.
  observe(R, 'OVMXA', '$ SHOW TIME\n'.repeat(40));
  assert.equal(R.member.OVMXA, true, 'a node does not un-join because its console scrolled');
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

test('CN=3 needs BOTH OVMX nodes announcing their membership AND the real VAX admitting both', () => {
  const R = cn3Observations();
  assert.equal(isCN3(R), true);
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(verdictOf(R), 'CN=3');

  const noVaxc = cn3Observations();
  noVaxc.vaxc_admitted = [];
  assert.equal(isCN3(noVaxc), false, 'the real VAX admitting nobody is not CN=3');

  const oneAdmitted = cn3Observations();
  oneAdmitted.vaxc_admitted = ['OVMXA', 'OVMXA'];
  assert.equal(isCN3(oneAdmitted), false, 'the real VAX must admit EACH OVMX node by name');

  const oneShort = cn3Observations();
  delete oneShort.member.OVMXB;
  assert.equal(isCN3(oneShort), false, 'a node that never said it is a member is not CN=3');

  // A node does not become a member by printing ANOTHER node's membership.
  const R2 = newObservations();
  observe(R2, 'OVMXA', '%CNXMAN,  now a VAXcluster member -- system OVMXB\n');
  assert.equal(R2.member.OVMXA, undefined);
  // ... nor by the OVMX diagnostic the console no longer carries.
  observe(R2, 'OVMXA', '%CNXMAN, this node is now a VAXcluster member\n');
  assert.equal(R2.member.OVMXA, undefined);
});

test('rd vms-c56: every OVMX node must hold all three systems as MEMBER in its OWN SHOW CLUSTER', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), true, 'control');
  assert.deepEqual(R.cluster_view.OVMXA, ['OVMXB', 'OVMXA', 'VAXC']);

  // OVMXA joined before OVMXB and never learned of it: its table lacks OVMXB.
  observe(R, 'OVMXA', '\n' + SHOW_CLUSTER('OVMXA', ['OVMXA', 'VAXC']));
  assert.equal(GATE.isPass(R), false, 'the LAST table a node printed is its view');
  assert.equal(verdictOf(R), "CN=3 but NOT EVERY MEMBER IN SHOW CLUSTER: OVMXA's SHOW CLUSTER lacks OVMXB as MEMBER");

  // A node that never printed SHOW CLUSTER is not assumed to hold anybody.
  const N = cn3Observations();
  N.cn3 = true;
  loggedInEverywhere(N);
  delete N.cluster_view.OVMXB;
  assert.equal(GATE.isPass(N), false);
  assert.match(verdictOf(N), /OVMXB showed no SHOW CLUSTER/);

  // A row whose STATUS is not MEMBER is not a member.
  const t = SHOW_CLUSTER('OVMXA').replace('| VAXC   | 00010001 |                 | MEMBER           |',
                                          '| VAXC   | 00010001 |                 | BRK_NON_MEMBER   |');
  assert.deepEqual(GATE.clusterViewOf(t), ['OVMXB', 'OVMXA']);
  assert.equal(GATE.clusterViewOf('no table here'), null);
});

test('driveClusterView types SHOW CLUSTER at the prompt and reads the table back', async () => {
  const R = newObservations();
  let screen = '$ ';
  const typed = [];
  const io = {
    type: async (w, t) => { typed.push(t); screen += SHOW_CLUSTER('OVMXA').slice(2); },
    read: async () => screen, sleep: async () => {}, log: () => {},
  };
  observe(R, 'OVMXA', screen);
  const view = await GATE.driveClusterView(R, 'OVMXA', io);
  assert.deepEqual(typed, ['SHOW CLUSTER\r']);
  assert.deepEqual(view, ['OVMXB', 'OVMXA', 'VAXC']);
  assert.deepEqual(R.cluster_view.OVMXA, view);
});

test('an OVMX node founding a cluster is a REGRESSION, named as one', () => {
  const R = cn3Observations();
  observe(R, 'OVMXA', '%CNXMAN, this node has quorum by its own votes\n');
  assert.match(verdictOf(R), /^REGRESSION: an OVMX node founded a cluster \(OVMXA\)/);
  // ... and in the words VMS prints for it on the console (rd vms-c56).
  const V = cn3Observations();
  observe(V, 'OVMXB', '%CNXMAN,  proposing formation of a VAXcluster\n');
  assert.match(verdictOf(V), /^REGRESSION: an OVMX node founded a cluster \(OVMXB\)/);
  // The real VAX founding is how the demo is meant to form.
  const C = cn3Observations();
  observe(C, 'VAXC', '%CNXMAN,  proposing formation of a VAXcluster\n');
  assert.doesNotMatch(verdictOf(C), /REGRESSION/);
});

test('a bugcheck outranks every other reading', () => {
  const R = cn3Observations();
  R.cn3 = true;
  observe(R, 'VAXC', '**** FATAL BUGCHECK CNXMGRERR ****\n');
  assert.match(verdictOf(R), /^BUGCHECK:/);
});

test('a failure is reported as what was SEEN, never as a diagnosis', () => {
  // The real run this comes from: the real VAX transmitted SCA frames the
  // whole time and then restarted, and one OVMX node never got in. Calling
  // that "the OVMX pair clustered and the real VAX admitted nobody" asserted a
  // cause the evidence did not carry.
  const R = newObservations();
  observe(R, 'OVMXA', '%SYSINIT, waiting to form or join a VMScluster system\n');
  observe(R, 'OVMXB', CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', 'KA655-B V5.3, VMB 2.7\nx\nKA655-B V5.3, VMB 2.7\n');
  R.sca = { OVMXA: 10, OVMXB: 20, VAXC: 30 };

  const v = verdictOf(R);
  assert.match(v, /^NOT CN=3:/);
  assert.match(v, /OVMXA never said "now a VAXcluster member -- system OVMXA"/);
  assert.match(v, /the real VAX admitted nobody/);
  assert.match(v, /VAXC restarted 1x/);
  assert.doesNotMatch(v, /SPLIT BRAIN/i, 'the gate must not diagnose, only report');
});

test('a node the hub never heard from is named', () => {
  const R = newObservations();
  R.sca = { OVMXA: 5, OVMXB: 5 };
  assert.match(verdictOf(R), /no SCA frames from VAXC/);
});

test('the MEMBER pattern reads the node VMS names, in VMS\'s own form', () => {
  // The real VAX's own line (C.console.txt) and an OVMX node's, two spaces each.
  assert.deepEqual(setOf('%CNXMAN,  now a VAXcluster member -- system VAXC', MEMBER), ['VAXC']);
  assert.deepEqual(setOf('%CNXMAN,  now a VAXcluster member -- system OVMXA\r\n', MEMBER), ['OVMXA']);
  assert.deepEqual(setOf('%CNXMAN, this node is now a VAXcluster member', MEMBER), []);
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
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), true, 'quiet, clustered and logged in everywhere passes');

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

test('a DCL prompt after the VMS NUL fill is a DCL prompt (real VAX: <CR><NUL>$)', () => {
  // OVMXA's console in the V0.7-9 gate run (b-first): the login banner, the
  // terminal inquiry VMS sends, then CR NUL $ -- exactly the oracle's bytes.
  const t = ' Welcome to OpenVMX V0.7-9 - OpenVMS-compatible\r\u001b[c\r\r\n\u0000$';
  assert.equal(GATE.atDcl(t), true);
  const R = { transcript: { OVMXA: 'Password: \r\n' + t }, login_sent: { OVMXA: 10 },
              username_at: { OVMXA: 1 } };
  GATE.observeLogin(R, 'OVMXA', 7);
  assert.equal(R.dcl_at.OVMXA, 7);
  // Still not a prompt: a line that only CONTAINS $ after the fill, and a bare fill.
  assert.equal(GATE.atDcl('x\n\u0000$ SHOW TIME'), false);
  assert.equal(GATE.atDcl('x\n\u0000'), false);
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

// ---- rd vms-db1: CN=3 is not the end of a visitor's run ---------------------

test('a member real VAX that never offers Username: fails the run (rd vms-db1)', () => {
  const R = cn3Observations();
  R.cn3 = true;
  for (const w of ['OVMXA', 'OVMXB']) {
    observe(R, w, 'Username: ');
    GATE.observeLogin(R, w, 400);
    R.login_sent[w] = R.transcript[w].length;
    observe(R, w, 'SYSTEM\nPassword:\n$ ');
    GATE.observeLogin(R, w, 420);
  }
  // The VAX is a member (it admitted both) and its startup stops.
  observe(R, 'VAXC', '%STDRV-I-STARTUP, OpenVMS startup begun\n');
  GATE.observeLogin(R, 'VAXC', 1200);
  assert.equal(isCN3(R), true, 'membership alone reads CN=3');
  assert.equal(GATE.isPass(R), false, 'but the run fails: a visitor sees a dead console');
  assert.match(verdictOf(R),
    /^CN=3 but NOT EVERY NODE USABLE: VAXC never offered Username: \(last line "%STDRV-I-STARTUP, OpenVMS startup begun"\)/);
});

test('the real VAX passes on offering Username:, and is never logged in to', () => {
  const R = cn3Observations();
  R.cn3 = true;
  for (const w of ['OVMXA', 'OVMXB']) {
    observe(R, w, 'Username: ');
    GATE.observeLogin(R, w, 400);
    R.login_sent[w] = R.transcript[w].length;
    observe(R, w, 'SYSTEM\nPassword:\n$ ');
    GATE.observeLogin(R, w, 420);
    observe(R, w, '\n' + SHOW_CLUSTER(w));
  }
  observe(R, 'VAXC', 'Username: ');
  GATE.observeLogin(R, 'VAXC', 600);
  sessionsPassed(R);           // the OVMX-only session checks (rd vms-5843/6512)
  assert.equal(GATE.isPass(R), true, 'Username: on the VAX proves its startup completed');
  assert.equal(verdictOf(R), 'CN=3');
  assert.equal(GATE.loginAction(R, 'VAXC'), null, 'no login is attempted on the real VAX');
  assert.ok(!GATE.LOGIN_NODES.includes('VAXC'));
});

test('on an OVMX node, Username: alone is not enough: a logged-in DCL $ must follow', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  const R2 = cn3Observations();
  R2.cn3 = true;
  loggedInEverywhere(R2);
  delete R2.dcl_at.OVMXB;
  R2.login_sent.OVMXB = R2.transcript.OVMXB.length;
  observe(R2, 'OVMXB', 'SYSTEM\nPassword:\n');
  GATE.observeLogin(R2, 'OVMXB', 900);
  assert.equal(R2.dcl_at.OVMXB, undefined);
  assert.match(verdictOf(R2), /OVMXB offered Username: at t=540s but never showed a DCL \$ after login/);
  assert.equal(GATE.isPass(R), true);
});

test('a $ printed before the login was sent does not count as logged in', () => {
  const R = newObservations();
  observe(R, 'OVMXA', '$ @SYS$STARTUP:STARTUP\nUsername: ');
  GATE.observeLogin(R, 'OVMXA', 10);
  observe(R, 'OVMXA', '$ ');
  GATE.observeLogin(R, 'OVMXA', 20);
  assert.equal(R.dcl_at.OVMXA, undefined, 'no login was sent yet');
});

test('a refused login is reported as such, not as a hang', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  delete R.dcl_at.OVMXB;
  R.login_sent.OVMXB = R.transcript.OVMXB.length;
  observe(R, 'OVMXB', 'User authorization failure\nUsername: ');
  GATE.observeLogin(R, 'OVMXB', 700);
  assert.equal(R.login_failed.OVMXB, true);
  assert.match(verdictOf(R), /OVMXB refused the login \(User authorization failure\)/);
});

test('the bound is enforced: DCL reached too late fails', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R, 2000);
  R.login_bound_s = 1500;
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R), /reached DCL at t=2000s, past the 1500s bound/);
});

test('nothing is typed before the cluster forms; then wake until Username:, then log in (OVMX only)', () => {
  const R = newObservations();
  observe(R, 'OVMXB', 'booting\n');
  observe(R, 'VAXC', 'booting\n');
  assert.equal(GATE.loginAction(R, 'OVMXB'), null, 'membership forms with nobody typing');
  assert.equal(GATE.loginAction(R, 'VAXC'), null);
  R.cn3 = true;
  assert.equal(GATE.loginAction(R, 'OVMXB'), 'wake', 'a VMS console offers Username: when asked');
  assert.equal(GATE.loginAction(R, 'VAXC'), 'wake');
  observe(R, 'OVMXB', 'Username: ');
  GATE.observeLogin(R, 'OVMXB', 50);
  observe(R, 'VAXC', 'Username: ');
  GATE.observeLogin(R, 'VAXC', 50);
  assert.equal(GATE.loginAction(R, 'OVMXB'), 'login');
  assert.equal(GATE.loginAction(R, 'VAXC'), null, 'the real VAX is done once it offers Username:');
  R.login_sent.OVMXB = R.transcript.OVMXB.length;
  assert.equal(GATE.loginAction(R, 'OVMXB'), null, 'one login, not a stream of them');
});

test('the matrix boots Node C after an OVMX node is up (rd vms-db1)', () => {
  const held = MATRIX.filter((r) => r.after && r.after.C && r.after.C.startsWith('OVMX'));
  assert.ok(held.length >= 1, 'a row holds C until an OVMX node is up');
  for (const r of held) {
    assert.ok(r.order.indexOf('C') > r.order.indexOf(r.after.C.slice(-1)),
      `${r.label}: the node C waits for is clicked before C`);
  }
});

test('the OVMX login is read from the hint the site publishes, or not at all', () => {
  const html = '<figcaption class="logintip">Log in with username <b>SYSTEM</b>, password <b>MANAGER</b>\n' +
               '<span class="alt">(or <b>GUEST</b> / <b>GUEST</b>)</span></figcaption>';
  assert.deepEqual(GATE.parseLoginHint(html), { user: 'SYSTEM', pass: 'MANAGER' });
  assert.equal(GATE.parseLoginHint('<p>no hint here</p>'), null, 'no hint: the gate refuses, never guesses');
  assert.equal(GATE.parseLoginHint(''), null);
});

// ---- Every OVMX node runs the build under test ------------------------------
// V0.7-8..V0.7-10 were gated with Node B still on a hand-built V0.7-7 image.

test('the version under test is read from ovmx_identity.h, the one source', () => {
  const h = '#define OVMX_PRODUCT_NAME       "OpenVMX"\n#define OVMX_PRODUCT_VERSION    "V0.7-10"\n';
  assert.equal(GATE.productVersionOf(h), 'V0.7-10');
  assert.equal(GATE.productVersionOf('#define OVMX_PRODUCT_NAME "OpenVMX"\n'), null);
});

test('a clustered, usable run with a STALE OVMX node fails, and names it', () => {
  const R = newObservations();
  R.want_version = UNDER_TEST;
  observe(R, 'OVMXA', BANNER() + CN3_LINES('c3', 'c4', 'c5'));
  observe(R, 'OVMXB', BANNER('V0.7-7') + CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', VAXC_ADMITS);
  R.sca = { OVMXA: 1, OVMXB: 1, VAXC: 1 };
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isQuiet(R), true);
  assert.equal(GATE.allReady(R), true, 'everything else about the run is fine');
  assert.equal(GATE.isPass(R), false, 'a stale node is not the build under test');
  assert.equal(verdictOf(R),
    'CN=3 but NOT THE BUILD UNDER TEST: OVMXB announced V0.7-7 -- the build under test is V0.7-10');
});

test('a BUGCHECK beside a STALE OVMX node says it is not the build under test (vms-4f4)', () => {
  const R = newObservations();
  R.want_version = UNDER_TEST;
  observe(R, 'OVMXA', BANNER() + CN3_LINES('c3', 'c4', 'c5'));
  observe(R, 'OVMXB', BANNER('V0.7-7') + CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', VAXC_ADMITS + '**** FATAL BUGCHECK LOCKMGRERR ****\n');
  R.cn3 = true;
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R),
    /^BUGCHECK: .* -- and NOT THE BUILD UNDER TEST: OVMXB announced V0\.7-7 -- the build under test is V0\.7-10$/);
  // Control: the same bugcheck with every node at the version under test is a
  // plain BUGCHECK verdict, with no staleness clause.
  const C = newObservations();
  C.want_version = UNDER_TEST;
  observe(C, 'OVMXA', BANNER() + CN3_LINES('c3', 'c4', 'c5'));
  observe(C, 'OVMXB', BANNER() + CN3_LINES('c4', 'c3', 'c5'));
  observe(C, 'VAXC', VAXC_ADMITS + '**** FATAL BUGCHECK LOCKMGRERR ****\n');
  assert.match(verdictOf(C), /^BUGCHECK: \{[^}]*\}$/);
});

test('a node that announces the right version AND an old one (a mixed image) fails', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), true, 'control: both nodes at the version under test pass');
  observe(R, 'OVMXA', ' Welcome to OpenVMX V0.7-9 - OpenVMS-compatible\n');
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R), /OVMXA announced V0\.7-10\+V0\.7-9/);
});

test('silence is not the right version: a node that never announced one fails', () => {
  const R = newObservations();
  R.want_version = UNDER_TEST;
  observe(R, 'OVMXA', BANNER() + CN3_LINES('c3', 'c4', 'c5'));
  observe(R, 'OVMXB', CN3_LINES('c4', 'c3', 'c5'));
  observe(R, 'VAXC', VAXC_ADMITS);
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R), /OVMXB never announced an OpenVMX version/);
});

test('a gate that was not told the version under test does not pass anything', () => {
  const R = cn3Observations();
  R.want_version = null;
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), false);
  assert.match(GATE.versionWhy(R), /not told which OpenVMX version/);
});

test('the version a node announced survives its console scrolling', () => {
  const R = cn3Observations();
  observe(R, 'OVMXB', '$ SHOW SYSTEM\n'.repeat(4000));
  assert.deepEqual(R.ovmx_versions.OVMXB, [UNDER_TEST]);
  assert.equal(GATE.versionWhy(R), null);
});

// ---- rd vms-124: the login prompt that times out between two polls ----------

// A console whose Username: is abandoned after `idleS` seconds, goes quiet, and
// shows a fresh banner + prompt on the next RETURN -- an OVMX login session.
function idleCycleGuest(idleS) {
  // stage: 'quiet' (waiting for the wake RETURN) -> 'user' (Username:) ->
  // 'pass' (Password:) -> 'dcl'. Each prompt is abandoned after idleS seconds.
  const g = { now: 0, since: null, quiet: true, stage: 'quiet', screen: 'boot done\n', typed: [] };
  g.advance = (s) => {
    g.now += s;
    if ((g.stage === 'user' || g.stage === 'pass') && g.since !== null && g.now - g.since >= idleS) {
      g.screen += '\nError reading command input\nTimeout period expired\n';
      g.quiet = true; g.stage = 'quiet'; g.since = null;
    }
  };
  g.type = (text) => {
    g.typed.push(text);
    if (g.stage === 'quiet') {
      if (text.endsWith('\r')) {          // RETURN, or any first line, wakes the session
        g.screen += '\n Welcome to the OpenVMX VAX Operating System\n\nUsername: ';
        g.quiet = false; g.stage = 'user'; g.since = g.now;
      }
    } else if (g.stage === 'user') {
      g.screen += text.replace(/\r$/, '') + '\nPassword: ';
      g.stage = 'pass'; g.since = g.now;
    } else if (g.stage === 'pass') {
      g.screen += '\n Welcome to OpenVMX V0.7-12 - OpenVMS-compatible\n$ ';
      g.stage = 'dcl'; g.since = null;
    }
  };
  return g;
}

async function runLoginPolicy({ followUp, pollS }) {
  const g = idleCycleGuest(20);
  const R = cn3Observations();
  R.cn3 = true;
  const lastWake = {};
  const io = {
    type: async (w, t) => g.type(t),
    read: async () => g.screen,
    sleep: async (ms) => g.advance(ms / 1000),
    user: () => 'SYSTEM', pass: () => 'MANAGER', log: () => {},
  };
  for (let el = 0; el < 600 && !R.login_sent.OVMXB; el += pollS) {
    observe(R, 'OVMXB', g.screen);
    GATE.observeLogin(R, 'OVMXB', el);
    if (followUp) {
      await GATE.driveLoginStep(R, 'OVMXB', el, lastWake, 20, io);
    } else {
      // the gate as it was: strike RETURN, then look again only a poll later
      const act = GATE.loginAction(R, 'OVMXB');
      if (act === 'wake' && el - (lastWake.OVMXB ?? -1e9) >= 20) { lastWake.OVMXB = el; g.type('\r'); }
      else if (act === 'login') { R.login_sent.OVMXB = R.transcript.OVMXB.length; g.type('SYSTEM\r'); }
    }
    g.advance(pollS);
  }
  return { sent: R.login_sent.OVMXB !== undefined, typed: g.typed };
}

test('NEGATIVE CONTROL: waking then re-reading a poll later never reaches a 20 s Username: (rd vms-124)', async () => {
  const r = await runLoginPolicy({ followUp: false, pollS: 25 });
  assert.equal(r.sent, false, 'the old cadence is slower than the prompt lifetime: it never logs in');
});

test('a wake is followed up fast enough to log in inside the prompt lifetime (rd vms-124)', async () => {
  const r = await runLoginPolicy({ followUp: true, pollS: 25 });
  assert.equal(r.sent, true, 'OVMXB is logged in to');
  assert.deepEqual(r.typed.slice(-2), ['SYSTEM\r', 'MANAGER\r']);
});

test('the real VAX is woken but never followed up or logged in to (rd vms-124)', async () => {
  const R = cn3Observations();
  R.cn3 = true;
  const typed = [];
  const io = { type: async (w, t) => typed.push([w, t]), read: async () => '', sleep: async () => {},
               user: () => 'U', pass: () => 'P', log: () => {} };
  const act = await GATE.driveLoginStep(R, 'VAXC', 100, {}, 20, io);
  assert.equal(act, null);
  assert.deepEqual(typed, [['VAXC', '\r']]);
});

// ---- rd vms-25f: a login that lands after the prompt expired ----------------
//
// OVMXB's console ended "Username: MANAGER / Password: / Timeout period
// expired" and then went silent for the rest of the run. Not a wedged console:
// the user name arrived after the 20 s Username: window had closed, the fresh
// session took it as its wake keystroke, the password became the user name, and
// the gate -- which typed once and never looked again -- sent nothing more.

function loginRig({ lateS = 0, retriesSpent = false } = {}) {
  const g = idleCycleGuest(20);
  const R = cn3Observations();
  R.cn3 = true;
  if (retriesSpent) R.login_retries = { OVMXB: GATE.LOGIN_RETRIES };
  const lastWake = {};
  let late = lateS;
  const io = {
    type: async (w, t) => {
      if (late && t !== '\r') { g.advance(late); late = 0; }   // the keys land late, once
      g.type(t);
    },
    read: async () => g.screen,
    sleep: async (ms) => g.advance(ms / 1000),
    user: () => 'SYSTEM', pass: () => 'MANAGER', log: () => {},
  };
  const step = async (el) => {
    observe(R, 'OVMXB', g.screen);
    GATE.observeLogin(R, 'OVMXB', el);
    return GATE.driveLoginStep(R, 'OVMXB', el, lastWake, 20, io);
  };
  return { g, R, step };
}

test('a user name that lands after Username: expired is retried and logs in (rd vms-25f)', async () => {
  const { g, R, step } = loginRig({ lateS: 25 });
  await step(0);
  observe(R, 'OVMXB', g.screen);
  GATE.observeLogin(R, 'OVMXB', 5);
  assert.equal(R.login_retries.OVMXB, 1, 'one retry was needed and counted');
  assert.ok(R.login_retry_log.length === 1, 'the retry is said, not hidden');
  assert.equal(R.dcl_at.OVMXB, 5, 'and the node reached its DCL prompt');
  assert.deepEqual(g.typed.filter((t) => t !== '\r').slice(-2), ['SYSTEM\r', 'MANAGER\r']);
  assert.doesNotMatch(g.screen, /Username: MANAGER/, 'the password was never taken as a user name');
});

test('NEGATIVE CONTROL: with its retries spent the same late landing strands the node, as it did (rd vms-25f)', async () => {
  const { g, R, step } = loginRig({ lateS: 25, retriesSpent: true });
  await step(0);
  observe(R, 'OVMXB', g.screen);
  GATE.observeLogin(R, 'OVMXB', 5);
  assert.equal(R.dcl_at.OVMXB, undefined, 'never reaches DCL: the retry is what recovers it');
  assert.equal(GATE.isPass(R), false);
});

test('a login that lands first time is not counted as a retry (rd vms-25f)', async () => {
  const { g, R, step } = loginRig();
  await step(0);
  observe(R, 'OVMXB', g.screen);
  GATE.observeLogin(R, 'OVMXB', 5);
  assert.equal(R.dcl_at.OVMXB, 5);
  assert.equal(R.login_retries, undefined);
});

test('observeLogin starts a login over when the console timed it out after it was typed (rd vms-25f)', () => {
  const R = cn3Observations();
  R.cn3 = true;
  observe(R, 'OVMXB', 'x\nUsername: ');
  GATE.observeLogin(R, 'OVMXB', 1);
  R.login_sent.OVMXB = R.transcript.OVMXB.length;
  observe(R, 'OVMXB', 'x\nUsername: SYSTEM\nPassword: \nError reading command input\nTimeout period expired\n');
  GATE.observeLogin(R, 'OVMXB', 30);
  assert.equal(R.login_sent.OVMXB, undefined, 'forgotten: the next poll wakes the console and logs in again');
  assert.equal(R.login_retries.OVMXB, 1);
  assert.equal(GATE.loginAction(R, 'OVMXB'), 'wake');
});

// ---- rd vms-c56: an OVMX console carries only what a VMS console carries ----

const CAPTURES = new URL('../../../tests/lab/captures/', import.meta.url);
const C56 = (f) => fs.readFileSync(new URL(`vms-c56-console-chatter-20261021/${f}`, CAPTURES), 'utf8');

// Every real-VMS console in the lab captures: the real VAXen's own consoles
// (VAXC, vax1, vax2, vax3 ...), never an OVMX node's.
function realVmsConsoles() {
  const out = [];
  const walk = (dir) => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = new URL(e.name + (e.isDirectory() ? '/' : ''), dir);
      if (e.isDirectory()) walk(p);
      else if (/^(VAXC[^/]*\.(log|txt)|[^/]*vax\d[^/]*\.(log|txt)|C\.console\.txt)$/i.test(e.name) &&
               !/ovmx/i.test(e.name)) out.push(p);
    }
  };
  walk(CAPTURES);
  return out;
}

test('rd vms-c56: the V0.7-13 OVMX consoles FAIL the chatter assertion, and say why', () => {
  for (const [f, who] of [['A.console.txt', 'OVMXA'], ['B.console.txt', 'OVMXB']]) {
    const c = GATE.consoleChatter(C56(f));
    assert.ok(c.length > 10, `${who}: expected the V0.7-13 chatter, got ${c.length}`);
    const whys = c.map((x) => x.why).join('\n');
    assert.match(whys, /not a form VMS prints for %CNXMAN/, `${who}: non-VMS %CNXMAN lines`);
    assert.match(whys, /%DLM is not a message a VMS console prints/, `${who}: %DLM lines`);
    assert.match(whys, /not a form VMS prints for %PEA0/, `${who}: "%PEA0, channel verified"`);
  }
  const a = GATE.consoleChatter(C56('A.console.txt'));
  assert.ok(a.some((x) => x.why === 'substrate printk timestamp' &&
                          x.line === "[   37.218759] %CNXMAN, no coordinator CSID seen yet: this node's CSID was NOT learned"),
            'node A: the Linux printk stamp, on the exact line the field report quoted');
  const b = GATE.consoleChatter(C56('B.console.txt'));
  assert.ok(b.some((x) => x.line.startsWith('%DLM, refusing a lock message (cat 0x02 op 0x01) from system 0x10001')),
            'node B: the %DLM refusal that landed at the logged-in DCL prompt');
});

test('rd vms-c56: the REAL VAX\'s own console passes the same assertion -- the allowlist is VMS\'s', () => {
  assert.deepEqual(GATE.consoleChatter(C56('C.console.txt')), []);
  // ... and so does every real VAX console in the lab captures.
  const files = realVmsConsoles();
  assert.ok(files.length >= 50, `expected the VAX lab captures, found ${files.length}`);
  for (const f of files) {
    const text = fs.readFileSync(f, 'latin1');
    const bad = GATE.consoleChatter(text).filter((x) => !/%OVMX|\bOVMX[A-Z]?\b.*-- SYSKRNL/.test(x.line));
    // A capture file is one console; a real VAX's never carries a printk stamp
    // or an OVMX-only line. Lines glued by a capture tool (two messages with no
    // newline between) are not a VMS form and are skipped by name below.
    // A line the capture cut short ("%CNXMAN,  proposing addition of s"), or
    // ran the lab host's own output into ("...blocking activityarp: local IP
    // address ..."), is a VMS form mangled by the capture, not another message.
    const forms = [...GATE.CNXMAN_NAMED_FORMS, ...GATE.CNXMAN_PLAIN_FORMS].map((p) => `%CNXMAN,  ${p}`);
    const cutShort = (x) => { const m = x.line.trim(); return forms.some((f) => f.startsWith(m) || m.startsWith(f)); };
    // Which identifiers a real VAX prints at its prompt grows with every capture
    // (any DCL command's messages); that list is grounded in the test below. What
    // every real VAX console must ALSO satisfy is the rest of the rule: no printk
    // stamp, and the cluster facilities only in VMS's forms.
    const formOrStamp = bad.filter((x) => !/is not a message a VMS console prints$/.test(x.why));
    const unglued = formOrStamp.filter((x) => (x.line.match(/%[A-Za-z]/g) || []).length === 1 &&
                                      !/[^\x20-\x7e]/.test(x.line) && !cutShort(x));
    assert.deepEqual(unglued, [], `real VAX console ${f.pathname.split('/captures/')[1]}`);
  }
});

test('rd vms-c56: every allowlisted identifier and form is one a real VAX printed', () => {
  const corpus = realVmsConsoles().map((f) => fs.readFileSync(f, 'latin1')).join('\n');
  for (const id of GATE.VMS_CONSOLE_IDS) {
    assert.ok(corpus.includes(`%${id},`), `%${id}, is on no real VAX console in tests/lab/captures`);
  }

  for (const p of GATE.CNXMAN_NAMED_FORMS) assert.ok(corpus.includes(`%CNXMAN,  ${p} `), p);
  for (const p of GATE.CNXMAN_PLAIN_FORMS) assert.ok(corpus.includes(`%CNXMAN,  ${p}`), p);
  assert.ok(corpus.includes('%PEA0, Port has Closed Virtual Circuit - REMOTE NODE '));
  assert.ok(corpus.includes('%PEA0, Excessive packet losses on LAN Path from '));
  assert.ok(corpus.includes('%SYSINIT, waiting to form or join a VMScluster system'));
  // OVMX's own boot narration is listed apart, and is NOT claimed to be VMS's.
  for (const id of GATE.OVMX_BOOT_IDS) assert.ok(!GATE.VMS_CONSOLE_IDS.includes(id), id);
});

test('rd vms-c56/vms-158d: the console the fix produces passes', () => {
  // Node A after vms-158d: the substrate line, the product banner, SYSINIT,
  // STDRV, the connection manager's VMS lines, RUN's own line, the login.
  const after = [
    'OVMX/Linux -- SYSKRNL (Linux kernel)',
    '',
    '    OpenVMX V0.7-14 - OpenVMS-compatible',
    '    21-OCT-2026 15:05:36.62',
    '',
    '%SYSINIT, waiting to form or join a VMScluster system',
    '%STDRV-I-STARTUP, OpenVMX startup begun at 21-OCT-2026 15:05:49.85',
    '%CNXMAN,  sending VAXcluster membership request to system VAXC',
    '%CNXMAN,  now a VAXcluster member -- system OVMXA',
    '%RUN-S-PROC_ID, identification of created process is 10000003',
    ' Welcome to the OpenVMX X86_64 Operating System, Version V0.7-14 (OpenVMS-compatible)',
    'Username: SYSTEM',
    '$ ',
  ].join('\n');
  // The firmware and the page's own line before the OS are not the OS's console.
  assert.deepEqual(GATE.consoleChatter('%NIC-CFG, initramfs=x mac=y\nSeaBIOS (version 1)\n' + after), []);
  // Node B (NetBSD/VAX) has no substrate line: its console starts at the banner.
  assert.deepEqual(GATE.consoleChatter('KA655-B V5.3, VMB 2.7\n>>>B DUA0\n' + after.split('\n').slice(1).join('\n')), []);
});

test('rd vms-158d: no OVMX boot narration is allowed on the console any more', () => {
  assert.deepEqual(GATE.OVMX_BOOT_IDS, [], 'the allowlist is empty: the gate is strict');
  const one = (line) => GATE.consoleChatter('    OpenVMX V0.7-14 - OpenVMS-compatible\n' + line).map((x) => x.why);
  for (const [line, id] of [
    ['%OVMX-I-EXEC, VMS executive attached on /dev/vms', 'OVMX-I-EXEC'],
    ['%OVMX-I-SYSDISK, mounting system disk VDA0:', 'OVMX-I-SYSDISK'],
    ['%OVMX-I-MOUNTED, system disk VDA0: mounted', 'OVMX-I-MOUNTED'],
    ['%OVMX-I-SCSNODE, node name OVMXA set from SYS$SYSTEM:OVMXVMSSYS.PAR', 'OVMX-I-SCSNODE'],
    ['%OVMX-I-EXEC, system identity SYSTEM [1,4] established by the executive', 'OVMX-I-EXEC'],
    ['%DECNET-I-STARTNET, NETACP launched detached to serve inbound $ SET HOST.', 'DECNET-I-STARTNET'],
  ]) assert.deepEqual(one(line), [`%${id} is not a message a VMS console prints`], line);
  // An identifier with no '%' is still an identifier (V0.7-13 Node B, STARTNET).
  assert.deepEqual(one("DECNETD-E-NOSOCKET, bpf datalink open on 'qe0' (0x6003) failed: Permission denied"),
                   ['DECNETD-E-NOSOCKET is not a message a VMS console prints']);
  assert.deepEqual(one(' DECNETD-I-DATALINK, datalink open on ETH0'),
                   ['DECNETD-I-DATALINK is not a message a VMS console prints']);
  // ... but a DCL continuation line ("-RMS-E-FNF, ...") is VMS's own form.
  assert.deepEqual(one('-RMS-E-FNF, file not found'), []);
  const b = GATE.consoleChatter(C56('B.console.txt'));
  assert.ok(b.some((x) => x.why === 'DECNETD-E-NOSOCKET is not a message a VMS console prints'),
            'V0.7-13 Node B: the bare DECNETD-E-NOSOCKET line is caught');
  assert.ok(b.some((x) => x.why === '%OVMX-I-EXEC is not a message a VMS console prints'),
            'V0.7-13 Node B: its boot narration is caught');
});

test('rd vms-c56: each forbidden kind is caught on its own (no blanket pass)', () => {
  const one = (line) => GATE.consoleChatter('    OpenVMX V0.7-14 - OpenVMS-compatible\n' + line).map((x) => x.why);
  assert.deepEqual(one('[   37.218759] %CNXMAN,  now a VAXcluster member -- system OVMXA'),
                   ['substrate printk timestamp'], 'a VMS form with a printk stamp');
  assert.deepEqual(one('%CNXMAN, now a VAXcluster member -- system OVMXA'),
                   ['not a form VMS prints for %CNXMAN'], 'ONE space after the comma is not VMS\'s');
  assert.deepEqual(one('%CNXMAN,  now a VAXcluster member -- system ovmxa'),
                   ['not a form VMS prints for %CNXMAN'], 'a node name VMS would not print');
  assert.deepEqual(one('%PEA0, virtual circuit open'), ['not a form VMS prints for %PEA0']);
  assert.deepEqual(one('%MSCP_CL, the member answered NOT PRESENT HERE'),
                   ['%MSCP_CL is not a message a VMS console prints']);
  assert.deepEqual(one('%OVMX-W-CLUSTERPORT, cluster port did not start'),
                   ['%OVMX-W-CLUSTERPORT is not a message a VMS console prints']);
  assert.deepEqual(one('%CNXMAN,  proposing formation of a VAXcluster'), []);
});

test('rd vms-c56: chatter on an OVMX console fails a run that otherwise passes; the real VAX is not held to it', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), true, 'control');
  observe(R, 'VAXC', '%CNXMAN,  completing VAXcluster state transition\n');
  assert.equal(GATE.isPass(R), true, 'the real VAX\'s own lines are VMS by definition');
  observe(R, 'OVMXB', '\n%DLM, refusing a lock message (cat 0x02 op 0x01) from system 0x10001\n$ ');
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R), /^CN=3 but CONSOLE CHATTER: OVMXB 1\+ line\(s\), first "%DLM, refusing a lock message/);
  // And a run that did not form names it among its reasons.
  const N = newObservations();
  observe(N, 'OVMXA', '%OVMX-I-EXEC, x\n[   37.2] %PEA0, channel verified\n');
  assert.match(verdictOf(N), /console chatter: OVMXA/);
});


// ---------------------------------------------------------------------------
// rd vms-6512: SHOW SYSTEM against the real VAX's system-process list.
// ---------------------------------------------------------------------------

test('SHOW SYSTEM rows parse as the node printed them, unnamed rows included', () => {
  const rows = GATE.parseShowSystem('junk\n' + OVMX_SHOW_SYSTEM);
  assert.deepEqual(rows.map((r) => r.name), ['JOB_CONTROL', '', 'NETACP', 'SYSTEM']);
  assert.equal(rows[2].pid, '10000003');
  assert.equal(GATE.parseShowSystem('no table here'), null);
});

test('the real VAX\'s own SHOW SYSTEM parses to the oracle list (VAXC capture, 2026-10-21)', () => {
  const vaxc = [
    '  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages',
    '20200201 SWAPPER         HIB     16        0   0 00:00:00.05         0      0   ',
    '20200206 CLUSTER_SERVER  HIB     12       11   0 00:00:00.04       207    315   ',
    '20200207 CONFIGURE       HIB     11        6   0 00:00:00.04       114    175   ',
    '20200208 LANACP          HIB     13       39   0 00:00:00.12       357    841   ',
    '2020020A IPCACP          HIB     10        6   0 00:00:00.01        99    177   ',
    '2020020B ERRFMT          HIB      9       21   0 00:00:00.04       146    227   ',
    '2020020C CACHE_SERVER    HIB     16        6   0 00:00:00.00        84    134   ',
    '2020020D OPCOM           HIB      9       62   0 00:00:00.08       210    259   ',
    '2020020E AUDIT_SERVER    HIB      9       42   0 00:00:00.13       514    758   ',
    '2020020F JOB_CONTROL     HIB      8       15   0 00:00:00.03       192    343   ',
    '20200210 SECURITY_SERVER HIB     10       22   0 00:00:00.06       791   1178   ',
    '20200211 SMISERVER       HIB      7       33   0 00:00:00.10       343    544   ',
    '20200212 TP_SERVER       HIB     10       12   0 00:00:00.04       205    317   ',
    '20200213 SYSTEM          CUR      7      116   0 00:00:00.14       862    512   ',
  ].join('\n');
  const names = GATE.parseShowSystem(vaxc).map((r) => r.name).filter((n) => n !== 'SYSTEM');
  for (const n of names) assert.ok(GATE.SHOW_SYSTEM_ORACLE.includes(n), n);
  // every oracle name is either required of OVMX or ledgered as a gap -- never both
  for (const n of GATE.SHOW_SYSTEM_ORACLE) {
    assert.ok(GATE.SHOW_SYSTEM_REQUIRED.includes(n) !== (n in GATE.SHOW_SYSTEM_GAPS), n);
  }
});

test('SHOW SYSTEM: JOB_CONTROL and NETACP are required, the gaps are honestly absent', () => {
  const ok = GATE.parseShowSystem(OVMX_SHOW_SYSTEM);
  assert.equal(GATE.showSystemWhy(ok), null);
  const noNet = ok.filter((r) => r.name !== 'NETACP');
  assert.match(GATE.showSystemWhy(noNet), /missing NETACP/);
  assert.match(GATE.showSystemWhy(null), /no SHOW SYSTEM table/);
});

test('SHOW SYSTEM: a ledgered gap that appears fails -- a placeholder, or a ledger to move', () => {
  const rows = [...GATE.parseShowSystem(OVMX_SHOW_SYSTEM), { pid: '10000009', name: 'OPCOM' }];
  assert.match(GATE.showSystemWhy(rows), /shows OPCOM, which the gap ledger says OVMX lacks/);
});

test('a run whose SHOW SYSTEM falls short does not pass, and says why', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  assert.equal(GATE.isPass(R), true);
  R.show_system.OVMXB = { rows: [], ok: true, why: 'missing JOB_CONTROL,NETACP' };
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R), /^CN=3 but OVMXB SHOW SYSTEM: missing JOB_CONTROL,NETACP/);
});

// ---------------------------------------------------------------------------
// rd vms-5843: SET HOST both ways between the OVMX nodes.
// ---------------------------------------------------------------------------

test('a run whose SET HOST fails does not pass; checks that never ran do not pass either', () => {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  R.sethost['OVMXB->OVMXA'] = { ok: false, step: 'remote Username:', why: '%SYSTEM-F-UNREACHABLE, remote node is not currently reachable' };
  assert.equal(GATE.isPass(R), false);
  assert.match(verdictOf(R), /SET HOST OVMXB->OVMXA failed at remote Username:: %SYSTEM-F-UNREACHABLE/);
  delete R.session_checked;
  assert.match(GATE.sessionWhy(R), /never ran/);
});

// A scripted pair of OVMX consoles: what each prints in answer to what is typed.
function setHostRig(script) {
  const R = cn3Observations();
  R.cn3 = true;
  loggedInEverywhere(R);
  delete R.session_checked; R.sethost = {}; R.show_system = {};
  const typed = [];
  const io = {
    type: async (w, t) => { typed.push([w, t]); const out = script(w, t); if (out) observe(R, w, R.transcript[w] + out); return true; },
    read: async (w) => R.transcript[w],
    sleep: async () => {}, log: () => {},
    user: () => 'SYSTEM', pass: () => 'MANAGER',
  };
  return { R, io, typed };
}

test('SET HOST leg: Username:, Password:, remote NODENAME, LOGOUT, %REM-S-END back home', async () => {
  const { R, io, typed } = setHostRig((w, t) => {
    const peer = w === 'OVMXA' ? 'OVMXB' : 'OVMXA';
    if (t.startsWith('SET HOST')) return `SET HOST ${peer}\n\n Welcome to OpenVMX\n\nUsername: `;
    if (t === 'SYSTEM\r') return 'SYSTEM\nPassword: ';
    if (t === 'MANAGER\r') return '\n Welcome to OpenVMX V0.7-13\n$ ';
    if (t.startsWith('WRITE')) return `WRITE SYS$OUTPUT "SETHOST-"+F$GETSYI("NODENAME")\nSETHOST-${peer}\n$ `;
    if (t === 'LOGOUT\r') return `LOGOUT\n  SYSTEM logged out\n%REM-S-END, control returned to node ${w}::\n$ `;
    return '';
  });
  const rec = await GATE.runSetHost(R, 'OVMXA', 'OVMXB', io);
  assert.equal(rec.ok, true, JSON.stringify(rec));
  assert.equal(rec.remote_node, 'OVMXB');
  assert.deepEqual(typed.map(([, t]) => t.split(' ')[0].replace('\r', '')),
                   ['SET', 'SYSTEM', 'MANAGER', 'WRITE', 'LOGOUT']);
});

test('SET HOST leg: a network error is reported in VMS\'s words, and nothing more is typed', async () => {
  const { R, io, typed } = setHostRig((w, t) =>
    t.startsWith('SET HOST') ? 'SET HOST OVMXB\n%SYSTEM-F-UNREACHABLE, remote node is not currently reachable\n$ ' : '');
  const rec = await GATE.runSetHost(R, 'OVMXA', 'OVMXB', io);
  assert.equal(rec.ok, false);
  assert.match(rec.why, /UNREACHABLE/);
  assert.equal(typed.length, 1);
});

test('SET HOST leg: a session that lands on the wrong node fails (the local DCL answered)', async () => {
  const { R, io } = setHostRig((w, t) => {
    if (t.startsWith('SET HOST')) return 'SET HOST OVMXB\nUsername: ';
    if (t === 'SYSTEM\r') return 'SYSTEM\nPassword: ';
    if (t === 'MANAGER\r') return '\n$ ';
    if (t.startsWith('WRITE')) return 'SETHOST-OVMXA\n$ ';
    return '';
  });
  const rec = await GATE.runSetHost(R, 'OVMXA', 'OVMXB', io);
  assert.equal(rec.ok, false);
  assert.match(rec.why, /on OVMXA, not OVMXB/);
});
