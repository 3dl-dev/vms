# rd vms-a4d7 / rd vms-28bd fire `a4d1` — PRE-REGISTERED, committed before the OVMX node booted

**What is under test.** rd vms-28bd: a LOCAL release that flips a queued
cross-node waiter to granted must TELL that peer. Measured failing three times
at two real V7.3 VAXes — fires q85f1 (2026-10-22 00:31:44Z), q85f2 (01:27:59Z)
and the rd vms-5cd fix fire `d5cd1` (`tests/lab/captures/vms-5cd-fixfire-20261022`):
the engine reported the flip and **no frame went out**. The last of those read the
door for the first time: `deferred grants sent=0 owed=1`, decomposed as
`keep_refused=0 table_overflow=0 no_body=0 send_failed=1`, with the connection
manager's own leg reporting `sends=14 refused=0` — the frame never reached it.

rd vms-a4d7 is why, and the fix is not a wider gate. The frame rd vms-f87 built
was the requester's own queued request echoed back as a SECOND `cat-0x82 op-0x01`
answer carrying `0xfa`, and `vms_dlm_shape_fit_for_any_member` rejects every
cat-0x82 toward a system that has not proved it runs this implementation. **The
gate was right:** over the whole committed corpus
(`tools/cluster/dlm_late_grant_census.py`) 0 of 111 real queued transactions were
ever answered `0xfa` on that transaction, and 0 of the 3,974 real `0xfa` grants
whose request their capture lacks name a handle pair that master had QUEUED.

What a real master sends at that moment is a **`cat-0x02 op-0x02`**: 95 of those
111 queued answers are followed by one from the MASTER to the REQUESTER naming
that lock's own `(master, requester)` handle pair, **0 are preceded by one**, and
87 of the 95 are followed by the requester's own `$DEQ`/CONVERT on those handles.
387 are in the corpus and **OVMX has emitted none**. This fire is the first time
OVMX puts that frame in front of a real VMS lock manager.

## The arrangement

| what | value |
|---|---|
| build under test | `lab/vms-a4d7-fire` head **`f8f462511`** = `work/vms-a4d7-grant-notify` (PR #1877) + the lab-only RMHOLD staging commit |
| artifacts | `build-boot-artifacts.yml` run **39737735927**; `tests/lab/tools/ovmx_fire.sh` is the only way in and refuses a `SOURCE_COMMIT` mismatch (checked again IN the pod), a dirty tree, or a frame type whose conformance gate is red |
| frame types declared to the gate | `dlm_enq_req dlm_enq_ans_queued dlm_grant_notify dlm_deq dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14` — `dlm_grant_notify` is the NEW type and the whole reason the gate exists (ovmx-never-crashes-a-peer: no frame type reaches a real VAX before every byte of it is grounded) |
| pod | **`laba4d7`**, `ovmx-lab`, image `ovmx-vaxlab:7`, requests == limits `1500m`/`2Gi`, k3s-worker, **deleted when this evidence is committed** |
| topology | the GENERAL configuration: OVMXE `LOCKDIRWT 0`, VAX1 and VAX2 `LOCKDIRWT 1`, three members |
| the arm | `RMHOLD.MAR` (committed, `../vms-50d-ra5-14cells-20261013/RMHOLD.MAR`) typed onto VAX1 and VAX2 through `ovmx-lab-console`, `MACRO`+`LINK`, smoke-tested before the fire |

## The sequence — the same nine steps fire `d5cd1` ran, on the fixed build

1. **VAX1**: `SPAWN/NOWAIT/PROCESS=HOLD1 RMH 123-<NAME>` — VAX1 takes a fresh
   tree and masters it.
2. **OVMXE**: `RMHOLD 100-<NAME>` — OVMX takes the ROOT at CR and hibernates
   holding it. *Arrangement check:* this request must reach VAX1 with NO
   directory redirect (`0xf8`), which is what makes the hand-over a
   ONE-op-0x13 event. A redirect means the name hashed its directory onto the
   other node; the arm is then re-run on another fresh name and every name
   tried is recorded. This reads the cluster's own on-wire assignment and
   computes nothing (Rule 8).
3. **VAX1**: `STOP HOLD1` — the holder ends, so the tree is handed to OVMXE,
   which answers 0e/0f/10, adopts, reaches HELD and originates its op-0x14.
4. **VAX2**: `SPAWN/NOWAIT/PROCESS=HOLD2 RMH 500-<NAME>` — a conflicting **EX**
   request from a member that holds nothing in the tree. With the rd vms-5cd
   fix this is **QUEUED** (`0xfb`), not granted; fire `d5cd1` proved that half.
5. **VAX2**: `SHOW SYSTEM/PROCESS=HOLD2` — the waiter's state BEFORE the
   release, recorded so the after-state means something.
6. **OVMXE**: `^Y` on the hibernating holder — **a LOCAL release of a lock in an
   adopted tree. THIS IS THE STEP UNDER TEST.**
7. **VAX2**: `SHOW SYSTEM/PROCESS=HOLD2`, then `ANALYZE/SYSTEM` →
   `SHOW RESOURCE/NAME=<NAME>` and `SHOW LOCK/BLOCKING`.
8. **OVMXE**: `vms_cnxtrace` — the DLMOWED and DLMPOST lines.
9. **SECOND ARM, roles swapped** (heldout-proof-must-vary-topology): the same
   steps on a second fresh name with **VAX2** as the old master and **VAX1** as
   the queued EX requester, so the grant goes to a different peer.

## The predictions — registered now, scored after

**P1 — THE ONE. OVMXE ORIGINATES A `cat-0x02 op-0x02` AFTER STEP 6.** At least
one frame whose source is OVMXE's MAC, `body[8] = 0x02`, `body[9] = 0x02`,
addressed to VAX2, appears in the capture AFTER the local release and did not
appear before it. *Falsified if* the capture's last OVMXE-originated DLM frame is
still the queued answer, as it was in all three earlier fires.

**P2 — ITS CELLS ARE WHAT THE EXECUTIVE HOLDS.** In that frame:
`body[20:24]` = the handle OVMX's own engine assigned VAX2's lock (the value
`body[20:24]` of the `0xfb` queued answer at step 4 carried), `body[24:28]` =
VAX2's own handle (the value VAX2's request carried at `body[24:28]`),
`body[30] = body[31] = 0x05` (EX, the mode VAX2 asked for), `body[28:30] = 14 00`,
`body[4:8] = 0` and `body[47] = 0`. Scored byte-for-byte against the step-4 pair,
not against a template.

**P3 — THE ARM'S OWN LEDGER AGREES.** `vms_cnxtrace` reads
`deferred grants sent=1` (2 after the second arm) with
`owed=0`, and the DLMOWED line `keep_refused=0 table_overflow=0 refused=0
send_failed=0`; `local advances=1` and `stale=0`. *Falsified if* `send_failed`
or `refused` is non-zero — that would mean the new shape is still being refused,
at a different door.

**P4 — VAX2's PROCESS LEAVES ITS WAIT AND HOLDS EX.** `SHOW
SYSTEM/PROCESS=HOLD2` at step 7 shows it NOT in `LEF`/`LEFO` on the lock (it
hibernates holding, which is what `RMHOLD` does once granted), and SDA's
`SHOW RESOURCE/NAME=<NAME>` on VAX2 lists its lock **Granted at EX**. *This is
the hang the ci.6 close criteria name* ("zero hangs — every DCL step returns"):
`STOP HOLD2` must also complete.

**P5 — NEVER-CRASH.** Zero `bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR|
INVLOCKID|LOCKMGRERR` on all three whole consoles. A new frame type in front of
a real lock manager is exactly the class that bugchecked two lab VAXes.

**P6 — THE ECHO DOES NOT APPEAR.** No second `cat-0x82 op-0x01` on VAX2's step-4
transaction token, and no OVMXE-originated `0xfa` naming that handle pair. The
shape the corpus has no instance of must have no instance here either.

**P7 — IT IS A NOTIFICATION.** `body[4:8] = 0` in every op-0x02 OVMXE sends, and
the capture holds ZERO `cat-0x82 op-0x02` from either VAX: nobody answers one, so
no transaction is left open on the CSB (the envelope-inconsistency class that
produced CNXMGRERR).

**P8 — THE SECOND ARM BEHAVES IDENTICALLY** with the roles swapped, so the result
is not a property of which node is which.

## What this fire does NOT claim

It does not settle `body[34]`, the three residue spans, or what makes the two
mode cells differ when they do (41 of 267 in the CONVERT-correlated population).
Each is recorded as measured in the census and left alone (Rule 8).
