# DECnet Provenance Register — AF_DECnet GPL lineage + oracle-validation plan

**Status:** register / 1.0 DECnet-lane groundwork (bead `vms-b8a6`). Opened as Phase 0 of the
DECnet build (`docs/design-decnet-ovmx.md §5`, referenced by name from that doc's §7).
**Companion design:** `docs/design-decnet-ovmx.md` (the architecture; this file is its provenance
and validation ledger).
**Author of record:** Technical Writer / provenance, 2026-08-12.
**Governing invariants:** CLAUDE.md Project-Specific **Rule 8** (Clean-room VMS RE — HARD),
**INV-6** (no per-process fake of an executive facility), **INV-0** (trademark ceiling),
licensing stance = **grant-all** (`[[licensing-stance-grant-all]]`).

---

## 0. Why this register exists

OVMX's DECnet Phase IV plan (per `design-decnet-ovmx.md`) does something none of the other OVMX
interop work does: its **engine is not clean-room-derived by OVMX at all** — it is the **restored
Linux `net/decnet` (`AF_DECnet`) kernel stack**, a pre-existing GPL body of code. That splits the
provenance question into **two independent streams** that must never be conflated:

| Stream | What it covers | Provenance regime | Where the bytes come from |
|---|---|---|---|
| **(i) VMS-visible DECnet behavior** | The wire protocol OVMX must speak (addressing, timers, adjacency), and the VMS-authentic userspace surface (NCP grammar, node DB, CTERM/FAL, `SET HOST`, `NODE::` filespecs) | **Clean-room, Rule 8** | Public DNA Phase IV specs + **lab oracle observation** only. Never VSI/HPE source or binaries. |
| **(ii) AF_DECnet Linux transport engine** | The datalink + NSP + Phase IV routing that actually moves frames (L1–L2 of `design-decnet-ovmx.md §4`) | **GPL lineage** (Linux-kernel copyleft), *not* a VMS-source question | Upstream Linux `net/decnet` GPL source, forward-ported out-of-tree. Never VSI/HPE source. |

The two streams share one hard prohibition — **no VSI/HPE/DEC source or binary is ever
disassembled, decompiled, or copied** — but they are otherwise governed differently. This
register documents both, and the boundary between them (§3), so the use of AF_DECnet is legally
clean *and* the wire behavior is provably clean-room.

**If the AF_DECnet forward-port fails** and OVMX falls back to the hand-built userspace NSP engine
(`design-decnet-ovmx.md §4b`), stream (ii) disappears and the entire L1–L2 datalink reverts to
stream (i) discipline — full field-by-field clean-room derivation from the DNA specs, cited like
the cluster stack (`docs/clean-room/PROVENANCE.md`). That pivot is a tracked decision, never
silent. See §5.

---

## 1. AF_DECnet GPL lineage (stream ii)

### 1.1 What AF_DECnet was

`AF_DECnet` (address family, socket layer `net/decnet/` in the Linux tree) was a **complete,
independently-written implementation of the DECnet Phase IV protocol stack** for the Linux
kernel — datalink (ethertype `0x6003`, `AA-00-04-00-xx-yy` MACs derived from `area.node`, Phase IV
multicasts), NSP (logical links + flow control), and Phase IV routing/HELLO adjacency. It let a
Linux box act as a Phase IV endnode/router and interoperate with real DEC/DECnet nodes.

Key provenance property: it was **written from the public DEC Digital Network Architecture (DNA)
Phase IV specifications** by Linux kernel contributors — it is **not** DEC/VSI/HPE source, and not
a port of DEC's own DECnet code. That is precisely why it could ship in a GPL kernel. For OVMX
this means restoring it inherits an **already-clean clean-room implementation** of stream (i)'s
wire protocol, rather than OVMX re-deriving every field by hand.

- `verify:` original author(s) of the Linux DECnet stack (attribution commonly credited to
  Patrick Caulfield / Steven Whitehouse and later maintainers — confirm from the actual
  `net/decnet/` file headers and `MAINTAINERS` before asserting names in shipped docs).
- `verify:` the DNA Phase IV public specification editions the upstream code cites (DECnet Phase IV
  General Description / Routing / NSP functional specs) — pin exact document titles/numbers from
  the upstream source comments, do not paraphrase from memory.

### 1.2 When and why it was removed from mainline

The DECnet stack was long-deprecated (marked `BROKEN`/orphaned as usage collapsed) and was
**removed from the mainline Linux kernel in the 6.1 series (late 2022)** per
`design-decnet-ovmx.md §1`. Removal does not revoke the license: the code remains GPL and remains
retrievable from Linux git history and from every kernel tree ≤ the last release that carried it.

- `verify:` **exact removal commit + first kernel version without `net/decnet`.** The design doc
  states "kernel 6.1 (2022)". Confirm the precise commit hash, author, and the exact tag (e.g. was
  the final carrying release 6.0.x?) from `git log` on `torvalds/linux` before citing a specific
  version as fact in any customer-facing doc. Record the confirmed values in §6.
- `verify:` the last mainline commit SHA of `net/decnet/` (the exact upstream base OVMX
  forward-ports **from**) — this is the anchor of the whole lineage chain and must be exact.

### 1.3 License and how a GPL kernel module coexists with OVMX

The Linux `net/decnet` code is **GPL-2.0** (the kernel's license). Restoring it means shipping an
**out-of-tree GPL-2.0 kernel module** forward-ported to the OVMX kernel.

- `verify:` the exact SPDX identifier / license header in the upstream `net/decnet/` files
  (`GPL-2.0-only` vs `GPL-2.0-or-later`, and any dual-license notices). Quote the header verbatim
  in §6; do not assume.

**Coexistence with OVMX's stance — clean.** OVMX's licensing posture is **grant-all / give-it-away
free software** (`[[licensing-stance-grant-all]]`: OVMX has no reason to gate anything; the LMF
exists only as a grant-all *compatibility* surface). A GPL-2.0 kernel module is fully compatible
with that posture:

1. **The engine is a kernel module, not linked into OVMX userspace.** `net/decnet` runs in kernel
   space and is reached from userspace through the **`AF_DECnet` socket ABI** (a stable syscall
   boundary) and, in the VMS view, through `/dev/vms` as transport surfaced as VMS device names
   (`design-decnet-ovmx.md §2b`). The GPL kernel↔userspace syscall boundary is the ordinary Linux
   userspace exception — OVMX's userspace DECnet product (NCP, session control, FAL) is **not** a
   derivative work of the GPL module by virtue of talking to it over sockets.
2. **OVMX ships the module as GPL, keeping its license intact.** Forward-porting GPL-2.0 code
   produces a GPL-2.0 derivative; OVMX distributes it under GPL-2.0 with upstream copyright notices
   preserved. This is unremarkable — it is exactly how any out-of-tree Linux driver is shipped.
3. **No conflict with grant-all.** OVMX's give-everything-away stance is strictly *more* permissive
   in intent than copyleft requires; there is no proprietary-gating pressure that GPL would
   obstruct. (The one operational note: the OVMX kernel image + this module are a combined GPL
   work — already true of any Linux kernel OVMX ships.)

- `verify:` whether the OVMX SYSKRNL/Linux build already ships other out-of-tree GPL modules
  (`vms.ko`, `vmsfs.ko`) under the same combined-work terms, so the DECnet module is documented as
  the same class, not a new licensing situation. (`vms.ko` is GPL per the kernel-module norm —
  confirm its `MODULE_LICENSE` string and mirror that convention.)

### 1.4 The provenance chain that keeps stream (ii) clean

```
Public DNA Phase IV specs  ──►  Linux net/decnet (GPL, clean-room by upstream)  ──►
   upstream last-carrying commit SHA (retained)  ──►  OVMX out-of-tree forward-port
   (kernel-API drift bridged, §4)  ──►  validated against lab oracle (§4)
```

Every link is a **public, retained artifact** — no DEC/VSI/HPE input anywhere on the chain. The
retained anchors that make this auditable:

- The upstream `net/decnet/` source at the pinned commit (archive + SHA-256, alongside the
  cluster-stack model in `docs/clean-room/`).
- The forward-port diff (OVMX's changes vs that pinned upstream base) — every OVMX edit is a
  kernel-API adaptation, traceable and reviewable, never a DEC-sourced insertion.
- The `verify:` items in §6, resolved with exact commits/versions/headers before 1.0.

---

## 2. Clean-room boundary — what may and may not be copied from each stream

This is the load-bearing separation. State it explicitly so no one launders VSI material through
either stream.

### 2.1 Stream (i) — VMS-visible behavior (Rule 8, clean-room)

**May be used:**
- Public **DNA Phase IV specifications** (General Description, Routing, NSP, Data Link) — the
  published protocol architecture.
- Public **OpenVMS documentation** — the VSI/HP/DEC *DECnet for OpenVMS Networking Manual*,
  *Guide to DECnet Networking*, NCP command reference, `$...`/`SYS$NET` documented interfaces.
- **Lab oracle observation** — passively captured frames and documented tool output (`NCP SHOW`,
  `SHOW KNOWN/ADJACENT NODES`, `SET HOST` behavior) from a legitimately-run VAX (V7.3) / Alpha
  (V8.4) lab node (`tests/lab/`, `tests/lab-alpha/`). This is behavioral observation of a running
  system's public network interface — the frames on the wire **are** the interface being
  interoperated with.

**May NOT be used:**
- **No disassembly/decompilation** of any VSI/HPE/DEC image (no `NETACP`, `NETDRIVER`, `SYS.EXE`,
  no `.EXE`/`.STB`). No leaked/licensed/"reference" VMS source.
- **No verbatim copying** of any VSI byte-level layout the public docs do not publish. Where the
  public docs don't publish a layout, OVMX **defines its own** and **labels it an OVMX design
  choice** — never presented as VMS-authentic (same discipline as the cluster stack's opaque
  Con.ID values, `docs/clean-room/PROVENANCE.md §2`).

### 2.2 Stream (ii) — AF_DECnet Linux transport (GPL lineage)

**May be used:**
- The upstream **Linux `net/decnet` GPL-2.0 source** at the pinned commit, forward-ported, with
  license and copyright notices preserved.
- Standard kernel API documentation for the forward-port (socket layer, netdev, `AF_PACKET` for
  the fallback).

**May NOT be used:**
- **No VSI/HPE/DEC DECnet source or binary** — the same hard prohibition as stream (i). The GPL
  module is upstream-Linux code, and nothing from a VSI DECnet image may ever be merged into it.
- Do not copy stream (ii) GPL code **into** OVMX's non-GPL userspace components — the boundary is
  the `AF_DECnet` socket ABI. Userspace stays on its own licensing footing; the kernel module
  stays GPL.

### 2.3 The one shared rule

**No VSI/HPE/DEC source or binary crosses into either stream, ever.** Stream (i) is clean because
it derives only from public specs + observed behavior; stream (ii) is clean because it derives only
from upstream GPL Linux code. Neither derives from DEC-proprietary material. That is what keeps the
interop legally protected — DMCA §1201(f) (reverse engineering for interoperability) and EU
Software Directive (2009/24/EC) Article 6.

---

## 3. INV-6 / INV-0 provenance touch-points

Provenance is not only about source lineage — the register also records that the VMS-visible seam
stays honest (INV-6) and correctly branded (INV-0), because a fake there would be its own
authenticity defect regardless of clean source.

- **INV-6 (no per-process fake).** The DECnet **device**/circuit (`_NET:`/`NET$`), the
  `NET$`/`DECNET$` **system** logical names, and cross-process **logical-link/object** visibility
  must be honest — registered in the executive device table and reached via `/dev/vms` as
  transport, surfaced as VMS device names (`EWA0:` + the NCP circuit over it,
  `design-decnet-ovmx.md §2b`). Where the executive device-namespace bridge (`vms-a7e`) is
  unbuilt, the correct behavior is to **fail honestly** (`SS$_NOSUCHDEV`), never a per-process
  fake reporting success. No silent userspace fallback for an executive facility (Rule 9).
- **INV-0 (trademark).** "DECnet" is a DEC/HPE/VSI mark. The OVMX product is branded
  **"DECnet-compatible networking for OVMX"** (badge: "OpenVMS-compatible"); OVMX does not ship a
  product literally named "DECnet". `verify:` final mark wording is an open operator call
  (`design-decnet-ovmx.md §9.3`).

---

## 4. Oracle-validation plan

The GPL lineage makes stream (ii) *legally* clean, but it does **not** by itself prove the OVMX
node speaks Phase IV correctly on the segment. Rule 8 still requires validation against the **lab
oracle** — the only thing that proves wire-faithfulness. This is the acceptance spine for DECnet
Phase 0/1.

### 4.1 The oracle

- **VAX V7.3** lab node (`tests/lab/`, lab-2 k3s pod model — one pod = one isolated cluster with
  `br0` + taps) and/or **Alpha V8.4** (`tests/lab-alpha/`), both of which speak DECnet Phase IV
  natively. Follow each lab's README protocol and traps before driving it. Prefer the lab-2 /
  lab-Alpha on-demand pods over the single-instance lab-1.
- `verify:` the lab VAX/Alpha images actually have **DECnet Phase IV configured and licensed**
  (bundled on those versions) — Phase 0 confirms this and captures specimens before any OVMX code
  claims adjacency. (Under grant-all, OVMX's *own* side never gates on a license; the check here is
  only that the *oracle* is running DECnet so there is something to validate against.)

### 4.2 Infrastructure preconditions (hard)

Raw Phase IV Ethernet is **non-IP** (ethertype `0x6003`, L2 multicast) and **cannot traverse QEMU
user-mode/SLIRP**. Validation requires:
- A **tap/bridge NIC** on the OVMX VM (delivered by TCP/IP Phase 0, virtio NIC `vms-7bd`), and
- The OVMX tap must **share an L2 segment** with the oracle — reuse the lab-2 pod-bridge model
  (an OVMX-VM + lab-node pod on a shared `br0`).

### 4.3 What to observe (two-specimen minimum per value)

Following the cluster-stack derivation discipline (`docs/clean-room/PROVENANCE.md §3`), every
observed value that OVMX asserts as fact needs **≥2 captured specimens** (retained pcap +
decoder + citation), never a single reading:

| Layer | Observe from the oracle | Assert about OVMX |
|---|---|---|
| **Datalink** | Ethertype `0x6003`; `AA-00-04-00-xx-yy` source MAC derivation from `area.node`; Phase IV multicast dst (`AB-00-00-03-00-00` / `AB-00-00-04-00-00`) | OVMX emits the identical ethertype, the correct MAC for its assigned `area.node`, and joins the same multicasts |
| **Routing / adjacency** | HELLO/Router-Hello cadence and timers; endnode vs router hello; adjacency formation; the areas/nodes advertised | A lab VAX/Alpha `SHOW KNOWN NODES` / `SHOW ADJACENT NODES` lists the OVMX node as a reachable endnode with correct address |
| **NSP** | Connect Initiate/Confirm, data/ack segmentation, flow control, disconnect | An OVMX↔lab logical link establishes, carries data, and tears down matching observed NSP choreography |
| **Session / objects** | Object numbers (0 = task-to-task, 17 = FAL, CTERM object), access-control string handling | OVMX dispatches inbound objects and originates outbound to the correct object numbers |
| **Applications** | `SET HOST` (CTERM) both directions; `COPY`/`DIRECTORY` over `NODE"user pw"::dev:[dir]file` (FAL) | OVMX `SET HOST <lab node>` and `NODE::` file ops round-trip in both directions |

Retain: the pcaps, the decoder scripts (extend the `docs/clean-room/tools/` model), and
in-source citations on every reversed value (e.g. a comment naming the capture + timestamp).

### 4.4 Conformance to assert (the done-bar)

The e2e oracle gate (`design-decnet-ovmx.md §5` Phase 6): an **OVMX node joins DECnet Phase IV
with a lab VAX/Alpha on a shared segment**, and both **`SET HOST` and `NODE::` COPY succeed in
both directions**, run in a lab-2-style pod as a CI/lab gate. Nothing "announces running" without a
configured circuit (no LARP — `design-decnet-ovmx.md §8`).

### 4.5 The go/no-go: AF_DECnet-restore vs userspace-NSP fallback

Phase 1 is an explicit **GO/NO-GO** (`design-decnet-ovmx.md §5`):

- **GO (restore path):** the forward-ported `net/decnet` module **loads on the OVMX kernel, binds
  the `EWA0:` netdev, and a lab VAX/Alpha sees the OVMX node as a reachable endnode** (§4.3 datalink
  + adjacency rows pass). Then proceed on stream (ii); this register's §1/§4 are the standing
  provenance + validation record.
- **NO-GO → fallback (`design-decnet-ovmx.md §4b`):** if the forward-port to a modern OVMX kernel
  is infeasible at acceptable cost (kernel-API drift too deep, module won't load or won't form
  adjacency), pivot to the **userspace NSP + Phase IV routing engine over `AF_PACKET SOCK_RAW`**
  (forking `src/vmsscs/` LAVC datalink). **This flips L1–L2 from stream (ii) to stream (i):** the
  hand-built datalink/routing/NSP now needs **full field-by-field clean-room derivation** from the
  DNA specs, cited exactly like the cluster stack. The L3–L6 VMS surface is identical either way —
  only the engine boundary (and its provenance regime) moves.
- **Decision recording:** the go/no-go verdict, its evidence (which §4.3 rows passed/failed), and
  the pivot (if taken) are filed as a **tracked rd decision** and reflected in §6 — never a silent
  greenfield.

The forward-port surface itself (which kernel APIs drifted from the last-carrying release to the
OVMX kernel, and how each was bridged) is recorded in §4 of the design doc's intent and enumerated
here as it is discovered:

- `verify:` enumerate the kernel-API deltas between the pinned upstream `net/decnet` base and the
  OVMX kernel (socket layer, netdev registration, `proc`/`sysctl`, memory/skb APIs) — one row per
  bridged API, with the upstream call and the OVMX adaptation. Populate during Phase 1.

### 4.6 Phase 0 capture — first oracle observation (`vms-3be`, 2026-08-19)

**Oracle used:** lab-2 (`tests/lab/`), a fresh k3s StatefulSet replica (`vaxlab-0`, `NODES="vax1
vax2"`, image `ovmx-vaxlab:3`), driven per `tests/lab/README.md`'s console protocol
(prompt-synchronised login, `SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST`). Observation only —
no crafted frames injected; `tcpdump -i br0` run inside the pod netns. Lab-Alpha was **not**
used for this capture — see the note at the end of this section.

**Oracle confirmation.** DECnet Phase IV starts automatically during VAX1's `SYSTARTUP` (OPCOM:
`Message from user DECNET on VAX1 / DECnet event 4.10, circuit up / From node 1.1 (VAX1) /
Circuit QNA-0`, 19-AUG-2026 15:45:09). Confirmed interactively:

```
$ MCR NCP SHOW EXECUTOR
Node Volatile Summary as of 19-AUG-2026 15:46:43
Executor node = 1.1 (VAX1)
State                    = on
Identification           = DECnet for OpenVMS VAX V7.3

$ MCR NCP SHOW KNOWN NODES
Known Node Volatile Summary as of 19-AUG-2026 15:46:51
Executor node = 1.1 (VAX1)
State                    = on
Identification           = DECnet for OpenVMS VAX V7.3
    Node           State      Active  Delay   Circuit     Next node
                              Links
 1.2 (VAX2)                                   QNA-0          0
```

**Wire specimens** (`tcpdump -i br0 -w decnet_oracle.pcap -U -s 0` inside pod `vaxlab-0`, filter
`ether proto 0x6003`; capture window 15:46:40–15:49:55 UTC, 268476 bytes, host path below):

| # | Layer | Timestamp | Src MAC → Dst MAC | Decode | Notes |
|---|---|---|---|---|---|
| 1 | Datalink/routing | 15:46:40.469728 | `aa:00:04:00:01:04` → `ab:00:00:03:00:00` | `endnode-hello endnode vers 2 eco 0 ueco 0 src 1.1 blksize 1498 rtr 0.0 hello 15 data 2` | First of **16** endnode-HELLO frames captured, one per ~15s cycle |
| 2 | Datalink/routing | 15:47:10.504441 | `aa:00:04:00:01:04` → `ab:00:00:03:00:00` | identical decode to #1 | Second specimen, confirms periodicity |
| 3 | NSP | 15:47:39.042253 | `aa:00:04:00:01:04` → `aa:00:04:00:02:04` | `1.1 > 1.2 51 conn-initiate 8193>0 ver 4.1 segsize 1459`; payload includes plaintext access-control string `SYSTEM` (Session Control default-username field) | First of **10** NSP Connect-Initiate/Retransmit-Connect-Initiate frames — VAX1 originating `SET HOST VAX2` |
| 4 | NSP | 15:47:44.546400 | `aa:00:04:00:01:04` → `aa:00:04:00:02:04` | `1.1 > 1.2 51 retrans-conn-initiate 8193>0 ver 4.1 segsize 1459` | Second specimen, RQR/retransmit behavior on an unanswered Connect Initiate |

Ethertype `DN (0x6003)` on every frame; source MAC `aa:00:04:00:01:04` matches the documented
`AA-00-04-00-xx-yy` derivation for area.node 1.1; HELLO multicast destination
`ab:00:00:03:00:00` matches the documented Phase IV endnode multicast — **both §4.3 datalink
assertions observed and match the design doc's cited addressing**, satisfying the two-specimen
minimum independently for HELLO and for NSP.

Hex dump, specimen #1 (HELLO):
```
0x0000:  2200 0d02 0000 aa00 0400 0104 03da 0500
0x0010:  0000 0000 0000 0000 aa00 0400 0000 0f00
0x0020:  0002 aaaa 0000 0000 0000 0000 0000
```
Hex dump, specimen #3 (NSP Connect Initiate):
```
0x0000:  3300 812e 0000 aa00 0400 0204 0000 aa00
0x0010:  0400 0104 0000 0000 1800 0001 2001 03b3
0x0020:  0500 2a02 001a 0220 2006 5359 5354 454d
0x0030:  2700 0000 00
```

**What this does not establish (honest observation, not a defect to fix here).** VAX1's Connect
Initiate never reached completion — no Connect Confirm, data segment, or disconnect was observed,
because **VAX2's DECnet permanent database was never configured** in this golden image: VAX2's
`SYSTARTUP` hit a `%DCL-W-PARMDEL` parse error against a `NODENAME`/`NODEMAME`-keyed template
value, printed `You have not yet configured your DECnet permanent database...`, and left VAX2's
own `MCR NCP SHOW EXECUTOR` failing `%NCP-W-OPEFAI` / `%SYSTEM-W-NOSUCHDEV`. `SET HOST VAX2` from
VAX1 correspondingly failed `%SYSTEM-F-UNREACHABLE, remote node is not currently reachable` after
8 retransmits. This is a **golden-image asymmetry** (VAX1's permanent DB is configured, VAX2's is
not) — not a wire-protocol question, and out of scope for this observation-only capture to repair
(no golden-image edit was made; `NETCONFIG.COM`/`NCP DEFINE` on VAX2 would fix it for a future
session, per the design doc §6's config-is-allowed carve-out). Recorded here as a **capture
condition**, not an inferred protocol fact: the retransmit cadence and RQR/IE flags observed above
are genuine unanswered-Connect-Initiate behavior, not a confound with the addressing/HELLO facts.

**Retention.** The pcap and console logs are lab artifacts, **not committed** (Rule 8 practice
mirrors `docs/clean-room/PROVENANCE.md` — retain on the lab volume, cite in-doc). Host path
(tank volume, readable from `workshop`):
`/data/training/vax/k8s-labs/vaxlab-0/logs/decnet_oracle.pcap` (also
`.../vaxlab-0/logs/vax1.log`, `vax2.log` for the full console transcripts). The lab-2 replica was
scaled back to 0 after capture (`kubectl -n ovmx-lab scale sts/vaxlab --replicas=0`); no host taps
or processes were left running.

**Why lab-Alpha was not used.** `tests/lab-alpha/README.md` §"Why no DECnet" records that
DECnet Phase IV is **not installed** on the lab-Alpha V8.4 golden image, deliberately: AXPbox has
two open emulator bugs (#39, #84) where starting DECnet Phase IV machine-checks OpenVMS Alpha 8.4
(`MACHINECHK`, bugcheck 0x215). Installing it would put a known crash in the golden image for no
benefit to this capture. The VAX oracle alone satisfies the done-condition (a VAX **or** Alpha node
confirmed running Phase IV); Alpha-side DECnet observation is deferred, tracked by this note, not
a new item — pursue only if a future need specifically requires the 64-bit answer.

---

## 5. Fallback provenance regime (if NO-GO)

If §4.5 takes the fallback, this register's stream (ii) section is superseded for L1–L2 by
cluster-style clean-room provenance:

- Every reversed datalink/routing/NSP value carries the wire → fact → code chain
  (`docs/clean-room/PROVENANCE.md §3`): capture → decoder script → documented fact → cited code.
- Two-specimen minimum, retained pcaps + SHA-256 index, in-source citations.
- The DNA-spec editions used are pinned (the `verify:` items in §1.1 become mandatory citations).
- No VSI/HPE binary is disassembled — the fallback is *more* work, not a loosening of Rule 8.

The L3–L6 userspace surface (NCP, session, FAL, DCL/RMS integration) is stream (i) clean-room in
**both** the GO and NO-GO worlds — the fallback only changes who provides the L1–L2 engine.

---

## 6. Open `verify:` items (resolve before the claims they gate ship)

| # | Verify | Gates | Source to confirm from |
|---|---|---|---|
| V1 | Exact kernel removal commit + first version without `net/decnet` (design doc says "6.1, 2022") | §1.2 factual claims in customer docs | `torvalds/linux` git log |
| V2 | Last mainline commit SHA carrying `net/decnet/` (the forward-port base anchor) | §1.4 lineage chain; §4 forward-port diff | `torvalds/linux` git log |
| V3 | Exact SPDX / license header text in upstream `net/decnet/` files | §1.3 coexistence argument | upstream file headers |
| V4 | Upstream author/maintainer attribution | §1.1 lineage narrative | `net/decnet/` headers + `MAINTAINERS` |
| V5 | DNA Phase IV spec editions the upstream code cites | §1.1 / §5 fallback citations | upstream source comments |
| V6 | `vms.ko`/`vmsfs.ko` `MODULE_LICENSE` convention (to class the DECnet module the same way) | §1.3 coexistence-as-same-class | OVMX kernel sources |
| V7 | **RESOLVED 2026-08-19 (`vms-3be`, §4.6).** Lab-2 VAX1 runs DECnet Phase IV unlicensed-but-functional (`SHOW EXECUTOR` reports `State = on`; no `%LICENSE-` error observed on the executor/HELLO path). VAX2's *permanent database* is unconfigured (golden-image asymmetry, §4.6) — a config gap, not a licensing gap. Lab-Alpha does **not** have DECnet installed (AXPbox bugs #39/#84 crash it) — VAX is the oracle for Phase IV. | §4.1 oracle exists | Phase 0 lab check, `tests/lab/README.md`, `tests/lab-alpha/README.md` |
| V8 | Final INV-0 product mark wording | §3 branding | operator call (`design-decnet-ovmx.md §9.3`) |
| V9 | Kernel-API drift table (base → OVMX kernel) | §4.5 forward-port surface | Phase 1 forward-port work |

Until resolved, the register **flags** these rather than asserting them — per the constraint that
no license term, date, or commit is fabricated.

---

## 7. Cross-references

- `docs/design-decnet-ovmx.md` — the DECnet architecture; §7 references this register by name.
- `docs/design-tcpip-services-ovmx.md §2` — the shared networking-is-a-layered-product seam ruling.
- `docs/clean-room/PROVENANCE.md` — the cluster-stack clean-room model this register mirrors
  (sources permitted/forbidden, wire→fact→code chain, two-specimen discipline).
- `tests/ods2/PROVENANCE-real_vax_ods2.md` — the lab-observation-as-oracle-output precedent.
- CLAUDE.md Project-Specific Rules 8 (clean-room) and 9 (one runtime / no fake executive facility);
  INV-0, INV-6; `[[licensing-stance-grant-all]]`.
