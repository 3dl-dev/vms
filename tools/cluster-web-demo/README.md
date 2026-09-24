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

## Node identities (single source of truth)

`mk_democonfig.py`'s `DEMO_NODE_A` / `DEMO_NODE_B` / `DEMO_NODE_C` (and `DEMO_ROSTER`, the three in
genesis-join order: C forms, A joins, B joins) are the **only** place the demo's three cluster identities
(SCSNODE/SCSSYSTEMID/VOTES/EXPECTED_VOTES/GROUP) are declared — `OVMXA`/1987, `OVMXB`/1988, `VAXC`/1989, all
group 257 — the group Node C's real OpenVMS volume was configured with (V7.3 per decision rd vms-d24 /
build rd vms-2570, `tools/lab-vax/build_nodeC_vms73_cluster.sh`; supersedes the original V5.5-2H4 choice
`build_nodeC_vms55_cluster.sh` still documents), which puts the whole demo
segment on SCA HELLO multicast `ab:00:04:01:01:02` (`AB-00-04-01-<LE16(group + 0x100)>`, rd vms-147). The
generator, the injectors and the demo page must import/consume this, never hardcode a second copy
(single-ledger).

## `build-cluster-demo <TAG>` — the per-release generator (rd vms-f0f)

Reproducible, parameterized by release tag. Consumes that tag's already-built OVMX x86_64 boot artifacts
(vmlinuz/initramfs/sysdisk — from `build-boot-artifacts.yml` / `distro/Dockerfile.bootable`; this script does
NOT rebuild them) plus an `openvmx-site` checkout (for the `demo/cluster/` page + qemu-wasm runtime, copied
verbatim), injects Node A's real cluster identity via the two existing injectors above, and emits a
self-contained deployable bundle at `<out>/<TAG>/` — a **fresh** cluster per release, never an in-place
upgrade (a stale bundle dir for the same tag is wiped before writing).

```
build-cluster-demo <TAG> --out <dir> \
    --x86-vmlinuz <path> --x86-initramfs <path> --x86-sysdisk <path> \
    --site-dir <openvmx-site checkout> \
    [--vax-image <path>] [--vms-image <path>]
```

`--vax-image` (Node B) and `--vms-image` (Node C, the pinned real-VMS reference) are **optional** —
omitted, the bundle honestly carries Node A only (`manifest.json`'s `nodes.B.staged`/`nodes.C.staged` are
`true`) rather than fabricating a multi-node bundle before those lanes prove their join (rd vms-613, the
pcjs embed). Node C is never config-injected — it is a pinned, pre-configured, operator-maintained volume
(the real cluster password is an operator fact, never in this repo); its entry is a verbatim copy.

`manifest.json` records the tag, the roster SSOT it read from, per-node status, and a SHA-256 of every
output file — the reproducibility record: run the generator twice against identical inputs and the file
hashes must match exactly (`test/build-cluster-demo.test.sh` proves this against a genuine ODS-2 fixture
built the same way `inject-ods2-config.test.sh` does, and caught+fixed a real non-determinism bug: writing
the hash-listing file *inside* the tree being hashed raced `find`'s own walk).

Not yet wired into this script: fetching a tag's artifacts over the network (mirrors
`openvmx-site/.github/workflows/track-release.yml`'s "Obtain boot assets" step — prefer a published
Release, else build from source at the tag) and the multi-node CN=N reproducibility gate, which already
exists as `openvmx-site/demo/cluster/e2e/e2e-boot.js` (parameterized by `NODE_B`/`NODE_C` env vars) and
should be driven against this generator's bundle, not reimplemented.

## Scope of this component

This is lane (a)'s transport seam + per-node config injection + the `build-cluster-demo <TAG>` generator.
The NIC backends and the demo page/consoles live in the sibling `openvmx-site` repo (`demo/cluster/`); this
repo is the source of the wire contract, the node-identity SSOT, and the config-injection tooling the
generator + `openvmx-site`'s own e2e gate both consume. Separate items under vms-735.
