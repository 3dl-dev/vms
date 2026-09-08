# Design — DECnet for OVMX (Phase IV, clean-room)

**Status:** design / teed for the 1.0 march (new networking lane, parallel to clustering + parity).
**Companion:** `docs/design-tcpip-services-ovmx.md` (shares the networking seam ruling, §2 there);
`docs/design/faithful-sessions-and-network-subsystems.md` (rd vms-515 — the ratified NETACP /
sessions architecture this doc now records; see esp. its §2 Finding B, §3.3, §6 P0).
**Author of record:** conductor, 2026-08-11; engine ruling + NETACP architecture updated 2026-09-08
per rd vms-515 §6-P0 (doc half, split from vms-558). Grounds: OVMX standing rulings (esp. Rule 8
clean-room) + public DNA Phase IV / VSI DECnet Networking Manual + the VAX/Alpha lab oracles +
a full `origin/main` surface inventory + rd vms-a1c (engine ruling) + rd vms-515
(`docs/design/faithful-sessions-and-network-subsystems.md`, NETACP/RTAn:/object-dispatch
architecture).

---

## 1. What we are building

A faithful **DECnet Phase IV for OVMX** layered product: the classic Digital Network
Architecture stack that makes `SET HOST 0`, task-to-task, and `NODE"user pw"::disk:[dir]file`
feel authentically VMS — the `NCP` management program + node database, Session Control objects
(CTERM/FAL), and the RMS remote-filespec path, **layered over a userspace Phase IV wire engine
hidden behind the executive device face**, dispatched by **NETACP**.

**Engine = userspace Phase IV over `AF_PACKET` (ratified ruling vms-a1c, superseding the
2026-08-11 AF_DECnet-restore plan).** Linux carried a complete, interoperable DECnet Phase IV
stack (`net/decnet`, `AF_DECnet`) from ~2000 until it was **removed from mainline in kernel 6.1
(2022)** — unlike `AF_INET`, which OVMX still rides live, there is no in-kernel DECnet stack left
to restore on a modern kernel, so "engine = what the Linux kernel provides" cannot be honored the
way it is for TCP/IP. rd vms-a1c therefore rules the engine is a **userspace Phase IV NSP +
routing + datalink implementation over `AF_PACKET SOCK_RAW`** (ethertype **0x6003**,
**AA-00-04-00-xx-yy** MACs derived from `area.node`), written clean-room from the public DNA
Phase IV specs, forking the proven `src/vmsscs/` LAVC datalink pattern. This is the same shape as
the cluster's `scsd`: a userspace socket **hidden entirely behind the executive device face**
(§2b), never exposed above the VMS layer. See `docs/design/faithful-sessions-and-network-
subsystems.md` (rd vms-515) §2 Finding B and §3.4 for the full architecture and the isolation
rationale (the wire-parsing engine must not run at NETACP's privilege — §2b/§4 below).

**Phase IV, not Phase V.** DECnet-Plus (Phase V / OSI, `NCL`, DECnet-over-IP) is explicitly
**deferred** — Phase IV is the iconic, self-contained, lab-testable target and is what the VAX
(V7.3) and Alpha (V8.4) lab nodes speak natively, and what NETACP's userspace engine speaks. See §9.

## 2. Seam ruling

Inherits the **networking-is-a-layered-product** ruling from `design-tcpip-services-ovmx.md §2`.
The DECnet-specific differences from TCP/IP:

- **Rule 9 engine — a userspace Phase IV engine hidden behind the executive device face (ruling
  vms-a1c).** Unlike a live kernel facility we simply ride (IP), Linux **dropped `AF_DECnet` in
  6.1** — there is no in-kernel DECnet stack left on a modern kernel to ride, so the "engine =
  what the Linux kernel provides" ruling that grounds TCP/IP cannot be honored the same way here.
  The faithful substitute, ratified in rd vms-a1c and detailed in `docs/design/faithful-sessions-
  and-network-subsystems.md` (rd vms-515) §2 Finding B / §3.3: **NETACP** — a privileged
  `RUN/DETACHED` process (JOB_CONTROL's category, *not* kernel-resident; DECnet has no
  survival-across-death or cluster-timing need that would justify moving it into `vms.ko`, unlike
  the cluster's DLM) — owns a **userspace Phase IV NSP + routing + datalink engine over
  `AF_PACKET SOCK_RAW`** (ethertype **0x6003**, **AA-00-04-00-xx-yy** MACs derived from
  `area.node`), forking the proven `src/vmsscs/` LAVC datalink pattern. The `AF_PACKET` socket is
  **hidden entirely behind the executive device face** (§2b) — exactly as the cluster's `scsd`
  socket is hidden behind the SCS surface — so nothing above the VMS layer ever sees a Linux
  socket. Attacker-controlled wire parsing (NSP/CTERM codecs, adjacency state machine) runs at low
  privilege and hands NETACP's thin, privileged control path only a validated, typed connection
  descriptor (vms-515 §3.4) — the wire engine is never itself privileged.
- **Rule 8 (clean-room) — built field-by-field from public DNA Phase IV specs, no shortcut.**
  Because there is no in-kernel stack to inherit provenance from, the userspace engine is written
  **clean-room from the public DNA Phase IV specifications** — the full field-by-field discipline
  the register (§7) already tracks for the cluster protocol, not a documented-lineage shortcut.
  What the register (§7) carries: (a) the specs cited per field, and (b) **validating wire
  behaviour against the lab VMS nodes** (addressing, timers, adjacency) — a two-specimen minimum
  per observed value. **Never** disassemble/decompile VSI/HPE DECnet images or paste leaked
  source.
- **INV-6 touch-points.** Same three as TCP/IP: the DECnet **device**/circuit (`_NET:`/`NET$`)
  registration in the executive device table, `NET$`/`DECNET$` **system** logical names, and
  cross-process **logical-link / object** visibility must be honest (via `/dev/vms` as transport,
  surfaced as VMS device names — see §2b, never a Linux `/dev/` path, never a per-process fake).
  Inbound network logins add a fourth: **`RTAn:`** must be a real, cross-process-visible executive
  device (`$GETDVI` from a *different* process than the session) before a network `SET HOST`
  session may be considered real — the vms-515 §7.5 anti-LARP tell.
- **INV-0 (trademark).** "DECnet" is a DEC/HPE/VSI mark. Brand the OVMX product carefully
  (e.g. **"DECnet-compatible networking for OVMX"**); badge "OpenVMS-compatible". See §9.

## 2b. The NIC is a VMS device, not a `/dev/` path (operator point, 2026-08-11)

`/dev/` does not exist in the VMS universe. The QEMU virtio NIC is surfaced as a **VMS Ethernet
device `EWA0:`** (or `EZAn:`) registered in the executive device table (`vms.ko` `vms_devtab`,
beside `DKA0:`/`DKA100:`), reached by `$ASSIGN`/`$QIO`/`$GETDVI` and listed by `SHOW DEVICE`. The
DECnet **circuit/line** that `NCP` configures layers over that `EWA0:` device. `/dev/vms` is only
the executive *transport* between userspace and `vms.ko`; it is **never** the VMS-visible device
face. **NETACP** (vms-515 §3.3) owns this device face — the `_NET:`/`EWA0:` `vms_devtab` entry
(alongside `ETH0:`/`PEA0:`) and the **network-object dispatch table** (name/number/dispatch,
cross-process real). NETACP's userspace `AF_PACKET` datalink binds the Linux netdev underneath,
but every VMS-visible name (`EWA0:`, the circuit, `_NET:`) comes from the executive device
namespace, never from the raw socket.

Inbound Session Control object dispatch is by **object number**, not uniform: object 42
(CTERM/SET HOST) mints an `RTAn:` device (the OPA0:-shape terminal pattern, vms-515 §3.2) and
routes it through `$CREPRC` to run **LOGINOUT.EXE** on it — the same authenticator every other
session-creating path uses (vms-515 §2 Finding A, §3.1) — while object 17 (FAL) and object 0
(task) dispatch to their own target images. "Every object runs LOGINOUT" would be an invented
internal; only the interactive-terminal objects do.

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
4. Session Control      NETACP (detached)     object dispatch table; object 42→RTAn:+$CREPRC LOGINOUT
3. VMS device face      NETACP (detached)     _NET:/NET$ + circuit over EWA0:; AF_PACKET↔$QIO bridge
── engine boundary (userspace, hidden behind the device face — ruling vms-a1c) ────────────────────
2. NSP + routing        NETACP datalink        logical links, flow control, Phase IV routing, HELLO
1. Datalink            NETACP datalink (AF_PACKET) ethertype 0x6003, AA-00-04 MAC, binds EWA0: netdev
```

- **L1–L2 engine = NETACP's userspace Phase IV datalink, clean-room from the DNA specs.**
  Datalink (0x6003, AA-00-04 MAC from `area.node`, Phase IV multicasts AB-00-00-03/04-00-00), NSP
  logical links + flow control, and Phase IV routing/HELLO adjacency are built clean-room over
  `AF_PACKET SOCK_RAW`, forking the `src/vmsscs/` LAVC datalink pattern (ruling vms-a1c — Linux
  dropped `AF_DECnet` in 6.1, so there is no in-kernel stack left to ride or forward-port on a
  modern kernel). This engine is **NETACP's low-privilege datalink internal**, never exposed
  above the device face (vms-515 §3.3/§3.4).
- **L3–L4 VMS device face + Session Control = NETACP** (a privileged `RUN/DETACHED` process,
  JOB_CONTROL's category — *not* kernel-resident). NETACP owns the `_NET:`/`NET$` device + the
  NCP-named **circuit** registered in the executive device table over `EWA0:` (§2b), the
  **network-object dispatch table** (name/number/dispatch, cross-process real via the executive
  device/table layer), and NCP's view. Inbound object dispatch is by object number (§2b): object
  42 (CTERM/SET HOST) mints an `RTAn:` device and routes it through `$CREPRC` to a LOGINOUT
  session (vms-515 §3.1–§3.3); object 17 (FAL) and object 0 (task) dispatch to their own target
  images — task-to-task `$QIO` to DECnet objects is bridged to NETACP's `AF_PACKET` connections.
  This is the honest INV-6 seam (via `/dev/vms` transport, VMS device names out); NETACP's
  privileged control path does not itself parse attacker wire bytes (vms-515 §3.4).
- **L5 NCP** — the Network Control Program grammar + the node databases (`NETNODE_REMOTE.DAT`,
  executor characteristics), the `NETCONFIG.COM` equivalent, and NICE for remote NCP.
- **L6 DCL/RMS** — replace the `SET HOST` stub with real CTERM (LOGINOUT on `RTAn:`, not a
  bespoke authenticator); wire the existing syntactic `NODE"user pw"::` parse to a FAL remote-open
  so `COPY`/`DIRECTORY` cross nodes; task-to-task `$QIO` to DECnet objects.

## 4b. Provenance note (supersedes the 2026-08-11 AF_DECnet-restore plan)

The original 2026-08-11 plan proposed restoring Linux's removed `net/decnet` kernel module as an
out-of-tree forward-port, engine-riding it the way TCP/IP rides `AF_INET`, with the userspace
`AF_PACKET` engine kept only as a documented fallback if the forward-port proved infeasible.
**That plan is superseded.** Ruling vms-a1c and the ratified design in `docs/design/faithful-
sessions-and-network-subsystems.md` (rd vms-515) establish the userspace engine as the **primary
and only** engine, hidden behind NETACP's executive device face — not a fallback, and not a
forward-ported kernel module. The L3–L6 VMS surface described in §4 is unchanged from the original
plan's intent; only the engine boundary and its owning process (NETACP, not a kernel module) moved.

## 5. Scope & phasing (→ rd children)

| Phase | Outcome (verifiable end state) | Notes / deps |
|---|---|---|
| **0. Provenance + oracle + tap** | Provenance register (DNA Phase IV spec citations per field + validation plan) opened; a **lab node confirmed running Phase IV** with captured HELLO/NSP specimens; a **tap/bridge NIC** on the OVMX VM sharing an L2 segment with a lab node. | Tap = TCP/IP Phase 0 (virtio NIC); lab uses the lab-2 pod-bridge model. |
| **1. NETACP datalink + adjacency** | NETACP's clean-room userspace Phase IV datalink (`AF_PACKET`, vms-a1c) binds `EWA0:`'s netdev, and a lab VAX/Alpha `SHOW KNOWN/ADJACENT NODES` sees the OVMX node as a reachable endnode. | Clean-room from DNA specs (vms-515 §2 Finding B), not a kernel forward-port. |
| **2. VMS device face + task-to-task** | `_NET:`/`NET$` + the NCP circuit are VMS devices over `EWA0:` (no `/dev/`), owned by NETACP; task-to-task `$QIO`↔`AF_PACKET` bridge round-trips OVMX↔OVMX then OVMX↔lab. | Executive device table (`vms-6b8`), honest INV-6; NETACP architecture per vms-515 §3.3. |
| **3. NCP + node DB** | `NCP SHOW EXECUTOR`/`SHOW KNOWN NODES`/`DEFINE NODE`/`SET EXECUTOR` real against a persisted node DB. | Replaces facades with honest state. |
| **4. Session Control + SET HOST** | Real `SET HOST <node>` (CTERM) both directions with a lab node via object-42 dispatch → `RTAn:` → `$CREPRC` LOGINOUT (vms-515 §3.1–§3.3); the stub is gone. | Iconic proof; no bespoke CTERM authenticator. |
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

Because the engine is a **hand-built userspace implementation** (ruling vms-a1c — there is no
in-kernel `AF_DECnet` left on a modern Linux to inherit provenance from), the full cluster-style
field-by-field discipline applies to L1–L2 from the start — there is no documented-lineage
shortcut to lean on. `docs/decnet-provenance-register.md` therefore records: (a) the exact public
DNA Phase IV spec citation for every implemented field (addressing, ethertype/multicast MACs, NSP
header layout, routing/HELLO timers), (b) NETACP's clean-room `AF_PACKET` datalink surface, and
(c) **wire-behaviour validation against the lab oracle** — a two-specimen minimum for any observed
value (addressing, timers, adjacency), plus the vms-515 ⚑ blocking prerequisite: a real VAX↔VAX
`SET HOST` capture (NETACP/RTTDRIVER/object-table/LOGINOUT sequence + CTERM access-control
credential semantics) pinned **before** the network-terminal-creation path (§4, object 42) is
coded. **No** VSI/HPE binary is ever disassembled. This is what keeps the interop legally
protected (DMCA 1201(f), EU SW Directive Art. 6).

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
