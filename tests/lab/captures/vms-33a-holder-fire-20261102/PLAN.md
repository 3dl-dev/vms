# vms-33a + vms-bc51 lab fire: OVMX as the HOLDER when a real VAX tree moves to another VAX

Registered 2026-11-02 07:58Z, BEFORE the OVMX node boots. Pod `lab33a` (this
lane's own, reset at 07:54Z, deleted after). Build `lab/vms-33a-fire` `1f039561b`
= PR #1932 `ed80f81e6` (the holder sender; #1932's later head `7465e6d72` adds
only host_defects.sh mutation sets, no code) plus the lab-only RMHOLD staging.
Artifacts: `build-boot-artifacts.yml` run `41407426191`, `cluster_auth_group=1`.
`ovmx_fire.sh` gates `dlm_enq_req dlm_enq_req_sub dlm_enq_ans_queued
dlm_grant_notify dlm_deq dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14
dlm_rm_12 dlm_rm_13` (passed locally at 07:56Z, negative control refused).

Arrangement = vms-380 RA2 with OVMXE in VAX3's place: O = VAX1 (old master),
N = VAX2 (new master, by activity), H = OVMXE (holder). LOCKDIRWT as in the lab.

1. VAX1 `RMH 112WR33T`: VAX1 masters R33T (root CR, 2 CR subs), writes
   `RMLVB-0123456789`.
2. OVMXE `RMH 112-R33T`: root CR + 2 CR subs. P0: VAX1 `SDA> SHOW RESOURCE`
   on R33T lists MSTCPY locks for OVMXE's CSID.
3. VAX2 `RMH 114BR33T`: root CR + 4 subs, 3000 rounds of NL<->CR activity.
4. If the tree has not moved 120 s after step 3 starts, VAX1 `STOP` its holder
   (RA2: the move followed the master's holder stopping).
5. After the move: VAX2 SDA SHOW RESOURCE/NAME=R33T and the sub resources.
6. OVMXE stops its RMHOLD ($DEQ of all three locks).

Predictions, each scored on its own:
- H1 (vms-33a, the fire's point): VAX1 -> OVMXE op-0x0f whose body[28:32] is
  VAX2's CSID, then op-0x11; OVMXE -> VAX2 one op-0x12 per lock it holds in the
  tree, ROOT first, stop-and-wait; each answered 82/01 by VAX2; each SUB record's
  body[40:44] equals the body[20:24] of VAX2's 82/01 answer to OVMXE's root
  record; then OVMXE -> VAX2 op-0x13.
- H2: VAX2's SDA after the move shows R33T mastered on VAX2 (CSID 0) with
  OVMXE's three locks as MSTCPY, CR, under the lock ids VAX2's 82/01s carried,
  and the sub resources parented to the root.
- H3 (vms-bc51): VAX2 -> OVMXE op-0x14 is answered 82/15 by OVMXE; no OVMX
  console error; OVMX's subsequent behaviour (H4) shows it rehomed to VAX2.
- H4: step 6's $DEQs go to VAX2 addressed with VAX2's handles; no INVLOCKID on
  any console; VAX2's SDA afterwards has no OVMXE lock on R33T.
- H5: no bugcheck, CNXMGRERR, LOCKMGRERR or quorum-lost line on any console;
  CLUSTER_NODES 3 on both VAXes throughout; every DCL step returns.
- If the tree moves to OVMXE instead of VAX2 (RA7's role), H1-H4 are recorded as
  NOT EXERCISED and the arm is re-registered; it is not a pass.
A failed prediction is the result; it is not re-cut.

## Fire 1 result (08:00-08:10Z), recorded before fire 2
- P0 PASS. VAX1's SDA lists OVMXE's root as MSTCPY 1E0005E5, "Master copy of lock 01000386 on system 00010003", with Flags empty.
- H1 PASS. VAX1 -> OVMXE 0f (body[28:32] = VAX2's CSID 00010002) and 11. OVMXE -> VAX2 0x12 root, S0, S1 (subs body[40:44] = 01000411, VAX2's root handle), each answered fa, then 0x13.
- H2 PASS. VAX2's SDA shows 01000411/12/13 MSTCPY CR, subs parented to 01000411, "Master copy of lock 01000386/7/8 on system 00010003".
- H3 FAIL. VAX2 -> OVMXE 0x14 was never answered, and R33T stayed RM_IP/RM_WAIT on VAX2.
- H4/H5 FAIL. OVMXE's Ctrl-Y rundown sent 0x03 for root 01000411 first, then 0412 and 0413. VAX2: Fatal BUG CHECK LOCKMGRERR, R1=01000411. VAX2 was halted at sim> before an auto-reboot to SYS0.
- Fixes: #1932 (holder answers 0x14, ENDED slot) and #1935 / rd vms-4ebf (leaf-first release).

## Fire 2, registered before the OVMX node boots
Build `lab/vms-33a-fire2` = #1932 `22a6fdc8e` + #1935 fix + RMHOLD staging (run 41412779028). Pod reset. Same steps 1-6, tree R33U.
Predictions: H1, H2 and H5 as above, plus
- H3': OVMXE answers VAX2's 0x14 with 82/15 (echo, body[34] = fa), and VAX2's SDA status for R33U loses RM_IP/RM_WAIT.
- H4': OVMXE's rundown sends 0x03 for S0 and S1 BEFORE the root, all to VAX2 with VAX2's handles. No bugcheck. VAX2's SDA afterwards shows no OVMXE lock on R33U.
