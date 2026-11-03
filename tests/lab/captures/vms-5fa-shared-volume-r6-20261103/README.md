# vms-5fa re-fire r6: one MSCP-served ODS-2 volume mounted by a real VAX and OVMX at once (2026-11-03) -- PASS

Verdict (`ci6_evac.sh grade-share ci6evac-024102`, `ci6evac-024102/grade.txt`, re-run from the capture alone and identical):
**S1 PASS, S2 PASS, S3 PASS, S4 PASS, S5 PASS, S6 PASS.** No bugcheck on any node. r3, r4 and r5 failed S3 and S4.

## Bed

- Own pod `lab5fa6` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4). This is the r2-r5 bed.
  VAX1 (1025) alone owns the RA81 `$2$DUA2:`. VAX2 (1026) and OVMXE reach it only through VAX1's MSCP server.
  Read with `F$GETSYI` before the fire: VAX1 VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 1; VAX2 VOTES 0, LOCKDIRWT 1. Cluster group 1.
- The artifacts come from build-boot-artifacts run 41566284003, built from `lab/vms-5fa-r6` (SOURCE_COMMIT `5fa8de625`).
  That commit is r5's tree with #2003's head `ff69ca156` merged in (vms-d63 r5 fix: OVMX's F11B$s sub request carries the
  sub-RSB name hash at body[128:132]; r5 sent 0 there). SHA256SUMS were verified.
- OVMXE (1030) was booted by `ovmx_fire.sh` from the `cluster-5fa-lab3` tree (`5fa8de625`, the build's own commit and grounding
  table), tag `r5fa6`, `OVMX_FIRE_EXPVOTES=1`, `OVMX_FIRE_GROUP=1`, `LOCKDIRWT=0`, every frame type in the grounding table
  named. The gate binaries were built from that tree. Gate PASS: `frame_conformance: every byte grounded`, all three negative
  controls refused, group 1 read out of the initramfs. OVMXE read back EXPECTED_VOTES 1, LOCKDIRWT 0, VOTES 1. VAX2 read `Q=2 V=2 N=3`.
- Driver: `ci6_evac.sh share lab5fa6` with `CI6_INIT=1`, from the driver tree (#2000, `2bf9e6874`). It ran end to end with
  nothing typed by hand. OVMX's unit was `VAX1$DUA16386:`, as in r2-r5.

## What the consoles and the wire show

| step | evidence |
|---|---|
| S1 | VAX1 `MOUNT/SYSTEM $2$DUA2: EVAC5FA` (`vax1.log` 17) and OVMXE `MOUNT/SYSTEM VAX1$DUA16386: EVAC5FA` (`OVMXE.console.log` 26): both `%MOUNT-I-MOUNTED` |
| S2 VAX1 SDA `F11B$vEVAC5FA` | RSB 879B64C0, `Lock Count: 2` (`vax1.log` 50-54). OVMX's master copy `0D0005F3`, `Master copy of lock 01000E68 on system 00010003` (72-84) |
| S2 rounds | OVMXE: `took EVAC$WORKLOAD EX at seq 11` (line 43) and `at seq 31` (line 47). r5 had 11 and 21: VAX1's round 2 now continued at 21 instead of rewriting 11..20 |
| S3 | `STOP EVACVAX1`, then OVMXE: `took EVAC$WORKLOAD EX at seq 96` (line 65). OVMX read VAX1's seq 95 |
| S4 | OVMXE `%DISMOUNT-I-DISMOUNTED` (line 67). VAX1 `TYPE EVAC.DAT` (`vax1.log` 387-497): **110 records, seq 1..110 contiguous, runs VAX1 1-10, OVMXE 11-20, VAX1 21-30, OVMXE 31-40, VAX1 41-95, OVMXE 96-110** |
| S5 | VAX1 `ANALYZE/DISK_STRUCTURE $2$DUA2:`: no -W/-E/-F line, `ANALDISK STATUS=%X00000001` (`vax1.log` 498-504) |
| VAX1 after the fire (`vax1-evidence.log`) | `DIRECTORY/FILE_ID`: `EVAC.DAT;1 (12,1,0) 14/39` (line 9). `DUMP/HEADER`: EOF block 14 byte 384 (= 110 x 64 bytes), revision 6, revision date 02:47:37.40, which is OVMXE's seq 110 timestamp (lines 15-65). Map: `Count 3 LBN 9`, `Count 36 LBN 15` |
| S6 | VAX2 `Q=2 V=2 N=3` (`vax2.log` 3, 19). The bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over the pod's whole console life, supplemental arm included (VAX1 29947, VAX2 310, OVMXE 137 lines): **0/0/0** |

Raw: OVMXE's record timestamps run about 3.4 s behind VAX1's (OVMXE seq 11 is stamped 02:44:30.94, VAX1 seq 10 02:44:34.33).
The order on the disk follows the lock, not the clocks.

## The vms-d63 evidence: OVMX's sub request names VAX1's RSB, and the value block flows both ways

`f11b-ser-trace.txt` lists every F11B$s frame plus every MSCP WRITE OVMXE sent: 1,875 entries. The driver's fire is lines 4-394.
Line 395 marks where the supplemental arm starts. `f11b-frames.txt` holds the full hex of the frames named below.

| | a real node (VAX2, fid 42) | OVMXE (EVAC.DAT, fid 12) |
|---|---|---|
| ENQ body[28:30] | `39 02` (frame 9) | `19 00` on all 319 OVMX F11B$s requests (8 fid 11, 311 fid 12), as in r5 |
| ENQ body[128:132], the sub-RSB hash | `d7 68 a6 0b` (frame 9) | **`3b 14 08 9a`** on all 311 fid 12 requests, `37 44 21 87` on the 8 fid 11 (frame 3915). r5 sent `00 00 00 00` |
| ENQ body[32:36] | `12 00 01 00` | `00 00 00 00` (frame 3915, unchanged from r5) |
| VAX1's GRANT, value block | (1,1), vbseq 633 (frame 11) | **non-zero on 319 of 319**; none is (0,0) and none has vbseq 0. First EVAC.DAT grant (14,1) vbseq 15 (frame 3916) |
| $DEQ, value block | (1,1) marked 1/1 (frame 16) | marked 1/1; the header-writing releases carry header+1, e.g. (15,1) after a (14,1) grant (frames 3942 -> 3962) |

1. **VAX1's grants carry the non-zero value block.** Met: 319/319 (trace 104-394 in the fire, 396 on in the supplemental).
2. **vbseq steps.** Met: across the 311 fid 12 grants to OVMX vbseq rises every time (310/310 successive pairs), 15 -> 407.
3. **The header counter steps.** Met: 308 of OVMX's 310 grants that follow an OVMX release hand back exactly the counter that
   release wrote. The other two hand back more, by what VAX1's own rounds wrote in between: OVMX released 25 (frame 4324) and
   its next grant was 37 after VAX1's round 2 (frame 4710); OVMX released 48 (frame 5529) and got 105 after VAX1's S3 run
   (frame 6020). Successive OVMX releases step by 1 (305), 0 (3, the fid 12 cycles that did not write the header), 12 and 57.
4. **VAX1 continues seq after OVMX's records.** Met: VAX1's round 2 started at 21 and its S3 run at 41 (`vax1.log` 408, 428),
   and after the supplemental at 411 (below).
5. **TYPE EVAC.DAT on VAX1 shows OVMXE records.** Met: 35 of 110 in the fire, 335 of 420 after the supplemental.
6. **ANALYZE is clean.** Met in the fire (`vax1.log` 504). See the supplemental's second ANALYZE below.

## Step 5: VAX1's SDA on EVAC.DAT's F11B$s resource while OVMX cycles the lock

After the driver's END VAX1 was read with `ANALYZE/SYSTEM`. SDA only reads; nothing on VAX1 was changed. OVMXE remounted the
volume and ran `EVACWL 100` and then `EVACWL 200`. EVAC.DAT is FID (12,1,0), so its resource name is `F11B$s` + `0C 00 00 00`,
which SDA prints as `000C7324 42313146`.

| read | when | result (`vax1-evidence.log` line) |
|---|---|---|
| whole resource database `R6RSB0.LIS`, SEARCH for `000C7324 42313146` | 02:51:41-49, OVMX not mounted | **2 matches** (74-122). RSB **879BE500**, parent **879B64C0** (= RSB `F11B$vEVAC5FA`, S2): `Lock Count 1`, granted only **0B0005F9 NL**, `Valblk 00000001 00000079` (counters 0x79,1 = 121,1), `Seqnum 0000008B` (139). 121 is what OVMX's last release in the fire wrote (trace, frame 6641, `hdr=121`), and 139 is the vbseq of OVMX's next grant (frame 9317). Also RSB 87956400 under 879552C0 (another volume, as in r5) |
| `SHOW LOCK 0B0005F9` | | VAX1's own lock: `VALBLK CONVERT SYNCSTS CVTSYS`, `Par. id 010005ED` (= VAX1's F11B$vEVAC5FA lock, `vax1.log` 176-186), granted NL, `Local copy` (126-139) |
| `SHOW RESOURCE/LOCKID=0B0005F9` x600, one SDA run (`R6S.LIS`) | during `EVACWL 100` | 600 of 600: RSB 879BE500, Lock Count 1, `Valblk 0x86,1`, `Seqnum 0x99` (193-200). That is one instant: OVMX's release of 134 (frame 9859) and its next grant, vbseq 153 = 0x99 (frame 9881), 1.0 s apart. One SDA run of 600 reads fits inside one gap between two OVMX cycles |
| `R6LOOP.COM`: 100 `ANALYZE/SYSTEM` runs x 50 `SHOW RESOURCE/LOCKID=0B0005F9` (`R6L.LIS;*`) | during `EVACWL 200` | 5,000 samples, always RSB 879BE500. `Valblk` walks **0xE5 -> 0xFE** (229 -> 254) in steps of one, about 200 samples per value (240-274). **2 samples** show `GGMODE: PW`, `Lock Count: 2`, granted queue **`0C000666 PW`** plus `0B0005F9 NL`, `Valblk 0xF6,1`, `Seqnum 0x10A` (286-295, console 11507 and 11512) |
| the wire for that sample | | VAX1's GRANT frame 16298 to OVMX carries body[20:24] = `66 06 00 0c` = master copy **0C000666**, block (246,1) = 0xF6, vbseq 266 = 0x10A. OVMX's ENQ is frame 16297 (hash `3b14089a`) and its release frame 16318 (247,1), 2.85 ms after the grant |

What this records:

- **OVMX's F11B$s lock sits in the same RSB as VAX1's own lock on EVAC.DAT**: 879BE500, under `F11B$vEVAC5FA`. SDA lists it
  in that RSB's granted queue at PW, and its lock id is the master copy id VAX1 sent OVMX in that grant.
- **That RSB's Valblk and Seqnum move with OVMX's releases.** 0x79 after the fire, 0x86 during `EVACWL 100`, 0xE5..0xFE during
  `EVACWL 200`. Each value equals the counter in an OVMX release on the wire, and each Seqnum equals the vbseq of OVMX's next grant.
  In r5, 879D7380 stayed at 0x51 and 0x5E through 147 OVMX releases.

## The supplemental's end: VAX1 continues, and a second ANALYZE reports FREESPADRIFT

`vax1-evidence.log` 302-743. OVMXE wrote seq 111..410 in the supplemental (`took ... at seq 111`, `at seq 211`, 752/754) and dismounted.

- VAX1 `EVACWL 10`: `VAX1 took EVAC$WORKLOAD EX at seq 411` (306).
- VAX1 `TYPE EVAC.DAT`: **420 records, seq 1..420 contiguous**, runs VAX1 1-10, OVMXE 11-20, VAX1 21-30, OVMXE 31-40,
  VAX1 41-95, OVMXE 96-410, VAX1 411-420 (308-727). `DIRECTORY/FILE_ID`: `(12,1,0) 53/54` (732).
- `ANALYZE/DISK_STRUCTURE $2$DUA2:` this time printed **`%ANALDISK-W-FREESPADRIFT, free block count of 890889 is incorrect
  (RVN 1); the correct value is 890874`** (740), status `%X00000001` (743). The drift is 15 blocks. EVAC.DAT's allocation went
  from 39 to 54 (+15) over the supplemental, in which OVMXE wrote 300 records and VAX1 10. Which node allocated the blocks, and
  how the volume's free count travels between nodes, was not traced here. The fire's own ANALYZE (S5) is clean.

Per the fire rules no code was changed. The pcap (`/lab/run-fire/r5fa6/wire.pcap`) is not committed.

References: `OVMXE.console.log` 26 (mount), 43/47/65 (EX grants), 52 (`5/6`), 67 (dismount); `vax1.log` 50-84 (S2 SDA),
387-497 (TYPE), 504 (ANALDISK status); `vax1-evidence.log` 9/15 (FID 12, EOF), 74-122 (the two F11B$s<12> RSBs), 126-139
(lock 0B0005F9), 193-200 (600-sample tally), 240-274 (5,000-sample tally), 286-295 (OVMX's lock in RSB 879BE500), 306 (seq 411),
308-727 (TYPE, 420 records), 740 (FREESPADRIFT); `f11b-ser-trace.txt` 4-6 (VAX2 cycle), 103-110 (OVMX's first cycles), 395
(supplemental start); `f11b-frames.txt` frames 9/11/16, 3915/3916/3921, 3941/3942/3962, 16297/16298/16318.
