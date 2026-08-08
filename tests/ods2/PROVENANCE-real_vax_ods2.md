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

## Addendum (increment 3, the ODS-2 WRITER): reserved-file names/order

While building the writer (`src/vmsfs/ods2/ods2_writer.c`), this SAME fixture
was decoded further -- reading the ident area and `fh2_filechar` of every
reserved-file header (LBN `hdr_base + (fid - 1)` for fid 1..13) -- to ground
`hm2_resfiles == 10` and the reserved-file names/order the writer needed but
increment 2 never examined. This is observed-oracle grounding under Rule 8
(re-reading the already-collected real fixture), not a new lab session and
not a public-doc citation:

```
FID  1  INDEXF.SYS;1    filechar=0x0000  map_inuse=6 (3 extents)
FID  2  BITMAP.SYS;1    filechar=0x0080  map_inuse=2 (1 extent, CONTIG)
FID  3  BADBLK.SYS;1    filechar=0x0000  map_inuse=0 (no data)
FID  4  000000.DIR;1    filechar=0x2080  map_inuse=2 (1 extent, DIRECTORY|CONTIG)
FID  5  CORIMG.SYS;1    filechar=0x0000  map_inuse=0 (no data)
FID  6  VOLSET.SYS;1    filechar=0x0000  map_inuse=0 (no data)
FID  7  CONTIN.SYS;1    filechar=0x0000  map_inuse=0 (no data)
FID  8  BACKUP.SYS;1    filechar=0x0000  map_inuse=0 (no data)
FID  9  BADLOG.SYS;1    filechar=0x0000  map_inuse=0 (no data)
FID 10  SECURITY.SYS;1  filechar=0x0080  map_inuse=2 (1 extent, CONTIG)
FID 11  OVMXDIR.DIR;1   filechar=0x2080  map_inuse=2 (the [OVMXDIR] created earlier)
FID 12  HELLO.TXT;1     filechar=0x0000  map_inuse=2
FID 13  WORLD.TXT;1     filechar=0x0000  map_inuse=2
```

`0x0080` and `0x2000` were then cross-checked against Nankervis's
`access.h` (`FH2$M_CONTIG = 0x80`, `FH2$M_DIRECTORY = 0x2000`, both public,
same repo increments 1-2 already cite) -- confirming BITMAP.SYS, SECURITY.SYS,
and both directories are marked `CONTIGUOUS`, and both directories are also
marked `DIRECTORY`. `INDEXF.SYS` is the only reserved file with more than one
retrieval-pointer extent; `ods2_writer.c` deliberately does NOT reproduce that
fragmentation (see its `[OVMX-inferred]` simplification note) -- it gives
INDEXF.SYS a single contiguous extent instead. Storage-bitmap bit semantics
(1 = free, 0 = allocated; 32-bit-word/4096-bits-per-block packing) are NOT
from this fixture (a raw bit dump wasn't cross-checked against a SHOW
DEVICE/FULL free-block count in this pass) -- they come from Nankervis's
`deallocfile()` in `access.c`, cited in full in `ods2.h`'s WRITER section.

## Addendum (increment 3): lab-2 MOUNT bisection trail

The writer's cross-checked-genuineness step: write a volume with `ods2_writer.c`,
attach it to a **real** OpenVMS VAX V7.3 node on lab-2 (pod `vaxlab-5`, RQ3
temporarily repurposed exactly as the increment-2 procedure above, RX50
geometry: 800 blocks / 10 sectors / 80 tracks / 1 cylinder, chosen to match
the already-proven-working increment-2 geometry), and `MOUNT` it for real.
SIMH's own RQDX3 attach-time probe recognized every image in this trail as
"Contains ODS2 File system" with the correct volume name/format/size --
that only proves the HOME BLOCK parses, not that VMS's own F11X ACP accepts
it. Four rounds of real MOUNT failures were bisected to their root causes:

1. **First real MOUNT attempt**: `Files-11 home block not found on volume`
   preceded by `%MOUNT-W-IDXHDRBAD, index file header is bad; backup used`.
   Root cause found by re-decoding the real fixture's own `fh2_recattr`
   (`hiblk`/`efblk` word order was backwards, see the `ods2_recattr_t`
   comment) and `fh2_backlink` (was zero; must point to the containing
   directory, FID 4/MFD for every reserved file -- see `ods2.h` [F2]).
   Fixed both; MOUNT still failed identically.
2. **Isolating "is IDXHDRBAD+backup-used even survivable?"**: corrupted
   ONLY the real fixture's own FID 1 checksum (leaving its real, correct
   alternate at LBN 24 untouched) -- MOUNT recovered via the backup and
   mounted cleanly. Corrupting BOTH real FID 1 and its real alternate
   reproduced our EXACT failure pair on an otherwise 100% real, working
   volume -- proving the symptom is generically "both index headers look
   bad to MOUNT", not evidence of a deep structural issue elsewhere.
3. **Bisecting our own construction** via three more hybrid images (real
   fixture bytes everywhere except one deliberately swapped structure):
   - Real FID1/BITMAP/etc + OUR home block (fed the real geometry) →
     **mounted cleanly**. Home-block construction exonerated.
   - Real home + OUR reconstructed FID1 header (exact real 3-extent map,
     exact real recattr/backlink, but OUR chosen `fh2_idoffset`/
     `fh2_mpoffset`/`fh2_acoffset` values 54/114/114 instead of real's
     40/100/255) → **failed identically**. Header construction implicated.
   - Real FID1 header verbatim, with ONLY its three offset bytes changed
     to 54/114/114 (ident/map bytes physically relocated to match,
     checksum recomputed, every other field -- fileowner, `fh2_reserved1`,
     fileprot, highwater, recattr, backlink, fid -- untouched) → **failed
     identically**. This isolated the fault to the offset VALUES
     themselves, not their surrounding content.
   - Same test with ONLY `fh2_acoffset` changed back to the real sentinel
     255 (idoffset/mpoffset left at 54/114) → **mounted cleanly**.
     **Root cause: `fh2_acoffset` must be the sentinel 255 ("no ACL
     area"), not an arbitrary "empty area starts right after mpoffset"
     value.** See `ods2.h` [F4].
4. **End-to-end with the writer's own complete output** (all fixes
   applied): MOUNT progressed past the entire index/home/bitmap/MFD
   validation with no IDXHDRBAD at all, then failed differently --
   `%MOUNT-W-QUOTAFAIL` (non-fatal, no `QUOTA.SYS` present, expected) then
   `%MOUNT-F-BADSECSYS, failed to create or access SECURITY.SYS` (fatal).
   Tried SECURITY.SYS as a fake 1-block CONTIG allocation, a size-matched
   (6 blocks, the real fixture's observed size) zero-filled allocation,
   and a zero-length stub (matching the other 6 unpopulated reserved
   files) -- **all three fail identically**. Real MOUNT requires genuine
   ACL-database CONTENT in SECURITY.SYS, not merely its presence or
   correct size. **KNOWN LIMITATION, not resolved in this increment**:
   SECURITY.SYS's internal format is VSI-proprietary and undocumented,
   out of clean-room reach per Rule 8 -- and copying the real fixture's
   own SECURITY.SYS bytes into every OVMX-written volume would be
   redistributing VSI-generated content wholesale, not deriving a format
   from observed behavior, so that path was deliberately NOT taken.

**What this proves and does not.** Steps 1-3 prove, on a real VAX, that
this writer's home block, INDEXF.SYS, BITMAP.SYS/SCB, MFD, and (via the
full end-to-end run reaching well past all of their validation with zero
IDXHDRBAD warnings) directory-record insertion and file creation are
genuinely byte-correct enough for OpenVMS's own F11X ACP to accept and
walk -- not just this reader's own (separately-implemented) validation.
It does NOT prove a full end-to-end `MOUNT` of a fresh, from-scratch
`ods2_writer.c` volume succeeds today: SECURITY.SYS blocks that. A
follow-up item should either research SECURITY.SYS's real structure
through legitimate means (if any exist) or get an operator ruling on
shipping a reduced/no-security-subsystem volume characteristic instead.
