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
