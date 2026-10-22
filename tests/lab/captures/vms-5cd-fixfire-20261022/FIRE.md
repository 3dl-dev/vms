# rd vms-5cd fire `d5cd1`, 2026-10-22 04:05-04:28Z — SCORED

Scored against [`PLAN.md`](PLAN.md), which was committed (`a4d2d51d3`, on
`lab/vms-5cd-fire`) **before the OVMX node booted**. The predictions were not
re-cut after the run.

**Result in one line: the double grant is GONE at two real V7.3 VAXes. In the
arrangement that granted EX over OVMX's own CR twice in four minutes three
hours earlier, OVMX now answers the conflicting request `0xfb` QUEUED — twice
out of twice — the real VAX's process sits in a normal, stoppable `LEF` lock
wait, VAX2's own SDA shows its lock on the WAITING queue with the granted queue
EMPTY, and the engine counts the withheld grant (`grants withheld by a proxy
lock of ours=2`). Over the whole capture OVMX originated TWO `cat-0x82 op-0x01`
answers and ZERO of them carry `0xfa`.**

## The arrangement, and why it is attributable

| what | value |
|---|---|
| build under test | `lab/vms-5cd-fire` head **`3e0255e34`** = `work/vms-5cd-adopt-held` (PR #1868, the fix + its host rungs) + the lab-only RMHOLD staging commit |
| artifacts | `build-boot-artifacts.yml` run **`39717113493`**, `SOURCE_COMMIT` `3e0255e3436bf4f0e6ed651416a5dd0bf5a41e2e`, read back **out of the pod** by `ovmx_fire.sh` before the node booted |
| frame-conformance gate | PASSED for `dlm_enq_req dlm_enq_ans_queued dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14 dlm_deq` — "every byte grounded", and its negative control refused the corrupted frame |
| pod | `lab5cd`, `ovmx-lab`, image `ovmx-vaxlab:7`, requests == limits `1500m`/`2Gi`, k3s-worker, **deleted after this evidence was committed** |
| substrate attribution | in-pod `entrypoint.sh` md5 `3912fef8ad40e15465b74187406f4796`, `nodedrv.py` md5 `4d3c455984f527e17348b38ae930d0a2` — both byte-identical to this tree's `tests/lab/{entrypoint.sh,nodedrv.py}`; SIMH `2e0d51e9e97b2c3a28c55261108550d1f533916b`; `vax1/vax.ini` line 5 `set clock nocatchup` |
| cluster | `CLUSTER_NODES` = **2** on both VAXes before the fire, **3** after OVMXE joined and **3** again at the end |
| topology | the GENERAL configuration: OVMXE `LOCKDIRWT 0`, VAX1 and VAX2 `LOCKDIRWT 1` |

## The sequence, as it ran

| # | node | step | time | result |
|---|---|---|---|---|
| 1 | VAX1 | `SPAWN/NOWAIT/PROCESS=HOLD1 RMH 123WRA7V` | 04:16:04 | `HOLD1` `HIB`; VAX1 masters `RA7V` |
| 2 | OVMXE | `RMH 100-RA7V` | 04:17:5x | granted at CR, `root lkid 01000351`. **f5814 went STRAIGHT to VAX1 — no `0xf8` redirect**, so VAX1 is both the directory and the master for this name: the ONE-op-0x13 arrangement the defect needed |
| 3 | VAX1 | `STOP HOLD1` | 04:18:4x | completes; the tree is handed to OVMXE (f6347-f6358) |
| 4 | VAX2 | `SPAWN/NOWAIT/PROCESS=HOLD2 RMH 500-RA7V` (EX) | 04:19:3x | **answered `0xfb` QUEUED (f6487)**; `HOLD2` **`LEF`** |
| 5 | VAX2 | `ANALYZE/SYSTEM` → `SHOW RESOURCE/NAME=RA7V` | 04:20:5x | `CSID 00010003` (OVMXE), **granted queue EMPTY**, waiting queue `4F000387 EX` |
| 6 | VAX2 | `STOP HOLD2` | 04:21:3x | **completes** — absent from a full `SHOW SYSTEM` (16 processes listed) |
| 7 | VAX2 | `SPAWN/NOWAIT/PROCESS=HOLD3 RMH 500-RA7V` | 04:22:3x | **answered `0xfb` QUEUED (f7726)**; `HOLD3` `LEF` |
| 8 | OVMXE | `^Y` | 04:23:1x | `%DCL-F-ABORT, image SYS$SYSTEM:RMHOLD.EXE terminated abnormally (signal 2)` — a LOCAL release of a lock in an adopted tree |
| 9 | OVMXE | `RUN SYS$SYSTEM:CNXTRACE` | 04:23:3x | the row in [`cnxtrace.txt`](cnxtrace.txt) |
| 10 | VAX2 | read `HOLD3`, then `STOP HOLD3` | 04:23:5x | still `LEF`; `STOP` completes |
| 11 | — | **ARM 2, roles swapped**: VAX2 masters `RA7W`, OVMXE takes CR, `STOP HOLD4`, VAX1 asks EX | 04:24-04:27 | **the hand-over did not happen** — see "What arm 2 measured" |

## Scoring

**P1 — ZERO DOUBLE GRANTS. PASS, as a count.**
[`absence.txt`](absence.txt), over the whole 1,665-frame capture, by the
shipping census walker:

```
EVERY frame OVMXE ORIGINATED, by (category, opcode):   ... cat=0x82 op=0x01 : 2 ...
ITS cat-0x82 op-0x01 ANSWERS, by the outcome cell body[34]:
  body[34]=0xfb QUEUED   : 2
*** 0xfa (GRANTED) count: 0 ***
```

The defect's own frame — `cat-0x82 op-0x01 body[34]=0xfa` for a tree whose root
OVMX's own process holds at CR — **does not exist in this capture**. It existed
twice, three hours earlier, on the build without the fix
(`../vms-5cd-doublegrant-20261022`). The arm's own counter agrees from the other
side: `%CNXTRACE-I-DLMREQ, … grants sent=0`.

**P2 — THE HONEST ANSWER IS SENT, TWICE. PASS.** f6487 and f7726, both
`52540000e501 -> 08002ba72bc0  body[34]=0xfb`, one per request
([`frames.txt`](frames.txt), [`score_answers.py`](score_answers.py)).
`%CNXTRACE-I-DLMEMIT, … queued=2 answered=2 unanswerable=0`.

**P3 — AND IT IS ATTRIBUTABLE TO THIS EXECUTIVE. PASS.**

```
%CNXTRACE-I-DLMHOLDOWN, grants withheld by a proxy lock of ours=2
```

Exactly two — one per request — from the counter the fix added
(`vms_lock_dlm_proxy_compat_blocks()`, read through
`VMS_IOCTL_CLUSTER_DIAG_DLM` `0xC0E4566E`). This is the whole point of counting
it: a `0xfb` on the wire alone cannot distinguish "the compatibility check saw
the lock this node holds" from "something else queued the request".

**P4 — THE WAIT IS A NORMAL, STOPPABLE LOCK WAIT. PASS.** `HOLD2` and `HOLD3`
both **`LEF`** (never `HIB`, never `RWSCS`), and `STOP` completed for both —
`HOLD2` was absent from a full `SHOW SYSTEM` afterwards. In the two measured
failures the equivalent process went to **`HIB`**, which is a process that
believes it HOLDS `EX`.

**P5 — VAX2's OWN LOCK DATABASE AGREES. PASS**, and this is the half rd vms-eb0
says a fire cannot read on the OVMX side — here it is read on the REAL node:

```
SDA> SHOW RESOURCE/NAME=RA7V
Address of RSB:  879B0640  GGMODE: NL  Status: VALID
Lock Count: 1  CSID: 00010003
Granted queue (Lock ID / Gr mode / Range):   *** EMPTY QUEUE ***
Waiting queue (Lock ID / Rq mode / Range):
 4F000387  EX 00000000-FFFFFFFF
```

VAX2's own lock manager says: the resource is mastered on CSID `00010003`
(OVMXE), VAX2 holds nothing on it, and its `EX` is **waiting**.

**P6 — AND OVMX REALLY WAS STILL HOLDING. PASS.**
[`console-ovmxe.log`](console-ovmxe.log): `RMHOLD: root lkid 01000351 … holding
-- hibernating` at step 2, and the next line on that console is the `^Y`
`*INTERRUPT*` at step 8 — after both answers went out. `proxy_lkbs=1` in the
row.

**P7 — THE LIVENESS DEFECT REPRODUCES, MEASURED. PASS (as a prediction; the
defect is rd vms-083).**

```
f6347 VAX1 -> OVMXE  02/0e RA7V   -> 82/15 by OVMXE (f6349)
f6350 VAX1 -> OVMXE  02/0f RA7V   -> 82/15 by OVMXE (f6352)
f6354 VAX1 -> OVMXE  02/10 RA7V   -> 82/15 by OVMXE (f6357)
f6358 VAX1 -> OVMXE  02/13 RA7V   (master_lkid 0x01000351 -- OVMX's own lock)
                     ... and NO op-0x14, ever
%CNXTRACE-I-DLMRM, trees opened=1 refused=0  records adopted=0 refused=0  ends=1
%CNXTRACE-I-DLMRMHELD, trees held=0  op-0x14 sent=0 unsent=0
*** op-0x14 (tree is held here) originated by OVMXE: 0 ***
```

**ONE** op-0x13, from the old master only, exactly as in the two failing runs.
**P1 holding WHILE P7 holds is the result**: the master refuses to grant out of
a tree it has not finished adopting, instead of serving a conflicting request
from a half-built picture.

**P8 — NEVER CRASH A PEER. PASS.** Zero matches for
`bugcheck|BUG CHECK|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR|LOCKMGRERR` on all
three whole consoles (555 + 387 + 265 lines, committed here).
`CLUSTER_NODES` = 3 on both VAXes at the end. Quorum never lost.

**P9 — rd vms-28bd's half IS still red, AND THE FIRE NAMED THE DOOR.** This is
new information the item asked for in as many words ("the arithmetic over the
shipped code says `no_body` or `send_failed`, but 'must be' is not a
measurement"):

```
%CNXTRACE-I-DLMRECV, … deferred grants sent=0 owed=1  local advances=1 stale=0
%CNXTRACE-I-DLMOWED, owed grants by door: keep_refused=0 table_overflow=0 no_body=0 send_failed=1
```

So the engine reported the flip, the pending table **did** keep the requester's
frame and **did** hand it back (`keep_refused=0`, `no_body=0`), and the
**SEND** is what refused — while the connection manager's own leg says
`sends=14 refused=0`, i.e. the frame never reached it. The gate is inside the
DLM arm's own send path, not the pending table and not the CM. rd vms-28bd
stays open with that one line as its next read.

## What arm 2 measured, stated rather than quietly dropped

The roles-swapped arm (VAX2 masters `RA7W`, OVMXE holds its root at CR, VAX2's
holder ends, VAX1 asks `EX`) **did not reproduce the hand-over**: no `02/0e` for
`RA7W` ever left VAX2, within ~4 minutes of `STOP HOLD4`, so OVMX never became
that tree's master and the arm tested nothing about OVMX's compatibility check.
`releases sent=1` in the row is OVMX's own `op-0x03` for that lock, correctly
addressed to VAX2, the node that really masters it.

**But it did produce a REAL-VMS ORACLE for exactly the behaviour the fix gives
OVMX**, and that is worth more than the arm would have been:

```
f9376 VAX1 -> VAX2  02/01 RA7W  EX
f9378 VAX2 -> VAX1  82/01 RA7W  body[34]=0xfb QUEUED
```

A real V7.3 master, holding a remote `CR` for a third node (OVMX), answered a
conflicting `EX` **`0xfb` QUEUED** — the same decisive cell, from a different
master, in a different direction. OVMX's answer in arm 1 is byte-identical in
that cell to what the real lock manager does.

**One correlation this fire recorded and did NOT draw a conclusion from**
(it belongs to rd vms-083's census): the tree whose old master was ALSO its
directory node (`RA7V`, arm 1) was handed over; the tree whose directory was
the OTHER node (`RA7W`, arm 2) was not, within the observation window. That is
the same variable the one-vs-two op-0x13 discriminator turns on.

## Departures from PLAN.md, stated

1. **`RMHOLD` had to be staged on the VAXes**, as in the previous fire: the
   golden volume carries none. `RMHOLD.MAR` (committed,
   `../vms-50d-ra5-14cells-20261013/RMHOLD.MAR`) was typed onto VAX1 through
   `ovmx-lab-console`, `MACRO`+`LINK` with no diagnostics, `COPY`d to
   `SYS$COMMON:[SYSEXE]`, and smoke-tested (`RMTEST` `HIB`, `STOP` completed).
2. **On OVMXE, `RMHOLD` is not a DCL verb** — `%DCL-W-IVVERB` both as a spawned
   command and in the foreground. The image IS staged
   (`DIR SYS$SYSTEM:RMHOLD.EXE` → one file); it needed the foreign-command
   symbol `RMH :== $SYS$SYSTEM:RMHOLD.EXE`. No frame left the node for the two
   refused attempts, so nothing was perturbed.
3. **The pod was built twice.** The first `lab5cd`'s VAX2 failed
   `%EXECINIT, error initializing boot device` and halted; a hand `B DUA0`
   brought up a SECOND node calling itself VAX1 on the same system root (the
   hazard `nodedrv.py`'s header warns about — the correct command is
   `B/R5:10000000 DUA0`). That clone was **discarded** (pod deleted,
   `/lab/k8s-labs/lab5cd` removed) and the pod recreated so the entrypoint
   booted both roots itself. Nothing in this fire ran on it; `PLAN.md` was
   written after, and says so.
4. **`$GETLKIW` fails inside `RMHOLD` on OVMXE** (`status %X00000014`, "not
   answered") — rd vms-6f2, already open, unrelated to this fire: the lock IS
   granted (the root lkid is printed, the process hibernates holding, and the
   wire shows the grant).

## What this fire does and does not close

* **rd vms-5cd: the SAFETY half is proven at two real VAXes.** Zero double
  grants, both requests queued and answered, the real VAX's process in a normal
  stoppable wait, its own SDA agreeing, and this executive's own counter naming
  the reason.
* **rd vms-083 (the liveness half) is UNCHANGED and reproduced**: the tree never
  reaches HELD, so the honest outcome is a WAIT that only ends when the holder
  lets go. That is a hang for a workload, and it still blocks rd vms-ci.6.
* **rd vms-28bd is still red**, now with its door named: `send_failed=1`.
