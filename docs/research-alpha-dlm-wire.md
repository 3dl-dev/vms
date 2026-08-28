# Research — OpenVMS Alpha cross-node DLM wire (clean-room oracle)

> **Purpose.** Ground OVMX's cross-node Distributed Lock Manager (DLM) — the
> `vms_lock_dlm_xnode_dispatch` GRANT path in `src/kernel-core/vms_lock.c`, which
> today honestly returns `SS$_UNSUPPORTED` (rung 1, transport only) — against the
> **real** OpenVMS Alpha DLM. This is the fidelity oracle the DLM rung-2 GRANT
> implementation (`vms-17c`) and the rung ladder (`vms-94c` rung 1 →
> `vms-904c` contention → `vms-1bba` directory/mastering → `vms-6ee` remastering
> → `vms-d81` LVB → `vms-ec75` deadlock) cite for correctness.
>
> **Clean-room invariant (Project Rule 8, HARD).** Every fact below is derived
> from exactly two permitted classes of source, and each claim is tagged with
> which one:
>
> - **[OBSERVED]** — bytes seen on the reference-lab wire. Source: lab-Alpha
>   (`tests/lab-alpha/`, real OpenVMS Alpha V8.4 on AXPbox), captured with
>   `tcpdump` on the pod bridge. Nothing here is disassembled or decompiled.
> - **[DOCUMENTED]** — public OpenVMS references: the *OpenVMS System Services
>   Reference* (`$ENQ`/`$DEQ`/`$GETLKI`), `$LCKDEF`, the *OpenVMS Cluster Systems*
>   manual, and the *OpenVMS Internals and Data Structures Manual* (IDSM, SCS/DLM
>   chapters). These publish the **semantics and interfaces**; they do **not**
>   publish a byte-level DLM-over-SCS message layout — that layout is the RE
>   target, and where OVMX must choose bytes the docs don't specify, it is
>   labelled an **OVMX design choice**, never presented as VMS-authentic.
>
> No VSI/HPE source or binary was disassembled, decompiled, or copied.

---

## 0. Provenance summary (read this first)

| Layer | What grounds it | Status |
|---|---|---|
| SCS transport framing (Ethernet/NISCA/SCA) | **[OBSERVED]** lab-Alpha cluster-formation capture | **Solid** — real 64-bit VMS SCS on the wire |
| DLM lock **semantics** ($ENQ→GRANT, modes, LVB, directory, mastering, $DEQ, blocking AST) | **[DOCUMENTED]** public OpenVMS references | **Solid** — fully grounds rung-2 GRANT behaviour |
| DLM **message byte layout** inside an SCS datagram | **[OBSERVED]** if a 2-node lock op is captured; else **RE target** | **Partial** — see §4 and the AXPbox #83 constraint (§6) |

The rung-2 GRANT **behaviour** (what the master does on an incoming ENQ, what the
GRANT reply must carry) is fully grounded by the **[DOCUMENTED]** half. The
**[OBSERVED]** half corroborates the SCS transport the DLM rides on and — where
the emulator permits — the concrete message bytes. Per the conductor's ruling,
an emulator-capped observation is sufficient: the OVMX two-node harness ladder
(`vms-4b6` H0 → `vms-534` H1 → `vms-4bd0` H2 → `vms-209` H3 → `vms-e8f1` rung-2
GRANT) independently proves transport + real-executive reach via the status flip
`SS$_NOSUCHDEV` (2680) → `SS$_UNSUPPORTED` (2296) → `SS$_NORMAL`.

---

## 1. [OBSERVED] SCS transport — the substrate the DLM rides on

Source: `tests/lab-alpha`, two-node OpenVMS Alpha V8.4 VMScluster (`ALPHA1` +
`ALPHA2`), `tcpdump -i br0` on the pod bridge. Reference capture:
`/data/training/vax/alpha/captures/alpha-cluster-formation.pcap` (3 494 frames).
**This is the project's first non-VAX SCS capture** — the same protocol family
`vms-2f3` studies on VAX, now from 64-bit hardware.

**Ethernet / cluster addressing [OBSERVED]:**

| Field | Value | Note |
|---|---|---|
| Cluster ethertype | **`0x6007` (SCA)** | all SCS/NISCA cluster traffic |
| Node MAC — ALPHA1 | `08:00:2b:00:00:01` | DEC OUI `08:00:2b`; low byte = node index |
| Node MAC — ALPHA2 | `08:00:2b:00:00:02` | |
| Cluster HELLO multicast | `ab:00:04:01:ea:08` | periodic PATH/channel HELLO |
| MOP remote-console | ethertype `0x6002`, dst `ab:00:00:02:00:00` | boot-time, not DLM |

Traffic was bidirectional unicast between the two node MACs (≈150 frames each
direction) plus HELLO multicast (61 frames). Observed SCA frame length
distribution: 134 (most common), 256, 124, 108, 100, 80, 76, 72, 55 bytes.

**NISCA HELLO datagram, length-134 frame [OBSERVED]** (representative, ALPHA1 →
HELLO multicast):

```
08:00:2b:00:00:01 > ab:00:04:01:ea:08, ethertype SCA (0x6007), length 134
0x0000:  7600 ab00 0401 ea08 ea07 aa00 0400 0108   v...............
0x0010:  a000 0800 0080 0601 0000 0641 4c50 4841   ...........ALPHA
0x0020:  3100 8001 ff83 0004 0000 0000 0000 0000   1...............
0x0030:  0010 0700 0000 ...                         ................
0x0050:  9205 b87d 17d1 a808 bc00 0345 5741 0000   ...}.......EWA..
```

Readable structure **[OBSERVED]**: the node name `ALPHA1` appears as a
counted string (`06 41 4c 50 48 41 31` = len 6 + "ALPHA1"); the local NIC
device `EWA` (the DE500) appears near the tail. This confirms the NISCA
channel-control HELLO carries the SCSNODE name and the datalink device — the
node-identity fields OVMX's `scs_env`/`scs_dlm` frame builders substitute.

**HELLO sequence field [OBSERVED]:** across a 277-frame single-node HELLO run
(§6), the beacon is structurally constant except a **monotonic 32-bit counter at
payload offset `0x50`** (`92 05 <incrementing>`), i.e. a HELLO sequence/timestamp
— the only field that changes frame-to-frame. The ALPHA2 HELLO is byte-for-byte
structurally identical to the ALPHA1 HELLO above (counted node name, `8001 ff83`
and `0010 0700` constant framing, `EWA` device, trailing station address),
differing only in the node name/index and that counter. This corroborates the
NISCA channel-control HELLO layout on both nodes.

**What this capture proves and does not prove [OBSERVED]:** it proves the SCS
**transport** (ethertype, NISCA framing, node addressing, HELLO) on real 64-bit
VMS. It does **not** contain DLM lock messages — no `$ENQ`/`$DEQ` was issued
during formation, so no ENQ/GRANT/DEQ/BLKAST datagrams are present. The DLM
message layer (§4) needs a lock operation on a *formed, running* cluster (§6).

---

## 2. [DOCUMENTED] The DLM lock model

Source: *OpenVMS System Services Reference* (`$ENQ`, `$DEQ`, `$GETLKI`),
`$LCKDEF`, IDSM (lock-manager chapter).

- **Resource** — a named entity (resource name ≤ 31 bytes on `$ENQ`; OVMX carries
  a 32-byte null-terminated `resnam` field). Resources form a tree via parent
  lock ids. A resource is represented cluster-wide by a **Resource Block (RSB)**.
- **Lock modes** — six, and their `$LCKDEF` values are the wire mode byte:

  | Mode | `$LCKDEF` value | Meaning |
  |---|---|---|
  | `LCK$K_NLMODE` | 0 | null (no access, holds the resource/LVB) |
  | `LCK$K_CRMODE` | 1 | concurrent read |
  | `LCK$K_CWMODE` | 2 | concurrent write |
  | `LCK$K_PRMODE` | 3 | protected read |
  | `LCK$K_PWMODE` | 4 | protected write |
  | `LCK$K_EXMODE` | 5 | exclusive |

  Compatibility is the standard VMS matrix (NL compatible with all; EX with none
  but NL; etc.). OVMX validates `lkmode <= LCK$K_EXMODE` in the dispatch — matches.
- **Lock states** — granted / waiting / converting, on the RSB's three queues.
- **Lock Value Block (LVB)** — a **16-byte** value attached to the resource
  (`LCK_VALBLK_SIZE` = 16 in OVMX; `$LCKDEF` LKB$V_VALBLK). Returned to a caller
  that grants/converts with `LCK$M_VALBLK`; **written** to the resource when a
  holder of `EX`/`PW` releases or converts **down** with `LCK$M_VALBLK` set.

---

## 3. [DOCUMENTED] Resource directory + mastering — the distributed core

Source: *OpenVMS Cluster Systems*; IDSM (SCS + lock-manager distribution).

Two roles per resource, both derived cluster-wide, no central server:

- **Master node** — holds the master RSB and *all* lock queues for the resource.
  Every grant decision for that resource is made here.
- **Directory node** — determined by hashing the resource name into a
  cluster-wide **directory vector** weighted by each node's `LOCKDIRWT`. The
  directory node records *which* node currently masters the resource (or that it
  is unmastered).

**The three-case `$ENQ` lookup (IDSM) [DOCUMENTED]** — when a process on node A
enqueues on resource R:

1. **Local RSB exists** — A already masters R (or holds a process-copy RSB) →
   grant/queue **locally**, no SCS message.
2. **A is the directory node for R** — A consults its own directory. If R is
   mastered, A forwards to the master (case 3 mechanics to that master); if R is
   **unmastered**, A **becomes the master** and grants locally.
3. **A is neither** — A sends a **directory lookup** to R's directory node,
   receives the **master's CSID**, then sends the **lock request to the master**.
   (If the directory says unmastered, the requester or directory drives
   mastering per the standard rules.)

**Maps to OVMX `struct vms_resmaster_args` [DOCUMENTED↔code]:** `dir_csid`
(directory node CSID for `resnam`), `master_csid` (mastering node CSID; 0 =
unmastered), `is_local_master`, `local_csid`, `n_granted`. This is exactly the
"resource-directory / mastering lookup" the oracle must ground: OVMX resolves the
master via `GET_RESMASTER` before it can send an ENQ to the right node.

---

## 4. Cross-node message flow — [DOCUMENTED] semantics, [OBSERVED/RE] bytes

The four cross-node DLM message kinds and their direction — OVMX's
`VMS_DLM_OP_*` (`src/kernel-netbsd/vms_lock_nb.h`), which `scsd.c` static-asserts
equal to `scs_dlm.h`'s `SCS_DLM_OP_*`:

| OVMX op | value | direction | `$ENQ`/`$DEQ` correspondence [DOCUMENTED] |
|---|---|---|---|
| `VMS_DLM_OP_ENQ` | 1 | requester → master | `$ENQ` new lock **or** convert |
| `VMS_DLM_OP_GRANT` | 2 | master → requester | status/completion (grant or queued→granted) |
| `VMS_DLM_OP_DEQ` | 3 | requester → master | `$DEQ` release |
| `VMS_DLM_OP_BLKAST` | 4 | master → holder | blocking AST (holder is blocking a waiter) |

**The exchange the oracle must ground [DOCUMENTED]:**

```
Node A ($ENQ on R, R mastered by B)                Node B (master of R)
  1. GET_RESMASTER(R) → dir_csid, master_csid=B    (directory lookup, §3 case 3)
  2. ENQ  {op=ENQ, mode, flags, req_lkid,     ───▶  run through B's lock manager
           req_csid=A, resnam=R, [valblk]}          (grant if compatible, else queue)
  3.                                          ◀───  GRANT {op=GRANT, status=SS$_NORMAL,
                                                     master_lkid, [valblk if read]}
  ── later, a waiter arrives on B for R ──
  4.                                          ◀───  BLKAST {op=BLKAST} to the holder
  5. DEQ  {op=DEQ, req_lkid, master_lkid,     ───▶  release; if EX/PW + VALBLK, write LVB;
           [valblk]}                                grant waiters; send their GRANTs
```

**LVB on conversion / $DEQ [DOCUMENTED]:** the 16-byte value block travels in the
ENQ (convert-down carrying a new value) and the DEQ (release writing a value),
and is returned in the GRANT. OVMX carries it as `valblk[16]` in every
`vms_dlm_xnode_args`.

**[OBSERVED — pending / RE target]** the concrete **byte layout** of these
messages inside the SCS sequenced-message datagram. The transport frame is
**[OBSERVED]** (§1). The DLM body bytes are captured only when a lock op runs on
a formed 2-node cluster; see §5 for OVMX's current (clean-room-replayed) layout
and §6 for the emulator constraint on observing the real one.

---

## 5. [code] How OVMX encodes it today — what observation must corroborate

`src/vmsscs/scs_dlm.c` builds the DLM datagram as a **clean-room replay** of an
[OBSERVED] NISCA sequenced-message frame: the fields OVMX understands (destination
/ source logical address, `recv_ack` / `send_seq` / incarnation, the SCS envelope
at content-offset 42) are **substituted**, and the PPD fields OVMX has **not**
decoded (`0x4b`-class markers, format `0x13`) are **replayed verbatim** from the
captured frame — the standard clean-room approach when the public docs don't
publish a field. The DLM body (`scs_dlm_build_body`) then appends: op (one of the
four), mode (0..`LCK$K_EXMODE`), and the 32-byte resource-name field.

**This is where the oracle bites:** the replayed PPD bytes and the body offsets
are OVMX's current best reconstruction. A captured real-VMS DLM datagram (§6)
would either **confirm** the replayed framing carries a genuine lock message or
reveal a field OVMX is mis-placing. Until then the framing is corroborated at the
**transport** level (§1, solid) and the **body semantics** level (§2–4, solid);
the exact DLM-body byte offsets remain the RE target and any OVMX-chosen offset
the docs don't specify is an **OVMX design choice**, not claimed as VMS-authentic.

---

## 6. [OBSERVED] Live 2-node capture — the AXPbox #83 constraint

**Attempted** on lab-Alpha (`alphalab-0`, `NODES="alpha1 alpha2"`), one
time-boxed run (conductor's ruling: a single stabilisation attempt, then stop).
Two independent emulator-fragility modes bound what can be observed:

- **AXPbox #83 — `PROCGONE` on join.** When a 2-node Alpha cluster *does* form
  (`%CNXMAN, Now a VMScluster member`), **ALPHA1 bugchecks `PROCGONE` (0x36C)
  reproducibly, immediately after join — 4-for-4** across prior pod instances.
  MSCP disk-serving is the suspect (bug #83 is a disk-I/O bug); the README's
  untested mitigation is `MSCP_LOAD=0` on both nodes. A formed Alpha cluster is
  "a few seconds of usable wire, not a standing lab."
- **DE500 pcap-TX goes silent after repeated SRM power-cycles.** In this run the
  cluster never *reached* formation to test #83, because after ALPHA1's emulator
  was power-cycled through SRM (`Ctrl-P` halt → boot) ~4 times during
  reconfiguration, its libpcap **transmit** handle on `veth1` went silent —
  `%EWA0, Link state: UP` yet **zero frames on the wire** (the same "Link UP,
  zero packets" symptom the README documents for the tap-vs-veth trap, here from
  TX-handle exhaustion rather than misconfiguration). Recovery requires a **pod
  redeploy** (fresh golden clone + fresh veths), which past the time-box is
  "fighting the emulator" — so the run stopped there.

> **Live-capture result [OBSERVED] — no cluster, single-node HELLO only.** Neither
> node reached `Now a VMScluster member`; both ended at `waiting to form or join`.
> The configuration was driven to correctness first — ALPHA2 re-stamped to
> `ALPHA2`/2050 (distinct SCSSYSTEMID), ALPHA1 corrected from `VAXCLUSTER=0` to 2,
> and ALPHA1's missing `CLUSTER_AUTHORIZE.DAT` recreated at group 2026 via
> `SYSMAN CONFIGURATION SET CLUSTER_AUTHORIZATION` — so the failure was **not**
> configuration; it was ALPHA1's TX-silence above. The 26-minute capture
> (`dlm-run.pcap`, preserved beside the reference capture as
> `alpha-hello-singlenode-2026-08-28.pcap`) holds **277 SCA (0x6007) frames, all
> length 134, all ALPHA2 → HELLO multicast** `ab:00:04:01:ea:08`; **zero**
> node↔node unicast (ALPHA1 transmitted nothing). This adds no DLM lock traffic,
> but it independently **corroborates the §1 HELLO layout** on a second node and
> exposes the offset-`0x50` monotonic HELLO counter (§1). **The DLM message layer
> (§4) remains DOCUMENTED-only pending a stable formed cluster** — gated by the
> two emulator modes above, not by any OVMX or configuration fault.

**Licensing note (operator-reserved).** These nodes form the cluster **unlicensed**
(`%LICENSE-E-NOAUTH`); VMScluster is a licensed facility and this is not a
supported configuration. The §1 capture and any live capture are taken in that
same state — consistent with the existing reference capture. Alpha PAKs (VSI
Community Licence Programme) are an **operator decision**; this work does **not**
attempt to work around the licence (Rule 8 unaffected — observation only).

---

## 7. What this grounds for the rung-2 GRANT (`vms-17c`)

`vms_lock_dlm_xnode_dispatch` (rung 1) reaches each op **decoded** and returns
`SS$_UNSUPPORTED`. Rung 2 makes each op act on the real single-node lock manager
on the mastering node. This oracle grounds that behaviour:

- **`VMS_DLM_OP_ENQ` → `vms_enq_core()` on the master**, with `proc` bound to the
  remote requester's cluster identity (`req_csid`). Grant/queue per the
  §2 compatibility matrix; **[DOCUMENTED]**.
- **`VMS_DLM_OP_GRANT`** completes the originating node's pending request; must
  return `master_lkid` and, when the lock reads the LVB, the current 16-byte
  `valblk`; **[DOCUMENTED]**.
- **`VMS_DLM_OP_DEQ`** releases on the master; writes the LVB iff the releaser
  held `EX`/`PW` with `LCK$M_VALBLK`; then grants waiters and issues their GRANTs;
  **[DOCUMENTED]**.
- **`VMS_DLM_OP_BLKAST`** notifies a holder that it blocks a waiter; **[DOCUMENTED]**.
- **Master resolution** before any ENQ send uses the §3 three-case algorithm via
  `GET_RESMASTER` (`dir_csid` / `master_csid` / `is_local_master`); **[DOCUMENTED]**.

The GRANT **semantics** are therefore fully specified from public sources; the
**wire framing** is corroborated at transport (solid, §1) and pending at the DLM
body-byte level (§5–6). No fabricated cross-node grant is introduced — rung 1's
honest `SS$_UNSUPPORTED` (INV-6) stays until rung 2 wires the real lock manager.

---

## Sources

- **[OBSERVED]** `tests/lab-alpha/` — OpenVMS Alpha V8.4 / AXPbox; `tcpdump` on
  the pod `br0`. Reference capture
  `/data/training/vax/alpha/captures/alpha-cluster-formation.pcap`.
- **[DOCUMENTED]** *OpenVMS System Services Reference* (`$ENQ`, `$DEQ`,
  `$GETLKI`); `$LCKDEF`; *OpenVMS Cluster Systems*; *OpenVMS Internals and Data
  Structures Manual* (SCS + lock-manager chapters).
- **[code]** `src/kernel-core/vms_lock.c`, `src/kernel-netbsd/vms_lock_nb.h`,
  `src/vmsscs/scs_dlm.c` (OVMX DLM-over-SCS transport + dispatch).

_Clean-room (Rule 8): observation + public documentation only. No VSI/HPE source
or binary was disassembled, decompiled, or copied._
