// rd vms-e79 mode (b): the gate's worker-stack grab, against a fake CDP session.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { grabWorkerStacks } from '../worker-stacks.mjs';

const FAST = { settleMs: 1, waitMs: 200, pollMs: 5 };

// A CDP session that auto-attaches one worker and answers Debugger.pause with a
// paused event carrying two frames (or never answers, if `pauses` is false).
function fakeCtx({ pauses = true, workerType = 'worker' } = {}) {
  const h = {}; const sent = [];
  const cdp = {
    on: (ev, fn) => { h[ev] = fn; },
    detach: async () => {},
    send: async (method, params) => {
      sent.push({ method, params });
      if (method === 'Target.setAutoAttach')
        h['Target.attachedToTarget']({ sessionId: 's1', targetInfo: { type: workerType, url: 'http://x/demo/cluster/node-worker.js' } });
      if (method === 'Target.sendMessageToTarget') {
        const m = JSON.parse(params.message);
        if (m.method === 'Debugger.pause' && pauses)
          setTimeout(() => h['Target.receivedMessageFromTarget']({ sessionId: 's1', message: JSON.stringify({
            method: 'Debugger.paused', params: { reason: 'other', callFrames: [
              { functionName: 'checkMailbox', url: 'http://x/boot/out.js', location: { lineNumber: 5661, columnNumber: 19 } },
              { functionName: '', url: 'http://x/node-worker.js', location: { lineNumber: 0, columnNumber: 0 } }] } }) }), 1);
      }
      return {};
    },
  };
  return { ctx: { newCDPSession: async () => cdp }, sent };
}
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), 'wstk-'));

test('a blocked worker is paused and its stack is kept in the case directory', async () => {
  const { ctx, sent } = fakeCtx(); const dir = tmp();
  const g = await grabWorkerStacks(ctx, {}, dir, 'test', FAST);
  assert.equal(g.workers.length, 1);
  assert.equal(g.workers[0].paused, true);
  assert.deepEqual(g.workers[0].frames, ['checkMailbox out.js:5662:20', '(anonymous) node-worker.js:1:1']);
  assert.ok(sent.some((s) => s.method === 'Target.sendMessageToTarget' && JSON.parse(s.params.message).method === 'Debugger.resume'),
    'the worker is resumed after the grab');
  const kept = JSON.parse(fs.readFileSync(path.join(dir, 'nodeA-worker-stacks.json'), 'utf8'));
  assert.equal(kept.workers[0].frames[0], 'checkMailbox out.js:5662:20');
});

test('a worker that never pauses is recorded as not paused, and the grab is bounded', async () => {
  const { ctx } = fakeCtx({ pauses: false }); const t0 = Date.now();
  const g = await grabWorkerStacks(ctx, {}, tmp(), 'test', FAST);
  assert.equal(g.workers[0].paused, false);
  assert.ok(Date.now() - t0 < 2000);
});

test('a CDP failure is reported, never thrown', async () => {
  const ctx = { newCDPSession: async () => { throw new Error('no cdp'); } };
  const g = await grabWorkerStacks(ctx, {}, tmp(), 'test', FAST);
  assert.match(g.error, /no cdp/);
  assert.equal(g.workers.length, 0);
});

test('only workers are grabbed (an iframe target is not)', async () => {
  const { ctx } = fakeCtx({ workerType: 'iframe' });
  const g = await grabWorkerStacks(ctx, {}, tmp(), 'test', FAST);
  assert.equal(g.workers.length, 0);
});
