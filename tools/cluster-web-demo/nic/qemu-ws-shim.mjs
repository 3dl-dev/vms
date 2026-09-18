// qemu-ws-shim.mjs — the in-worker fake WebSocket that turns the QEMU socket-netdev
// stream into per-Ethernet-frame callbacks, and vice versa.
//
// The empirical PROXY_TO_PTHREAD probe (vms-b16) showed QEMU's SOCKFS socket op is
// proxied to the qemu-worker scope, so this shim installs `scope.WebSocket` in
// qemu-worker.js BEFORE importScripts('out.js'). When QEMU's `-netdev socket,connect=`
// opens, Emscripten constructs one WebSocket here instead of a real one:
//   guest TX (framed bytes) -> ws.send() -> Deframer -> onNicTx(frame)
//   onNicRx(frame) -> deliverToGuest() -> enframe -> ws.onmessage -> guest RX
// The frame handoff is the FROZEN L2 contract's unit (a raw Ethernet frame); the
// worker wires onNicTx/onNicRx to postMessage {nic-tx}/{nic-rx} to the iframe,
// which runs connectNicPipe (contract v1) to the parent switch.

import { enframe, Deframer } from './qemu-socket-framing.mjs';

/**
 * @param {object}   opts
 * @param {object}   opts.scope     the global to install WebSocket on (self, in qemu-worker)
 * @param {(frameU8:Uint8Array)=>void} opts.onNicTx  called per Ethernet frame the guest transmits
 * @param {(reason:string)=>void} [opts.onError]     called on a stream desync
 * @returns {{ deliverToGuest:(frame)=>boolean, connected:()=>boolean, uninstall:()=>void }}
 */
export function installQemuNicWebSocket({ scope, onNicTx, onError = null }) {
  if (!scope) throw new TypeError('installQemuNicWebSocket needs a scope');
  if (typeof onNicTx !== 'function') throw new TypeError('onNicTx must be a function');
  let active = null;

  class FakeWebSocket {
    constructor(url) {
      this.url = String(url);
      this.binaryType = 'blob';        // Emscripten overwrites to 'arraybuffer'
      this.readyState = FakeWebSocket.CONNECTING;
      this.onopen = this.onmessage = this.onerror = this.onclose = null;
      this._l = { open: [], message: [], error: [], close: [] };
      this._deframer = new Deframer(onError);
      active = this;
      // Open asynchronously: Emscripten waits for the open event before writing.
      queueMicrotask(() => {
        if (this.readyState !== FakeWebSocket.CONNECTING) return;
        this.readyState = FakeWebSocket.OPEN;
        this._emit('open', { type: 'open' });
      });
    }
    addEventListener(t, fn) { (this._l[t] || (this._l[t] = [])).push(fn); }
    removeEventListener(t, fn) { const a = this._l[t]; if (a) { const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); } }
    _emit(t, ev) {
      const h = this['on' + t]; if (typeof h === 'function') h.call(this, ev);
      for (const fn of (this._l[t] || []).slice()) fn.call(this, ev);
    }
    // guest -> us: the QEMU 4-byte-length-framed stream. Deframe to Ethernet frames.
    send(data) {
      const u8 = data instanceof ArrayBuffer ? new Uint8Array(data)
        : (ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
          : (typeof data === 'string' ? new TextEncoder().encode(data) : null));
      if (!u8) return;
      for (const frame of this._deframer.push(u8)) onNicTx(frame);
    }
    close() {
      if (this.readyState === FakeWebSocket.CLOSED) return;
      this.readyState = FakeWebSocket.CLOSED;
      this._emit('close', { type: 'close', wasClean: true });
      if (active === this) active = null;
    }
  }
  FakeWebSocket.CONNECTING = 0; FakeWebSocket.OPEN = 1; FakeWebSocket.CLOSING = 2; FakeWebSocket.CLOSED = 3;

  const prev = scope.WebSocket;
  scope.WebSocket = FakeWebSocket;

  return {
    // us -> guest RX: enframe an Ethernet frame and deliver it as an inbound message.
    deliverToGuest(frame) {
      if (!active || active.readyState !== FakeWebSocket.OPEN) return false;
      active._emit('message', { type: 'message', data: enframe(frame).buffer });
      return true;
    },
    connected: () => !!active && active.readyState === FakeWebSocket.OPEN,
    uninstall: () => { scope.WebSocket = prev; if (active) active.close(); },
  };
}
