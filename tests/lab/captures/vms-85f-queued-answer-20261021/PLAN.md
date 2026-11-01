# vms-85f + vms-28bd fire: the real VAX is ANSWERED, waits NORMALLY, and is GRANTED

**Registered before the pod exists**, and before the build under test reaches
one. Two items, ONE fire, because they are two halves of one sequence and the
hang they fix was measured as one episode: a real VAX's `$ENQW` on a resource
OVMX masters and holds got no answer at all (rd vms-85f), and then no grant when
OVMX's own lock went away (rd vms-28bd). Splitting them would leave each half
unmeasurable -- a queued answer with no grant behind it still hangs, and a
deferred grant with no retained request body counts `deferred_grants_no_body`
and sends nothing (this item's own note says so).

Build under test: `work/vms-85f-queued-answer` (PR #1852) on top of
`work/vms-3ac-complete` (PR #1843) on top of `work/vms-c2b-arm` (PR #1841).
`ovmx_fire.sh` must refuse unless `SOURCE_COMMIT` is the tested commit and the
tree is clean, and the frame-conformance gate must pass for
`dlm_enq_req dlm_enq_ans_queued dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans
dlm_rm_14` before the node boots -- `dlm_enq_ans_queued` is a frame type OVMX
has NEVER emitted at a real node, so the never-crash rule's own condition
applies to it (`ovmx-never-crashes-a-peer`: no lab fire that sends a new frame
type to a real VAX until an automated frame-conformance gate exists for that
type).

## Why this topology

The GENERAL configuration -- OVMX `LOCKDIRWT 0`, both real VAXes `LOCKDIRWT 1`
-- the one rd vms-ci.6 must close in, and the one the measured hang happened in.
The sequence is deliberately the one that produced it
(`../vms-3ac-promote-20261021/FIRE.md`, "What this fire ALSO found"): rotating
it would measure something else. Varying it is RA7 fire B's job (rd vms-6ad1).

## The sequence

| # | node | step |
|---|---|---|
| 1 | VAX1 | `RMHOLD 123WRA7T` -- takes the tree first and therefore MASTERS it (root CR, value block `RMLVB-0123456789`) |
| 2 | OVMXE | `RMHOLD 100-RA7T` -- the root at CR. No sub-locks: rd vms-b67a is still open, and a queued SUB-LOCK request is deliberately NOT answered by this build (`queued_unanswerable`) |
| 3 | VAX1 | `STOP HOLD1` -- the tree remasters onto OVMXE, which now masters RA7T **and holds its root at CR** |
| 4 | VAX2 | `RMHOLD 500-RA7T` -- an **EX** request on that root. **This is the request that got no answer.** |
| 5 | VAX2 | `SHOW SYSTEM` / `SHOW PROCESS/ID=` on the holder -- read the wait state |
| 6 | VAX2 | `STOP HOLD2` -- **the step that could not complete before** |
| 7 | VAX2 | `RMHOLD 500-RA7T` again (a fresh process), so there is a queued request to GRANT |
| 8 | OVMXE | `^Y` -- end the holder. A **LOCAL** release of a lock in an adopted tree |
| 9 | VAX2 | read the holder's console: the EX must be reported GRANTED |

Steps 6 and 7 are two different questions and both are required. Step 6 asks
whether the wait is a NORMAL lock wait (rd vms-85f: it was `RWSCS`, and `STOP`
could not complete it). Step 7 re-creates the queued request that step 8 must
grant, because step 6 deliberately destroyed the first one.

## Predictions, each scored on its own

**P1 -- the hand-over still happens**, unchanged by this build: `0e/0f/10` from
VAX1 to OVMXE each answered `82/15`, a `0x13` from each other member, an
`op-0x14` from OVMXE to each, each answered `82/15`. This build touches no
remaster code; a difference here invalidates the fire.

**P2 -- THE QUEUED ANSWER IS ON THE WIRE.** After step 4, OVMXE originates a
`cat-0x82 op-0x01` to VAX2 with `body[34] = 0xfb`. Scored on the capture, and
against the committed census: the frame must differ from VAX2's request in
exactly `body[0:4]`, `body[8]`, `body[20:24]`, `body[28]`, `body[29]`,
`body[32]`, `body[34]`, `body[52]` and `body[53]` -- the fifteen-offset rule
`dlm_queued_answer_census.py` pins over 17 real ROOT pairs, with `body[22]`,
`body[23]` and `body[21]` inside the handle. **FAIL if any OTHER offset
differs**: that is a cell this node has no business rewriting.
*Before this build the count of OVMX-originated `82/01` frames in this step was
ZERO* (`../vms-3ac-promote-20261021/absence.txt`), which is this prediction's
measured control.

**P3 -- body[20:24] IS A HANDLE OVMX'S OWN DATABASE HOLDS.** The value in that
cell equals the master lock id `SDA> SHOW RESOURCE/NAME=RA7T` on OVMXE reports
for the LKB held FOR VAX2 (and `vms_cnxtrace` reports the same arm). It is not
zero and it is not VAX2's own handle. **FAIL if it is either**: that is the
minted-value class that bugchecked two lab VAXes.

**P4 -- THE WAIT IS A NORMAL LOCK WAIT, AND IT IS STOPPABLE.** At step 5 VAX2's
process is in a lock wait (`LEF`/`LEFO`), **not** `RWSCS`. At step 6 `STOP
HOLD2` **completes** -- the process is gone from `SHOW SYSTEM` afterwards.
**FAIL/STOP if the process is in `RWSCS` or survives the `STOP`**: that is the
measured defect, unfixed.

**P5 -- THE DEFERRED GRANT IS ON THE WIRE, AND IT IS THE REQUESTER'S OWN FRAME.**
After step 8 OVMXE originates a `cat-0x82 op-0x01` to VAX2 with
`body[34] = 0xfa`, `body[28] = 0x10`, `body[32:36] = 01 00 fa 00`, and
`body[24:28]` equal to the handle VAX2's own step-7 request carried. **FAIL if
`body[24:28]` is anything else**: the correlation is the whole point, and
breaking it is what caused the 65,000-frame storm (rd vms-b5b0).

**P6 -- AND VAX2 BELIEVES IT.** At step 9 the `RMHOLD` process on VAX2 reports
its EX **granted**, and `SDA> SHOW RESOURCE/NAME=RA7T` on VAX2 shows its own
lock granted at EX. A frame OVMX is happy with that VAX2 ignores is not a fix.

**P7 -- THE COUNTERS AGREE WITH THE WIRE, on both halves.** `vms_cnxtrace` on
OVMXE after step 8:
`queued=1 answered=1 unanswerable=0` (rd vms-85f) and
`local advances=1 stale=0` with `deferred grants sent=1 owed=0` (rd vms-28bd).
This is the half a pcap cannot show -- that the executive's own arm put those
frames there, from its own lock database. **FAIL if `answered` is 0 while a
`0xfb` is in the capture** (then the frame came from somewhere else), **or if
`unanswerable` or `stale` moved** (then something was omitted and the fire must
say what).

**P8 -- NO BUGCHECK ANYWHERE, AND NO CNXMGRERR.** Zero
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR` lines on all
three consoles, counted over the whole run. Two new frame types reach a real
VAX's executive in this fire (`82/01` with `0xfb`, and an ORIGINATED `82/01`
grant for a request nobody just re-asked), and `ovmx-never-crashes-a-peer` makes
that the release-gating prediction.

**P9 -- the cluster stays whole.** `F$GETSYI("CLUSTER_NODES")` reads 3 on both
VAXes after step 9, and quorum is never lost.

**P10 -- NOTHING IS LEFT UNANSWERED.** Counted over the untrimmed capture: every
`cat-0x02` request either real VAX addressed to OVMXE in the window has an
OVMXE-originated `cat-0x82` answer, or is one of the shapes this build
deliberately does not answer, named and counted (`queued_unanswerable`,
`redirects_sent`, and the op-0x1a refusal of rd vms-f68). ci.6's close criteria
name "zero VAX DLM/CM request to OVMX left unanswered in the pcap", and this is
that criterion asked of this fire.

## What this fire does NOT test, stated

* a queued **SUB-LOCK** request. This build refuses to answer one (93 of 93 real
  answers carry the master's own S0 address at `body[40:44]`), so step 2 holds
  the root only. rd vms-b67a and the driven arm it needs come first.
* the `body[28:30]` / `body[52]` determinant. OVMX writes the commonest real
  form, recorded as a `deviation` row; what SELECTS the other form is ungrounded
  and this fire is not a one-variable arm for it. If a real VAX rejects the
  frame, that is what P6 is for -- and then the deviation rows are where to look.

## Files this directory must carry afterwards

| file | what it is |
|---|---|
| `FIRE.md` | the scored predictions, pass or fail, with the numbers |
| `trimmed.pcap.gz` | the frames, by `tools/cluster/trim_lab_pcap.py`'s committed rule, payloads unaltered |
| `frames.txt` | those frames decoded, in order |
| `absence.txt` | the absences, counted over the UNTRIMMED capture |
| `console-*.log` | all three consoles, whole, for the whole run |
| `cnxtrace-*.txt` | P7's readings, at the moments named |
