# vms-0db: does a real holder carry its WAITING and CONVERTING locks in op-0x12 and op-0x08?

**Registered 2026-11-02, BEFORE the pod exists and before any byte of this capture is taken.**
Branch `work/vms-0db-waiting-capture`, off `origin/main` `e1596e190`. The holder program
`RMWAIT.MAR` in this directory is committed with this plan.

## The question

OVMX's holder sender (`src/kernel-core/vms_lock.c`, `dlm_handover_fill`) refuses any lock with
`lock->waiting` set, and the op-0x08 rebuild path shares that fill. So a waiting or converting
OVMX lock is never told to a new master, and it hangs. Real holders do not look like that. Every
cat-0x02 op-0x12 / op-0x08 frame in the committed corpus, ROOT vs SUB by body[36:44], tallied by
(body[30], body[31]):

| op | kind | (30,31) equal | (30,31) differing |
|---|---|---|---|
| 0x12 | ROOT | 859 | (0,3) x327, (4,1) x13, (3,1) x3, (2,3) x2, (4,0) x2, (5,0) x2, (5,2) x4 |
| 0x12 | SUB | 682 | (4,0) x2,038, (3,0) x9 |
| 0x08 | ROOT | 1,566 | (5,2) x16, (4,0) x4, (5,0) x2, (0,3) x1, (1,0) x1 |
| 0x08 | SUB | 376 | none |

rd vms-2fa (#1943) falsified "body[30] is always the higher of the two" (arm1's CACHE$ ROOT, 0 < 3).
The one SDA-named differing record is RA6's `RMT6` (`vms-50d-ra6-14cells-20261013`): SDA listed
lock `13000192` granted **NL**; RMHOLD had taken it EX and converted it down to NL; its op-0x12
record carries (5,0). Nothing in the corpus is a lock SDA showed on a conversion or waiting queue.

Four things are not grounded: which cell is the granted mode and which the requested mode; whether
a waiting lock (a new request, never granted) and a converting lock (granted, blocked converting
up) are recorded at all, in op-0x12 and in op-0x08; what their cells carry (a queue/state byte,
the lkid, the parent pair); and where they land on the new master's queues.

## Hypotheses

- **HR (records):** a holder sends one record per LKB it owns in the tree, granted, converting
  and waiting alike, in op-0x12 and in op-0x08.
  - **Rival HR0:** waiting and converting LKBs are skipped (OVMX's current behaviour).
  - **Rival HN:** VMS does not remaster a tree while its conversion or waiting queue is non-empty
    (no op-0x0e for it while the queues hold a lock).
- **Hx (cells):** body[30] = the LKB's REQUESTED mode, body[31] = its GRANTED mode. RA6's (5,0)
  is then "requested EX (left over from the first $ENQW), granted NL".
  - **Rival Hy:** body[30] = granted, body[31] = requested.
- **HQ (state):** a waiting record and a converting record are told apart from a granted one by a
  cell other than body[30:32], because under Hx a waiting EX request (requested 5, granted NL) has
  the same mode pair as RA6's granted-NL lock (5,0). Candidates the census already knows vary:
  body[17], body[28], body[29], body[34], body[71], body[108:111].
- **HO (order):** within one resource, the holder sends its records in queue order, granted
  first, then converting, then waiting. Rival: lkid order, or process order.

## The arms

VAX ONLY. **No OVMX node, so OVMX emits nothing at a real VAX.** Own pod `lab0db` in `ovmx-lab`
(requests == limits 1500m/2Gi, `ovmx-vaxlab:7`), deleted after the run. vax1 (SYS0) and vax2
(SYS1) come from the entrypoint, vax3 (SYS2) from `tests/lab/tools/oracle_stage.sh node vax3`.
`RMHOLD.MAR` (unchanged, from `vms-50d-ra5-14cells-20261013/`) and `RMWAIT.MAR` are put on the
shared system disk with `vput.py` (same directory as RMHOLD) and assembled and linked on VAX1.
Each node's LOCKDIRWT and CLUSTER_NODES are read with `F$GETSYI` before each arm.

`RMH` = RMHOLD, `RMW` = RMWAIT. Lock modes: 0 NL, 1 CR, 2 CW, 3 PR, 4 PW, 5 EX.

### Arm A: activity remaster, op-0x12 (RA2/RA5/RA8 trigger). Tree `WQRA`. O=VAX1, N=VAX2, H=VAX3.

In this order (every granted request before any queued one, so no new request queues behind a
waiter):

1. VAX1: `RMH 110WWQRA` (root CR, value block written). VAX1 masters WQRA.
2. VAX3: `RMH 112-WQRA` (root CR, S0 and S1 at CR); `RMW 11GWQRA` (root CR, W0 CR: the W0
   blocker); `RMW 15VWQRA` (root CR, W0 CR, then CONVERT W0 to EX: **converting sub**);
   `RMW 15UWQRA` (root CR, then new W0 at EX: **waiting sub**).
3. VAX2: four `RMH 312BWQRA` (root PR, S0 and S1 at CR, 3,000 rounds of activity).
4. VAX3: `RMW 51CWQRA` (root CR, then CONVERT root to EX: **converting root**); `RMW 50QWQRA`
   (new root at EX: **waiting root**). Both stay blocked by every CR and PR granted above.
5. SDA on all three nodes: `SHOW RESOURCE/NAME=WQRA`, `SHOW RESOURCE/NAME=W0`, and
   `SHOW PROCESS/LOCKS` for every VAX3 RMH/RMW process.
6. Start the capture, then `STOP` VAX1's holder. Wait for SDA on VAX2 to show WQRA with
   CSID 00000000. If VAX2's activity finishes first, spawn more `RMH 312BWQRA` on VAX2. If the
   tree has not moved 10 minutes after the STOP, HN is scored and the arm is reported as such.
7. SDA on all three nodes again, then stop the capture.

Contingency, registered now: if WQRA moves to VAX3 (the holder of the waiters) instead of VAX2,
VAX3 sends no records and arm A is re-run as `WQRC` with the waiters on VAX2 and the activity on
VAX3. That re-run is added to this plan as an addendum before it fires.

### Arm B: master departure, op-0x08 (vms-2fa arm 1 style). Tree `WQRB`. O=VAX1 leaves.

1. Stop every arm-A process on VAX2 and VAX3 (their tree is not the question here).
2. VAX1: `RMH 110WWQRB`. VAX3: `RMH 112-WQRB`, `RMW 11GWQRB`, `RMW 15VWQRB`, `RMW 15UWQRB`.
   VAX2: `RMH 312-WQRB` (no activity, so nothing remasters the tree before the departure).
   Then VAX3: `RMW 51CWQRB`, `RMW 50QWQRB`.
3. SDA on all three, as in arm A step 5.
4. Start the capture. On VAX1: `@SYS$SYSTEM:SHUTDOWN` with `REMOVE_NODE`. If VAX1 reads
   LOCKDIRWT 0 it may hand WQRB over by op-0x12 at shutdown (vms-2fa arm 3). If it does, that
   remaster is scored under arm A's predictions instead, and VAX1 is removed by stopping its
   simulator (`^E`) in a repeat with tree `WQRD`, added as an addendum before it fires.
5. SDA on VAX2 and VAX3 after the departure transition, then stop the capture.

## Predictions, each scored PASS/FAIL

Arm A (op-0x12):

- **PA1 (against HN).** WQRA moves to VAX2 with the conversion and waiting queues non-empty: an
  op-0x0e for WQRA, and SDA on VAX2 afterwards shows WQRA with CSID 00000000.
- **PA2 (HR).** VAX3 sends one op-0x12 record per LKB it owns in WQRA: 11 records (root and two
  subs for RMH; root and W0 for each of G, V and U; root for C; root for Q), each answered 82/01.
  Each record's body[24:28] is a lkid that SDA listed on VAX3 before the move. HR0 predicts 7
  (no Q, no U, and no C or V records). Any count other than 11 is reported with which LKBs are
  missing.
- **PA3 (Hx).** The converting root C carries (body[30], body[31]) = (5,1); the converting sub
  V (5,1); the waiting root Q (5,x) and the waiting sub U (5,x), where x is the granted-mode
  cell of a never-granted lock (predicted 0). Every granted lock carries (mode, mode). Hy
  predicts (1,5) for C and V. If neither fits, the pair is reported as measured.
- **PA4 (HQ).** At least one cell other than body[30:32] has one value on every converting or
  waiting record and another on every granted record of the same capture. The census names
  each such cell and its values. If no cell separates them, HQ is falsified, and the report
  says so: the new master would then have to tell a waiting lock by its modes alone.
- **PA5 (HO).** For WQRA and W0 alike, VAX3's records list granted LKBs before converting ones
  and converting before waiting.
- **PA6 (parent pair).** The waiting sub U and the converting sub V each carry body[36:40] = the
  holder's own root lkid of that process (SDA) and body[40:44] = the 82/01 answer body[20:24]
  for that root's record (the parent's handle on N), exactly as a granted SUB does (RA8 P8).
- **PA7 (queue placement).** SDA on VAX2 after the move: WQRA's conversion queue holds C's
  master copy (CR -> EX), its waiting queue holds Q's (EX), W0's conversion queue holds V's
  (CR -> EX) and its waiting queue holds U's (EX). None of the four is granted, and no process
  received SS$_DEADLOCK.
- **PA8.** No node bugchecks or logs LOCKMGRERR, CNXMGRERR or INVLOCKID; all three stay members.

Arm B (op-0x08):

- **PB1 (HR).** Every VAX3 LKB in WQRB, waiting and converting included, is sent as an op-0x08
  record inside the departure transition, after release 5 or 6 by the vms-2fa rule (VAX3's own
  LOCKDIRWT read before the arm). HR0 predicts no record for C, Q, U or V.
- **PB2.** The mode cells and the separating cell(s) of PA3/PA4 carry the same values in op-0x08
  as in op-0x12, LKB for LKB (C with C, Q with Q, U with U, V with V).
- **PB3.** SDA on the new master of WQRB after the transition shows C and V on conversion
  queues and Q and U on waiting queues, none granted. (VAX1's own CR has gone, but the survivors'
  CR and PR still block EX.)
- **PB4.** No bugcheck. VAX2 and VAX3 stay a 2-node cluster (quorum loss with VOTES 0 is
  expected, as in vms-2fa, and is not a failure).

## What the OVMX sender change would be if HR and Hx hold

`dlm_handover_fill` stops refusing `lock->waiting`. For a converting LKB it writes the
requested mode and the granted mode into the cells Hx names; for a waiting one it writes the
requested mode and the never-granted value measured in PA3; and it writes the state cell(s) PA4
names, read from the LKB's queue (granted / conversion / waiting), never as a constant. Records go
in the order PA5 measures. The op-0x08 path takes the same fill. If HR0 holds instead, OVMX's
skip is right, and rd vms-0db closes with the capture as evidence.

## What gets committed

`README.md` scoring every prediction (failures included), the trimmed pcaps (every 0x6007 frame
of each remaster or departure window), console logs with the SDA truth before and after, and a
census in `tools/cluster/` that pins the rule with `--selftest` and runs as a ctest. Every
`tools/cluster/*census*.py --selftest` is re-run, and only the populations these pcaps move are
re-pinned, each one named.

## Addendum 1, registered 2026-11-02T13:11Z, after run A0 and before arm A fires

**Run A0 (tree WQRA) was not captured, and is not scored.** The capture was to start at step 6.
WQRA moved from VAX1 to VAX2 during the setup itself, from VAX2's activity, before VAX1's holder
was stopped. SDA on VAX3 before step 6 already named VAX2 (CSID 00010002) as the master of every
VAX3 lock, including the converting root C and the waiting root Q. SDA on VAX2 then showed WQRA
mastered locally, with C (CR -> EX) on its conversion queue, Q (EX) on its waiting queue, and V
and U likewise on W0's queues. So a remaster did carry them, but whether C and Q were queued
before that move cannot be known without frames. The console logs are kept as `run-A0`.

**Arm A is re-run as tree WQRC, with the capture started before step 1.** Run A0 also exposed
that any new root request (VAX2's activity processes `RMH 312B`) would queue behind Q. So
`RMWAIT.MAR` gains a kind `A`, committed with this addendum: root at r and a sub A0 at s, both
granted at once; then a 120 s wait; then 3,000 rounds of A0 NL <-> s. Its root is granted before
C and Q queue, and its activity starts after they have. Step 3 becomes four
`RMW 31AWQRC` on VAX2, and step 4 (C, Q on VAX3) follows within the 120 s. The STOP of VAX1's
holder in step 6 is kept. Every prediction is unchanged, with WQRC for WQRA.

## Addendum 2, registered after arm A's first firing and before its second

**Arm A, first firing (WQRC, capture `0db-armA` running from step 1): the tree did not move.**
SDA on VAX3 before the STOP showed every VAX3 lock in WQRC mastered on VAX1 (CSID 00010001):
C (0A0003F2, CR -> EX) on the root's conversion queue, Q (070003F4, EX) on its waiting queue,
V (080003ED) and U (080003EF) on W0's. VAX1's holder was stopped at 13:13:53Z. VAX2's four
delayed-activity processes then sent all 12,000 conversions to VAX1 (02/06 and 02/07 x12,001
each in the capture). SDA on VAX3 at 13:17:27Z and 13:22:57Z still showed CSID 00010001, and
the capture holds no op-0x0e for WQRC. **PA1 fails on this firing: it is a measurement for HN.**

Run A0 (uncaptured) did move, with V and U already queued on W0. So the next firing separates
the root's queues from the sub's, in the same pod and the same tree, with only the root's
queues changed:

- STOP CC and CQ on VAX3, so WQRC's root has empty conversion and waiting queues. V and U stay
  queued on W0, and CH, CG and VAX2's CN1-4 stay granted.
- VAX2: four more `RMW 31AWQRC` (CM1-CM4), 120 s delay then 3,000 rounds each. Capture still on.

Predictions for this firing:

- **PA1b (HN, root queues):** WQRC now moves to VAX2 within 10 minutes of the activity
  starting. If it does not, HN is not about the root's queues, and the report says the trigger
  failed instead.
- **PA2b:** VAX3 sends 9 op-0x12 records (CH root, S0, S1; CG root, W0; CV root, W0; CU root,
  W0). The CV and CU W0 records are the converting and the waiting sub. PA3-PA8 are scored on
  them as registered, for the sub only.

## Addendum 3, registered after arm A's second firing and before its third

**Second firing: the tree did not move either, with the root's queues empty.** CC and CQ were
stopped at 13:23:22Z; SDA on VAX3 then showed Lock Count 4 and no conversion or waiting entry on
WQRC's root. CM1-CM4 sent 12,000 more conversions to VAX1 (24,001 02/06 in the capture by
13:31Z). SDA at 13:27:41Z and 13:32:30Z: still CSID 00010001. **PA1b fails.** So the first
firing's non-move does not ground HN: the trigger failed, as PA1b said it would read.

What differs from run A0, which did move: in A0, VAX1's own holder was still alive while VAX2's
activity ran. In both WQRC firings, VAX1's holder had been stopped before any activity began
(the STOP came first, the delayed activity 90 s later), so VAX1 kept mastering a tree in which it
holds nothing.

**Third firing, same tree and capture:** VAX1 takes WQRC again (`RMH 110-WQRC`, CO2, root CR),
then VAX2 starts four more delayed-activity processes (`RMW 31AWQRC`, CP1-CP4), then VAX3 queues
the root's converter and waiter again (`RMW 51CWQRC` CC2, `RMW 50QWQRC` CQ2). Nothing is stopped.
SDA on VAX3 before the activity starts. V and U are still queued on W0.

- **PA1c:** WQRC moves to VAX2 within 10 minutes of CP1-CP4's activity starting. If it moves,
  PA2-PA8 are scored on this firing as registered (VAX3's records: CH 3, CG 2, CV 2, CU 2, CC2 1,
  CQ2 1 = 11). If it does not, a fourth firing stops CC2 and CQ2 and starts four more
  delayed-activity processes with CO2 still alive: a move then is HN for the root's queues.
