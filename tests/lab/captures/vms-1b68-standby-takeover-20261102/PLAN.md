# vms-1b68: the OVMX standby's queued $ENQ is granted after the workload VAX leaves

Registered on `lab/vms-1b68` and pushed before any OVMX node boots.

## Build
lab/vms-1b68 = origin/work/vms-1b68-standby-takeover (4f000fad5: 1e263e14e vms-0db codec + 4f000fad5
vms-1b68) + LAB-ONLY RMHOLD staging (cherry-pick of 25b352c1e) + LAB-ONLY EVACWL staging (same recipe
as the evac/int staging commit 00e1f610b) + this plan. build-boot-artifacts with cluster_auth_group=1.
Gate: ovmx_fire.sh (OVMX_FIRE_GROUP=1) over every frame type this fire can send, including dlm_rm_08,
dlm_rm_08_ans, dlm_dir_ans_you, dlm_enq_ans_queued and dlm_grant_notify. Refuse to fire if it fails.

## Topology (lab93r.yaml copy, pod lab1b68, requests == limits)
VAX1 (LOCKDIRWT 1), VAX2 (LOCKDIRWT 1), OVMXE (LOCKDIRWT 0). VOTES/EXPECTED_VOTES/CSID of both VAXes are
read with F$GETSYI before OVMXE boots and committed as an addendum; OVMXE's votes/expected are chosen so
that the survivors (VAX2 + OVMXE) stay quorate after VAX1 leaves.

## Arms
- A (waiter): VAX1 `RMH 500-WKLD` (root EX, no subs, VAX1 is first requester so VAX1 masters WKLD).
  OVMXE `RMH 500-WKLD` (fresh EX, queued at VAX1 behind VAX1's EX).
  (The draft's `150-WKLD` was a typo: digit 1 is the ROOT mode, so it would be CR and not queue. The
  arm's intent, a fresh EX waiter, is `500-`.)
- B (converter = EVACWL STANDBY shape): resource EVAC$WORKLOAD. VAX1 `EVACWL` direct (EX, VAX1 masters;
  EVAC$DATA defined to a local directory). OVMXE `EVACWL STANDBY` (NL granted, then $ENQW convert to EX,
  which waits behind VAX1's EX).
- Both arms are set up together before one departure: VAX1 `SHUTDOWN` (0, reason, NO, NO, NO, later,
  REMOVE_NODE). br0 is captured throughout.

## Predictions (cells per the vms-0db verdict, PR #1956)
- P1a (arm A waiter): OVMXE sends one op-0x08 ROOT for WKLD inside its rebuild window (after release 6,
  its LOCKDIRWT being 0, and before its own step 7) with [30]=EX(05) [31]=00 [110]=ff [108]=04, read off
  its LKB; it is answered (0xf9 or 0xfa) before step 7.
- P1b (arm B converter): OVMXE sends one op-0x08 ROOT for EVAC$WORKLOAD with [30]=EX(05) [31]=NL(00 on
  the wire is NL's encoding as written by the codec) [110]=00 [108]=04 and [28] carrying CONVERT (02);
  answered before step 7.
- P2. VAX2 answers 0xf9 for both (no survivor holds either); OVMXE masters both, and grants its own
  requests at rebuild window close: RMH on OVMXE prints its WKLD lock granted EX, and EVACWL on OVMXE
  prints "holding EVAC$WORKLOAD" / "took EVAC$WORKLOAD EX".
- P3. After takeover, a VAX2 `RMH 500-WKLD` (spawned) waits (LEF) while OVMXE holds EX; VAX2's SDA shows
  WKLD mastered on OVMXE's CSID with VAX2's EX on the waiting queue.
- P4. No Fatal / CNXMGRERR / LOCKMGRERR / INVLOCKID on any console; VAX2 never shows quorum loss; OVMXE
  prints no "quorum lost".

STOP RULE: any VAX bugcheck -> stop firing, keep console + pcap, identify the frame, report; no re-fire.
Pcaps carrying OVMX-originated op-0x08/0x12 records are NOT committed (census tools would count them).

## Addendum (2026-11-02, before any OVMX node boots)
Rebased onto work/vms-1b68-standby-takeover 7eb359853, which answers GATE-REFUSAL.md: frame-grounding
rows for dlm_rm_08/dlm_rm_12/dlm_rm_08_ans select on the queue the emitting test reads off the LKB
(st:queue=01/00/ff): [30] requested, [31] granted, [108] 04 and [110] 00/ff on converting/waiting
records; test_dlm_rebuild dumps a waiting and a converting record. Predictions unchanged. Pod manifest
requests 200m CPU, limit 4; the CPU the fire actually got is recorded with the results.

## Addendum 2 (F$GETSYI on both VAXes, before OVMXE boots)
VAX1 VOTES 1 EXPECTED_VOTES 1 LOCKDIRWT 1; VAX2 VOTES 0 EXPECTED_VOTES 1 LOCKDIRWT 1; CLUSTER_VOTES 1,
CLUSTER_QUORUM 1. (NODE_CSID read without a node argument returned the same value on both and is not
recorded.) OVMXE boots VOTES 1 (ovmx_fire_node.sh), EXPECTED_VOTES 1 (OVMX_FIRE_EXPVOTES=1), so quorum
stays 1 and VAX2 + OVMXE (1 vote) stay quorate after VAX1's REMOVE_NODE departure.

## Addendum 3 (16:1xZ, arms set, BEFORE the departure)
- Arm A runs on resource WKL2, not WKLD. WKLD was invalidated during setup: a console ^Y on VAX1
  ended VAX1's holders, WKLD remastered to OVMXE, OVMXE's holder was then stopped, and VAX2's
  directory kept naming OVMXE. VAX1's next $ENQ was redirected to OVMXE, answered 0xf9, and VAX1
  mastered it locally; OVMXE's later $ENQ got "you master it" from VAX2. Result: two EX holders,
  one per master (SDA on VAX1: WKLD EX granted, empty queues, while OVMXE's RMHOLD held EX).
  Filed separately; the pcap stays out of the repo.
- SDA on VAX1 before departure: WKL2 CSID 0 (VAX1 masters), DIRENTR, granted 5300067C EX, waiting
  1A00067F EX (OVMXE). EVAC$WORKLOAD CSID 0, DIRENTR, granted 360004FA EX, conversion 7C000679 NL->EX
  (OVMXE's EVACWL STANDBY).
- Departure: VAX1 is HALTED at the SIMH console (^E, left at sim>), not SHUTDOWN. SHUTDOWN stops
  VAX1's user processes first, so VAX1 itself would grant OVMXE's waiter and converter before it
  left, and the departure would carry no queued lock. The item's case is a master that leaves
  holding its locks. Predictions P1a, P1b, P2, P4 are unchanged; the survivors' directory/master
  is whichever node the rebuild picks (VAX2 or OVMXE). P3 is unchanged.

## Addendum 4 (re-fire, before any OVMX node boots; supersedes addenda 2 and 3 for this fire)
Rebased onto #1961 65e34db74 (main + vms-0db/vms-1b68 sender + OVMX_FIRE_VOTES). Fresh pod, CPU
request 200m, limit 4; the CPU the fire actually used is recorded with the results.

Votes. The first fire lost quorum (VAX1 1 + OVMXE 1 = 2, quorum 2; halting VAX1 left 1). This fire
boots OVMXE with OVMX_FIRE_VOTES=2 and OVMX_FIRE_EXPVOTES=3: VAX1 1 + VAX2 0 + OVMXE 2 = 3, quorum 2,
and VAX2 + OVMXE keep 2 after VAX1 leaves. Re-read VOTES/EXPECTED_VOTES with F$GETSYI on both VAXes
and CLUSTER_VOTES/CLUSTER_QUORUM after OVMXE joins, and commit them as addendum 5 before the arms.

Departure shape: VAX1 `SHUTDOWN` with REMOVE_NODE -- the shape of the real capture in
tests/lab/captures/vms-0db-waiting-records-20261102 arm B (VAX1 the master leaves by SHUTDOWN
REMOVE_NODE; the survivors' converting and waiting records were rebuilt by op-0x08, 14:01:03Z).
Addendum 3's objection to SHUTDOWN (VAX1's own holder is stopped first and would grant the queue)
is answered by putting the BLOCKING holders on VAX2, a survivor, as arm B of that capture did.
VAX1 holds only an NL lock that makes it the first requester and so the master.

- Arm A (waiter), resource WKL3 (LCK$M_SYSTEM): VAX1 `RMH 000-WKL3` (NL, first, masters);
  VAX2 `RMH 300-WKL3` (PR granted); OVMXE `RMH 500-WKL3` (EX, waits behind VAX2's PR).
- Arm B (converter), resource EVAC$WORKLOAD (group 0, no LCK$M_SYSTEM, so RMH cannot stand in):
  VAX1 `EVACWL STANDBY NL=99999` (NL, first, masters, never converts during the fire); VAX2
  `EVACWL` (EX granted); OVMXE `EVACWL STANDBY` (NL granted, convert NL->EX waits behind VAX2).
- SDA on VAX1 before SHUTDOWN shows both resources mastered on VAX1 with OVMXE's waiting EX and
  converting NL->EX queued. If VAX1's shutdown remasters a tree off VAX1 before the departure
  (op-0x12, as the capture notes), record which; OVMXE's queued record then travels in op-0x12.

Predictions (cells per vms-0db):
- P1. OVMXE's waiter and converter each go out in exactly one holder record (op-0x08 in the
  departure rebuild, or op-0x12 if remastered first) with [30]=05, [31]=00, [110]=ff (waiter) /
  00 (converter, [28] carries 02), [108]=04, read off its LKB; each is answered (0xf9 or 0xfa).
- P2. After the departure both stay QUEUED (VAX2 still holds PR / EX): no SS$_UNSUPPORTED, no
  grant. SDA on VAX2 shows each resource's new master with OVMXE's requests on the waiting /
  conversion queue.
- P3. STOP/ID VAX2's two holders: OVMXE's RMH prints WKL3 granted EX, and EVACWL on OVMXE prints
  that it took EVAC$WORKLOAD EX.
- P4. No Fatal / CNXMGRERR / LOCKMGRERR / INVLOCKID on any console; neither survivor shows
  "quorum lost".

STOP RULE unchanged: any VAX bugcheck -> stop, keep console + pcap, identify the frame, report.

## Addendum 5 (17:1xZ, OVMXE joined, BEFORE the arms)
Pod lab1b68r (fresh, CPU request 200m / limit 4), OVMXE built from 5849a3396 (SOURCE_COMMIT in
/lab/art-r1b68). F$GETSYI on each node after OVMXE joined (via VAX2, 16:50:50Z):
- VAX1:  VOTES=1 EXPECTED_VOTES=1 CLUSTER_VOTES=3 CLUSTER_QUORUM=2 CLUSTER_NODES=3
- VAX2:  VOTES=0 EXPECTED_VOTES=1 CLUSTER_VOTES=3 CLUSTER_QUORUM=2 CLUSTER_NODES=3
- OVMXE: VOTES=2 EXPECTED_VOTES=3 CLUSTER_VOTES=3 CLUSTER_QUORUM=2 CLUSTER_NODES=3
VAX1's departure leaves VAX2 0 + OVMXE 2 = 2 >= quorum 2. Arms proceed as addendum 4.
