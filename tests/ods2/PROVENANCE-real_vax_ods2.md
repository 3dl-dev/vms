# Provenance: `real_vax_ods2.dsk`

Increment 2 of the genuine-ODS-2 effort (`src/vmsfs/ods2/`). This file is a
**real, byte-for-byte raw disk image** created by a genuine OpenVMS VAX
system running `INITIALIZE` and normal DCL file operations -- it is
observed tool *output*, not VSI/HPE source or binaries, and is collected
under CLAUDE.md Rule 8's clean-room allowance (observing behavior of the
reference lab).

## Where it came from

- **Lab**: lab-2 (`tests/lab/`, k3s StatefulSet `vaxlab`), pod **`vaxlab-9`**.
  Lab-1 (`/data/training/vax/cluster/`) was not disturbed -- per CLAUDE.md
  Rule 8, lab-2 is the correct choice when lab-1 may be in use, and no
  cluster/quorum state was touched (this used only vax1's console, and
  changes were confined to a spare virtual disk unit, never the shared
  system/data disks).
- **OS**: OpenVMS VAX V7.3, node VAX1 (SCSSYSTEMID 1025), already booted and
  logged in as SYSTEM in that pod's long-running session.
- **Emulator**: SIMH VAX (`open-simh`, commit `2e0d51e` per `tests/lab/README.md`),
  RQDX3 (MSCP) controller.

## Exact procedure

The stock lab config's RQDX3 controller has only 4 units (RQ0-3), all in
use (RQ0/RQ1 = the shared SYSDSK1/VAX1DATA RA92 packs, RQ2/RQ3 = read-only
CD-ROM ISOs). To get a scratch unit without touching the shared disks, RQ3
(the *second*, less-critical ISO -- `openvms-internet-product-suite-v11.iso`)
was temporarily repurposed for the length of this session only (the pod's
`entrypoint.sh` regenerates the whole SIMH config from scratch on every
restart, so this does not persist and did not require any file edits):

```
sim> detach rq3
sim> set rq3 rx50
sim> set rq3 format=raw
sim> attach rq3 /lab/k8s-labs/vaxlab-9/data/scratch-ods2.dsk
```

(The container file was pre-created with `dd if=/dev/zero bs=512 count=800`
so RAW-format attach -- which does not create new files itself -- had
something to open.) RX50 was chosen as the smallest MSCP disk geometry SIMH
offers (400KB/800 blocks) -- true to a real, small VMS-formatted medium
(RX50 floppies were routinely Files-11 volumes) and small enough to commit.

Then, from the already-logged-in VMS console (`SYSTEM`, prompt-synchronized
per the lab README):

```
$ INITIALIZE $2$DUA3: OVMXTEST /STRUCTURE=2
$ MOUNT $2$DUA3: OVMXTEST
$ CREATE/DIRECTORY $2$DUA3:[OVMXDIR]
$ COPY SYS$MANAGER:SYSTARTUP_VMS.COM $2$DUA3:[OVMXDIR]HELLO.TXT
$ COPY SYS$MANAGER:SYCONFIG.COM $2$DUA3:[OVMXDIR]WORLD.TXT
$ DISMOUNT $2$DUA3:
```

Then back at the SIMH console: `detach rq3` (flushes the file), and the
409,600-byte raw image was pulled out via `kubectl cp` unmodified. No bytes
were touched after extraction. `sha256sum`: see git history of this file if
verification is needed; re-derive rather than trust a stale hash comment.

## What OpenVMS itself reported for this volume (the cross-check oracle)

`SHOW DEVICE/FULL $2$DUA3:`, same session, before dismount:

```
Total blocks 800    Sectors per track 10
Total cylinders 1   Tracks per cylinder 80
Volume label "OVMXTEST"   Cluster size 1
Maximum files allowed 200   Host name "VAX1"
```

`DIRECTORY/FILE_ID` / `DIRECTORY/SIZE` on `[OVMXDIR]`:

```
HELLO.TXT;1  (12,1,0)   34/34 blocks
WORLD.TXT;1  (13,1,0)    2/2  blocks
```

These are the ground-truth values `tests/ods2/test_ods2_real.c` and the
`ods2.h` field comments cite.

## What this proved (and did not)

Confirmed byte-exact against the reader (`src/vmsfs/ods2/`):

- Home block: both additive checksums, `"DECFILE11B  "` format string,
  struclev `0x0201`, `hm2_cluster`, `hm2_maxfiles`, `hm2_ibmaplbn`/`hm2_ibmapsize`
  (hence the INDEXF.SYS header base LBN), `hm2_resfiles`.
- INDEXF.SYS (FID 1), BITMAP.SYS (FID 2), the MFD-created `OVMXDIR.DIR`
  (FID 11), and both data files' (FID 12, 13) FH2 headers: checksum,
  filename (ident area), FM2 map area.
- FM2 retrieval-pointer decode (format-1 encoding) for three independent
  files, cross-checked against VMS's own reported block counts (34 and 2
  blocks) and directory-listed FIDs.
- Directory record decode: real `[OVMXDIR]` data block, both entries,
  exact name/version/FID match.
- SCB (`ods2_scb_parse`, added this increment): checksum, struclev,
  cluster, volsize, plus `scb_sectors`/`scb_tracks`/`scb_cylinders`
  cross-checked against `SHOW DEVICE/FULL` geometry, and `scb_volockname`'s
  leading bytes against the reported host name.

**Not proven, and not claimed as proven** (single real sample; no
induced-error test was run in this pass): `scb_blksize`, `scb_status`,
`scb_status2`, `scb_writecnt` semantics, and the exact width/trailing
layout of `scb_volockname` / `scb_reserved`. See the per-field comments on
`ods2_scb_t` in `src/vmsfs/include/vmsfs/ods2.h` -- each says explicitly
what was confirmed, corrected, or left open. A second real volume (ideally
with an *induced* error/mount-verify condition, and a second mount to see
whether `scb_writecnt` moves) would be needed to close those.
