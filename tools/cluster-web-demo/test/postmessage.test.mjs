import test from 'node:test';
import assert from 'node:assert/strict';
import { L2Hub } from '../l2/hub.mjs';
import { attachSwitch, connectNicPipe } from '../l2/port-postmessage.mjs';
import { MIN_FRAME, ETHERTYPE_SCA } from '../l2/wire.mjs';

// An in-memory postMessage fabric. Delivery uses structuredClone WITH the transfer option, so it
// faithfully reproduces browser transfer semantics — INCLUDING neutering the source ArrayBuffer.
// If the switch ever wrongly transferred one buffer to two recipients, the 2nd clone would throw.
function makeFabric() {
  const parentListeners = [];
  const childListeners = new Map();               // token -> [handler]
  const clone = (msg, transfer) =>
    (transfer && transfer.length) ? structuredClone(msg, { transfer }) : structuredClone(msg);
  return {
    parentAddListener(h) { parentListeners.push(h); return () => {}; },
    childAddListener(token, h) {
      if (!childListeners.has(token)) childListeners.set(token, []);
      childListeners.get(token).push(h);
      return () => {};
    },
    // child -> parent (event.source = token identifies the child)
    postUpFrom(token, msg, transfer) {
      const data = clone(msg, transfer);
      for (const h of parentListeners) h({ data, source: token });
    },
    // parent -> a specific child
    postDownTo(token, msg, transfer) {
      const data = clone(msg, transfer);
      for (const h of (childListeners.get(token) || [])) h({ data, source: 'parent' });
    },
  };
}

function scaFrame(tag) {
  const u8 = new Uint8Array(MIN_FRAME + 8);
  for (let i = 0; i < 6; i++) u8[i] = 0xff;                    // dst
  for (let i = 6; i < 12; i++) u8[i] = 0x10 + i;              // src
  u8[12] = (ETHERTYPE_SCA >> 8) & 0xff; u8[13] = ETHERTYPE_SCA & 0xff;
  u8[MIN_FRAME] = tag;
  return u8;
}

// Wire up a 3-node page: parent switch + one NIC pipe per node iframe.
function makeCluster() {
  const fabric = makeFabric();
  const hub = new L2Hub();
  const names = ['OVMXA', 'OVMXB', 'VAXC'];
  const rx = { OVMXA: [], OVMXB: [], VAXC: [] };
  const pipes = {};

  attachSwitch(hub, {
    addMessageListener: fabric.parentAddListener,
    nodes: names.map((name) => ({
      name,
      post: (msg, transfer) => fabric.postDownTo(name, msg, transfer),
      match: (event) => event.source === name,
    })),
  });

  for (const name of names) {
    pipes[name] = connectNicPipe({
      postUp: (msg, transfer) => fabric.postUpFrom(name, msg, transfer),
      addMessageListener: (h) => fabric.childAddListener(name, h),
      toNicRx: (frameU8) => rx[name].push([...frameU8]),
      fromParent: (event) => event.source === 'parent',
    });
  }
  return { hub, pipes, rx };
}

test('a 0x6007 frame from one node reaches the other two, never itself', () => {
  const { pipes, rx } = makeCluster();
  pipes.OVMXA.nicTx(scaFrame(0xA1));
  assert.equal(rx.OVMXA.length, 0, 'sender does not receive its own frame');
  assert.equal(rx.OVMXB.length, 1);
  assert.equal(rx.VAXC.length, 1);
  assert.equal(rx.OVMXB[0][MIN_FRAME], 0xA1, 'frame bytes intact across the postMessage hops');
  assert.equal(rx.VAXC[0][MIN_FRAME], 0xA1);
});

test('bidirectional: every node can transmit to every other', () => {
  const { pipes, rx } = makeCluster();
  pipes.OVMXA.nicTx(scaFrame(1));
  pipes.OVMXB.nicTx(scaFrame(2));
  pipes.VAXC.nicTx(scaFrame(3));
  assert.deepEqual(rx.OVMXA.map((f) => f[MIN_FRAME]).sort(), [2, 3]);   // got B's and C's, not its own
  assert.deepEqual(rx.OVMXB.map((f) => f[MIN_FRAME]).sort(), [1, 3]);
  assert.deepEqual(rx.VAXC.map((f) => f[MIN_FRAME]).sort(), [1, 2]);
});

test('transfer neutering is real in the fabric (broadcast must copy per recipient)', () => {
  // This passes only because attachSwitch copies the buffer per recipient before transferring.
  // A regression to a single shared transferable would throw "already detached" on the 2nd node.
  const { pipes, rx } = makeCluster();
  assert.doesNotThrow(() => pipes.VAXC.nicTx(scaFrame(0x55)));
  assert.equal(rx.OVMXA.length, 1);
  assert.equal(rx.OVMXB.length, 1);
});

test('a malformed inbound message is dropped, not delivered to any NIC', () => {
  const fabric = makeFabric();
  const hub = new L2Hub();
  const rx = [];
  attachSwitch(hub, {
    addMessageListener: fabric.parentAddListener,
    nodes: [
      { name: 'A', post: (m, t) => fabric.postDownTo('A', m, t), match: (e) => e.source === 'A' },
      { name: 'B', post: (m, t) => fabric.postDownTo('B', m, t), match: (e) => e.source === 'B' },
    ],
  });
  connectNicPipe({
    postUp: (m, t) => fabric.postUpFrom('B', m, t),
    addMessageListener: (h) => fabric.childAddListener('B', h),
    toNicRx: (f) => rx.push([...f]),
  });
  // Node A posts junk directly onto the fabric (not via a pipe): wrong tag + a too-short frame.
  fabric.postUpFrom('A', { t: 'not-ours', v: 1, kind: 0, frame: new Uint8Array(20).buffer });
  fabric.postUpFrom('A', { t: 'ovmx-l2', v: 1, kind: 0, frame: new Uint8Array(4).buffer });
  assert.equal(rx.length, 0, 'neither malformed message reached the NIC');
});
