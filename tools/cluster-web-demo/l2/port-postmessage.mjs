// port-postmessage.mjs — the postMessage transport binding for the in-page L2 switch.
// See ../CONTRACT.md. The messaging primitives are injected (post / addMessageListener) so
// this is unit-testable without a browser and reused verbatim with real window.postMessage.

import { makeDataMessage, parseMessage } from './wire.mjs';

/**
 * PARENT side: make each node iframe a port on the switch `hub`.
 *
 * @param {import('./hub.mjs').L2Hub} hub
 * @param {object} deps
 * @param {(handler:(event:any)=>void)=>(()=>void)} deps.addMessageListener
 *        Subscribe to inbound messages; returns an unsubscribe fn. Browser:
 *        (h)=>{const f=e=>h(e);window.addEventListener('message',f);return()=>window.removeEventListener('message',f);}
 * @param {Array<{name:string, post:(msg:any,transfer:Transferable[])=>void, match:(event:any)=>boolean}>} nodes
 *        post(msg, transfer): deliver a message DOWN to this node's iframe
 *          (browser: iframe.contentWindow.postMessage(msg, targetOrigin, transfer))
 *        match(event): is this inbound event from this node? (browser: e.source === iframe.contentWindow)
 * @returns {{ports: Map<string, object>, close: ()=>void}}
 */
export function attachSwitch(hub, { addMessageListener, nodes }) {
  const ports = new Map();
  for (const n of nodes) {
    const handle = hub.addPort({
      name: n.name,
      send: (frameU8) => {
        // makeDataMessage owns a private copy, so this transfer never neuters the hub's buffer
        // and each recipient's message is independent.
        const { msg, transfer } = makeDataMessage(frameU8);
        n.post(msg, transfer);
      },
    });
    ports.set(n.name, { handle, node: n });
  }
  const unsub = addMessageListener((event) => {
    for (const { handle, node } of ports.values()) {
      if (!node.match(event)) continue;
      const parsed = parseMessage(event.data);   // drops anything not a valid v1 DATA message
      if (parsed) handle.emit(parsed.frame);
      return;                                     // an event belongs to exactly one port
    }
  });
  return {
    ports,
    close: () => { unsub(); for (const { handle } of ports.values()) handle.remove(); },
  };
}

/**
 * NODE side (runs inside a node's iframe): pipe the emulator NIC <-> the parent switch.
 *
 * @param {object} deps
 * @param {(msg:any, transfer:Transferable[])=>void} deps.postUp
 *        Send a message UP to the parent (browser: window.parent.postMessage(msg, '*', transfer)).
 * @param {(handler:(event:any)=>void)=>(()=>void)} deps.addMessageListener  subscribe to messages from the parent.
 * @param {(frameU8:Uint8Array)=>void} deps.toNicRx  inject an inbound frame into the emulator NIC receive path.
 * @param {(event:any)=>boolean} [deps.fromParent]  guard: is this event really from the parent? (origin/source check)
 * @returns {{nicTx:(frame:Uint8Array|ArrayBuffer)=>void, close:()=>void}}
 */
export function connectNicPipe({ postUp, addMessageListener, toNicRx, fromParent = () => true }) {
  const unsub = addMessageListener((event) => {
    if (!fromParent(event)) return;
    const parsed = parseMessage(event.data);
    if (parsed) toNicRx(parsed.frame);
  });
  return {
    // Call when the guest NIC transmits a frame. Single recipient (parent) -> zero-copy transfer.
    nicTx: (frame) => {
      const { msg, transfer } = makeDataMessage(frame);
      postUp(msg, transfer);
    },
    close: () => unsub(),
  };
}
