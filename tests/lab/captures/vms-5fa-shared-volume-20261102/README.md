# vms-5fa: one MSCP-served ODS-2 volume mounted by a real VAX and OVMX at once (2026-11-02) -- FAIL

Verdict (`ci6_evac.sh grade-share ci6evac-182708`, `ci6evac-182708/grade.txt`):
S1 PASS, **S2 FAIL, S3 FAIL, S4 FAIL**, S5 PASS, S6 PASS. No bugcheck on any node.

## Bed

- Own pod `lab5fa` (ovmx-lab, image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, request 200m CPU / limit 4).
  VAX1 (1025) alone owns the RA81 `$2$DUA2:`; VAX2 (1026) and OVMXE reach it only through VAX1's MSCP server.
  VAX1 VOTES 1 EV 1 LOCKDIRWT 1; VAX2 LOCKDIRWT 1. Cluster group 1.
- OVMXE (1030) booted by `ovmx_fire.sh` (frame-conformance gate PASS, group 1, EXPECTED_VOTES 1) from
  artifacts built off `lab/vms-5fa` = origin/main `9c6af9422` + a LAB-ONLY commit staging `SYS$SYSTEM:EVACWL.EXE`
  (SOURCE_COMMIT `051682c03`, build-boot-artifacts run 41497359465). LOCKDIRWT 0.
- Driver: `tests/lab/tools/ci6_evac.sh share lab5fa` with `CI6_INIT=1`. It stopped at its S3 check
  ("OVMXE standby never took NL", 18:34:22); the read-back (OVMX DISMOUNT, STOP, TYPE, ANALYZE/DISK_STRUCTURE,
  quorum) was then typed by hand through `lab-console.sh` and is in the timeline. The first attempt
  (18:24, not committed) aborted before any mount because OVMX answers `SHOW DEVICE D` with
  `%SYSTEM-W-NOSUCHDEV`; the driver now lists `SHOW DEVICE`.

## What the consoles show

| step | evidence |
|---|---|
| VAX1 `INITIALIZE`, `MOUNT/SYSTEM $2$DUA2: EVAC5FA` | `%MOUNT-I-MOUNTED, EVAC5FA mounted on _$2$DUA2: (VAX1)`; SHOW DEVICE/FULL: `mounted ... served to cluster via MSCP Server`, Mount status System |
| OVMXE `MOUNT/SYSTEM VAX1$DUA16386: EVAC5FA` (its MSCP class driver's name for VAX1 unit 2) | `%MOUNT-I-MOUNTED, EVAC5FA mounted on _VAX1$DUA16386:`; SHOW DEVICE/FULL `is online, mounted` |
| VAX1 SDA `SHOW RESOURCE/NAME="F11B$vEVAC5FA     "` after both mounts | `Status: DIRENTR VALID`, `CSID: 00000000` (VAX1 directory + master), **`Lock Count: 1`**: VAX1's own CR lock only |
| wire (`lock-frames.txt`) | OVMXE's only frame naming `F11B$vEVAC5FA` went to **VAX2** (18:29:29.250, answered 0.16 ms later); none went to VAX1, which holds the directory entry. OVMX's volume lock therefore lives under a second master |
| round 1: VAX1 `EVACWL 10`, OVMXE `EVACWL 10` queued | VAX1 wrote seq 1..10; OVMXE's EX request queued at VAX1 (frame 5781, answer 5782) |
| VAX1 SDA `SHOW RESOURCE/NAME=EVAC$WORKLOAD` + `SHOW LOCK` (18:35) | master VAX1. **Granted EX = `Master copy of lock 0100040F on system 00010003`** (OVMXE's round-1 lock); waiting: VAX1 EX, OVMXE EX (01000416), VAX1 `EVACVAX1` EX, OVMXE standby NL (0100042E) |
| OVMXE `SHOW SYSTEM` | SYSTEM_1/_2/_3 alive; none printed anything; EVAC.DAT got no OVMXE record. After VAX1's grant (VAX1->OVMXE frame at 18:30:44.18 names master lkid 100006A4 / OVMXE lkid 0100040F) OVMXE sent no MSCP I/O to the shared unit and no `F11B$s`/`F11B$v` request for EVAC5FA |
| OVMXE `DISMOUNT VAX1$DUA16386:` | `%DISMOUNT-I-DISMOUNTED` |
| VAX1 `TYPE EVAC.DAT` | 10 records, all VAX1, seq 1..10 |
| VAX1 `ANALYZE/DISK_STRUCTURE $2$DUA2:` | only `%ANALDISK-I-OPENQUOTA` (no QUOTA.SYS); `$STATUS %X00000001` |
| VAX2 after | `Q=2 V=2 N=3`; bugcheck grep (fatal, bugcheck, CNXMGRERR, LOCKMGRERR, INVLOCKID, quorum lost) over all three consoles for the pod's life: 0/0/0 |

## Two executive gaps this measured

1. **OVMX's Files-11 volume lock does not reach the cluster master.** OVMX sent its `F11B$vEVAC5FA` request
   to VAX2; VAX1 holds the directory entry and masters the resource with only its own lock. So OVMX's
   XQP serialization is not cluster-wide: the precondition of the whole item.
2. **OVMX does not carry on after a VAX master grants a queued EX.** VAX1 granted OVMXE's queued
   `EVAC$WORKLOAD` EX (SDA: master copy granted EX); the OVMX process never wrote, never exited, and its
   EX blocks every later requester in the cluster. The capture does not say which side of the grant
   (delivery to the process, or the RMS/XQP open on the MSCP unit that follows) stalls; OVMX ran at
   loglevel 3.

The pcap (`/lab/run-fire/r5fa/wire.pcap` on the lab volume) is not committed: it carries OVMX-originated
DLM records that census tools would count. `lock-frames.txt` holds the five frames that name either
resource, in full hex.

Filed: rd vms-7649 (gap 1), vms-f29f (gap 2), vms-582 (`SHOW DEVICE D`). vms-5fa stays open, blocked on the first two.
