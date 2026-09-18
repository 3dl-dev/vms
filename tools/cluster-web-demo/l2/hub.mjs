// hub.mjs — the transport-agnostic L2 broadcast switch (the parent-page "virtual Ethernet hub").
// See ../CONTRACT.md. Pure: no DOM, no postMessage, no dependencies. Fully unit-testable.
//
// A "port" connects one node to the switch. The caller supplies `send` (deliver an inbound
// frame INTO the node's RX) and gets back a handle with `emit` (the node's NIC transmitted a
// frame; flood it to every OTHER port). This is the seam both NIC backends target.

import { asFrameU8, frameLengthOk } from './wire.mjs';

export class L2Hub {
  /**
   * @param {object}   [opts]
   * @param {function} [opts.onDrop]  called (reasonString, portName) when a frame is dropped
   * @param {function} [opts.onError] called (error, portName) if a port's send throws
   */
  constructor({ onDrop = null, onError = null } = {}) {
    this._ports = new Set();
    this._onDrop = onDrop;
    this._onError = onError;
    this._seq = 0;
  }

  /** Number of connected ports. */
  get size() { return this._ports.size; }

  /** Names of connected ports (diagnostics/UI). */
  portNames() { return [...this._ports].map((p) => p.name); }

  /**
   * Connect a node to the switch.
   * @param {object}   spec
   * @param {string}   [spec.name]  node name (diagnostics)
   * @param {function} spec.send    (frameU8: Uint8Array) => void — inject into this node's RX
   * @returns {{ name:string, id:number, emit:(frame)=>boolean, remove:()=>void, connected:()=>boolean }}
   */
  addPort({ name = `port${this._seq}`, send } = {}) {
    if (typeof send !== 'function') throw new TypeError('addPort requires a send(frameU8) function');
    const port = { id: this._seq++, name, send, live: true };
    this._ports.add(port);

    const handle = {
      name: port.name,
      id: port.id,
      connected: () => port.live && this._ports.has(port),
      // The node transmitted a frame. Flood to every OTHER live port. Returns true if forwarded.
      emit: (frame) => {
        if (!port.live) return false;
        const u8 = asFrameU8(frame);
        if (!frameLengthOk(u8)) { this._drop('bad-frame', port.name); return false; }
        let delivered = 0;
        for (const p of this._ports) {
          if (p === port) continue;         // INV: no loopback
          if (!p.live) continue;
          try {
            // INV: forward verbatim; each recipient gets its OWN copy so one node cannot corrupt
            // another's frame through a shared buffer (never-crash-a-peer — Node C is real VMS).
            p.send(u8.slice());
            delivered++;
          } catch (err) {
            if (this._onError) this._onError(err, p.name);
          }
        }
        return delivered > 0;
      },
      remove: () => {
        port.live = false;
        this._ports.delete(port);
      },
    };
    return handle;
  }

  _drop(reason, portName) { if (this._onDrop) this._onDrop(reason, portName); }
}
