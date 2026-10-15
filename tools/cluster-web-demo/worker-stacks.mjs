// worker-stacks.mjs -- grab the JS call stack of every dedicated worker a
// page has, over CDP (rd vms-e79 mode (b)). Used by visitor-gate.mjs.
import fs from 'fs';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// rd vms-e79 mode (b): Node A's worker JS thread itself stops (its 1 Hz
// heartbeat freezes for the rest of the run) -- a blocked thread, not a slow
// one. When that happens, attach to every dedicated worker the page has over
// CDP (non-flattened, through the page's own session), pause it, and keep the
// JS call stack in the case directory: what the thread was blocked in is the
// root-cause evidence. Every step is bounded; the gate never waits on it.
export async function grabWorkerStacks(ctx, page, dir, why, opts = {}) {
  const { settleMs = 2000, waitMs = 15000, pollMs = 500 } = opts;
  const out = { at: new Date().toISOString(), why, workers: [] };
  let cdp;
  try {
    cdp = await ctx.newCDPSession(page);
    const sessions = [];
    const replies = new Map();
    cdp.on('Target.attachedToTarget', (e) => {
      if (e.targetInfo && e.targetInfo.type === 'worker') sessions.push({ id: e.sessionId, url: e.targetInfo.url });
    });
    cdp.on('Target.receivedMessageFromTarget', (e) => {
      let m; try { m = JSON.parse(e.message); } catch { return; }
      const q = replies.get(e.sessionId) || []; q.push(m); replies.set(e.sessionId, q);
    });
    await cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: false, flatten: false });
    await sleep(settleMs);
    let n = 0;
    const sendTo = (sid, method, params = {}) =>
      cdp.send('Target.sendMessageToTarget', { sessionId: sid, message: JSON.stringify({ id: ++n, method, params }) });
    for (const w of sessions) {
      const rec = { url: w.url, paused: false, frames: [] };
      try {
        await sendTo(w.id, 'Debugger.enable');
        await sendTo(w.id, 'Debugger.pause');
        const t0 = Date.now();
        while (Date.now() - t0 < waitMs) {
          const q = replies.get(w.id) || [];
          const p = q.find((m) => m.method === 'Debugger.paused');
          if (p) {
            rec.paused = true;
            rec.reason = p.params.reason;
            rec.frames = (p.params.callFrames || []).slice(0, 40).map((f) =>
              `${f.functionName || '(anonymous)'} ${String(f.url || '').split('/').pop()}:${f.location.lineNumber + 1}:${f.location.columnNumber + 1}`);
            await sendTo(w.id, 'Debugger.resume');
            break;
          }
          await sleep(pollMs);
        }
      } catch (e) { rec.error = String(e.message).slice(0, 200); }
      out.workers.push(rec);
    }
  } catch (e) { out.error = String(e.message).slice(0, 200); }
  try { if (cdp) await cdp.detach(); } catch (e) {}
  try { fs.writeFileSync(`${dir}/nodeA-worker-stacks.json`, JSON.stringify(out, null, 1)); } catch (e) {}
  return out;
}
