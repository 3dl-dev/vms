# vms-93fa: a real VAX departs with a tree OVMX is chosen to master

**Registered 2026-11-02, before the pod for this arm exists.**

## The question

When a real VAX that masters a lock tree shuts down with `REMOVE_NODE` and
OVMX is the node the selection rule picks, does the departure complete? And
does the tree keep working for the surviving VAX afterwards, served by OVMX?

The selection rule (rd vms-50d, measured 2026-10-10 E1-E3): the new master is
chosen among the nodes currently holding locks on the tree, highest
`LOCKDIRWT`, tie to the earliest joiner. A shutdown remastered 273-293 trees in
those runs. RA7 (rd vms-6ad1, #1916) proved the activity path hands OVMX a
tree, and that the op-0x14 OVMX sends is safe in the old-master and directory
roles. This arm uses the **shutdown** path, which RA7 did not.

## Build, bed, gate

- Same build as RA7: `lab/vms-6ad1-ra7` `ef0b7e3fc` (#1900's tree plus the
  lab-only RMHOLD staging), artifacts from run `41352991717`.
- Own pod `lab93fa`, `requests == limits` 1500m/2Gi, reset before the arm,
  deleted after.
- `ovmx_fire.sh` with `dlm_enq_req dlm_enq_ans_queued dlm_grant_notify dlm_deq
  dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14`.
- `LOCKDIRWT` stays as in RA7: VAXes 1, OVMXE 0.

## The arm (arm 1)

| # | node | step |
|---|---|---|
| 1 | VAX1 | `RMHOLD 123WDEPT` (process `HOLD1`): VAX1 masters `DEPT` |
| 2 | OVMXE | `RMHOLD 100-DEPT`. **P0:** VAX1's SDA lists a master copy of OVMXE's lkid on system `00010003` |
| 3 | VAX1 | `@SYS$SYSTEM:SHUTDOWN`: 0 minutes, no disk spin-down, no automatic reboot, option `REMOVE_NODE`. `HOLD1` dies with the shutdown, so the only holder left on `DEPT` is OVMXE |
| 4 | VAX2 | after VAX1 is gone, `RMHOLD 100-DEPT` (process `HOLD2`) |
| 5 | VAX2 | SDA `SHOW RESOURCE/NAME=DEPT`, then `STOP HOLD2` |
| 6 | OVMXE | `^Y` to end the holder |

VAX1 is **not** rebooted afterwards. The pod is deleted.

VAX2 holds nothing on `DEPT` at the departure. That is deliberate: with
`LOCKDIRWT` 1 on VAX2 and 0 on OVMXE, a holding VAX2 would win the selection.
The holder role (op-0x12 to OVMX) is not this arm.

## Predictions, each scored on its own

- **P1. OVMX is chosen and adopts.** During VAX1's shutdown the capture shows
  `DEPT` going VAX1→OVMXE as `0e`, `0f`, `10`, each answered `82/15` by OVMXE;
  `0x13`s; and OVMXE's `0x14`, each answered `82/15`. Rules W1/W2 hold.
- **P2. The answers are the real ones.** `score_remaster` is byte-exact on
  every pair.
- **P3. The departure completes.** VAX1's console reaches `SYSTEM SHUTDOWN
  COMPLETE`. VAX2 logs VAX1's removal, and `CLUSTER_NODES` reads 2 on VAX2.
  No CNXMAN request to OVMXE is left unanswered in the capture.
- **P4. The tree is OVMX's afterwards.** VAX2's directory entry for `DEPT`
  names `CSID 00010003`.
- **P5. OVMX serves the survivor.** Step 4 is granted CR from OVMXE (`HOLD2`
  in `HIB`; VAX2 SDA: master `00010003`, its lock granted). `STOP HOLD2`
  completes.
- **P6. No bugcheck, no hang.** Zero bugcheck lines on VAX2's and OVMXE's
  consoles, and on VAX1's up to SHUTDOWN COMPLETE. Every DCL step on VAX2
  returns.

## FAIL / STOP

- A bugcheck on any node is a win for the lab, and it is a release-gating
  defect: capture it and open its own item before anything else moves.
- If the shutdown hangs waiting on OVMX (any remaster or CM frame to OVMXE left
  unanswered), that is the arm's result. Record what was waited on.
- If some other node is chosen instead of OVMX, record it. The selection rule
  is then wrong as written. Do not re-arrange the arm to force OVMX.
- A prediction that fails is the result, not a reason to re-cut it.
