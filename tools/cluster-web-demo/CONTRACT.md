# FROZEN SEAM CONTRACT — in-browser L2 switch for the cluster web demo

**Status: FROZEN v1 (2026-09-18).** This is the seam between the web-demo lane (the switch) and every NIC backend
(pcjs-vax `pcjsvax-d1bf` DELQA `EthernetLink`; the OVMX/x86 qemu-wasm net backend). Both lanes build against this.
Changes are additive-only under a bumped `v` — the v1 DATA path below does not change.

Anchor: rd vms-735. Design: `docs/design/cluster-web-demo.md`. Resolves `pcjsvax-708` → **in-page postMessage
switch (no server)**.

## The model

Three emulated nodes are iframes on **one** page in **one** browser session. The **parent page is the virtual
Ethernet switch**: a dumb L2 broadcast hub. Each iframe is a *port*. A frame in on one port is flooded to every
*other* port, never looped back. The switch never inspects or mutates the frame payload — it forwards the exact
bytes the real executive's NIC emitted (never-crash-a-peer: Node C is real VMS).

```
 node NIC (guest)                             parent page                         other node NICs
  TX ── worker→iframe ── postMessage(up) ──►  L2Hub.emit ──flood──► postMessage(down) ── iframe→worker ──► RX
```

There is **no server**. Transport is `window.postMessage` between the parent page and each (cross-origin) iframe.

## The DATA message (v1 — normative, frozen)

A single Ethernet frame is carried as one structured-clone object, in **both** directions (node→parent, parent→node):

```js
{
  t:    'ovmx-l2',      // string tag — identifies our messages; ignore any message where t !== 'ovmx-l2'
  v:    1,              // protocol version (number); receivers MUST ignore unknown v
  kind: 0,              // 0 = DATA (an Ethernet frame). 1..255 reserved; receivers MUST ignore unknown kind
  frame: <ArrayBuffer>  // the raw Ethernet frame: dst[6] src[6] ethertype[2] payload[…]. NO preamble, NO FCS.
}
```

- `frame` is the exact on-wire Ethernet frame the guest NIC produced/expects. Byte 12..13 is the ethertype
  (SCA/cluster = `0x6007`; ARP = `0x0806`; DECnet = `0x6003`). The switch does not care which — it forwards all.
- Length bounds: `MIN_FRAME = 14` (header only), `MAX_FRAME = 1600` bytes. Outside → **dropped, not delivered**
  (defensive; never disconnects the port).
- **Transfer semantics:** each DATA message owns a **private, exactly-sized copy** of the frame, so it is always
  safe to `postMessage`-transfer without neutering a buffer the sender still holds, and every broadcast recipient
  gets an independent buffer. The switch never hands one buffer to two ports. (The per-frame copy is negligible at
  SCS rates; correctness/never-crash-a-peer beats a shaved memcpy.)
- Reserved `kind`/`v` values let control frames (join/leave/hello for the UI) be added later **without** touching
  this DATA path. A v1 receiver silently ignores them.

## The port API (what a NIC backend implements) — `hub.mjs`

The switch core is transport-agnostic. A NIC backend connects by giving the hub a `send` (how to deliver an inbound
frame *into* this node's RX) and receives back a handle with `emit` (call it when the node *transmits*):

```js
import { L2Hub } from './l2/hub.mjs';
const hub = new L2Hub();                                   // one per demo page (the switch)

const port = hub.addPort({
  name: 'OVMXA',                                           // node name, for diagnostics/UI
  send: (frameU8) => { /* inject frameU8 into THIS node's NIC receive path */ },
});

port.emit(frameU8);   // call when the node's NIC transmits a frame -> hub floods to all OTHER ports
port.remove();        // node leaves the segment
```

- `frameU8` is a `Uint8Array` view of the Ethernet frame. `send` receives one; `emit` takes one.
- The postMessage binding (`l2/port-postmessage.mjs`) wraps this: on the parent side it maps each iframe to a hub
  port whose `send` posts a DATA message down, and forwards each iframe's inbound DATA message to `port.emit`.
- Because the hub is transport-agnostic, a NIC backend can be developed and tested against an **in-process** hub
  port before any postMessage/iframe wiring exists — the two lanes never block each other on transport.

## Invariants (any implementation MUST hold)

1. **No loopback** — a frame emitted on port P is never delivered back to P.
2. **No mutation / no origination** — the switch forwards frame bytes verbatim; it never edits a field or
   synthesizes a frame. (INV: executive-backed, never-crash-a-peer.)
3. **Drop, don't crash** — a malformed message (wrong `t`/`v`/`kind`, non-ArrayBuffer frame, length out of bounds)
   is dropped; the port stays connected.
4. **Per-visitor isolation** — the switch is JS in the visitor's own tab; no shared backend, no cross-visitor state.
