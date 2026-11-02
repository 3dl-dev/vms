# vms-93fa re-fire on merged #1936, plus the vms-427 re-proof (PR #1952)

Registered and pushed on `lab/vms-93fa-refire` BEFORE the pod exists and before the OVMX node boots.

## Why

vms-93fa's re-registered arm (a) passed on lab874f (`vms-874f-rebuild-window-20261102`) on #1936's
pre-merge head. This re-fire runs the same arm on the code as merged (main 76a5f0f69). PR #1952
(vms-427: a holder told 0xf9 installs its proxy's value block on the RSB it adopts, and the
deferred op-0x02 grant carries the block at body[36:56]) is on top. The arm is lab874f's,
unchanged, so W1-W2/D1-D5/V1 re-score the merged sender and the added Q1/Q2 score vms-427.

## Build

`lab/vms-93fa-refire` = origin/main 76a5f0f69 + #1952 (52e54afc7) + LAB-ONLY RMHOLD staging + this
plan. Artifacts come from `build-boot-artifacts` with `cluster_auth_group=1`. Gate: `ovmx_fire.sh`
with every type the fire can send, including `dlm_rm_08 dlm_rm_08_ans dlm_dir_ans_you
dlm_grant_notify`.

## Topology (lab874f's, unchanged)

Own pod `lab93r` (copy of lab874f.yaml, requests == limits), with VAX1, VAX2 (OpenVMS VAX V7.3
golden) and OVMXE (`LOCKDIRWT=0`). Each node's LOCKDIRWT, VOTES and CSID are read with F$GETSYI
before firing. If they differ from lab874f's (VAX1 1/1, VAX2 1/0, OVMXE 0/1), every prediction is
re-scored against what was read.

## Steps (lab874f's)

1. VAX1: `SPAWN/NOWAIT/PROCESS=HOLDT RMH 110WDEPT` and `.../PROCESS=HOLDU RMH 110WDEPU`. VAX1 masters
   both and writes RMLVB-0123456789.
2. OVMXE: `CREATE HU.COM` (`RMH :== $SYS$SYSTEM:RMHOLD` / `RMH 110-DEPU`), then
   `SPAWN/NOWAIT/PROCESS=HOLDE @HU.COM`, then `RMH 110-DEPT` in the foreground.
3. VAX2: `SPAWN/NOWAIT/PROCESS=HOLDU2 RMH 100-DEPU`.
4. P0: VAX1's SDA shows OVMXE's master copy on DEPT and on DEPU.
5. Capture br0 throughout. VAX1 runs SHUTDOWN with REMOVE_NODE.
6. After the transition, VAX2 runs `SPAWN/NOWAIT/PROCESS=EXT RMH 500-DEPT` (EX, LCK$M_VALBLK).
7. VAX2: SDA `SHOW RESOURCE/NAME=DEPT` and `/NAME=DEPU`. Then OVMXE presses Ctrl-Y, and VAX2 runs SDA
   on DEPT again after EXT reports "holding".

## Predictions

- **W1.** OVMXE (LDW 0) sends op-0x08 only after the coordinator's release 6 and before its own
  step 7. None goes out before, after step 7, or after release 12.
- **W2.** Every OVMXE op-0x08 is answered (82/01) before OVMXE's step-7 report.
- **D1.** One DEPT ROOT goes to VAX2, which answers 0xf9. OVMXE masters DEPT and keeps its CR.
- **D2.** VAX2's EX on DEPT waits while OVMXE holds CR. SDA shows DEPT mastered on OVMXE's CSID.
- **D3.** After OVMXE's Ctrl-Y, VAX2's EX is granted (EXT prints "holding").
- **D4.** VAX2 masters DEPU, holding VAX2's CR and OVMXE's CR, and OVMXE's DEPU ROOT is answered
  0xfa.
- **V1.** OVMXE's DEPT and DEPU ROOTs carry RMLVB-0123456789 with a non-zero sequence.
- **Q1 (vms-427).** The op-0x02 deferred grant OVMXE sends VAX2 for EXT's EX on DEPT carries
  RMLVB-0123456789 at body[36:52] and the sequence OVMXE's proxy was granted (1) at body[52:56].
  On lab874f the same frame carried zeros.
- **Q2 (vms-427).** After the grant, VAX2's SDA on DEPT shows Valblk 31302D42 564C4D52 ...
  (RMLVB-0123456789), not zero.
- **D5.** No console shows Fatal, CNXMGRERR, LOCKMGRERR or INVLOCKID. VAX2 keeps quorum, with
  CLUSTER_NODES 2 after the departure.

## Stop rule

If any VAX bugchecks, firing stops. The console and the pcap are kept. Nothing merges, and #1952 is
dequeued if it is queued. The cause is found from the frames, and a fuzz or host negative test
for it is added before any re-fire.
