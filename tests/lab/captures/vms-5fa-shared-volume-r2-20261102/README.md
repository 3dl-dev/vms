# vms-5fa re-fire r2: one MSCP-served ODS-2 volume mounted by a real VAX and OVMX at once (2026-11-02) -- FAIL

Verdict (`ci6_evac.sh grade-share ci6evac-221157`, `ci6evac-221157/grade.txt`):
S1 PASS, **S2 PASS** (was FAIL), **S3 FAIL, S4 FAIL, S5 FAIL** (S5 was PASS), S6 PASS. No bugcheck on any node.

## Bed

- Own pod `lab5fa2` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4).
  VAX1 (1025) alone owns the RA81 `$2$DUA2:`. VAX2 (1026) and OVMXE reach it only through VAX1's MSCP server.
  Read with `F$GETSYI` before the fire: VAX1 VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 1; VAX2 LOCKDIRWT 1. Cluster group 1.
- OVMXE (1030) was booted by `ovmx_fire.sh` (the frame-conformance gate PASSed over every type in
  `frame-grounding.tsv`; group 1, EXPECTED_VOTES 1, LOCKDIRWT 0) from build-boot-artifacts run 41532755913.
  That run was built from `lab/vms-5fa-r2` (SOURCE_COMMIT `5a74cebbc`): main `e21289d22` + the S2 fix #1989 (vms-7649)
  + the `SHOW DEVICE D` fix #1998 (vms-582) + a LAB-ONLY commit that stages `SYS$SYSTEM:EVACWL.EXE`.
  vms-f29f (#1988) was already on main.
- Driver: `ci6_evac.sh share lab5fa2` with `CI6_INIT=1`, taken from that same commit. It aborted at 22:18:16 with
  "OVMXE took EX while VAX1 ran". That check is a **false positive**: it greps the console from the
  fire's begin marker, and that window includes S2. The only two `took EVAC$WORKLOAD EX` lines (seq 11 and seq 21,
  `OVMXE.console.log` lines 35 and 39) come before the S3 `SPAWN @OVSTBY.COM` (line 51). After that spawn the console shows only
  `standby holds EVAC$WORKLOAD NL` (line 54) until the takeover (line 56). The driver now has its own S3 marker (#2000). The rest of the
  sequence (STOP, takeover, DISMOUNT, TYPE, ANALYZE, quorum) was typed by hand through `lab-console.sh`, exactly
  as the script would have done it. Those steps are marked `HAND` in `timeline.txt`.

## What the consoles and the wire show

| step | evidence |
|---|---|
| S1 VAX1 `INITIALIZE` + `MOUNT/SYSTEM $2$DUA2: EVAC5FA`; OVMXE `MOUNT/SYSTEM VAX1$DUA16386: EVAC5FA` | both `%MOUNT-I-MOUNTED`. OVMX found the unit through `SHOW DEVICE D`, so vms-582 is fixed |
| S2 VAX1 SDA `SHOW RESOURCE/NAME="F11B$vEVAC5FA     "` | `DIRENTR VALID`, CSID 0, **`Lock Count: 2`**, and one is `Master copy of lock 010003FA on system 00010003` (OVMXE). vms-7649 is fixed |
| S2 rounds 1/2 | OVMXE: `took EVAC$WORKLOAD EX at seq 11`, then `at seq 21`. OVMXE `DIRECTORY` shows `EVAC.DAT 4/4` while VAX1 shows `3/3` |
| S3 before STOP, VAX1 SDA `EVAC$WORKLOAD` | granted EX `0E0006B6` (VAX1 EVACVAX1, HIB); conversion queue `260006C3 NL -> EX` (OVMXE's standby) |
| S3 `STOP EVACVAX1` (22:36:3x) | OVMXE: `EVACWL: OVMXE took EVAC$WORKLOAD EX at seq 1170`. So vms-f29f's grant delivery works |
| S4 OVMXE `DISMOUNT VAX1$DUA16386:` | `%DISMOUNT-I-DISMOUNTED` |
| S4 VAX1 `TYPE EVAC.DAT` | **1169 records, seq 1..1169, every one written by VAX1; 0 OVMXE records** |
| S5 VAX1 `ANALYZE/DISK_STRUCTURE $2$DUA2:` | `%ANALDISK-W-ALLOCCLR, blocks incorrectly marked allocated  LBN 89118 to 89120`; `$STATUS %X00000001` |
| VAX1 `DUMP/HEADER EVAC.DAT` | retrieval pointers `Count 3 LBN 9`, `Count 165 LBN 15`; volume cluster size 3 (`SHOW DEVICE/FULL`) |
| wire (`ovmx-mscp-writes.txt`) | OVMXE sent 75 MSCP WRITEs to VAX1's unit. Data went to LBN 10/11 in round 1, 29706 in round 2 and 158/159 after the takeover. There were 38 writes to LBN 445653 (header), 1 to LBN 445547 (bitmap) |
| S6 | VAX2 `Q=2 V=2 N=3` afterwards. The bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over all three consoles for the pod's life: **0/0/0** |

## The gap this measured: OVMX's writes to a shared file do not reach the VAX

The lock path now works end to end: the volume lock goes to VAX1's master, the queued EX is granted, and OVMX
takes over at seq+1. **But nothing OVMX writes ends up in the file as VAX1 sees it.**

1. **The file header and EOF are not coherent across nodes.** In round 1 OVMX wrote seq 11..20 into LBN 10/11,
   which is VBN 2/3 of the file under VAX1's own map (LBN 9, count 3). It also rewrote the header (LBN 445653).
   VAX1's round 2 then wrote *its* seq 11..20 over the same records: VAX1 kept its cached EOF. Whatever
   OVMX does on the header/EOF update does not invalidate VAX1's cached FCB. Reading the other way works: OVMX
   saw VAX1's seq 10 and seq 1169.
2. **On extend, OVMX writes data to the cluster number instead of the LBN.** In round 2 OVMX set bitmap
   cluster 29706 (a write to the bitmap at LBN 445547). With a cluster size of 3, that cluster is LBN
   89118..89120, the exact range ANALDISK reports. OVMX then wrote the data to **LBN 29706** (7 WRITEs).
   VAX1's header never gained that extent, so the bitmap bits are orphaned (ALLOCCLR), and LBN 29706 was written
   with data that belongs to no file.

`mscp-frames.txt` gives the full hex of frames 3863/3872 (round-1 data write to LBN 10 and header write),
4687 (bitmap write), 4693 (data write to LBN 29706) and 4708 (header). The MSCP opcode is at byte 0x50 and
the LBN at 0x64. The pcap (`/lab/run-fire/r5fa2/wire.pcap`) is not committed.

vms-5fa stays open.

References: `vax1.log` line 54/84 (S2 Lock Count 2 / OVMXE master copy), 1610 (ALLOCCLR), 1672-1673 (EVAC.DAT map),
35 (cluster size 3); `OVMXE.console.log` 44 (`EVAC.DAT 4/4`); `mscp-frames.txt` frame 3863 (WRITE LBN 0x0a),
3872 (WRITE LBN 445653 header), 4687 (WRITE LBN 445547 bitmap), 4693 (WRITE LBN 0x740a = 29706), 4708 (header).
