// wire.mjs — the FROZEN v1 seam envelope for the in-browser L2 switch.
// See ../CONTRACT.md. Zero dependencies; runs in Node and the browser.

export const L2_MSG = 'ovmx-l2';   // message tag; ignore any message whose t !== this
export const L2_VERSION = 1;       // protocol version
export const KIND_DATA = 0;        // an Ethernet frame follows in .frame

export const MIN_FRAME = 14;       // dst[6] + src[6] + ethertype[2]
export const MAX_FRAME = 1600;     // generous cap (classic Ethernet frame ~1518)

export const ETHERTYPE_SCA = 0x6007;   // VMScluster SCA/SCS
export const ETHERTYPE_ARP = 0x0806;
export const ETHERTYPE_DECNET = 0x6003;

/** Coerce a frame argument (Uint8Array | ArrayBuffer) to a Uint8Array, or null. */
export function asFrameU8(frame) {
  if (frame instanceof Uint8Array) return frame;
  if (frame instanceof ArrayBuffer) return new Uint8Array(frame);
  if (ArrayBuffer.isView(frame)) return new Uint8Array(frame.buffer, frame.byteOffset, frame.byteLength);
  return null;
}

/** True iff a frame's byte length is within [MIN_FRAME, MAX_FRAME]. */
export function frameLengthOk(u8) {
  return !!u8 && u8.byteLength >= MIN_FRAME && u8.byteLength <= MAX_FRAME;
}

/** Ethertype (bytes 12..13, big-endian) of an Ethernet frame, or -1 if too short. */
export function ethertypeOf(u8) {
  if (!u8 || u8.byteLength < MIN_FRAME) return -1;
  return (u8[12] << 8) | u8[13];
}

/**
 * Build a v1 DATA message + its transfer list.
 * The message ALWAYS owns a private, exactly-sized copy of the frame — so transferring it can never
 * neuter a buffer the caller still owns (a guest NIC's live TX buffer, or a buffer shared across
 * broadcast recipients). At SCS rates the copy is negligible; correctness beats a shaved memcpy.
 */
export function makeDataMessage(frame) {
  const u8 = asFrameU8(frame);
  if (!frameLengthOk(u8)) throw new RangeError(`frame length ${u8 ? u8.byteLength : 'n/a'} out of [${MIN_FRAME},${MAX_FRAME}]`);
  const buf = u8.slice().buffer;   // standalone, exactly-sized; ours to transfer
  const msg = { t: L2_MSG, v: L2_VERSION, kind: KIND_DATA, frame: buf };
  return { msg, transfer: [buf] };
}

/**
 * Parse an inbound structured-clone message.
 * Returns { kind, frame: Uint8Array } for a valid v1 DATA message, or null to drop
 * (not ours / unknown v / unknown kind / bad frame / out-of-bounds length).
 */
export function parseMessage(msg) {
  if (!msg || typeof msg !== 'object') return null;
  if (msg.t !== L2_MSG) return null;
  if (msg.v !== L2_VERSION) return null;          // unknown version -> ignore
  if (msg.kind !== KIND_DATA) return null;        // unknown kind (reserved) -> ignore
  const u8 = asFrameU8(msg.frame);
  if (!frameLengthOk(u8)) return null;            // malformed / out of bounds -> drop
  return { kind: KIND_DATA, frame: u8 };
}
