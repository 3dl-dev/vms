import test from 'node:test';
import assert from 'node:assert/strict';
import { installQemuNicWebSocket } from '../nic/qemu-ws-shim.mjs';
import { enframe } from '../nic/qemu-socket-framing.mjs';

function frame(len, seed = 0) {
  const u8 = new Uint8Array(len);
  for (let i = 0; i < len; i++) u8[i] = (i * 31 + seed * 7 + 1) & 0xff;
  return u8;
}
const tick = () => new Promise((r) => setTimeout(r, 0));

test('shim installs WebSocket on the given scope and opens asynchronously', async () => {
  const scope = {};
  installQemuNicWebSocket({ scope, onNicTx: () => {} });
  assert.equal(typeof scope.WebSocket, 'function');
  const ws = new scope.WebSocket('ws://ovmx/');
  assert.equal(ws.readyState, 0);            // CONNECTING synchronously
  let opened = false; ws.onopen = () => { opened = true; };
  await tick();
  assert.equal(ws.readyState, 1);            // OPEN after a tick
  assert.ok(opened);
});

test('guest TX: framed bytes in ws.send() emerge as Ethernet frames on onNicTx', () => {
  const scope = {};
  const tx = [];
  installQemuNicWebSocket({ scope, onNicTx: (f) => tx.push([...f]) });
  const ws = new scope.WebSocket('ws://ovmx/');
  const f1 = frame(60, 1), f2 = frame(1400, 2);
  // QEMU writes the length-prefixed stream; deliver it split across two send()s.
  const s = new Uint8Array(enframe(f1).byteLength + enframe(f2).byteLength);
  s.set(enframe(f1), 0); s.set(enframe(f2), enframe(f1).byteLength);
  ws.send(s.subarray(0, 30));            // partial
  ws.send(s.subarray(30));               // remainder
  assert.equal(tx.length, 2);
  assert.deepEqual(tx[0], [...f1]);
  assert.deepEqual(tx[1], [...f2]);
});

test('guest RX: deliverToGuest enframes and the guest sees it via ws.onmessage as an ArrayBuffer', async () => {
  const scope = {};
  const ctl = installQemuNicWebSocket({ scope, onNicTx: () => {} });
  const ws = new scope.WebSocket('ws://ovmx/');
  ws.binaryType = 'arraybuffer';
  const rx = [];
  ws.onmessage = (e) => rx.push(new Uint8Array(e.data));
  await tick();                          // must be OPEN before delivery
  assert.equal(ctl.connected(), true);
  const f = frame(200, 5);
  assert.equal(ctl.deliverToGuest(f), true);
  assert.equal(rx.length, 1);
  // The guest receives the QEMU-framed bytes (4-byte length + frame).
  assert.deepEqual([...rx[0]], [...enframe(f)]);
});

test('round-trip: a frame delivered to the guest and echoed back survives byte-identical', async () => {
  // Model a guest that echoes: whatever RX it gets (deframed), it re-sends (reframed).
  const scope = {};
  let ctl;
  const echoed = [];
  ctl = installQemuNicWebSocket({ scope, onNicTx: (f) => echoed.push([...f]) });
  const ws = new scope.WebSocket('ws://ovmx/');
  ws.binaryType = 'arraybuffer';
  ws.onmessage = (e) => {
    // guest deframes internally; here we simulate it re-sending the same framed bytes
    ws.send(new Uint8Array(e.data));
  };
  await tick();
  const f = frame(517, 9);
  ctl.deliverToGuest(f);
  assert.equal(echoed.length, 1);
  assert.deepEqual(echoed[0], [...f]);
});

test('deliverToGuest before OPEN is refused (no lost/duplicated frames)', () => {
  const scope = {};
  const ctl = installQemuNicWebSocket({ scope, onNicTx: () => {} });
  new scope.WebSocket('ws://ovmx/');       // still CONNECTING (no tick)
  assert.equal(ctl.connected(), false);
  assert.equal(ctl.deliverToGuest(frame(10)), false);
});

test('uninstall restores the previous WebSocket', () => {
  const sentinel = function () {};
  const scope = { WebSocket: sentinel };
  const ctl = installQemuNicWebSocket({ scope, onNicTx: () => {} });
  assert.notEqual(scope.WebSocket, sentinel);
  ctl.uninstall();
  assert.equal(scope.WebSocket, sentinel);
});
