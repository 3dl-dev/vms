# OVMX cluster web demo — in-page L2 switch

The transport layer for the browser 3-node VMScluster demo (rd **vms-735**). Three emulated nodes run as
iframes in **one** browser session; the parent page is a virtual Ethernet switch that floods raw Ethernet
frames (ethertype `0x6007` SCA + ARP/DECnet) between them via `postMessage`. **No server, no hosting** — the
switch is pure JS in the visitor's own tab. Every visitor gets a private cluster.

- **`CONTRACT.md`** — the FROZEN v1 seam. Both NIC lanes (pcjs-vax DELQA `EthernetLink`; OVMX/x86 qemu-wasm net
  backend) build against this. Read it first.
- **`l2/wire.mjs`** — the v1 DATA message envelope + validation (zero deps).
- **`l2/hub.mjs`** — the transport-agnostic broadcast Hub (the switch core): flood, no-loopback, drop-malformed,
  per-recipient copy. Pure; unit-testable; usable in-process (the pcjs lane can develop against it before any
  iframe wiring exists).
- **`l2/port-postmessage.mjs`** — the postMessage binding: `attachSwitch` (parent side) + `connectNicPipe` (node
  iframe side). Messaging primitives are injected, so it's testable without a browser and reused verbatim with
  real `window.postMessage`.

## Design

`../../docs/design/cluster-web-demo.md` (§4 transport). Resolves `pcjsvax-708` → in-page postMessage switch.

## Test

```
node --test test/*.test.mjs      # or: npm test
```

Covers the wire envelope, the Hub invariants (no-loopback, per-recipient independent copy, drop-malformed,
one-port-throwing-isolation), and an in-memory postMessage integration that faithfully simulates ArrayBuffer
transfer/neutering across a 3-node page.

## Scope of this component

This is lane (a)'s transport seam only. The NIC backends, per-node cluster config injection, the
`build-cluster-demo <TAG>` generator, and the demo page/consoles are separate items under vms-735.
