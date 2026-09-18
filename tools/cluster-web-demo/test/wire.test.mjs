import test from 'node:test';
import assert from 'node:assert/strict';
import {
  makeDataMessage, parseMessage, ethertypeOf, asFrameU8, frameLengthOk,
  L2_MSG, MIN_FRAME, MAX_FRAME, ETHERTYPE_SCA,
} from '../l2/wire.mjs';

function frame(ethertype, payloadLen = 0, fill = 0x11) {
  const u8 = new Uint8Array(MIN_FRAME + payloadLen);
  for (let i = 0; i < 6; i++) u8[i] = 0xff;          // dst broadcast
  for (let i = 6; i < 12; i++) u8[i] = 0xaa;         // src
  u8[12] = (ethertype >> 8) & 0xff; u8[13] = ethertype & 0xff;
  for (let i = MIN_FRAME; i < u8.length; i++) u8[i] = fill;
  return u8;
}

test('DATA message round-trips and preserves bytes', () => {
  const f = frame(ETHERTYPE_SCA, 40);
  const { msg, transfer } = makeDataMessage(f);
  assert.equal(msg.t, L2_MSG);
  assert.equal(msg.v, 1);
  assert.equal(msg.kind, 0);
  assert.ok(msg.frame instanceof ArrayBuffer);
  assert.equal(transfer.length, 1);
  const parsed = parseMessage(msg);
  assert.ok(parsed);
  assert.deepEqual([...parsed.frame], [...f]);
  assert.equal(ethertypeOf(parsed.frame), ETHERTYPE_SCA);
});

test('ethertypeOf reads bytes 12..13 big-endian', () => {
  assert.equal(ethertypeOf(frame(0x6007)), 0x6007);
  assert.equal(ethertypeOf(frame(0x0806)), 0x0806);
  assert.equal(ethertypeOf(new Uint8Array(4)), -1);   // too short
});

test('length bounds are enforced', () => {
  assert.ok(!frameLengthOk(new Uint8Array(MIN_FRAME - 1)));
  assert.ok(frameLengthOk(new Uint8Array(MIN_FRAME)));
  assert.ok(frameLengthOk(new Uint8Array(MAX_FRAME)));
  assert.ok(!frameLengthOk(new Uint8Array(MAX_FRAME + 1)));
  assert.throws(() => makeDataMessage(new Uint8Array(MIN_FRAME - 1)), RangeError);
  assert.throws(() => makeDataMessage(new Uint8Array(MAX_FRAME + 1)), RangeError);
});

test('parseMessage drops anything not a valid v1 DATA message', () => {
  const good = makeDataMessage(frame(ETHERTYPE_SCA, 10)).msg;
  assert.equal(parseMessage(null), null);
  assert.equal(parseMessage('x'), null);
  assert.equal(parseMessage({ ...good, t: 'nope' }), null);   // wrong tag
  assert.equal(parseMessage({ ...good, v: 2 }), null);        // unknown version
  assert.equal(parseMessage({ ...good, kind: 1 }), null);     // unknown/reserved kind
  assert.equal(parseMessage({ ...good, frame: 'x' }), null);  // non-buffer frame
  assert.equal(parseMessage({ ...good, frame: new Uint8Array(4).buffer }), null); // too short
});

test('makeDataMessage accepts a subarray view and emits an exactly-sized buffer', () => {
  const big = new Uint8Array(200); big.fill(0x22);
  const view = big.subarray(10, 10 + 60);            // offset != 0
  const { msg } = makeDataMessage(view);
  assert.equal(msg.frame.byteLength, 60);
  assert.deepEqual([...asFrameU8(msg.frame)], [...view]);
});
