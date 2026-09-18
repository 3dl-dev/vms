import test from 'node:test';
import assert from 'node:assert/strict';
import { enframe, Deframer, LEN_PREFIX, MAX_FRAME_BYTES } from '../nic/qemu-socket-framing.mjs';

// Build a distinctive frame of a given length (content is position-encoded so a
// mis-reassembly is caught, not just a length mismatch).
function frame(len, seed = 0) {
  const u8 = new Uint8Array(len);
  for (let i = 0; i < len; i++) u8[i] = (i * 31 + seed * 7 + 1) & 0xff;
  return u8;
}

// Concatenate several enframed frames into one contiguous byte stream.
function stream(frames) {
  const parts = frames.map(enframe);
  const total = parts.reduce((n, p) => n + p.byteLength, 0);
  const out = new Uint8Array(total);
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.byteLength; }
  return out;
}

// Feed a byte stream to a Deframer in the given chunk sizes; collect frames.
function feed(bytes, chunkSizes) {
  const d = new Deframer();
  const got = [];
  let o = 0, i = 0;
  while (o < bytes.byteLength) {
    const sz = chunkSizes[i % chunkSizes.length];
    const end = Math.min(o + Math.max(1, sz), bytes.byteLength);
    for (const f of d.push(bytes.subarray(o, end))) got.push(f);
    o = end; i++;
  }
  return { got, pending: d.pending };
}

function assertFramesEqual(got, expected) {
  assert.equal(got.length, expected.length, `frame count ${got.length} != ${expected.length}`);
  for (let i = 0; i < expected.length; i++) {
    assert.deepEqual([...got[i]], [...expected[i]], `frame ${i} bytes differ`);
  }
}

test('enframe prepends a 4-byte big-endian length', () => {
  const f = frame(3, 1);
  const e = enframe(f);
  assert.equal(e.byteLength, LEN_PREFIX + 3);
  assert.deepEqual([...e.subarray(0, 4)], [0, 0, 0, 3]);
  assert.deepEqual([...e.subarray(4)], [...f]);
  const big = enframe(frame(0x010203));   // 66051
  assert.deepEqual([...big.subarray(0, 4)], [0x00, 0x01, 0x02, 0x03]);
});

test('MANDATORY: loopback byte-identity across every chunking (split + coalesced)', () => {
  const frames = [frame(1, 1), frame(64, 2), frame(1500, 3), frame(46, 4), frame(1, 5), frame(1498, 6)];
  const bytes = stream(frames);
  const chunkings = [
    [1],                        // one byte at a time (splits every prefix + payload)
    [bytes.byteLength],         // all at once (fully coalesced)
    [2],                        // splits mid-4-byte-length-prefix
    [3],                        // prefix split unevenly
    [5],                        // small, straddles boundaries
    [7, 3, 11, 1, 29],          // irregular
    [1000],                     // large chunks coalesce multiple frames
  ];
  for (const cs of chunkings) {
    const { got, pending } = feed(bytes, cs);
    assertFramesEqual(got, frames);
    assert.equal(pending, 0, `chunking ${JSON.stringify(cs)} left ${pending} pending bytes`);
  }
});

test('fuzz: random chunk sizes reassemble byte-identical', () => {
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  for (let trial = 0; trial < 200; trial++) {
    const n = 1 + Math.floor(rnd() * 6);
    const frames = [];
    for (let k = 0; k < n; k++) frames.push(frame(Math.floor(rnd() * 1518), trial * 7 + k));
    const bytes = stream(frames);
    const d = new Deframer();
    const got = [];
    let o = 0;
    while (o < bytes.byteLength) {
      const sz = 1 + Math.floor(rnd() * 40);
      const end = Math.min(o + sz, bytes.byteLength);
      for (const f of d.push(bytes.subarray(o, end))) got.push(f);
      o = end;
    }
    assertFramesEqual(got, frames);
    assert.equal(d.pending, 0);
  }
});

test('empty (zero-length) frame round-trips', () => {
  const frames = [frame(0), frame(10, 1), frame(0)];
  const { got, pending } = feed(stream(frames), [1]);
  assertFramesEqual(got, frames);
  assert.equal(pending, 0);
});

test('a partial frame stays buffered until completed', () => {
  const f = frame(100, 9);
  const e = enframe(f);
  const d = new Deframer();
  assert.deepEqual(d.push(e.subarray(0, 50)), []);      // nothing complete yet
  assert.ok(d.pending > 0);
  const got = d.push(e.subarray(50));
  assert.equal(got.length, 1);
  assert.deepEqual([...got[0]], [...f]);
  assert.equal(d.pending, 0);
});

test('a corrupt oversized length desyncs -> onError + buffer reset, no unbounded buffering', () => {
  const errs = [];
  const d = new Deframer((r) => errs.push(r));
  const bad = new Uint8Array([0x7f, 0xff, 0xff, 0xff, 1, 2, 3]);   // declares ~2GB
  const got = d.push(bad);
  assert.equal(got.length, 0);
  assert.equal(errs.length, 1);
  assert.equal(d.pending, 0, 'buffer reset after desync');
  assert.ok(MAX_FRAME_BYTES < 0x7fffffff);
});
