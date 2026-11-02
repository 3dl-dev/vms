# vms-874f / vms-93fa (re-registered arm a) / vms-2fa: the rebuild window, fired at real VAXes

Registered and pushed BEFORE the OVMX node boots. Re-fire of lab33a fire 4 (09:41Z, PLAN-93fa,
VAX2 bugchecked LOCKMGRERR on OVMX's op-0x08 ROOT sent after barrier release 12, rd vms-bcb).

## Build

`lab/vms-874f-window` = PR #1936 head (rebuild window bound to the barrier, rd vms-2fa) + the
RMHOLD staging commit (LAB-ONLY) + this plan. Artifacts from `build-boot-artifacts` with
`cluster_auth_group=1`. Gate: `ovmx_fire.sh` with every type this fire can send, including
`dlm_rm_08 dlm_rm_08_ans dlm_dir_ans_you`.

## Topology (lab33a's, unchanged)

Pod `lab874f` (copy of lab33a.yaml, requests == limits): VAX1, VAX2 (OpenVMS VAX V7.3 golden),
OVMXE. Read with F$GETSYI at 12:4xZ before this plan was committed: VAX1 LOCKDIRWT 1 VOTES 1
CSID 00010001; VAX2 LOCKDIRWT 1 VOTES 0 CSID 00010002; CLUSTER_NODES 2. OVMXE boots with
`LOCKDIRWT=0`, so its own rule is R = 6; VAX2's (weighted) is R = 5. After VAX1 leaves, VAX2 is
the only directory node, so it directs both DEPT and DEPU.

## Steps

1. VAX1: `SPAWN/NOWAIT/PROCESS=HOLDT RMH 110WDEPT` and `.../PROCESS=HOLDU RMH 110WDEPU` (VAX1
   masters both and writes RMLVB-0123456789).
2. OVMXE: a subprocess holding DEPU at CR (`SPAWN/NOWAIT @HU.COM`, which defines RMH itself
   because SPAWN does not copy symbols, rd vms-d23), then `RMH 110-DEPT` in the foreground.
3. VAX2: `SPAWN/NOWAIT/PROCESS=HOLDU2 RMH 100-DEPU`.
4. P0: VAX1 SDA shows OVMXE's master copy on DEPT (and DEPU).
5. VAX1: SHUTDOWN with REMOVE_NODE. Capture br0 throughout.
6. After the transition: VAX2 `SPAWN/NOWAIT/PROCESS=EXT RMH 500-DEPT` (EX).
7. VAX2 SDA SHOW RESOURCE DEPT / DEPU; OVMXE Ctrl-Y; VAX2's EX must then be granted.

## Predictions

- **W1 (the window, rd vms-2fa).** In VAX1's removal transition OVMXE (LOCKDIRWT 0) sends its
  op-0x08 records only after the coordinator's release of step 6 (cat-0x01 op-0x0c, body[16]=6)
  and before OVMXE's own 01/0b step 7. None before release 6, none after step 7, none after
  release 12.
- **W2 (held step).** Every OVMXE op-0x08 is answered (82/01) before OVMXE's step-7 report goes
  out; OVMXE's step 7 follows the last answer.
- **D1.** OVMXE sends one op-0x08 ROOT for DEPT to VAX2 (DEPT's directory); VAX2 answers 0xf9
  (no entry: arm 3b's shape) and OVMXE masters DEPT, keeping its CR. No SUB (0 subs).
- **D2.** VAX2's `RMH 500-DEPT` waits (no "holding" line) while OVMXE holds CR; VAX2 SDA shows
  DEPT mastered on OVMXE with VAX2's EX waiting.
- **D3.** After OVMXE's Ctrl-Y, VAX2's RMHOLD prints "holding" (EX granted).
- **D4 (DEPU, two survivor holders).** VAX2 (weighted, R=5) rebuilds DEPU first and masters it;
  OVMXE's DEPU ROOT after release 6 is answered 0xfa; VAX2 SDA shows DEPU mastered on VAX2 with
  VAX2's CR and OVMXE's CR.
- **V1 (vms-8159, the value block carried).** OVMXE's DEPT and DEPU ROOT records carry
  RMLVB-0123456789 with a non-zero sequence at body[88:108]; VAX2's SDA on DEPU shows Valblk
  RMLVB-0123456789 (bytes 31302D42 564C4D52 ...).
- **D5.** No Fatal / CNXMGRERR / LOCKMGRERR / INVLOCKID on any console; VAX2 keeps quorum;
  CLUSTER_NODES 2 on VAX2 after the departure.

## Stop rule

If any VAX bugchecks: stop firing, keep console + pcap, root-cause from the frames, add a fuzz or
host negative test for the cause before any re-fire.
