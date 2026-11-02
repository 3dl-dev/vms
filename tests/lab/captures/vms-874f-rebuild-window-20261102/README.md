# vms-874f / vms-2fa: OVMX's op-0x08 rebuild records inside the barrier window, fired at real VAXes

`PLAN.md` was committed and pushed on `lab/vms-874f-window` (469cb4e7c) before the OVMX node
booted. It re-fires lab33a fire 4, the fire in which VAX2 bugchecked LOCKMGRERR because OVMX sent
its record after barrier release 12 (rd vms-bcb).

- **Build.** PR #1936 at 4e9204f8a (the rebuild window bound to the barrier), plus the LAB-ONLY
  RMHOLD staging commit, plus the plan. Artifacts came from run 41444215160
  (`cluster_auth_group=1`).
- **Gate.** `ovmx_fire.sh` ran over 25 frame types, including `dlm_rm_08`, `dlm_rm_08_ans` and
  `dlm_dir_ans_you`. Every byte was grounded.
- **Pod.** `lab874f`, with requests equal to limits, deleted after the fire.
- **Rebase.** The PR was later rebased onto main (d0417482f). The binding code is unchanged by
  the rebase.

## Topology, as read before firing

| node | LOCKDIRWT | VOTES | CSID | R (vms-2fa rule) |
|---|---|---|---|---|
| VAX1 | 1 | 1 | 00010001 | (departs) |
| VAX2 | 1 | 0 | 00010002 | 5 |
| OVMXE | 0 | 1 | 00010003 | 6 |

## Sequence (UTC, 2026-11-02)

1. **12:24.** VAX1 took `RMH 110WDEPT` and `RMH 110WDEPU`, so it masters both and wrote
   RMLVB-0123456789.
2. **12:44.** OVMXE took `SPAWN/NOWAIT @HU.COM`, holding DEPU at CR. The .COM file defines RMH
   itself, because SPAWN does not copy symbols (rd vms-d23). Then OVMXE ran `RMH 110-DEPT` in the
   foreground. VAX2 took `RMH 100-DEPU`.
3. **P0.** VAX1's SDA shows DEPT with 2 locks (VAX1, OVMXE) and DEPU with 3 locks. Both have Valblk
   RMLVB at Seqnum 1 and CSID 0, so VAX1 is local master.
4. **12:45:35.** VAX1 ran SHUTDOWN with REMOVE_NODE. **12:46:06.** VAX2 coordinated the removal
   transition (epoch 0x030e002c), with OVMXE as participant.
5. **12:47.** VAX2 ran `SPAWN RMH 500-DEPT` (EX), read SDA, then OVMXE pressed Ctrl-Y.

`transition-frames.txt` lists every SYSAP body of the removal transition. Its excerpt:

| frame | from > to | message |
|---|---|---|
| 5318 | OVMXE > VAX2 | 01/0b step 6 |
| 5320 | VAX2 > OVMXE | 01/0c release 6 |
| 5321 | OVMXE > VAX2 | 02/08 ROOT DEPU, lkid 01000390, CR/CR, RMLVB-0123456789 seq 1 |
| 5322 | OVMXE > VAX2 | 02/08 ROOT DEPT, lkid 01000391, CR/CR, RMLVB-0123456789 seq 1 |
| 5323 | VAX2 > OVMXE | 82/01 0xfa for DEPU, master lkid 0200057B, RMLVB |
| 5324 | VAX2 > OVMXE | 82/01 0xf9 for DEPT |
| 5325 | OVMXE > VAX2 | 01/0b step 7 |

`rebuild_step-score.txt` is the vms-2fa scorer (`rebuild_step.py`, PR #1943) run on the capture:
`participant LDW=0 n=2 rel 6..6 closed=True held=True`.

The wire capture is not committed. Its op-0x08 records are OVMX-originated, and the census tools
would count them as real senders' records. The text dump above stands in for it.

## Predictions

| | prediction | verdict |
|---|---|---|
| W1 | OVMXE (LDW 0) sends op-0x08 only after release 6 and before its own step 7; none before, none after step 7, none after release 12 | **PASS.** 2 of 2 records sit between frames 5320 and 5325, and no other op-0x08 from OVMXE appears in the capture |
| W2 | every record is answered before OVMXE's step 7 | **PASS.** Answers 5323 and 5324 precede step 7 at 5325 (scorer: closed, held) |
| D1 | one DEPT ROOT goes to VAX2, which answers 0xf9; OVMXE masters DEPT, keeps CR, sends no SUB | **PASS.** VAX2's SDA shows DEPT with CSID 00010003 (OVMXE) |
| D2 | VAX2's EX on DEPT waits while OVMXE holds CR | **PASS.** VAX2's SDA shows DEPT mastered on 00010003 with EX 0200050D on the waiting queue, and process EXT in LEF |
| D3 | after OVMXE's Ctrl-Y, VAX2's EX is granted | **PASS.** EXT went to HIB (RMHOLD's "holding"), and SDA shows EX 0200050D granted |
| D4 | VAX2 masters DEPU, holding VAX2's CR and OVMXE's CR | **PASS.** VAX2's SDA shows DEPU with CSID 0, 2 locks, and 0200057B, the handle its 0xfa gave OVMXE |
| V1 | OVMXE's ROOTs carry RMLVB-0123456789 with a non-zero sequence, and VAX2's SDA on DEPU shows RMLVB | **PASS.** Both records carry it at seq 1. VAX2's DEPU shows Valblk 31302D42 564C4D52 ... at Seqnum 2, status INVALID, the same status and seqnum the VAX-only vms-2fa arm 1 shows for a rebuilt root |
| D5 | no Fatal, CNXMGRERR, LOCKMGRERR or INVLOCKID; VAX2 keeps quorum; 2 nodes | **PASS.** No console carries one of those strings. VAX2 reads CLUSTER_NODES 2, QUORUM 1, VOTES 1 |

## Finding, not a registered prediction

DEPT's value block did not survive OVMXE becoming its master by 0xf9. OVMXE's own proxy, and the
record it sent, carried RMLVB at seq 1. VAX2's EX on DEPT was later granted by OVMXE with a zero
block at seq 0. The adoption on the 0xf9 path does not seed the RSB from the holder's proxy pair.
This is filed as rd vms-427. It is not a crash and not a wire-shape defect.

The op-0x12 change from vms-8159 (relaying the last grant's block when there was no op-0x1a) was
not exercised: no remaster took place in this fire.
