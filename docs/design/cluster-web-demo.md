# Design: Browser 3-node VMScluster demo (vms-735)

Status: **DRAFT — awaiting conductor gate (INV-0).** No cluster-forming code until gated.
Lane: web-demo-of-clustering. Anchor: rd vms-735 (Baron 2026-09-17). Conductor holds the gate + release watch.

## 1. The deliverable

A public browser page showing a **real** three-node VMScluster a visitor logs into and interacts with:

| Node | OS / build | Emulator | Substrate | Runs today? |
|---|---|---|---|---|
| **A** | OVMX/x86_64 (this release's images) | QEMU-wasm (TCG) | Linux `vms.ko` executive | yes, single-node, `-nic none` |
| **B** | OVMX/VAX (this release's image) | PCjs KA655 (JS) | NetBSD-VAX SYSKRNL | yes, single-node, no NIC |
| **C** | **real** OpenVMS VAX (5.5-2H4), pinned | PCjs KA655 (JS) | — (genuine VMS) | yes, single-node, no NIC |

All three wired into **one** cluster over a shared virtual L2. Visitor sees `SHOW CLUSTER` report **CN=3**,
logs into a DCL `$` on any node, interacts. This makes the V0.7 landmark (OVMX joins a real VAX cluster)
public and touchable — OVMX interoperating with **real** VMS, live in a browser.

## 2. LINCHPIN: every node is real-executive-backed (confirmed)

The non-negotiable (real cluster, not an animation) is **met at the executive layer**:

- **Node A** — QEMU-wasm boots real `vmlinuz 6.12.103-ovmx` + initramfs loading `vms.ko` (→ real `/dev/vms`) +
  real distro disk (`ovmx-distrib.img` → `sysdisk.qcow2`). Not a transcript; `openvmx-site` CI (`track-release.yml`
  → `verify.js` resume-gate) proves the live boot to `Username:` every deploy. `openvmx-site/boot/qemu-worker.js:47-49`.
- **Node B** — PCjs KA655 boots a real OVMX/VAX disk (`ovmx-vax-v0.6-14.img.gz`) over NetBSD-VAX. Real CPU emulation.
- **Node C** — PCjs KA655 boots a real OpenVMS 5.5-2H4 ODS-2 volume (the `vax.3dl.network` machine, live today).

**So `SHOW CLUSTER` on any node will read CN=3 from the executive's real membership block** — provided the nodes
actually form a cluster over a real transport. That is the whole build.

## 3. SECOND PREREQUISITE (flagged, not folded): no NICs + two NIC sub-projects

The executive is real, but **no node has an Ethernet NIC today**, and the two emulator families need **two
independent NIC efforts**:

1. **PCjs KA655 DELQA** — epic `pcjsvax-d1bf` (planned, **100% unstarted**). Serves **BOTH Node B (OVMX/VAX)
   and Node C (real VMS)** — they are the *same* PCjs emulator, different disks. One faithful DELQA port
   (from SIMH `pdp11_xq.c`) covers both. Chain: DELQA device → `EthernetLink` seam → TX → RX/interrupts →
   in-proc L2 hub → browser L2 hub. Owned by the **pcjs-vax lane** (repo `~/projects/pcjs-vax` + `~/projects/pcjs`).
2. **QEMU-wasm virtio-net + postMessage net backend** — **net-new, not in d1bf.** Node A is `-nic none`; it needs
   virtio-net enabled in the `3dl-dev/qemu-wasm` fork and a net backend that bridges the guest NIC frames out to the
   parent-page switch via postMessage (the frame crosses worker→iframe→parent). Comparable in size to the DELQA epic.

**Also unverified (de-risk spike, own item):** that the in-browser OVMX images actually run the SCS/cluster stack
to a join. Node A likely does (shipped distro auto-starts SCS — vms-5ad, the `labjoin_booted.sh` gate). OVMX/VAX
(NetBSD substrate) SCS-in-browser is unconfirmed. **Spike this before committing to Node B.** If Node B can't
cluster in-browser, the honest fallback is a **2-real + 1-OVMX** or **1-real + 1-OVMX/x86** cluster — still a real
heterogeneous cluster, still the headline; conductor/Baron decide scope if the spike reds.

## 4. The transport: shared virtual L2 = the parent page as an in-browser switch (postMessage)

Three heterogeneous in-browser VMs share no L2. VMScluster SCA/SCS traffic is **raw Ethernet, ethertype 0x6007**
(the frames the lab proved on `br0`). We need to move those frames between the three emulators — but they are all in
**one browser session**, so **no server is needed**. The three nodes are iframes on one page; **the parent page is
the virtual Ethernet switch**, brokering frames between the iframes with `postMessage`:

```
 Node A NIC (qemu-wasm virtio-net)  ──postMessage──┐
 Node B NIC (PCjs DELQA/EthernetLink)──postMessage─┼─► parent page = broadcast Hub ─► floods to other iframes
 Node C NIC (PCjs DELQA/EthernetLink)──postMessage─┘        (0x6007 SCA + ARP + DECnet + …)
```

- **Hub = a dumb L2 broadcast switch** running in the parent page (pure JS, `hub.mjs`). Each iframe is a "port."
  A frame in on one port is flooded to all *other* ports (never looped back). No L3 logic; no inspection of the
  0x6007 payload (INV: it never templates/originates a cluster wire field — it only forwards bytes the real
  executives emit).
- **Why not just BroadcastChannel:** it is same-origin only, and our nodes are cross-origin (`openvmx.3dl.dev` vs
  `vax.3dl.network`). Parent↔iframe `postMessage` *does* cross origins, and the site **already** embeds
  `vax.3dl.network` as a credentialless iframe today — so that boundary is proven in production. Frame path is a
  few postMessage hops (guest NIC → emulator worker → iframe → parent → iframe → worker); SCS is low-rate so
  latency is a non-issue, and the Ethernet frame rides as an `ArrayBuffer` (a small per-frame copy, then
  transferred to the recipient — negligible at SCS rates).
- **Wire seam (I own; the seam between lane (a) and lane (b)) — transport-agnostic Hub, postMessage binding:**
  each frame is a structured-clone message `{ t:'ovmx-l2', v:1, kind:0, frame:<ArrayBuffer> }` (frame = the raw
  Ethernet frame the guest NIC emitted: 14-byte header + payload, no FCS). Both the PCjs `EthernetLink` backend and
  the qemu-wasm net backend deliver/receive frames through the same `hub` port API. **This resolves `pcjsvax-708`
  (transport mechanism) → in-page postMessage switch** so all three nodes share one wire, with zero infra.
- **Transport-agnostic by construction:** `hub.mjs` (broadcast/no-loopback/drop-malformed) is pure and takes an
  abstract "port" (a `{ send(frame), onFrame(cb) }`). The postMessage binding is the shipped port. A WebSocket
  port remains a **future option** ONLY if a later demo wants nodes across *separate tabs or machines* — not needed
  for the one-page demo, and it carries no server today.
- **Never-crash-a-peer:** Node C is real VMS — the executive-backed invariant applies end-to-end. The Hub must
  never synthesize or mutate a frame; malformed messages from a buggy endpoint are dropped, not forwarded-as-crafted.

## 5. Cluster config + genesis order

Each node gets, injected by the generator (§6) before boot:

- Distinct **SCSNODE** (≤6 chars, e.g. `OVMXA`,`OVMXB`,`VAXC`) + distinct **SCSSYSTEMID**.
- Shared **CLUSTER_AUTHORIZE**: same cluster **group number + password** (the group-N/password the lab used).
- **VOTES=1** each; **EXPECTED_VOTES** tuned for genesis (below).
- LAVC enable: OVMX ≈ its SCS auto-start; real VMS 5.5 = `NISCS_LOAD_PEA0=1`, `VAXCLUSTER=2` (PEDRIVER/PEA0 on 0x6007).
- Same ethertype 0x6007, same padded/ALLOCLASS conventions as the lab.

**Genesis order (deterministic, faithful to "OVMX joins a real VAX cluster"):**
1. **Node C (real VMS)** boots as genesis — `VOTES=1 EXPECTED_VOTES=1` → forms a 1-node cluster (quorum=1). CN=1.
2. **Node A (OVMX/x86)** boots (`EXPECTED_VOTES=3`), joins over the relay → CN=2 (cluster raises EV, quorum=2, held).
3. **Node B (OVMX/VAX)** boots, joins → **CN=3**, quorum=2 held.

The visitor **watches CN grow 1→2→3 live** (more compelling than a pre-baked number).

**Boot-strategy consequence:** the x86 demo's single-node snapshot-resume (`loadvm ovmx`) **cannot** be reused as-is
for clustering — live SCS/VC connection state can't be frozen/restored per-node independently of its peers. The
cluster demo **cold-boots nodes live** (≈90s x86, ≈4min VAX) and orchestrates the join sequence. Optimization
(snapshot each node up to *just before* network attach, then resume+join live) is deferred (own item).

## 6. Generator pipeline (repeatable + per-release)

Baron's requirement: reproducible, parameterized by tag, **fresh cluster per release, no in-place upgrade**.

```
build-cluster-demo <TAG>
  inputs:  release <TAG> OVMX boot artifacts  (vmlinuz, initramfs-ovmx-slim.cpio.gz, ovmx-distrib.img — x86_64)
           release <TAG> OVMX/VAX image
           PINNED pcjs real-VMS reference image  (fixed; NOT versioned with OVMX — real VMS)
           PINNED pcjs emulator + DELQA build
  steps:   1. fetch/verify artifacts for <TAG> (reuse openvmx-site track-release "Obtain boot assets")
           2. inject per-node cluster config (SCSNODE/SCSSYSTEMID/VOTES/EV/CLUSTER_AUTHORIZE) into each OVMX image
              + select the pre-configured pinned VMS cluster volume
           3. emit deployable: cluster demo page (parent = the in-page switch) + 3 console panel iframes, all static
           4. reproducibility gate: headless-drive all 3 nodes → assert real CN=3 read from each executive
  output:  a self-contained, deployable per-<TAG> demo (static files). Re-runnable, deterministic.
```

- **Per-release isolation:** each `<TAG>` emits its own self-contained demo page (its own switch instance, its own
  pinned images) → a V0.7 cluster, a V0.8 cluster, … never share a wire, never upgrade in place. Isolation is by
  *page*, not by a shared server namespace.
- **The pinned VMS node** is built once as a cluster member and pinned; only the two OVMX nodes track the release.
- **Release-flow hook:** a **triggered follow-on AFTER the cut** (repository_dispatch `release-tagged` → run
  `build-cluster-demo <TAG>`), **not inline in the cut** — a demo-build failure must never block/hold a tag (conductor
  holds the release watch; the demo rides behind it, exactly like `openvmx-site`'s existing track-release follow-on).
  On failure it honestly pins to the last working tag (mirror `openvmx-site`'s DEMO_PIN pattern), never fakes green.

## 7. Hosting posture — fully static, no server (escalation retired)

The in-page postMessage switch (§4) means **the entire demo is static** — HTML/JS + the pinned emulator/disk assets,
served exactly like `openvmx-site` today (GitHub Pages, custom domain, `coi-serviceworker.js` for cross-origin
isolation). **No relay server, no stateful infra, no spend, nothing to babysit.**

- The switch is JS running in the visitor's own browser tab (`hub.mjs` in the parent page). Every visitor gets their
  own private cluster; there is no shared backend and no cross-visitor state.
- **This retires the relay-hosting escalation entirely** (previously teed to Baron): there is no externally-visible
  infra and no spend to approve. Large disk assets ride the existing S3+CloudFront path `openvmx-site` already uses.
- WebSockets/Durable-Objects would only re-enter if a *future* demo wanted a cluster spanning separate tabs or
  machines. Out of scope here; the transport-agnostic Hub keeps that door open at zero cost today.

## 8. Ownership split (a) / (b) — for the conductor to /swarm-plan

**(a) Web-demo lane (mine) — the cluster wiring, config, pipeline, page:**
- The in-page L2 **switch** (`hub.mjs` broadcast Hub, parent page) + the postMessage **port contract** (§4) both
  NIC backends deliver frames through.
- **Node A NIC endpoint:** virtio-net in the `3dl-dev/qemu-wasm` fork + a net backend that bridges the guest NIC to
  the parent switch via postMessage. *(NIC-endpoint work on the OVMX/x86 side, distinct from d1bf — I own it or
  sub-lane it; flagging for a call.)*
- **Per-node cluster-config injection** into the OVMX images (+ select the pinned VMS cluster volume).
- The **generator pipeline** `build-cluster-demo <TAG>` (§6) + the **release follow-on hook** (§6).
- The **demo page + 3 console panel iframes + genesis orchestration** + honest-scope copy (INV-0, conductor gates).
- **Integration:** wire Nodes A+B into the cluster; drive/verify the live CN=1→2→3 join.
- **De-risk spike:** does in-browser OVMX (x86 and VAX) actually run SCS to a join over the switch (§3).

**(b) pcjsvax-d1bf NIC/transport epic (pcjs-vax lane — conductor assigns):**
- Faithful **DELQA** device model (PCjs KA655) → serves **both** Node B and Node C.
- **`EthernetLink`** transport seam whose backend delivers/receives frames through **my port contract** (§4) — this
  resolves `pcjsvax-708` → in-page postMessage switch (no server), so we share one wire.
- TX / RX+interrupts / in-proc L2 hub (their internal milestones) → then bridge onto the switch.

**The seam between (a) and (b):** the §4 postMessage port contract + the `hub.mjs` port API. I publish + freeze it;
the pcjs-vax `EthernetLink` backend and my qemu-wasm net backend both deliver frames through it. Both lanes then move
in parallel against a frozen contract. (Because the Hub is transport-agnostic, the pcjs lane can even develop against
an in-process Hub port before the postMessage wiring exists — no cross-lane blocking on transport.)

## 9. Dependency structure (proposed tree under vms-735)

```
vms-735  cluster web demo (epic, web-demo lane)
├─ SPIKE: does in-browser OVMX (x86 + VAX) run SCS to a join?  [gates Node A/B scope]   (a)
├─ L2 switch: transport-agnostic broadcast Hub + postMessage port + frozen §4 contract   (a)  ← seam, do first
├─ qemu-wasm virtio-net + postMessage net backend → parent switch (Node A NIC)           (a)  dep: switch-contract
├─ per-node cluster-config injection (OVMX x86 + VAX) + pinned VMS cluster volume        (a)
├─ generator: build-cluster-demo <TAG> (reproducible, per-release, all static)           (a)  dep: switch, config-inject
├─ release follow-on hook (triggered after cut, honest-pin on fail)                      (a)  dep: generator
├─ demo page + 3 console iframes + genesis orchestration + INV-0 copy                     (a)  dep: switch, NICs
├─ INTEGRATION MILESTONE: live CN=1→2→3, SHOW CLUSTER=CN3 read from executives, public    (a)  dep: all above + (b)
└─ ══ pcjsvax-d1bf (pcjs-vax lane) ══  DELQA → EthernetLink→switch → serves Node B + C    (b)  ← conductor /swarm-plans
        (pcjsvax-708 resolved → in-page postMessage switch; EthernetLink backend targets §4 contract)
```

Cross-lane dep: the INTEGRATION milestone depends on **both** (a)'s relay+NICs and (b)'s DELQA landing.
The relay + wire contract is the **first** thing to build (unblocks both lanes).

## 10. Honest-scope copy (INV-0, conductor gates before ship)

The page states plainly: real executives on all three nodes; real OpenVMS on Node C; live cluster formation over a
WebSocket-bridged virtual Ethernet (the emulators' NICs, real 0x6007 SCA frames); what is emulation vs. real
hardware; and — until Node B's SCS-in-browser spike is green — the true node count shown. No overstatement.
