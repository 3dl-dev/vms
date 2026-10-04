import test from 'node:test';
import assert from 'node:assert/strict';
import { L2Hub } from '../l2/hub.mjs';
import { MIN_FRAME, ETHERTYPE_SCA } from '../l2/wire.mjs';

function frame(tag = 0) {
  const u8 = new Uint8Array(MIN_FRAME + 4);
  u8[12] = (ETHERTYPE_SCA >> 8) & 0xff; u8[13] = ETHERTYPE_SCA & 0xff;
  u8[MIN_FRAME] = tag;
  return u8;
}

// A port whose received frames are recorded.
function recordingPort(hub, name) {
  const rx = [];
  const handle = hub.addPort({ name, send: (f) => rx.push([...f]) });
  return { handle, rx };
}

test('a frame floods to every OTHER port, never back to the sender', () => {
  const hub = new L2Hub();
  const a = recordingPort(hub, 'A');
  const b = recordingPort(hub, 'B');
  const c = recordingPort(hub, 'C');

  const ok = a.handle.emit(frame(0x42));
  assert.equal(ok, true);
  assert.equal(a.rx.length, 0, 'no loopback to sender');
  assert.equal(b.rx.length, 1);
  assert.equal(c.rx.length, 1);
  assert.equal(b.rx[0][MIN_FRAME], 0x42, 'bytes forwarded verbatim');
  assert.equal(c.rx[0][MIN_FRAME], 0x42);
});

test('each recipient gets an independent copy (no shared mutation)', () => {
  const hub = new L2Hub();
  const a = recordingPort(hub, 'A');
  // b and c capture the live view, then we prove recording copied.
  const seen = [];
  hub.addPort({ name: 'B', send: (f) => seen.push(f) });
  hub.addPort({ name: 'C', send: (f) => seen.push(f) });
  a.handle.emit(frame(0x7));
  assert.equal(seen.length, 2);
  // Mutating one recipient's view must not corrupt the other's captured bytes-of-record.
  seen[0][MIN_FRAME] = 0x99;
  assert.equal(seen[1][MIN_FRAME], 0x7, 'recipients are not aliased through the sender buffer');
});

test('a removed port neither receives nor emits', () => {
  const hub = new L2Hub();
  const a = recordingPort(hub, 'A');
  const b = recordingPort(hub, 'B');
  b.handle.remove();
  assert.equal(hub.size, 1);
  a.handle.emit(frame());
  assert.equal(b.rx.length, 0, 'removed port receives nothing');
  assert.equal(b.handle.emit(frame()), false, 'removed port cannot emit');
});

test('malformed frames are dropped (not delivered), port stays live', () => {
  const drops = [];
  const hub = new L2Hub({ onDrop: (reason, name) => drops.push([reason, name]) });
  const a = recordingPort(hub, 'A');
  const b = recordingPort(hub, 'B');
  assert.equal(a.handle.emit(new Uint8Array(4)), false, 'too-short frame not forwarded');
  assert.equal(b.rx.length, 0);
  assert.deepEqual(drops, [['bad-frame', 'A']]);
  // Port still works afterward.
  assert.equal(a.handle.emit(frame(1)), true);
  assert.equal(b.rx.length, 1);
});

test('one port throwing does not stop delivery to the others', () => {
  const errors = [];
  const hub = new L2Hub({ onError: (err, name) => errors.push(name) });
  const a = recordingPort(hub, 'A');
  hub.addPort({ name: 'B', send: () => { throw new Error('boom'); } });
  const c = recordingPort(hub, 'C');
  const ok = a.handle.emit(frame(5));
  assert.equal(ok, true, 'still forwarded to at least one port');
  assert.deepEqual(errors, ['B']);
  assert.equal(c.rx.length, 1, 'C still got the frame despite B throwing');
});

test('portNames and size reflect connections', () => {
  const hub = new L2Hub();
  const a = recordingPort(hub, 'A');
  recordingPort(hub, 'B');
  assert.equal(hub.size, 2);
  assert.deepEqual(hub.portNames().sort(), ['A', 'B']);
  a.handle.remove();
  assert.deepEqual(hub.portNames(), ['B']);
});
