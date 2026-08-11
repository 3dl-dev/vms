# Design — DECnet for OVMX (Phase IV, clean-room)

**Status:** design / teed for the 1.0 march (new networking lane, parallel to clustering + parity).
**Companion:** `docs/design-tcpip-services-ovmx.md` (shares the networking seam ruling, §2 there).
**Author of record:** conductor, 2026-08-11. Grounds: OVMX standing rulings (esp. Rule 8
clean-room) + public DNA Phase IV / VSI DECnet Networking Manual + the VAX/Alpha lab oracles +
a full `origin/main` surface inventory.

---

## 1. What we are building

A faithful **DECnet Phase IV for OVMX** layered product: the classic Digital Network
Architecture stack that makes `SET HOST 0`, task-to-task, and `NODE"user pw"::disk:[dir]file`
feel authentically VMS — the `NCP` management program + node database, Session Control objects
(CTERM/FAL), and the RMS remote-filespec path, **layered over Linux's AF_DECnet Phase IV kernel
stack as the engine**.

**Engine = restored Linux AF_DECnet (operator ruling, 2026-08-11).** Linux carried a complete,
interoperable DECnet Phase IV stack (`net/decnet`, `AF_DECnet`) from ~2000 until it was removed
from mainline in **kernel 6.1 (2022)**. It was an **independent GPL clean-room** implementation
written from the public DEC DNA specifications — *not* DEC/VSI/HPE source — so restoring it as an
out-of-tree module is legal under Rule 8 and squarely within OVMX's free-software stance. This
makes DECnet **symmetric with TCP/IP**: the kernel provides the transport, OVMX provides the
VMS-faithful userspace. It collapses the from-scratch NSP/routing build to a **forward-port**.
The full userspace NSP-over-`AF_PACKET` engine (§4b) is kept only as a **documented fallback** if
the forward-port to a modern kernel proves infeasible — a go/no-go de-risked in Phase 1, never a
silent greenfield.

**Phase IV, not Phase V.** DECnet-Plus (Phase V / OSI, `NCL`, DECnet-over-IP) is explicitly
**deferred** — Phase IV is the iconic, self-contained, lab-testable target and is what the VAX
(V7.3) and Alpha (V8.4) lab nodes speak natively, and what the restored module speaks. See §9.

## 2. Seam ruling

Inherits the **networking-is-a-layered-product** ruling from `design-tcpip-services-ovmx.md §2`.
The DECnet-specific differences from TCP/IP:

- **Rule 9 engine — restore the kernel stack Linux used to ship.** Unlike a live kernel facility
  we simply ride (IP), the AF_DECnet stack was *removed* — so the "engine = what the Linux kernel
  provides" ruling is honored by **restoring `net/decnet` as an out-of-tree module forward-ported
  to the OVMX kernel** and riding `AF_DECnet` sockets. This is a real kernel module in the real
  QEMU kernel — *more* Rule-9-aligned than a userspace datalink daemon. The Phase IV wire it
  speaks is ethertype **0x6003** with **AA-00-04-00-xx-yy** MACs derived from `area.node`; the
  module already implements it. (Fallback engine, §4b: userspace NSP over `AF_PACKET SOCK_RAW`,
  forking the proven `src/vmsscs/` LAVC datalink — used only if the forward-port fails.)
- **Rule 8 (clean-room) — satisfied by lineage, still validated by the oracle.** The restored
  module is itself an independent GPL clean-room implementation from **public DNA Phase IV
  specs** — so we inherit clean-room provenance rather than re-deriving every field by hand. What
  remains is (a) documenting that lineage + the forward-port surface, and (b) **validating wire
  behaviour against the lab VMS nodes** (addressing, timers, adjacency) — the register (§7)
  carries both. **Never** disassemble/decompile VSI/HPE DECnet images or paste leaked source.
- **INV-6 touch-points.** Same three as TCP/IP: the DECnet **device**/circuit (`_NET:`/`NET$`)
  registration in the executive device table, `NET$`/`DECNET$` **system** logical names, and
  cross-process **logical-link / object** visibility must be honest (via `/dev/vms` as transport,
  surfaced as VMS device names — see §2b, never a Linux `/dev/` path, never a per-process fake).
- **INV-0 (trademark).** "DECnet" is a DEC/HPE/VSI mark. Brand the OVMX product carefully
  (e.g. **"DECnet-compatible networking for OVMX"**); badge "OpenVMS-compatible". See §9.

## 2b. The NIC is a VMS device, not a `/dev/` path (operator point, 2026-08-11)

`/dev/` does not exist in the VMS universe. The QEMU virtio NIC is surfaced as a **VMS Ethernet
device `EWA0:`** (or `EZAn:`) registered in the executive device table (`vms.ko` `vms_devtab`,
beside `DKA0:`/`DKA100:`), reached by `$ASSIGN`/`$QIO`/`$GETDVI` and listed by `SHOW DEVICE`. The
DECnet **circuit/line** that `NCP` configures layers over that `EWA0:` device. `/dev/vms` is only
the executive *transport* between userspace and `vms.ko`; it is **never** the VMS-visible device
face. The restored AF_DECnet module binds the Linux netdev underneath, but every VMS-visible
name (`EWA0:`, the circuit, `_NET:`) comes from the executive device namespace.

**Reject the roadmap's DECnet-over-IP (UDP 4711) shortcut** (`docs/roadmap-source-compat.md:
246-303`): tunnelling Phase IV over UDP is neither wire-faithful nor testable against the lab
oracle, and DECnet-over-IP is a Phase-V feature. Real Phase IV over Ethernet is the only path
that both *is* authentic and *proves* it against a real VAX/Alpha on the segment.

## 3. Current state (origin/main inventory, 2026-08-11)

**Total greenfield.** No implementation anywhere in `src/`.
- `SET HOST` is a stub: prints "%SET-I-NOTAVAIL, DECnet is not available on this system"
  (`src/vmsdcl/dcl_cmd_set.c:1542`).
- `NODE::` filespec parsing exists but is **syntactic only** — `rms_parse.c:173` sets
  `NAM$M_NODE`; `vmsfs_translate.c:81-131` + `filespec.h:17` parse `NODE"acc"::dev:[dir]file`
  and can reconstruct it — **nothing downstream acts on the node** (no remote open). This is the
  hook Phase 5 wires to a real FAL open.
- Roadmap prose only (`roadmap-source-compat.md` Phase 13; `design-authenticity-roadmap.md` C9).
- The **cluster stack** borrows DECnet-style logical MACs (`AA-00-04-00-<node>`,
  `cluster-protocol-spec.md`) — provenance-relevant, but not a DECnet impl.
- **Reuse:** `src/vmsscs/` is the working raw-Ethernet + executive-device template to fork from.

## 4. Architecture (layers, bottom-up)

```
6. DCL/RMS integration  src/vmsdecnet/dcl     SET HOST (CTERM), NODE"u p"::file → FAL, $QIO objects
5. Management (NCP)     src/vmsdecnet/ncp     SET/SHOW/DEFINE EXECUTOR|CIRCUIT|LINE|NODE, node DB, NICE
4. Session Control      src/vmsdecnet/session named objects, access control, task-to-task, CTERM, FAL
3. VMS device face      src/vmsdecnet/dev     _NET:/NET$ + circuit over EWA0:; AF_DECnet↔$QIO bridge
── engine boundary ─────────────────────────────────────────────────────────────────────────────
2. NSP + routing        (restored net/decnet)  logical links, flow control, Phase IV routing, HELLO
1. Datalink            (restored net/decnet)  ethertype 0x6003, AA-00-04 MAC, binds the EWA0: netdev
```

- **L1–L2 engine = the restored AF_DECnet module.** Datalink (0x6003, AA-00-04 MAC from
  `area.node`, Phase IV multicasts AB-00-00-03/04-00-00), NSP logical links + flow control, and
  Phase IV routing/HELLO adjacency all come from `net/decnet` forward-ported out-of-tree. OVMX
  does not reimplement them — it forward-ports and validates against the lab oracle.
- **L3 VMS device face** — the `_NET:`/`NET$` device + the NCP-named **circuit** are registered
  in the executive device table over `EWA0:` (§2b); task-to-task `$QIO` to DECnet objects is
  bridged to `AF_DECnet` sockets. This is the honest INV-6 seam (via `/dev/vms` transport, VMS
  device names out).
- **L4 Session Control** — named objects (object 0 = task, 17 = FAL, CTERM object), access
  control strings (`"user password"`), inbound object dispatch.
- **L5 NCP** — the Network Control Program grammar + the node databases (`NETNODE_REMOTE.DAT`,
  executor characteristics), the `NETCONFIG.COM` equivalent, and NICE for remote NCP.
- **L6 DCL/RMS** — replace the `SET HOST` stub with real CTERM; wire the existing syntactic
  `NODE"user pw"::` parse to a FAL remote-open so `COPY`/`DIRECTORY` cross nodes; task-to-task
  `$QIO` to DECnet objects.

## 4b. Fallback engine (only if the forward-port fails)

If `net/decnet` cannot be forward-ported to the OVMX kernel at acceptable cost, the fallback is a
**userspace NSP + Phase IV routing engine over `AF_PACKET SOCK_RAW`** (ethertype 0x6003, AA-00-04
MAC), forking the proven `src/vmsscs/` LAVC datalink and building L1–L2 as `src/vmsdecnet/{datalink,
routing,nsp}/**` clean-room from the DNA specs. The L3–L6 VMS surface is **identical either way** —
only the engine boundary moves. The Phase 1 go/no-go decides; a pivot is filed as a tracked
decision, never a silent greenfield.

## 5. Scope & phasing (→ rd children)

| Phase | Outcome (verifiable end state) | Notes / deps |
|---|---|---|
| **0. Provenance + oracle + tap** | Provenance register (AF_DECnet lineage + forward-port surface + validation plan) opened; a **lab node confirmed running Phase IV** with captured HELLO/NSP specimens; a **tap/bridge NIC** on the OVMX VM sharing an L2 segment with a lab node. | Tap = TCP/IP Phase 0 (virtio NIC); lab uses the lab-2 pod-bridge model. |
| **1. Restore engine + adjacency (GO/NO-GO)** | Forward-ported `net/decnet` module loads on the OVMX kernel, binds `EWA0:`'s netdev, and a lab VAX/Alpha `SHOW KNOWN/ADJACENT NODES` sees the OVMX node as a reachable endnode. **If the port fails → pivot to §4b fallback** (tracked decision). | Restore, don't greenfield. |
| **2. VMS device face + task-to-task** | `_NET:`/`NET$` + the NCP circuit are VMS devices over `EWA0:` (no `/dev/`); task-to-task `$QIO`↔`AF_DECnet` bridge round-trips OVMX↔OVMX then OVMX↔lab. | Executive device table (`vms-6b8`), honest INV-6. |
| **3. NCP + node DB** | `NCP SHOW EXECUTOR`/`SHOW KNOWN NODES`/`DEFINE NODE`/`SET EXECUTOR` real against a persisted node DB. | Replaces facades with honest state. |
| **4. Session Control + SET HOST** | Real `SET HOST <node>` (CTERM) both directions with a lab node; the stub is gone. | Iconic proof. |
| **5. RMS remote file (FAL)** | `COPY NODE"user pw"::dev:[dir]file *` round-trips to/from a lab node; `DIRECTORY node::` works. | Wires the existing `NODE::` parse to FAL. |
| **6. e2e oracle gate** | CI/lab gate: OVMX node joins DECnet with a lab VAX/Alpha; SET HOST + `NODE::` COPY both directions pass. | Release proof; runs in a lab-2-style pod. |

## 6. Infrastructure dependencies (hard)

- **Tap/bridge NIC required** — raw Phase IV Ethernet (L2 multicast, non-IP ethertype) cannot
  traverse QEMU user-mode/SLIRP. Needs the tap mode delivered by TCP/IP Phase 0, *and* the
  OVMX tap must share an L2 segment with the oracle. Reuse the **lab-2 model** (one pod = one
  isolated cluster with `br0` + taps): an OVMX-VM + lab-node pod on a shared bridge.
- **Executive device-namespace bridge** (`vms-a7e`) — the `_NET:` device + task-to-task `$QIO`
  registration must be honest; where the bridge is unbuilt, fail with `SS$_NOSUCHDEV`.
- **Lab DECnet oracle must be live** — confirm the VAX/Alpha lab images have DECnet Phase IV
  configured/licensed (bundled on those versions); Phase 0 verifies and captures specimens. The
  lab is the *only* oracle that proves wire-faithfulness (Rule 8).

## 7. Clean-room provenance (Rule 8) — non-negotiable

Because the engine is the restored GPL `net/decnet` — itself a clean-room implementation from
public DNA specs — the heavy field-by-field derivation is **already done and inherited**, not
repeated. `docs/decnet-provenance-register.md` therefore records: (a) the module's provenance and
GPL lineage (public specs, not DEC/VSI/HPE source), (b) the forward-port surface (which kernel
APIs drifted 6.1→6.8+ and how they were bridged), and (c) **wire-behaviour validation against
the lab oracle** — a two-specimen minimum for any observed value (addressing, timers, adjacency).
**No** VSI/HPE binary is ever disassembled. If the §4b fallback is taken, the register reverts to
the full cluster-style field-by-field discipline for the hand-built L1–L2. This is what keeps the
interop legally protected (DMCA 1201(f), EU SW Directive Art. 6).

## 8. Packaging

Layered-product kit installed via the Alpha/PCSI model (`vms-718`): the NSP/routing daemon,
`NCP` image, CTERM/FAL objects, `NETCONFIG.COM`, the `NET$`/`DECNET$` logical names + node DB
templates, launched by a `NET$STARTUP.COM`-equivalent from the STARTUP phases (`vms-46c`),
gated to not announce running without a configured circuit (no LARP).

## 9. Open operator calls

1. **Phase IV only for 1.0?** Recommend **yes** — Phase IV is self-contained, iconic, and the
   only flavour the lab oracle proves. DECnet-Plus (Phase V/OSI) deferred post-1.0. (Conductor
   default unless overridden.)
2. **OVMX node addressing in the lab namespace** — assign OVMX nodes real `area.node` addresses
   on the lab segment (which area? avoid colliding with lab nodes). Lab-authority call.
3. **Kit branding** under INV-0 — "DECnet-compatible networking for OVMX" proposed; confirm the
   exact mark (we do not ship a product literally named "DECnet").
