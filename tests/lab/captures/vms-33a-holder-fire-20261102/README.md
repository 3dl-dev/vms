# vms-33a / vms-bc51 lab fires: OVMX as the HOLDER when a real VAX tree moves to another VAX

Pod `lab33a` (this lane's own; deleted after). Arrangement = vms-380 RA2 with OVMXE
(CSID 00010003) in VAX3's place: O = VAX1 (00010001) masters the tree, N = VAX2
(00010002) takes it over through `RMHOLD 114B` activity, and H = OVMXE holds root CR
plus S0 and S1 CR. The predictions in [`PLAN.md`](PLAN.md) were written before each
OVMX node booted. Both fires passed `ovmx_fire.sh`'s gate (`dlm_rm_12 dlm_rm_13
dlm_rm_ack ...`: every byte grounded, negative control refused).

## Fire 1 (08:00-08:10Z, build `1f039561b` = this PR's `ed80f81e6` + RMHOLD staging): a real VAX BUGCHECKED

| prediction | result |
|---|---|
| P0: VAX1 has a master copy of OVMXE's root | PASS: `1E0005E5 ... MSTCPY`, "Master copy of lock 01000386 on system 00010003", Flags empty |
| H1: 0f names N, then 11; OVMXE sends 0x12 root first, then subs carrying the root's new handle, each answered, then 0x13 | PASS: 0f body[28:32] = 00010002. Root, S0 and S1 records were answered fa 01000411/12/13. The subs' body[40:44] = 01000411 |
| H2: N's SDA shows OVMXE's locks under N's handles | PASS: 01000411/12/13 MSTCPY CR, subs parented to 01000411 |
| H3: OVMXE answers N's 0x14 | **FAIL**: no 82/15. R33T stayed `RM_IP RMACCPT RM_RBLD RM_WAIT` on VAX2 |
| H4/H5: the release reaches N; no bugcheck | **FAIL**: rundown sent 0x03 for the ROOT first. VAX2 hit `Fatal BUG CHECK ... LOCKMGRERR`, R1 = 01000411 (`fire1-console-vax2.log.gz`) |

Fixes: this PR (a holder answers the new master's 0x14), and rd vms-4ebf / #1935 (release
leaf-first: 552 of 552 real sub-locks go before their parent).

## Fire 2 (08:47-08:49Z, build `9d93c92b0` = `22a6fdc8e` + #1935 + RMHOLD staging, run 41412779028): PASS

The frames are in `fire2-r33u-frames.txt`, decoded from the br0 capture (`/lab/run-fire/r33b/wire.pcap`
on the PVC). The capture itself is not committed: its OVMX-originated frames would move the
real-sender census pins.

| prediction | result |
|---|---|
| P0 | PASS: VAX1's R33U has 2 granted locks, CSID 0 |
| H1 | PASS: VAX1 -> OVMXE 0f, 11. OVMXE -> VAX2 0x12 R33U (answered fa 030003B8), S0 and S1 (body[40:44] = 030003B8, answered 010003BA/BB), then 0x13 |
| H3' | PASS: VAX2 -> OVMXE 0x14, then OVMXE -> VAX2 82/15 (echo, body[34] = fa). VAX2's R33U: `Status: DIRENTR VALID`, CSID 0, OVMXE's 030003B8 granted CR, with no RM_* flags |
| H4' | PASS: Ctrl-Y rundown sent 0x03 for 010003BA and 010003BB first, then 030003B8, all to VAX2 under VAX2's handles. VAX2's R33U then has only its own two locks |
| H5 | PASS: no Fatal / CNXMGRERR / LOCKMGRERR / INVLOCKID / quorum-lost line on any console. `F$GETSYI("CLUSTER_NODES")` = 3 on VAX1 and VAX2 |
