# vms-5fa re-fire r3: one MSCP-served ODS-2 volume mounted by a real VAX and OVMX at once (2026-11-03) -- FAIL

Verdict (`ci6_evac.sh grade-share ci6evac-235551`, `ci6evac-235551/grade.txt`):
S1 PASS, S2 PASS, **S3 FAIL, S4 FAIL**, **S5 PASS** (was FAIL in r2), S6 PASS. No bugcheck on any node.

## Bed

- Own pod `lab5fa3` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4). This is the same bed as r2.
  VAX1 (1025) alone owns the RA81 `$2$DUA2:`. VAX2 (1026) and OVMXE reach it only through VAX1's MSCP server.
  Read with `F$GETSYI` before the fire: VAX1 VOTES 1, EXPECTED_VOTES 1, LOCKDIRWT 1; VAX2 VOTES 0, LOCKDIRWT 1. Cluster group 1.
- OVMXE (1030) was booted by `ovmx_fire.sh` (tag `r5fa3`, `OVMX_FIRE_EXPVOTES=1`, `OVMX_FIRE_GROUP=1`). Its gate PASSed:
  `frame_conformance: every byte grounded` over every type the emitters produce, including the new `dlm_deq_vb`;
  every negative control refused; group 1 read out of the initramfs. OVMXE read back EXPECTED_VOTES 1, LOCKDIRWT 0, VOTES 1;
  VAX2 read `Q=2 V=2 N=3`. The artifacts come from build-boot-artifacts run 41546619574, built from `lab/vms-5fa-r3`
  (SOURCE_COMMIT `df35dde37`). That commit is main `ed482dc09` plus the open PRs #1989 (vms-7649), #1998 (vms-582),
  #2002 (vms-563, cluster-as-LBN) and #2003 at `b63de9f66` (vms-d63, F11B$s value block, the PR's current head), plus a
  LAB-ONLY commit that stages `SYS$SYSTEM:EVACWL.EXE`. The gate binaries were built from that same tree.
- Driver: `ci6_evac.sh share lab5fa3` with `CI6_INIT=1`, taken from #2000 (`2bf9e6874`, S3's checks scoped to an S3 marker).
  It ran end to end with nothing typed by hand. That tree does not carry #1998's one-line `SHOW DEVICE D` change,
  so OVMX's unit was found through plain `SHOW DEVICE` (`OVMXE.console.log` line 9). The unit was `VAX1$DUA16386:`, as in r2.

## What the consoles and the wire show

| step | evidence |
|---|---|
| S1 | VAX1 `MOUNT/SYSTEM $2$DUA2: EVAC5FA` and OVMXE `MOUNT/SYSTEM VAX1$DUA16386: EVAC5FA`: both `%MOUNT-I-MOUNTED` (`OVMXE.console.log` line 25) |
| S2 VAX1 SDA `F11B$vEVAC5FA` | `Lock Count: 2` (`vax1.log` line 54), `Master copy of lock 01000E3A on system 00010003` (line 84) |
| S2 rounds | OVMXE: `took EVAC$WORKLOAD EX at seq 11` (line 42) and `at seq 21` (line 46). OVMXE's `DIRECTORY` shows `EVAC.DAT 4/6` (line 51); VAX1's shows `3/3` (`vax1.log` 381) |
| S3 | `STOP EVACVAX1`, then OVMXE: `took EVAC$WORKLOAD EX at seq 76` (line 64). Takeover works, and OVMX read VAX1's seq 75 |
| S4 | OVMXE `%DISMOUNT-I-DISMOUNTED` (line 66). VAX1 `TYPE EVAC.DAT`: **75 records, seq 1..75, all written by VAX1; 0 OVMXE records** (`vax1.log` 388-462) |
| S5 | VAX1 `ANALYZE/DISK_STRUCTURE $2$DUA2:`: no -W/-E/-F line, `ANALDISK STATUS=%X00000001` (`vax1.log` 469). vms-563 is fixed: OVMX's data writes all fall in LBN 10..23, not at a cluster number |
| VAX1 after the fire (`vax1-evidence.log`) | `DIRECTORY/FILE_ID`: `EVAC.DAT;1 (12,1,0) 10/36` (line 6). `DUMP/HEADER`: EOF block 10 byte 192 (= 75 x 64-byte records), revision 3, revision date 00:02:14.31; map `Count 3 LBN 9`, `Count 33 LBN 15` |
| S6 | VAX2 `Q=2 V=2 N=3` (`vax2.log` 19). The bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over 20000 console lines for the pod's whole life: **0/0/0** |

## The gap this measured: VAX1 still never learns of OVMX's writes, because OVMX's F11B$s lock carries no value block

The sequence repeats r2's overwrite:

1. In round 1, OVMXE wrote seq 11..20 to LBN 10/11.
2. VAX1's round 2 wrote **its own** seq 11..20 (`vax1.log` 398). It kept its cached EOF.
3. OVMXE then wrote seq 21..30. It extended into LBN 12..14 and wrote the bitmap at LBN 445540.
4. VAX1's S3 run started at **seq 21** again (`vax1.log` 408). It extended into LBN 15+.
5. OVMXE's post-takeover seq 76..90 went to LBN 21..23. That is past the EOF that VAX1's header records.

Reading in the other direction works: OVMX picked up VAX1's seq 20 and seq 75.

vms-d63 should have made OVMX take part in the header-cache coherence protocol, but none of that protocol is on the wire.
`f11b-ser-trace.txt` lists every F11B$s frame (126 of them on EVAC.DAT, fid 12). `f11b-frames.txt` has the full hex.

| | a real node (VAX2, fid 42) | OVMXE (EVAC.DAT, fid 12) |
|---|---|---|
| ENQ body[28:30] | `39 02` (frame 21) | `00 00` (frame 4538) |
| VAX1's GRANT, value block | hdr 1, data 1, vbseq 677 (frame 22) | **hdr 0, data 0, vbseq 0** (frame 4539): every one of OVMX's 48 F11B$s grants (42 on fid 12, 6 on directory fid 11) |
| $DEQ, value block, body[30]/[32] | (1,1), marked 1/1 (frame 26) | **(0,0), unmarked 0/0** (frame 4544): every one of its 48 releases |

So VAX1, as master, never hands OVMX the counters, and OVMX's releases never write them. The header counter that VAX1's
cached FCB checks never steps. The census tool from #2003 (`f11b_ser_valblk_census.py`) agrees over the same pcap:
68 grants carry a block and 3 writing releases are stored, all of them VAX-to-VAX.

Because of that, the vms-d63 evidence list (VAX1's SDA value block stepping after an OVMX write, VAX1 continuing seq after
OVMX's records, OVMX records in `TYPE`) cannot be met on this fire. The first fact the protocol needs, a non-zero block on
OVMX's grant, is already absent on the wire. Per the fire rules the run stopped here: no supplemental arm and no code change.
Note that OVMX's ENQ writes body[28:30] as zero. The frame-grounding table records that cell as an open deviation (rd vms-6e6, likely the
$ENQ flags). A real XQP's PW lock sets LCK$M_VALBLK.

One more fact, not judged here: OVMX wrote data to LBN 12..14 (`f11b-ser-trace.txt` line 226, after the bitmap write at
line 224), and VAX1's final header does not map those blocks. ANALDISK reported nothing for them.

The pcap (`/lab/run-fire/r5fa3/wire.pcap`) is not committed.

vms-5fa and vms-d63 stay open.

References: `OVMXE.console.log` 42/46/64 (EX grants), 51 (`4/6`); `vax1.log` 54/84 (S2), 398/408 (VAX1 restarting seq 11 and 21),
469 (ANALDISK status); `vax1-evidence.log` 6 (FID 12), 31-32 (EOF); `f11b-ser-trace.txt` 4-6 (VAX2 cycle), 118-120 (OVMX cycle);
`f11b-frames.txt` frames 21/22/26 vs 4538/4539/4544.
