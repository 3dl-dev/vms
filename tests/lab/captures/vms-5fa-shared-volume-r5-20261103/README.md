# vms-5fa re-fire r5: one MSCP-served ODS-2 volume mounted by a real VAX and OVMX at once (2026-11-03) -- FAIL

Verdict (`ci6_evac.sh grade-share ci6evac-013048`, `ci6evac-013048/grade.txt`):
S1 PASS, S2 PASS, **S3 FAIL, S4 FAIL**, S5 PASS, S6 PASS. No bugcheck on any node. The result is the same as r3 and r4.

## Bed

- Own pod `lab5fa5` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4). This is the r2-r4 bed.
  VAX1 (1025) alone owns the RA81 `$2$DUA2:`. VAX2 (1026) and OVMXE reach it only through VAX1's MSCP server.
  Read with `F$GETSYI` before the fire: VAX1 VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 1; VAX2 VOTES 0, LOCKDIRWT 1. Cluster group 1.
- OVMXE (1030) was booted by `ovmx_fire.sh` (tag `r5fa5`, `OVMX_FIRE_EXPVOTES=1`, `OVMX_FIRE_GROUP=1`), using the script and
  grounding table from the artifact tree, as in r4. Its gate PASSed: `frame_conformance: every byte grounded` over every type in
  the grounding table, `dlm_deq_vb` included. All three negative controls were refused, and group 1 was read out of the initramfs.
  OVMXE read back EXPECTED_VOTES 1, LOCKDIRWT 0, VOTES 1. VAX2 read `Q=2 V=2 N=3`.
  The first attempt ran `ovmx_fire.sh` from the driver tree (#2000, `2bf9e6874`). That tree's grounding table predates #2003, so
  it has no `dlm_deq_vb` rows and still holds `dlm_enq_req_sub` body[28:30] as `const:0`. The gate REFUSED, with 133 failures
  (132 on the `dlm_deq_vb` frame, 1 on the sub request's `19 00`), and nothing booted. The second attempt used the artifact tree's
  script and passed.
- The artifacts come from build-boot-artifacts run 41558047109, built from `lab/vms-5fa-r5` (SOURCE_COMMIT `c93bc6f24`).
  That commit is r4's `931fbc9c9` with #2003's head `2b6c6f8d9` (vms-d63 r4 fix) merged in. That fix makes the request carry
  the LKB's $ENQ flags at body[28:30] in the public LCK$M_ encoding, and it makes the ACP take its F11B$v and F11B$s locks
  SYSTEM|SYNCSTS. SHA256SUMS were verified. The gate binaries were built from the same tree.
- Driver: `ci6_evac.sh share lab5fa5` with `CI6_INIT=1`, from the r3/r4 tree (#2000, `2bf9e6874`). It ran end to end with
  nothing typed by hand. OVMX's unit was `VAX1$DUA16386:`, as in r2-r4 (`OVMXE.console.log` line 15).

## What the consoles and the wire show

| step | evidence |
|---|---|
| S1 | VAX1 `MOUNT/SYSTEM $2$DUA2: EVAC5FA` (`vax1.log` 17) and OVMXE `MOUNT/SYSTEM VAX1$DUA16386: EVAC5FA` (`OVMXE.console.log` 25): both `%MOUNT-I-MOUNTED` |
| S2 VAX1 SDA `F11B$vEVAC5FA` | RSB 879E7A00, `Lock Count: 2` (`vax1.log` 51-54). OVMX's master copy `190006A5`, granted NL with **Flags empty**, `Master copy of lock 01000E2C on system 00010003` (lines 72-84). VAX1's own `010006A7` is CR with `VALBLK CONVERT NOQUEUE SYNCSTS NOQUOTA CVTSYS` (102-107) |
| S2 rounds | OVMXE: `took EVAC$WORKLOAD EX at seq 11` (line 42) and `at seq 21` (line 46). OVMXE's `DIRECTORY` shows `EVAC.DAT 4/6` (line 51) |
| S3 | `STOP EVACVAX1`, then OVMXE: `took EVAC$WORKLOAD EX at seq 75` (line 64). OVMX read VAX1's seq 74 |
| S4 | OVMXE `%DISMOUNT-I-DISMOUNTED` (line 66). VAX1 `TYPE EVAC.DAT`: **74 records, seq 1..74, all written by VAX1; 0 OVMXE records** (`vax1.log` 508-581) |
| S5 | VAX1 `ANALYZE/DISK_STRUCTURE $2$DUA2:`: no -W/-E/-F line, `ANALDISK STATUS=%X00000001` (`vax1.log` 588) |
| VAX1 after the fire (`vax1-evidence.log`) | `DIRECTORY/FILE_ID`: `EVAC.DAT;1 (12,1,0) 10/36` (line 6). `DUMP/HEADER`: EOF block 10 byte 128 (lines 27-28), revision 3, revision date 01:37:12.52 (lines 48, 50). Map: `Count 3 LBN 9`, `Count 33 LBN 15` (lines 55-56). All the same as r4 |
| S6 | VAX2 `Q=2 V=2 N=3` (`vax2.log` 3, 19). The bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over the pod's whole console life, supplemental arm included (VAX1 17493, VAX2 326, OVMXE 134 lines): **0/0/0** |

The overwrite sequence is the same as r3 and r4. VAX1's round 2 wrote **its own** seq 11..20 (`vax1.log` 518, at 01:35:10), and
its S3 run started again at **seq 21** (`vax1.log` 528, at 01:36:18). Both times OVMX had already written those numbers.

## The vms-d63 evidence: OVMX's request now carries the real flags, and VAX1 still grants it a zero block

`f11b-ser-trace.txt` lists every F11B$s frame plus every MSCP WRITE OVMXE sent: 1,140 entries. The driver's fire is lines 4-416.
Line 417 marks where the supplemental arm starts. `f11b-frames.txt` holds the full hex.

| | a real node (VAX2, fid 42) | OVMXE (EVAC.DAT, fid 12) |
|---|---|---|
| ENQ body[28:30] | `39 02` (0x0239) on 104 of VAX2's 104 fid 42 requests (frame 11) | **`19 00` (0x0019 = VALBLK, SYNCSTS, SYSTEM)** on all 161 of OVMX's F11B$s requests, the 48 in the fire and the 113 in the supplemental arm (frame 4383). r4 had `01 00` |
| ENQ body[32:36] | `12 00 01 00` (frame 11) | `00 00 00 00` (frame 4383) |
| ENQ parent pair body[36:44] | `01000008 1200022F` (frame 11) | `01000F00 090006B0` (frame 4383). This is OVMX's **per-operation** F11B$vEVAC5FA lock (request 4380, flags `18 00`) and its VAX1 master copy (grant 4381). It is not the mount's lock `01000E2C`. All 161 OVMX F11B$s requests are each preceded by a fresh F11B$vEVAC5FA request with flags `18 00` (161 of them, `b24` 01000EFE, 01000F00, 01000F0E, ...) |
| VAX1's GRANT, value block | hdr 1, data 1, vbseq 673 and up (frame 12). No VAX2 grant has vbseq 0 | **hdr 0, data 0, vbseq 0** (frame 4384) on all 161 of OVMX's grants, as in r3 and r4 |
| $DEQ, value block, body[30]/[32] | (1,1), marked 1/1 (frame 16) | marked 1/1 on all 161: (1,1) on 8 fid 11 and 6 fid 12 releases, (2,1) on 147 fid 12 releases that wrote the header (frame 4433) |

1. **OVMX's ENQ flags.** Met as specified: `flags=1900` on every OVMX request (trace 112, 115, 120). It is not VAX2's 0x0239.
2. **VAX1's grants carry a non-zero value block and vbseq.** Not met. All 161 are (0,0) vbseq 0. That includes every grant
   that follows one of OVMX's marked (2,1) releases (trace 119 -> 121).
3. **The counters step.** Not met. The header counter OVMX releases is always 2 (trace 119 ... 1144).
4. **VAX1 continues seq after OVMX's records.** Not met: it restarted at 11 and 21 (`vax1.log` 518, 528).
5. **TYPE EVAC.DAT on VAX1 shows OVMXE records.** Not met: 0 of 74.
6. **ANALYZE is clean.** Met (`vax1.log` 588).

## Step 5: VAX1's SDA on EVAC.DAT's F11B$s resource while OVMX cycles the lock

The grants stayed (0,0), so after the driver's END VAX1 was read with `ANALYZE/SYSTEM`. SDA only reads; nothing on VAX1 was
changed. To make OVMX cycle F11B$s<12>, OVMXE remounted the volume and ran `EVACWL 40` and then `EVACWL 100`
(`vax1-evidence.log`, tail). Those runs appended records past VAX1's EOF. That does not matter here: the run had already been
graded and ANALYZEd, and the disk is scratch. EVAC.DAT is FID (12,1,0), so its resource name is `F11B$s` + `0C 00 00 00`, which
SDA prints as `000C7324 42313146`.

| read | when | result (`vax1-evidence.log` line) |
|---|---|---|
| whole resource database `R5RSB0.LIS`, SEARCH for `000C7324 42313146` | 01:44:11-23, OVMX not mounted | **2 matches** (63-94). RSB **879D7380**, parent **879E7A00** (= RSB `F11B$vEVAC5FA`, S2 above): `Lock Count 1`, granted only **240006AB NL**, `Valblk 00000001 00000051` (SDA prints high longword first, so the counters are 0x51,1), `Seqnum 0000005E`. Also RSB 8795D700 under parent 8795D2C0 (another volume, not identified): `5300000E NL`, Valblk 1,1, Seqnum 4F |
| `SHOW LOCK 240006AB` | | VAX1's own lock: `VALBLK CONVERT SYNCSTS CVTSYS`, `Par. id 010006A7` (= VAX1's F11B$vEVAC5FA lock), granted NL, `Local copy` (101-110). VAX1 keeps the lock at NL and converts it; OVMX $ENQs and $DEQs each cycle |
| `SHOW RESOURCE/LOCKID=240006AB` x4 | before and during `EVACWL 40` | RSB 879D7380 unchanged: Lock Count 1, Valblk 0x51,1, Seqnum 5E, only 240006AB (111-174) |
| whole database `R5RSB1.LIS` | 01:47:34.74-53.98, during `EVACWL 40`: 19 OVMX F11B$s<12> cycles held 150.6 ms in total (trace, by timestamp) | the same 2 fid 12 RSBs, both unchanged (178-205). There is no third F11B$s<12> RSB and no OVMX lock in any queue |
| `SHOW RESOURCE/LOCKID=240006AB` x600 (`R5S.LIS`) | 01:52:06.54-24.00, during `EVACWL 100`: 18 OVMX cycles held 124.8 ms in total | **600 of 600** samples: RSB 879D7380, `Lock Count 1`, only `240006AB NL`, `Valblk 0x51,1`, `Seqnum 5E`, conversion and waiting queues empty (tallies at the file's end) |

What this records:

- **OVMX's lock was never seen in the RSB that holds VAX1's own lock on EVAC.DAT.** No snapshot caught it: the 4 single reads,
  the 2 full dumps, and the 600 samples taken over a window in which OVMX held F11B$s<12> 18 times for 124.8 ms in all. That is
  a duty cycle of about 0.7%, so samples drawn independently would have caught it about 4 times. This RSB has a Valblk of
  (0x51,1) and a Seqnum of 0x5E. Every grant VAX1 sends OVMX carries (0,0) vbseq 0, which is what a newly created resource
  hands out. After 147 of OVMX's marked (2,1) releases, the RSB's Valblk and Seqnum **never moved**. So OVMX's releases are not
  stored on VAX1's RSB.
- The full dumps show no second F11B$s<12> RSB under `F11B$vEVAC5FA`. If OVMX's lock sits on a resource of its own, that
  resource exists only while OVMX holds it (an RSB with no locks is removed), and no read caught it.
- These raw differences between OVMX's request and VAX2's remain. None of them is judged here as the cause. OVMX's F11B$s sub
  request names a per-operation F11B$v parent (flags 0x0018), not its mount lock. Its body[32:36] is zero where VAX2's is
  `00010012`. Its mount lock (`01000E2C`, request 3493, and the supplemental remount's) went out with flags 0, and SDA lists its master copy `190006A5` with
  Flags empty.

Per the fire rules no code was changed. The pcap (`/lab/run-fire/r5fa5/wire.pcap`) is not committed.

vms-5fa and vms-d63 stay open.

References: `OVMXE.console.log` 25 (mount), 42/46/64 (EX grants), 51 (`4/6`), 66 (dismount); `vax1.log` 51-107 (S2 SDA),
518/528 (VAX1 restarting at seq 11 and 21), 581 (seq 74), 588 (ANALDISK status); `vax1-evidence.log` 6 (FID 12), 27-28 (EOF),
63-94 (the two F11B$s<12> RSBs), 101-110 (lock 240006AB), 111-205 (RSB reads during OVMX cycles), file tail (600-sample tallies,
OVMXE supplemental console); `f11b-ser-trace.txt` 4-6 (VAX2 cycle), 112-121 (OVMX cycles), 417 (supplemental start);
`f11b-frames.txt` frames 11/12/16 vs 3493/3494, 4380/4381, 4383/4384/4389, 4409/4410/4433.
