# vms-5fa re-fire r4: one MSCP-served ODS-2 volume mounted by a real VAX and OVMX at once (2026-11-03) -- FAIL

Verdict (`ci6_evac.sh grade-share ci6evac-004946`, `ci6evac-004946/grade.txt`):
S1 PASS, S2 PASS, **S3 FAIL, S4 FAIL**, S5 PASS, S6 PASS. No bugcheck on any node. The result is the same as r3.

## Bed

- Own pod `lab5fa4` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4). This is the same bed as r2 and r3.
  VAX1 (1025) alone owns the RA81 `$2$DUA2:`. VAX2 (1026) and OVMXE reach it only through VAX1's MSCP server.
  Read with `F$GETSYI` before the fire: VAX1 VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 1; VAX2 VOTES 0, LOCKDIRWT 1. Cluster group 1.
- OVMXE (1030) was booted by `ovmx_fire.sh` (tag `r5fa4`, `OVMX_FIRE_EXPVOTES=1`, `OVMX_FIRE_GROUP=1`), using the script and
  grounding table from the artifact tree. Its gate PASSed: `frame_conformance: every byte grounded` over every type in the
  grounding table, `dlm_deq_vb` included. All three negative controls were refused, and group 1 was read out of the initramfs.
  OVMXE read back EXPECTED_VOTES 1, LOCKDIRWT 0, VOTES 1. VAX2 read `Q=2 V=2 N=3`.
- The artifacts come from build-boot-artifacts run 41552837456, built from `lab/vms-5fa-r4` (SOURCE_COMMIT `931fbc9c9`).
  That commit is r3's `df35dde37` with #2003's head `0de23260b` (vms-d63 r3 fix) merged in. That fix makes the F11B$s request
  set body[28] bit 0 (LCK$M_VALBLK), and the release queue keeps the written block. SHA256SUMS were verified. The gate binaries
  were built from the same tree.
- Driver: `ci6_evac.sh share lab5fa4` with `CI6_INIT=1`, from the same tree as r3 (#2000, `2bf9e6874`). It ran end to end
  with nothing typed by hand. As in r2 and r3, OVMX's unit was `VAX1$DUA16386:` (`OVMXE.console.log` line 9).

## What the consoles and the wire show

| step | evidence |
|---|---|
| S1 | VAX1 `MOUNT/SYSTEM $2$DUA2: EVAC5FA` (`vax1.log` 17) and OVMXE `MOUNT/SYSTEM VAX1$DUA16386: EVAC5FA` (`OVMXE.console.log` 25): both `%MOUNT-I-MOUNTED` |
| S2 VAX1 SDA `F11B$vEVAC5FA` | `Lock Count: 2` (`vax1.log` 54), `Master copy of lock 01000E39 on system 00010003` (line 84) |
| S2 rounds | OVMXE: `took EVAC$WORKLOAD EX at seq 11` (line 42) and `at seq 21` (line 46). OVMXE's `DIRECTORY` shows `EVAC.DAT 4/6` (line 51). VAX1's shows `3/3` (`vax1.log` 411) |
| S3 | `STOP EVACVAX1`, then OVMXE: `took EVAC$WORKLOAD EX at seq 75` (line 64). OVMX read VAX1's seq 74 |
| S4 | OVMXE `%DISMOUNT-I-DISMOUNTED` (line 66). VAX1 `TYPE EVAC.DAT`: **74 records, seq 1..74, all written by VAX1; 0 OVMXE records** (`vax1.log` 418-491) |
| S5 | VAX1 `ANALYZE/DISK_STRUCTURE $2$DUA2:`: no -W/-E/-F line, `ANALDISK STATUS=%X00000001` (`vax1.log` 498) |
| VAX1 after the fire (`vax1-evidence.log`) | `DIRECTORY/FILE_ID`: `EVAC.DAT;1 (12,1,0) 10/36` (line 6). `DUMP/HEADER`: EOF block 10 byte 128 (= 74 x 64-byte records, lines 39-40), revision 3, revision date 00:56:06.54 (lines 61, 63). Map: `Count 3 LBN 9`, `Count 33 LBN 15` (lines 69-70) |
| S6 | VAX2 `Q=2 V=2 N=3` (`vax2.log` 19). The bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over the pod's whole console life (VAX1 970, VAX2 326, OVMXE 122 lines): **0/0/0** |

The overwrite sequence is r3's. VAX1's round 2 wrote **its own** seq 11..20 (`vax1.log` 428), and its S3 run started at
**seq 21** again (`vax1.log` 438). OVMX wrote seq 11..20 and 21..30 to LBN 10..14 (with the bitmap write at LBN 445540,
`f11b-ser-trace.txt` 263). It wrote its post-takeover records to LBN 21..23, which lies past the EOF in VAX1's header.

## The vms-d63 evidence: OVMX's half of the protocol is now on the wire, but VAX1 never grants OVMX a value block

`f11b-ser-trace.txt` lists every F11B$s frame: 458 lines, 126 of them on EVAC.DAT (fid 12). Its ENQ lines now carry body[28:30]
as `flags=`. `f11b-frames.txt` holds the full hex.

| | a real node (VAX2, fid 42) | OVMXE (EVAC.DAT, fid 12) |
|---|---|---|
| ENQ body[28:30] | `39 02` (frame 23) | **`01 00`** (frame 5726): non-zero on all 48 of OVMX's requests. r3 had `00 00` |
| VAX1's GRANT, value block | hdr 1, data 1, vbseq 685 (frame 24) | **hdr 0, data 0, vbseq 0** (frame 5727): all 48 of OVMX's grants (42 on fid 12, 6 on directory fid 11), as in r3 |
| $DEQ, value block, body[30]/[32] | (1,1), marked 1/1 (frame 28) | **marked 1/1** on all 48 releases: (1,1) on the 6 fid 11 releases and the 4 fid 12 releases with no write (frame 5732), and (2,1) on the 38 fid 12 releases that wrote the header (frame 5776). r3 had (0,0) unmarked |

1. **OVMX's ENQ flags are non-zero.** Met: `flags=0100` on every OVMX request (trace 160, 163, 166).
2. **VAX1's grants carry a value block.** Not met. Every grant VAX1 sends OVMX carries a zero block and vbseq 0 (trace 161,
   164, 167, ... 417). That includes frame 5753 (trace 167). It is the grant that follows OVMX's marked (1,1) release at
   frame 5732 (trace 165), so VAX1, as master, did not store or hand back the block OVMX released.
3. **OVMX's releases are marked.** Met (trace 162, 165, 170, ... 419).
4. **The counters step.** Not met. Every grant hands OVMX (0,0), so every header-writing release writes the same (2,1).
   The header counter never passes 2 (trace 170, 415, 419), and VAX1's cached FCB has nothing to see change.
5. **VAX1 continues seq after OVMX's records.** Not met: it restarted at 11 and 21 (`vax1.log` 428, 438).
6. **TYPE EVAC.DAT on VAX1 shows OVMXE records.** Not met: 0 of 74.
7. **ANALYZE is clean.** Met (`vax1.log` 498).

So VAX1 still treats OVMX's F11B$s lock as a lock without a value block. Setting body[28] bit 0 was not enough. The ENQ bodies
differ in more cells than body[28:30]. Diffing frame 23 (VAX2) against frame 5726 (OVMX) over body[0:80] gives, besides
per-lock handles and the parent/name area: body[22:24] `40 20` vs `00 10`, body[29] `02` vs `00`, body[32] `12` vs `00`,
body[34] `01` vs `00`, and body[36:44]. These are raw differences. None of them is judged here as the cause. The
frame-grounding table still records body[29] and body[31:44] of `dlm_enq_req` as open deviations (rows 193, 195, rd vms-6e6).
`f11b_ser_valblk_census.py` (from #2003) over the same pcap counts 79 grants carrying a block and 4 stored writing releases.
All of them are VAX-to-VAX.

Per the fire rules the run stopped here. No supplemental arm was run and no code was changed. The pcap
(`/lab/run-fire/r5fa4/wire.pcap`) is not committed.

vms-5fa and vms-d63 stay open.

References: `OVMXE.console.log` 25 (mount), 42/46/64 (EX grants), 51 (`4/6`), 66 (dismount); `vax1.log` 54/84 (S2),
428/438 (VAX1 restarting seq 11 and 21), 491 (seq 74), 498 (ANALDISK status); `vax1-evidence.log` 6 (FID 12), 39-40 (EOF);
`f11b-ser-trace.txt` 4-6 (VAX2 cycle), 163-165 and 166-170 (OVMX cycles); `f11b-frames.txt` frames 23/24/28 vs
5726/5727/5732 and 5752/5753/5776.
