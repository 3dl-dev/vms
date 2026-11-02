# vms-93fa re-fire on merged #1936, plus the vms-427 re-proof (PR #1952): result

`PLAN.md` was committed and pushed on `lab/vms-93fa-refire` (e31350a70) before the pod existed.
The arm is lab874f's (`vms-874f-rebuild-window-20261102`), unchanged.

- **Build.** e31350a70 = origin/main 76a5f0f69 + #1952 (52e54afc7) + LAB-ONLY RMHOLD staging + the
  plan. Artifacts came from `build-boot-artifacts` run 41456254961 (`cluster_auth_group=1`),
  SOURCE_COMMIT e31350a70f41f3e9b46696ae4b56cef0afbd73a7.
- **Gate.** `ovmx_fire.sh` (`OVMX_FIRE_GROUP=1`, `LOCKDIRWT=0`) over lab874f's 25 frame types plus
  `dlm_grant_notify`. The gate build was made from the same commit. Every byte was grounded, the
  negative control was refused, and the initramfs carried cluster group 1.
- **Pod.** `lab93r` (copy of lab874f.yaml, requests == limits), deleted after the fire.
- **Holder.** `RMHOLD.MAR` from `vms-50d-ra5-14cells-20261013/` (md5 71c9f3b4...) was put on each
  VAX by FTP (that directory's `vput.py`, run in the pod), then `MACRO`, `LINK`, and `COPY` into
  `SYS$COMMON:[SYSEXE]`, as the consoles show.

## Topology, as read before firing

| node | LOCKDIRWT | VOTES | CSID |
|---|---|---|---|
| VAX1 | 1 | 1 | 00010001 |
| VAX2 | 1 | 0 | 00010002 |
| OVMXE | 0 | 1 | 00010003 (F$GETSYI on OVMXE reads 0; 00010003 is the CSID VAX2's SDA gives OVMXE's RSB) |

This is lab874f's topology, so the predictions are scored as written.

## Sequence (UTC, 2026-11-02)

1. **14:15.** VAX1 ran `RMH 110WDEPT` and `RMH 110WDEPU` (HOLDT and HOLDU, both HIB).
2. **14:18.** OVMXE joined. It ran `SPAWN/NOWAIT/PROCESS=HOLDE @HU.COM` (DEPU CR, lkid 0100040F),
   then `RMH 110-DEPT` in the foreground (lkid 01000410). VAX2 ran `RMH 100-DEPU` (HOLDU2).
3. **P0.** VAX1's SDA showed DEPT with 2 locks and DEPU with 3, both with Valblk RMLVB at Seqnum 1
   and CSID 0, so VAX1 was local master.
4. **14:20:00.** VAX1 ran SHUTDOWN with REMOVE_NODE. **14:20:25.2.** VAX2 coordinated the removal
   transition (epoch 0x030e002c, frames 8064-8139), with OVMXE as participant.
5. **14:23:34.** VAX2 ran `SPAWN/NOWAIT/PROCESS=EXT RMH 500-DEPT` and read SDA. **14:23:45.**
   OVMXE got Ctrl-Y. VAX2 read SDA on DEPT again after EXT went to HIB.

`transition-frames.txt` lists every SYSAP body of the transition and the DEPT grant exchange.
`op02-dept-grant.txt` is the vms-427 frame. `rebuild_step-score.txt` is the vms-2fa scorer:
`participant LDW=0 n=2 rel 6..6 closed=True held=True`. As with lab874f, the pcap is not
committed: its op-0x08 records come from OVMX and would be counted as real senders' records.

| frame | from > to | message |
|---|---|---|
| 8100 | OVMXE > VAX2 | 01/0b step 6 |
| 8102 | VAX2 > OVMXE | 01/0c release 6 |
| 8105 | OVMXE > VAX2 | 02/08 ROOT DEPU, lkid 0100040F, CR/CR, RMLVB-0123456789 seq 1 |
| 8106 | VAX2 > OVMXE | 82/01 0xfa for DEPU, master lkid 0200057B |
| 8107 | OVMXE > VAX2 | 02/08 ROOT DEPT, lkid 01000410, CR/CR, RMLVB-0123456789 seq 1 |
| 8109 | VAX2 > OVMXE | 82/01 0xf9 for DEPT |
| 8111 | OVMXE > VAX2 | 01/0b step 7 |
| 8735 | VAX2 > OVMXE | 02/01 DEPT EX request, VAX2 lkid 0600055E |
| 8737 | OVMXE > VAX2 | 82/01 0xfb (queued) |
| 8765 | OVMXE > VAX2 | 02/02 grant, body[36:52] RMLVB-0123456789, body[52:56] 01000000 |

## Predictions

| | prediction | verdict |
|---|---|---|
| W1 | OVMXE (LDW 0) sends op-0x08 only after release 6 and before its own step 7 | **PASS.** The capture holds 2 OVMXE op-0x08 records (8105 and 8107), both between release 6 (8102) and step 7 (8111) |
| W2 | every op-0x08 is answered before OVMXE's step 7 | **PASS.** Answers at 8106 and 8109 come before 8111. The scorer reports closed and held |
| D1 | one DEPT ROOT goes to VAX2, which answers 0xf9; OVMXE masters DEPT and keeps CR | **PASS.** One DEPT ROOT, answered 0xf9 at 8109. VAX2's SDA shows DEPT with CSID 00010003 |
| D2 | VAX2's EX on DEPT waits while OVMXE holds CR; DEPT mastered on OVMXE's CSID | **PASS.** SDA shows CSID 00010003 with EX 0600055E on the waiting queue. EXT is in LEF, and OVMXE answered 0xfb (queued) |
| D3 | after OVMXE's Ctrl-Y, VAX2's EX is granted | **PASS.** EXT went to HIB, and SDA shows EX 0600055E on the granted queue |
| D4 | VAX2 masters DEPU, holding VAX2's CR and OVMXE's CR; DEPU ROOT answered 0xfa | **PASS.** 0xfa at 8106 gave master lkid 0200057B. SDA shows DEPU with CSID 0, 2 locks: 0200057B and VAX2's 220000CC |
| V1 | OVMXE's DEPT and DEPU ROOTs carry RMLVB-0123456789 with a non-zero sequence | **PASS.** Both records carry it at seq 1. VAX2's DEPU shows Valblk RMLVB at Seqnum 2 |
| Q1 | OVMXE's op-0x02 for EXT's EX on DEPT carries RMLVB at body[36:52] and seq 1 at body[52:56] | **PASS.** Frame 8765, which echoes 0600055E: body[36:52] = `524d4c56422d30313233343536373839`, body[52:56] = `01000000`. On lab874f the same frame carried zeros |
| Q2 | after the grant, VAX2's SDA on DEPT shows Valblk RMLVB, not zero | **PASS.** Valblk 31302D42 564C4D52 39383736 35343332 at Seqnum 1. Before the grant the waiting RSB read zero at Seqnum 0 |
| D5 | no Fatal, CNXMGRERR, LOCKMGRERR or INVLOCKID; VAX2 keeps quorum with 2 nodes | **PASS.** No console contains one of those strings. VAX2 reads CLUSTER_NODES 2, QUORUM 1, VOTES 1, and SHOW CLUSTER lists VAX2 and OVMXE as MEMBER |

No VAX bugchecked, so the stop rule did not trigger.

## Observed, not a registered prediction

- After Ctrl-Y on OVMXE's foreground RMHOLD, the spawned subprocess HOLDE was gone as well:
  OVMXE's SHOW SYSTEM no longer lists it. OVMXE sent op-0x03 for its DEPU lock (frame 8766), and
  VAX2's DEPU then held only VAX2's CR. On VMS, Ctrl-Y aborting the parent's image does not delete
  a SPAWN/NOWAIT subprocess. This does not change any verdict, because DEPU was scored before
  Ctrl-Y.
- VAX2's console did not print the CNXMAN removal messages that lab874f's did. The transition is
  in the capture, and the membership change was read with F$GETSYI and SHOW CLUSTER.
