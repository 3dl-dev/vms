// qemu-socket-framing.mjs — QEMU `socket` netdev stream framing (de/enframe).
//
// QEMU's socket netdev in stream/connect (TCP) mode prefixes each Ethernet frame
// on the wire with a 4-byte BIG-ENDIAN length (net/socket.c net_socket_send: it
// writes htonl(size) then the frame). The transport under it (here the Emscripten
// SOCKFS WebSocket) is a BYTE STREAM, not message-aligned to frames — so inbound
// bytes arrive split and/or coalesced and must be REASSEMBLED. This module is the
// reassembling parser (guest TX → Ethernet frames) and the symmetric writer
// (Ethernet frames → framed bytes for guest RX). Pure, zero-dependency, testable.

export const LEN_PREFIX = 4;
// Upper bound on a single framed packet; a declared length beyond this means the
// stream has desynced/corrupted — we reset rather than buffer unbounded.
export const MAX_FRAME_BYTES = 65535;

/** Prepend the 4-byte big-endian length: Ethernet frame -> framed bytes. */
export function enframe(frame) {
  const u8 = frame instanceof Uint8Array ? frame
    : (frame instanceof ArrayBuffer ? new Uint8Array(frame) : null);
  if (!u8) throw new TypeError('enframe expects a Uint8Array/ArrayBuffer');
  const n = u8.byteLength;
  const out = new Uint8Array(LEN_PREFIX + n);
  out[0] = (n >>> 24) & 0xff;
  out[1] = (n >>> 16) & 0xff;
  out[2] = (n >>> 8) & 0xff;
  out[3] = n & 0xff;
  out.set(u8, LEN_PREFIX);
  return out;
}

/**
 * A reassembling deframer for the QEMU socket-netdev stream.
 * Feed arbitrary byte chunks with push(); it returns the complete Ethernet frames
 * recovered so far (each a fresh Uint8Array). Partial data is buffered across calls.
 */
export class Deframer {
  /** @param {(reason:string)=>void} [onError] called on a desync (then the buffer resets) */
  constructor(onError = null) {
    this._buf = new Uint8Array(0);
    this._onError = onError;
  }

  /** @param {Uint8Array|ArrayBuffer} chunk  @returns {Uint8Array[]} complete frames */
  push(chunk) {
    const c = chunk instanceof Uint8Array ? chunk
      : (chunk instanceof ArrayBuffer ? new Uint8Array(chunk) : null);
    if (!c) throw new TypeError('push expects a Uint8Array/ArrayBuffer');
    // Append to the running buffer.
    if (this._buf.byteLength === 0) {
      this._buf = c.slice();
    } else {
      const merged = new Uint8Array(this._buf.byteLength + c.byteLength);
      merged.set(this._buf, 0);
      merged.set(c, this._buf.byteLength);
      this._buf = merged;
    }
    const frames = [];
    let off = 0;
    const b = this._buf;
    while (b.byteLength - off >= LEN_PREFIX) {
      const len = ((b[off] << 24) | (b[off + 1] << 16) | (b[off + 2] << 8) | b[off + 3]) >>> 0;
      if (len > MAX_FRAME_BYTES) {
        // Desync/corruption: don't buffer unbounded. Report + reset the stream.
        if (this._onError) this._onError(`framed length ${len} exceeds max ${MAX_FRAME_BYTES}`);
        this._buf = new Uint8Array(0);
        return frames;
      }
      if (b.byteLength - off - LEN_PREFIX < len) break;   // frame not fully arrived yet
      frames.push(b.slice(off + LEN_PREFIX, off + LEN_PREFIX + len));
      off += LEN_PREFIX + len;
    }
    // Retain the unconsumed tail.
    this._buf = off === 0 ? b : b.slice(off);
    return frames;
  }

  /** Bytes currently buffered awaiting more input (diagnostics/tests). */
  get pending() { return this._buf.byteLength; }
}
