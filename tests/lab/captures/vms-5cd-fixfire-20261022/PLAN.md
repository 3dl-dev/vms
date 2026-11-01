# rd vms-5cd fire `d5cd1` — PRE-REGISTERED, committed before the OVMX node booted

**What is under test.** rd vms-5cd: on 2026-10-22 02:07-02:38Z, twice in four
minutes, **OVMX GRANTED a real VAX EX on a resource its own hibernating process
held at CR** (`tests/lab/captures/vms-5cd-doublegrant-20261022`). The fix is in
the engine: `lock_compatible()` now answers over `res->granted` **and** the
proxies the cluster has granted this node, so a tree handed to this node is no
longer decided against an empty picture. This fire is that fix at two real
V7.3 VAXes, in the arrangement that produced the defect.

## The arrangement

| what | value |
|---|---|
| build under test | `lab/vms-5cd-fire` head **`3e0255e34`** = `work/vms-5cd-adopt-held` (PR #1868) + the lab-only RMHOLD staging commit |
| artifacts | `build-boot-artifacts.yml` run **`39717113493`**, `SOURCE_COMMIT` `3e0255e3436bf4f0e6ed651416a5dd0bf5a41e2e`; `tests/lab/tools/ovmx_fire.sh` is the only way in and refuses a mismatch, a dirty tree, or a frame type whose conformance gate is red |
| pod | **`lab5cd`**, `ovmx-lab`, image `ovmx-vaxlab:7`, requests == limits `1500m`/`2Gi`, k3s-worker, **deleted when this evidence is committed** |
| substrate, attributed by CONTENT (rd vms-386) | in-pod `entrypoint.sh` md5 `3912fef8ad40e15465b74187406f4796` and `nodedrv.py` md5 `4d3c455984f527e17348b38ae930d0a2`, both byte-identical to this tree's `tests/lab/entrypoint.sh` / `tests/lab/nodedrv.py`; `/etc/simh-version.txt` = `open-simh 2e0d51e9e97b2c3a28c55261108550d1f533916b (x86_64)`, the `SIMH_REF` `tests/lab/Dockerfile` pins; `vax1/vax.ini` line 5 = `set clock nocatchup` |
| cluster verified BEFORE the fire (rd vms-70e2) | `F$GETSYI("CLUSTER_NODES")` = **2** on VAX1 and on VAX2 at 04:00Z |
| topology | the GENERAL configuration: OVMXE `LOCKDIRWT 0`, VAX1 and VAX2 `LOCKDIRWT 1`, three members |
| the arm | `RMHOLD.MAR` (committed, `../vms-50d-ra5-14cells-20261013/RMHOLD.MAR`, 105 lines) typed onto VAX1 through `ovmx-lab-console`, `MACRO`+`LINK` with no diagnostics, `COPY`d to `SYS$COMMON:[SYSEXE]`, smoke-tested (`RMTEST` in `HIB`, `STOP RMTEST` completed) |

**The pod was rebuilt once before this plan was written, and why it matters.**
The first `lab5cd` had VAX2 fail `%EXECINIT, error initializing boot device`
and halt; booting it by hand with `B DUA0` instead of
`B/R5:10000000 DUA0` brought up a SECOND node calling itself VAX1 on the same
system root — exactly the hazard `nodedrv.py`'s own header warns about. That
clone was **discarded** (pod deleted, `/lab/k8s-labs/lab5cd` removed) rather
than used, and the pod was recreated so the entrypoint booted both roots
itself. Nothing in this fire runs on the mis-booted volume.

## The sequence

1. **VAX1**: `SPAWN/NOWAIT/PROCESS=HOLD1 RMH 123WRA7V` — VAX1 takes the fresh
   tree `RA7V` and masters it.
2. **OVMXE**: `RMHOLD 100-RA7V` — OVMX takes the ROOT at CR and hibernates
   holding it. **The arrangement check**: this request must reach VAX1 with NO
   directory redirect, which is what makes the hand-over a ONE-op-0x13 event
   (the discriminator across the three earlier runs). If a `0xf8` redirect
   appears, the name hashed its directory onto the OTHER node, the arrangement
   is the two-sender one, and the arm is re-run on another fresh name — every
   name tried is recorded. This reads the cluster's own on-wire assignment and
   computes nothing (Rule 8).
3. **VAX1**: `STOP HOLD1` — the holder ends, so the tree is handed to OVMXE.
4. **VAX2**: `SPAWN/NOWAIT/PROCESS=HOLD2 RMH 500-RA7V` — a conflicting **EX**
   request, from a member that holds nothing in the tree. *This is the step
   that was GRANTED, twice.*
5. **VAX2**: `SHOW SYSTEM/PROCESS=HOLD2`, then `ANALYZE/SYSTEM` →
   `SHOW RESOURCE/NAME=RA7V`.
6. **OVMXE**: `vms_cnxtrace`.
7. **VAX2**: `STOP HOLD2`.
8. **OVMXE**: `^Y` — a LOCAL release of a lock in an adopted tree (rd vms-28bd's
   own sequence, carried along).
9. **SECOND ARM, roles swapped** (heldout-proof-must-vary-topology): the same
   nine steps on a second fresh name with **VAX2** as the old master and
   **VAX1** as the EX requester.

## The predictions

**P1 — THE ONE. ZERO DOUBLE GRANTS.** Over the whole capture, no frame OVMXE
originates is a `cat-0x82 op-0x01` with `body[34] = 0xfa` (GRANTED) naming a
tree whose root this node's own process holds at CR. Every OVMXE-originated
`cat-0x82 op-0x01` is tabulated with its `body[34]`, so a pass is a count and
not an absence of looking.

**P2 — THE HONEST ANSWER IS SENT.** VAX2's EX request is answered exactly once,
`body[34] = 0xfb` (QUEUED), by OVMXE.

**P3 — AND IT IS ATTRIBUTABLE TO THIS EXECUTIVE.** `vms_cnxtrace` prints
`%CNXTRACE-I-DLMHOLDOWN, grants withheld by a proxy lock of ours=N` with
**N >= 1**, and `grants sent` does not rise for this tree. This is the counter
the fix added; without it a queue on the wire cannot be told from a queue for
some other reason.

**P4 — THE WAIT IS A NORMAL, STOPPABLE LOCK WAIT.** `HOLD2` is **`LEF`** (not
`RWSCS`, not `HIB`), and `STOP HOLD2` completes — the process is absent from a
full `SHOW SYSTEM` afterwards. A `HIB` here would mean VAX2 believes it HOLDS
the lock, which is the defect.

**P5 — VAX2's OWN LOCK DATABASE AGREES.** `SDA> SHOW RESOURCE/NAME=RA7V` on
VAX2 lists its lock on the **waiting** queue at **EX**, with the resource's
master CSID being OVMXE's.

**P6 — AND OVMX REALLY IS STILL HOLDING.** At the moment of the answer, OVMX's
`RMHOLD` is alive and hibernating on its CR (its console shows no exit), and
the DLM row's `proxy_lkbs` is >= 1.

**P7 — THE LIVENESS DEFECT REPRODUCES, and is measured rather than hoped away
(rd vms-083).** Exactly ONE member sends an op-0x13 for this tree; OVMXE
originates NO op-0x14; the row reads `trees opened=1 … trees held=0  op-0x14
sent=0`. P1 holding WHILE P7 holds is the point: the master refuses to grant
out of a tree it has not finished adopting.

**P8 — NEVER CRASH A PEER.** Zero `bugcheck`, `CPUSPINWAIT`, `fatal`,
`VAXPORT`, `panic`, `CNXMGRERR`, `LOCKMGRERR` on all three whole consoles;
`CLUSTER_NODES` = 3 on both VAXes after the join and again at the end; quorum
never lost.

**P9 — STATED, NOT HOPED: rd vms-28bd's half is expected STILL RED on this
build.** After step 8 the deferred grant is expected NOT to reach the wire, and
the `%CNXTRACE-I-DLMOWED` line is expected to name the door
(`keep_refused` / `table_overflow` / `no_body` / `send_failed`) with
`deferred grants sent=0 owed>=1`. A grant that DOES go out is new information
and is recorded as such. **This fire does not close rd vms-28bd either way.**

## What would falsify the fix

A single `0xfa` in step 4, or `HOLD2` in `HIB`, or a `DLMHOLDOWN` of 0 beside a
queued answer (the queue would then be happening for a reason this executive
cannot name). Any of those and rd vms-5cd stays open.
