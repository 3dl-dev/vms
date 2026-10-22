# vms-85f + vms-28bd fire `q85f1`, 2026-10-22 00:14-00:35Z — SCORED

Scored against [`PLAN.md`](PLAN.md), which was committed (`30985e010`-style, on
`work/vms-85f-queued-answer`) **before the pod existed**. The predictions were
not re-cut after the run.

**Result in one line: rd vms-85f's half of the sequence works at a real VAX —
the queued answer is on the wire, the real VAX's process is in a NORMAL,
STOPPABLE lock wait — and rd vms-28bd's half DOES NOT: the local release
advanced the peer's queue, the engine reported the flip, and no grant frame was
sent. The arm COUNTED what it owed (`deferred grants sent=0 owed=1`) rather
than being silent, and the existing counter row cannot say which of two gates
refused it. Neither item closes.**

## The arrangement, and why it is attributable

| what | value |
|---|---|
| build under test | `lab/vms-85f-fire` head **`9165186b1`** = the stacked work through rd vms-194 (PRs #1841 → #1843 → #1852 → #1855 → #1860) + the lab-only RMHOLD staging commit |
| artifacts | `build-boot-artifacts.yml` run **`39686529965`**, `SOURCE_COMMIT` = `9165186b1`; `tests/lab/tools/ovmx_fire.sh` was the only way in and it refuses a mismatch or a dirty tree |
| frame-conformance gate | PASSED for `dlm_enq_req dlm_enq_ans_queued dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14` before the node booted ("every byte grounded"; the negative control refused the corrupted frame) |
| pod | `lab85f`, `ovmx-lab`, image `ovmx-vaxlab:7`, requests == limits `1500m`/`2Gi`, k3s-worker, deleted when the re-fire is done |
| topology | the GENERAL configuration: OVMXE `LOCKDIRWT 0` (read back `LDW=0`), VAX1 and VAX2 `LOCKDIRWT 1`, three members (`F$GETSYI("CLUSTER_NODES")` = 3 on both VAXes) |

**THE SUBSTRATE IS ATTRIBUTED, which rd vms-386 is about.** This fire's P4 is a
WAIT STATE, and a SIMH timer-calibration stall (rd vms-549c) fakes exactly that,
so the pod was not taken on trust:

* in-pod `/usr/local/bin/entrypoint.sh` md5 `3912fef8ad40e15465b74187406f4796`
  and `/usr/local/bin/nodedrv.py` md5 `4d3c455984f527e17348b38ae930d0a2` are
  **byte-identical to the committed `tests/lab/entrypoint.sh` and
  `tests/lab/nodedrv.py` at `ecedf4089` — the rd vms-549c clock-fix commit — and
  to `origin/main`'s**;
* `/etc/simh-version.txt` reads `open-simh 2e0d51e9e97b2c3a28c55261108550d1f533916b (x86_64)`,
  the exact `SIMH_REF` the committed `tests/lab/Dockerfile` pins;
* the generated `vax1/vax.ini` line 5 is `set clock nocatchup`.
* **Contrast, measured the same minute:** `vaxlab-4` (image `:5`, the tag the
  repo's own Dockerfile says has no provenance) has entrypoint md5
  `04936f7f8746653c2a5157e112f22b53` = the *pre-fix* committed version at
  `aa1d30fa` (2026-08-13), and its `vax.ini` has **no clock line at all**. The
  shared reference labs are missing the fix; this fire was not run on one.

## Two departures from PLAN.md, stated

1. **RMHOLD had to be staged.** A fresh pod clones the golden 3-node volume, and
   the golden carries no `RMHOLD.EXE` (`DIR SYS$COMMON:[SYSMGR]RMHOLD.*` →
   `%DIRECT-W-NOFILES`). The committed driver
   (`../vms-50d-ra5-14cells-20261013/RMHOLD.MAR`, 105 lines) was typed onto
   VAX1's `SYS$LOGIN` through `ovmx-lab-console`, `MACRO`'d and `LINK`ed with no
   errors, `COPY`d to `SYS$COMMON:[SYSEXE]RMHOLD.EXE` so both nodes see it, and
   smoke-tested (`SPAWN/NOWAIT/PROCESS=RMTEST RMH 110-RA7CHK` → `RMTEST` in
   `HIB`, i.e. the `$ENQW` sequence completed; stopped afterwards).
2. **Step 4 was issued twice.** The first `SPAWN .. RMH 500-RA7T` on VAX2 failed
   as `%DCL-W-IVVERB ... \RMH\` — the symbol definition had been swallowed by the
   login sequence. **No frame left VAX2**, so nothing was measured and nothing
   was perturbed; the step was re-issued at 00:28:03Z after `SHOW SYMBOL RMH`
   confirmed the definition.

## The sequence, as it ran

| # | node | step | time (UTC) | result |
|---|---|---|---|---|
| 1 | VAX1 | `SPAWN/PROCESS=HOLD1 RMH 123WRA7T` | 00:25:21 | `HOLD1` in `HIB`; VAX1 masters `RA7T` (root CR + S0..S2 CW, value block written) |
| 2 | OVMXE | `RMHOLD 100-RA7T` (foreground) | 00:25:57 | granted at CR, `root lkid 01000351`. Routing was the full two-hop path: f64/f65 to VAX2 (directory) then f66/f67 to VAX1 (master), whose answer carries VAX1's master handle `0x2900050f` |
| 3 | VAX1 | `STOP HOLD1` | 00:26:21 | completes; the tree remasters onto OVMXE (f68-f83). VAX1's SDA afterwards: `%SDA-E-NORSBNAM` for `RA7T` — no resource block left on the old master |
| 4 | VAX2 | `SPAWN/PROCESS=HOLD2 RMH 500-RA7T` (EX) | 00:28:03 | **answered QUEUED** (f6552) |
| 5 | VAX2 | `SHOW SYSTEM/PROCESS=HOLD2`, SDA | 00:28:35 | `HOLD2` **`LEF`**; SDA `RA7T`: `Status DIRENTR VALID`, `CSID 00010003` (OVMXE), waiting queue `3A0001DD EX` |
| 6 | VAX2 | `STOP HOLD2` | 00:29:38 | **completes** — `HOLD2` gone from `SHOW SYSTEM` |
| 7 | VAX2 | `SPAWN/PROCESS=HOLD3 RMH 500-RA7T` | 00:30:39 | answered QUEUED (f7810); `HOLD3` `LEF`; SDA waiting queue `41000351 EX` |
| 8 | OVMXE | `^Y` | 00:31:44 | `%DCL-F-ABORT, image SYS$SYSTEM:RMHOLD.EXE terminated abnormally (signal 2)` — a LOCAL release of a lock in an adopted tree |
| 9 | VAX2 | read the holder | 00:32:03 | **`HOLD3` still `LEF`** |

## Scoring

**P1 — the hand-over still happens. PASS.** [`frames.txt`](frames.txt), the
`RA7T` run of the trimmed capture:

```
f68  VAX1  -> OVMXE  02/0e RA7T  -> 82/15 by OVMXE (f69)
f70  VAX1  -> OVMXE  02/0f RA7T  -> 82/15 by OVMXE (f73)
f74  VAX1  -> OVMXE  02/1a       (refused: rd vms-f68)
f75  VAX1  -> OVMXE  02/10 RA7T  -> 82/15 by OVMXE (f76)
f78  VAX1  -> OVMXE  02/13 RA7T
f79  VAX2  -> OVMXE  02/13 RA7T
f80  OVMXE -> VAX1   02/14 RA7T  -> 82/15 by VAX1  (f82)
f81  OVMXE -> VAX2   02/14 RA7T  -> 82/15 by VAX2  (f83)
```

`trees opened=1 … trees held=1  op-0x14 sent=2 unsent=0  completions
unreported=0`. Both 82/15 acks echoed the identity span in this topology — worth
recording against rd vms-c05, whose 445-of-1,174 non-echoing answers are all
from 3-node arms under churn, not from a quiet hand-over like this one.

**P2 — THE QUEUED ANSWER IS ON THE WIRE. PASS.** Two of them, and before this
build the count was ZERO (`../vms-3ac-promote-20261021/absence.txt`):

```
f6549->f6552  00:28:04  OVMXE -> VAX2  cat-0x82 op-0x01  body[34]=0xfb
f7807->f7810  00:30:40  OVMXE -> VAX2  cat-0x82 op-0x01  body[34]=0xfb
```

Each differs from the request it answers at **exactly 12 offsets, and at no
other offset**: `[0,1,2,3]` (the CM envelope the codec does not own), `[8]`
(category), `[20,21,22,23]` (the master's handle), `[28]`, `[32]`, `[34]`. All
twelve are inside the fifteen `dlm_queued_answer_census.py` pins over 17 real
ROOT pairs. The other three registered offsets — `body[29]`, `body[52]`,
`body[53]` — **did not differ because the request already carried the value this
build writes** (`[29]=00 [52]=00 [53]=00`): the census's pairs rewrite those two
name-tail bytes because their resource names are long enough to put bytes there,
and `RA7T` is four characters. `body[28]` is `0x11 -> 0x14`, the commonest real
queued form, exactly as the `dlm_enq_ans_queued` deviation row says.

**P3 — `body[20:24]` IS A HANDLE OVMX'S OWN DATABASE HOLDS. PASS on the wire
half; the SDA half COULD NOT BE READ.** The two answers carry `0x01000352` and
`0x01000353` — OVMX's own lkid series (its own root lock in this very tree is
`0x01000351`), non-zero, and **not** VAX2's handle, which is echoed where it
belongs at `body[24:28]` (`0x3a0001dd`, `0x41000351` — the two ids VAX2's own SDA
printed). The registered cross-check against `SDA> SHOW RESOURCE/NAME=RA7T` *on
OVMXE* could not run: `ANALYZE/SYSTEM` on the slim boot image answers
`%ANALYZE-F-NOIMG, cannot execute ANALYZE.EXE`. **That is a measurement-capability
gap for this gate, not a pass** — filed as its own item; a fire cannot read
OVMX's own lock database at the moment of a hang today.

**P4 — THE WAIT IS A NORMAL LOCK WAIT, AND IT IS STOPPABLE. PASS.** This is the
measured defect, gone. At step 5 `HOLD2` is **`LEF`**, not `RWSCS`; at step 6
`STOP HOLD2` **completed** and the process is absent from a full `SHOW SYSTEM`
(16 processes listed, no `HOLD2`). In the rd vms-3ac promotion fire the same
process was `RWSCS` and `STOP HOLD2` did not take effect.

**P5 — THE DEFERRED GRANT IS ON THE WIRE. FAIL.** Counted over the untrimmed
capture ([`absence.txt`](absence.txt)): the last frame OVMXE originated in any
DLM category is **f7810 at 00:30:40**, the second queued answer. The release was
at **00:31:44**. After it, the only frames OVMXE originated at all are two
`cat-0x86 op-0x00` close answers at 00:34:43/44 — the same signature the
rd vms-3ac fire recorded. **No grant frame, no `0xfa`, nothing.**

**P6 — AND VAX2 BELIEVES IT. FAIL,** necessarily: `HOLD3` stayed `LEF` and
VAX2's SDA still shows `41000351 EX` on the waiting queue.

**P7 — THE COUNTERS AGREE WITH THE WIRE, on both halves. SPLIT: rd vms-85f's
half PASS, rd vms-28bd's half FAIL — and the arm says so itself.**
[`cnxtrace-after-step8.txt`](cnxtrace-after-step8.txt):

```
%CNXTRACE-I-DLMEMIT, … queued=2 answered=2 unanswerable=0 unparsed=0 foreign=1
%CNXTRACE-I-DLMRECV, releases received=1 refused=0 … deferred grants sent=0 owed=1  local advances=1 stale=0
%CNXTRACE-I-DLMPOST, posts queued=12 unqueued=0 lock_gone=0 refused=0
%CNXTRACE-I-DLMLEG, connection manager: sends=14 refused=0 frames_rx=18 replies_sent=5 declined=1
```

* `queued=2 answered=2 unanswerable=0` — both queued answers in the capture came
  from THIS executive's arm, and nothing was omitted. P7's rd vms-85f clause is
  met exactly as registered.
* `local advances=1 stale=0` with `deferred grants sent=0 owed=1` — the engine
  reported the flip ONCE, and the arm **owes** the grant. The hang is not silent:
  the counter the item built for exactly this moment moved.

**P8 — NO BUGCHECK ANYWHERE, AND NO CNXMGRERR. PASS.** Zero matches of
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR` over all three
whole consoles (`console-vax1.log` 536 lines, `console-vax2.log` 412,
`console-ovmxe.log` 153). Two frame types OVMX had never sent a real node — a
`82/01` with `0xfb`, twice — reached two real V7.3 executives and neither
flinched.

**P9 — the cluster stays whole. PASS.** `F$GETSYI("CLUSTER_NODES")` = 3 on VAX1
and on VAX2 after step 9; no quorum message on any console.

**P10 — NOTHING IS LEFT UNANSWERED. PASS, with the enumeration.** Over the
untrimmed capture, **9** `cat-0x02` frames were addressed to OVMXE and **5** of
them have an OVMXE-originated `cat-0x82` answer sharing `body[4:8]`. The four
that do not are each a shape this build deliberately does not answer with a
`cat-0x82`, named and counted:

| op | n | why it is not answered |
|---|---|---|
| `0x03` | 1 | a RELEASE is a CONSUME, not a RESPOND — the codec's own allowlist (no `cat-0x82` answer to a release appears anywhere in the reference corpus). `releases received=1` |
| `0x13` | 2 | answered by OVMX's own **ORIGINATED** `op-0x14` (f5939, f5940), which is a different transaction by construction. `op-0x14 sent=2 unsent=0` |
| `0x1a` | 1 | rd vms-f68, refused on purpose. `declined=1` |

`queued_unanswerable=0`.

## What the failure narrows to, and what it does NOT yet say

Read against the shipping code, the counters exclude two of the four gates:

* `posts unqueued=0` — the notification's work item WAS taken by the fork
  thread (`dlm_arm_queue_work`'s only failure counter did not move);
* `sends=14 refused=0` — the connection manager's leg refused nothing.

That leaves `deferred_grants_no_body` (the pending table had no retained request
body under the key the grant path looks it up by) as the overwhelmingly likely
cause — **and the diagnostic row CANNOT SAY SO, because
`deferred_grants_owed` is the SUM of `deferred_grants_no_body`, the
work-queue refusal and `send_failures`, and the pending table's own
`kept`/`replaced`/`overflow`/`dropped` counters are not projected at all.**
A fire that cannot name which gate refused is a fire that has to be run twice;
that is the first thing the fix changes.

**AND THE REASON NO R1 TEST CAUGHT IT IS STRUCTURAL, which matters more than
this one bug.** `src/kernel-core/vms_dlm_scs.c` is not host-linkable (it names
`exec_kbackend.h`, `vms_internal.h` and the fork API), so:

* `test_dlm_scs_arm.c` asserts the call by **source text** —
  `has("vms_dlm_pending_take(&d->pending, r->deferred_csid")`;
* `test_dlm_deq_reachable.c` section 12 **emulates** the arm: it calls
  `vms_dlm_pending_keep(&pending, CSID_PEER, q.req_lkid, …, VMS_CM_BODY_LEN)`
  with the engine's own `q.req_lkid` and a full-length body.

So the one thing that can disagree — **the KEY the arm retains under versus the
KEY the grant path takes with**, and the LENGTH the receive leg hands over — is
the one thing no test drives. 151 passing checks in section 12 and a real VAX
still waited.
