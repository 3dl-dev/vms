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

## Addendum (increment 4): SECURITY.SYS VBN1 checksum derivation, and the honest current state

Increment 4's job: make an OVMX-written volume `MOUNT` to completion on a
real VAX by giving SECURITY.SYS a genuine data block. This addendum records
what was derived, what was fixed, and -- just as importantly -- what is
still broken and how that was proven not to be about SECURITY.SYS itself.

### The oracle: 12 real `INITIALIZE` + `MOUNT` trials on lab-2

All work below used pod **`vaxlab-8`** (lab-1 not disturbed), repurposing
its `RQ3` unit exactly as increment-2/3's procedure (`sim> detach rq3` /
`set rq3 rx50` (or `rx33` for the size-differential trial) / `set rq3
format=raw` / `attach rq3 <scratch file>` / `cont`, reached by sending the
SIMH WRU character `^E` (0x05) through the console FIFO to break a running
node back to `sim>`). Each trial: `INIT $2$DUA3: <LABEL>`, `MOUNT`, `DUMP/
BYTE $2$DUA3:[000000]SECURITY.SYS` (cross-checked against a raw-image
`xxd`/`cmp -l` pull, since DCL's `DUMP` hex-column byte order turned out to
be unreliable to eyeball -- see below), `DISMOUNT`, pull the raw `.dsk` via
`kubectl cp`.

**Trap hit and worth recording**: DCL's `DUMP` (non-`/BYTE`) ASCII-column
rendering of a hex-grouped line is NOT simply "read the hex left to right".
The FIRST transcription attempt in this session mis-derived several bytes
this way and had to be discarded once a raw `xxd` pull of the same bytes
disagreed with it. **Always cross-check a DCL `DUMP` capture against a raw
disk-image byte read before trusting field boundaries.**

Labels used (chosen to vary length across every residue mod 4, and to
share/vary specific characters for differential isolation): `A`, `B`, `C`
(length 1), `AB`, `AC`, `BB` (length 2), `XYZ` (length 3), `SECTEST`,
`ALPHA9Z` (length 7, different content), `TESTVOL1` (length 8),
`ABCDEFGHIJKL` (length 12, the VMS volume-label maximum), and `BIGVOL` on a
**second device geometry** (RX33/2400 blocks, vs. RX50/800 for every other
trial) to confirm the format does not depend on volume size.

### Finding 1: SECURITY.SYS's VBN1 data block is a real, checksummed record -- not a stub

A fresh volume's `SECURITY.SYS` is NOT all-zero: it has `EFBLK=1`,
`allocated=6` (one populated block, matching the `map_inuse=2`/6-block
CONTIG extent increment-3 already found), and VBN1 contains a small,
deterministic record. Zeroing ONLY the first 4 bytes of an otherwise-real,
untouched, working volume's `SECURITY.SYS` VBN1 (leaving the rest of the
REAL volume alone) turns a clean `MOUNT` into:

```
%MOUNT-F-BADSECSYS, failed to create or access SECURITY.SYS
-SYSTEM-E-BADCHECKSUM, message checksum failure
```

This is direct, induced-error proof that (a) those 4 bytes are a checksum,
and (b) a real MOUNT enforces it -- the actual reason increment 3's
zero-filled/size-matched stubs were rejected (increment 3's own doc
recorded the primary `%MOUNT-F-BADSECSYS` line but not this secondary
status text for the specific zero-length-stub variant it shipped; see
Finding 4 below for why that distinction turned out to matter).

### Finding 2: the checksum algorithm (fully derived, 12/12 samples exact)

Repeating the same label twice (`SECTEST`, init'd at the start and again
after several other labels) reproduced the IDENTICAL checksum both times --
ruling out a time/session-seeded value. Comparing labels that differ by
exactly one character, one length, or one device geometry (see
`tests/ods2/test_ods2_security.c`'s `g_oracle[]` table for the raw
observed values) triangulated:

```
strlen  = length of the volume label
n       = ((78 + strlen) / 4) * 4        (integer division: round down to a multiple of 4)
lane[k] = XOR of every byte at block offset (4 + i) where i in [0, n) and i % 4 == k,  for k in 0..3
checksum (little-endian longword) = lane[0] | (lane[1] << 8) | (lane[2] << 16) | (lane[3] << 24)
```

This is a plain byte-lane XOR fold, computed over a domain that starts
right after the checksum field and extends through the label's own text
(rounded down to a 4-byte boundary -- for `strlen == 1` this rounds all
the way back to the fixed prefix, discarding the single label byte
entirely, which is why one-character labels are a genuine, verified edge
case: `A`/`B`/`C` all reproduce the SAME checksum `0x000f084d`). Standard
CRC-32 (multiple polynomial/init/reflection/xorout combinations) and
additive-16/32 sums were tried FIRST and did not reproduce any sample;
the byte-lane XOR fold reproduces all 12 samples across 2 device
geometries exactly. See `ods2_security_checksum()` in
`src/vmsfs/ods2/ods2_writer.c` and its reader-side twin
`ods2_security_parse()` in `ods2_reader.c`.

### Finding 3: field layout

Differencing same-length-different-label and same-label-different-geometry
samples pinned:

- Offset `0x08` (1 byte): `0x52 + strlen` -- the block offset one past the
  end of the label text (an "end of variable data" pointer).
- Offset `0x1C` (4 bytes): the volume's owner UIC (`ods2_uic_t` layout).
  Every trial used the implicit default (no `/OWNER_UIC`), reading back as
  `[1,4]` (SYSTEM) -- consistent with, but not independently proven
  against, the same field's role elsewhere on the volume.
- Offset `0x50` (word): `strlen + 4`.
- Offset `0x52` (`strlen` bytes): the label text, ASCII, not padded beyond
  its own extent.
- Everything else in `0x04..0x4F`: byte-for-byte IDENTICAL across all 12
  trials regardless of label OR geometry -- a fixed "zero ACL entries"
  template, reproduced verbatim (`ods2_security_template[]`) as
  deterministic structure, not copied VSI content (see ods2.h's [F6]
  provenance comment for the full reasoning on why this is NOT the "copy
  one real file's bytes into every volume" approach increment 3 correctly
  rejected).

### Finding 4: fixing the checksum was NOT enough -- a second, unrelated failure

With `ods2_security_build()` producing a byte-exact, oracle-validated
checksum, a full volume from `ods2_volume_format()` was dumped and MOUNTed
on lab-2. Result:

```
%MOUNT-W-QUOTAFAIL, failed to activate quota file; volume locked
-SYSTEM-W-FILENUMCHK, file identification number check
%MOUNT-F-BADSECSYS, failed to create or access SECURITY.SYS
-SYSTEM-W-FILENUMCHK, file identification number check
```

`QUOTAFAIL` is a red herring, confirmed separately: `MOUNT/NOQUOTA`
suppresses it entirely and does not change `BADSECSYS`'s outcome. The real
puzzle is `BADSECSYS` now citing **`FILENUMCHK`, not `BADCHECKSUM`** --
i.e. a DIFFERENT secondary status than the one the checksum fix targets.
Bisection trail, each step lab-2-verified on pod `vaxlab-8`:

1. **`fh2_recattr.fat_efblk` set to 6 (map_count) instead of the real
   fixture's own value.** Fixed to match the real fixture (which, re-
   decoded carefully this pass via that fixture's OWN home block rather
   than an assumed `hdr_base`, is `hiblk=6/efblk=2` -- NOT `efblk=1` as an
   earlier note in this file mis-stated). No change to the MOUNT outcome.
2. **Home block owner UIC `[0,0]` instead of `[1,4]`.** This writer never
   set `hm2_volowner` anywhere (a pre-existing gap, not previously
   flagged). Patched to SYSTEM `[1,4]`, checksums recomputed. No change.
3. **The decisive test: splice the REAL fixture's own COMPLETE
   SECURITY.SYS header** (every field -- owner UIC, fileprot, highwater,
   efblk, idoffset/mpoffset, checksum, everything) into this writer's own
   volume, with ONLY its retrieval-pointer LBN rewritten to point at this
   writer's own correctly-checksummed data block (its `security_lbn`,
   computed from this writer's own layout arithmetic, not the real
   fixture's). **STILL fails identically with `FILENUMCHK`.** This
   conclusively proves the defect is NOT in SECURITY.SYS's own header or
   content -- both were, at this point, 100% real bytes (or real-
   algorithm-derived bytes) pointing at each other correctly.
4. **Directory entries.** A control MOUNT of an untouched real fixture came
   back completely clean (no `QUOTAFAIL`, no `BADSECSYS` at all -- proving
   these are NOT unconditional/expected messages on a healthy volume, as
   an earlier increment-3 note had assumed). Decoding that real fixture's
   own `[000000]` MFD data block (`strings` over the raw bytes at its data
   LBN) showed it lists ALL 10 reserved files by name (`000000.DIR`,
   `INDEXF.SYS`, `BITMAP.SYS`, `BADBLK.SYS`, `CORIMG.SYS`, `VOLSET.SYS`,
   `CONTIN.SYS`, `BACKUP.SYS`, `BADLOG.SYS`, `SECURITY.SYS`), not just
   caller-created ones -- this writer inserted none of them. Added (see
   ods2.h's [F8]). **Still no change to the MOUNT outcome**, even combined
   with step 3's real-header splice.
5. **`maxfiles` reduced from 200 to 13** (matching the real fixture's
   actual file count, to rule out some large-pre-reserved-index-file
   interaction). **No change.**
6. **Regression control**: re-built `main`'s pre-increment-4 output
   (the original zero-length SECURITY.SYS stub) and MOUNTed it fresh.
   It ALSO shows `FILENUMCHK`, not `BADCHECKSUM` -- meaning this exact
   secondary status was already present before increment 4 touched
   anything; increment 3's own PROVENANCE text describing `BADCHECKSUM`
   for "a zero-length stub" did not capture this. (The BADCHECKSUM
   behavior IS real and reproducible -- see Finding 1 -- but only when
   induced onto an otherwise-conventional, fully-populated real volume,
   not onto this writer's structurally-different-in-some-other-way
   output.)

**Conclusion**: the specific, named increment-3 defect (a real MOUNT
enforcing SECURITY.SYS's content checksum) IS resolved and IS validated
(Finding 1's induced-error test, Finding 2's 12-sample-exact algorithm).
A full real-VAX MOUNT of this writer's own complete volume output still
does not reach completion, but the remaining cause is proven to be
somewhere else in the volume's structure -- most likely the already-
documented [OVMX-inferred] simplification where `INDEXF.SYS`'s own
retrieval map is written as ONE contiguous extent instead of the real
fixture's 3 fragmented extents (the only OTHER "not reproduced, presumed
OK" structural difference left in the writer). **Not bisected further in
this increment** -- out of the SECURITY.SYS-focused scope this work was
chartered under. Recommended next step: repeat step 3's splice technique,
but for `INDEXF.SYS`'s OWN header/map, on a real fixture, to test whether
a single-contiguous-extent `INDEXF.SYS` alone reproduces `FILENUMCHK` on
an otherwise-100%-real volume.

## Addendum (increment 5, `vms-0f3`): DUMP/ANALYZE characterization + MOUNT/OVERRIDE test, and the RECOMMENDATION

This increment's charter was narrow and bounded: (1) `DUMP`/`ANALYZE` a
real freshly-`INITIALIZE`d volume's `SECURITY.SYS` from the OS's own tool
vantage (not just raw-byte decode) to double-check increment 4's
characterization, (2) confirm increment 4's writer output against a fresh
lab-2 MOUNT, (3) test `MOUNT/OVERRIDE=SECURITY` as a candidate pragmatic
path, and (4) recommend A (derivable)/B (override)/C (wall). Lab-2 pod
`vaxlab-3` (fresh scratch RX50 unit on RQ3, restored to its original
CD-ROM attach afterward; lab-1 and no other pod's state touched).

### Step 1: `DUMP`/`ANALYZE` on a fresh real `INITIALIZE`, cross-checking increment 4

`$ INITIALIZE DUA3: OVMXI5 /STRUCTURE=2` + `$ MOUNT DUA3: OVMXI5` (clean,
`%MOUNT-I-MOUNTED`, no warnings). `DUMP/HEADER/BLOCK=(START:0,COUNT:1)`
on `SECURITY.SYS` confirms the file header increment 4 already derived:
`End of file block 1 / Allocated 6`, `Highest block: 6`, `Contiguous`,
`Map area words in use: 2`, owner `[SYSTEM]`. `DUMP/BLOCK=(START:1,COUNT:1)`
(the VBN1 data block itself) shows the volume label `OVMXI5` at the
expected offset (row `000040`, matching increment 4's `SECURITY_LABEL_OFF
= 0x52`) and, decisively: **every byte from roughly offset 0x66 through
the end of the 512-byte block (offsets `000060`-`0001E0` in the dump) is
literal zero.** This is the OS's own `DUMP` utility confirming, from a
different vantage than increment 4's raw-byte diffing, that a freshly
initialized volume's default `SECURITY.SYS` VBN1 content is NOT an
elaborate ACL database -- it is a small (~0x66-byte) fixed header +
label, followed by ~410 bytes of nothing. `ANALYZE/DISK_STRUCTURE` on the
same volume reports only the expected (harmless) `QUOTA.SYS` open failure
and raises nothing about `SECURITY.SYS` -- the analyzer itself does not
treat this file as needing further content. **This corroborates increment
4's Finding 3 rather than changing it**, from an independent tool vantage,
and rules out "the DUMP-decode missed something" as a residual doubt.

### Step 2: current writer output vs. lab-2, fresh confirmation

Built `tests/ods2/test_ods2_write.c` (`ODS2_WRITE_DUMP=/tmp/ovmx-i5.dsk`),
`kubectl cp`'d it into the pod (md5 verified equal before/after), attached
it as `RQ3`, `SIMH` recognized it (`Contains ODS2 File system`, correct
volume name/format/size). `MOUNT DUA3: OVMXWRIT`:

```
%MOUNT-W-QUOTAFAIL, failed to activate quota file; volume locked
-SYSTEM-W-FILENUMCHK, file identification number check
%MOUNT-F-BADSECSYS, failed to create or access SECURITY.SYS
-SYSTEM-W-FILENUMCHK, file identification number check
```

Exactly reproduces increment 4's end state on a fresh lab-2 trial
(different pod, same writer code) -- no regression, no improvement from a
plain `MOUNT`. Consistent with increment 4's splice-test proof that this
`FILENUMCHK` is not caused by `SECURITY.SYS`'s own content.

### Step 3: `MOUNT/OVERRIDE=SECURITY` -- mounts, but does NOT satisfy the acceptance bar

```
$ MOUNT/OVERRIDE=SECURITY DUA3: OVMXWRIT
%MOUNT-W-QUOTAFAIL, failed to activate quota file; volume locked
-SYSTEM-W-FILENUMCHK, file identification number check
%MOUNT-I-MOUNTED, OVMXWRIT mounted on _$2$DUA3: (VAX1)
```

`MOUNT/OVERRIDE=SECURITY` DOES suppress the fatal `BADSECSYS` and DOES
report `MOUNT-I-MOUNTED`. But the volume this produces is not actually
usable: every attempt to touch the file system through DCL after this
mount fails with the SAME `FILENUMCHK` the mount itself carried as a
warning --

```
$ DIRECTORY/SIZE $2$DUA3:[000000]
%DIRECT-E-OPENIN, error opening $2$DUA3:[000000]*.*;* as input
-RMS-E-DNF, directory not found
-SYSTEM-W-FILENUMCHK, file identification number check
$ TYPE $2$DUA3:[OVMXDIR]HELLO.TXT
%TYPE-W-SEARCHFAIL, error searching for $2$DUA3:[OVMXDIR]HELLO.TXT;
-RMS-E-DNF, directory not found
-SYSTEM-W-FILENUMCHK, file identification number check
```

Even a bare `[000000]` (MFD) directory listing and a direct-by-name file
open both fail. `/OVERRIDE=SECURITY` only silences the mount-time error
message for the specific subsystem it names; it does nothing about the
underlying, SECURITY.SYS-unrelated defect (per increment 4's splice
proof, most likely the `INDEXF.SYS` single-contiguous-extent
simplification) that makes every subsequent file lookup fail the same
`FILENUMCHK` check. **vms-0f3's acceptance criterion is "MOUNTs to
completion... and files read back"** -- this arm gets the MOUNT message
but not the file read-back, so it does not meet the bar.

### RECOMMENDATION: **A** for SECURITY.SYS specifically -- the format IS clean-room derivable and IS already implemented; **not B** (override doesn't deliver a working volume) as a solution to vms-0f3's stated goal; the real remaining wall is a DIFFERENT, already-identified defect outside this item's scope

- **SECURITY.SYS's on-disk content is DERIVABLE (A), and increment 4 already derived and shipped it.** Two independent characterization passes now agree (increment 4's byte-diff triangulation across 12 samples, and this increment's direct `DUMP`/`ANALYZE` on a fresh sample): the "ACL database" a real MOUNT enforces is, on a default volume, a small fixed checksum+header+label template followed by zero-fill -- not an open, VSI-proprietary ACL structure requiring undocumented content. `ods2_security_build()`/`ods2_security_parse()` in `src/vmsfs/ods2/ods2_writer.c` / `ods2_reader.c` already implement this, oracle-validated. **No further work on SECURITY.SYS's format is indicated.**
- **`MOUNT/OVERRIDE=SECURITY` is NOT a viable path to vms-0f3's goal.** It changes the mount-time message but does not produce a volume where files "read back" -- confirmed empirically this increment (Step 3). Do not document it as a supported workaround.
- **The actual remaining wall is the pre-existing `FILENUMCHK`, proven (increment 4, splice test) to be independent of SECURITY.SYS.** This item's scope was SECURITY.SYS; that scope is closed. The `INDEXF.SYS` single-contiguous-extent bisection increment 4 already recommended as the next step remains the right next move, but it is a SEPARATE defect and should be tracked as a new/distinct item (increment 6), not folded back into vms-0f3.
- **vms-600 (real VAX mounts an OVMX-served volume) stays blocked** -- not by SECURITY.SYS content (resolved) but by the `FILENUMCHK` defect this item did not investigate further, per its effort cap.
## Addendum (increment 6, vms-0f3): INDEXF.SYS genuinely fragmented -- FILENUMCHK resolved, BADIRECTORY is the new wall

Increment 6's job: chase FILENUMCHK to a real-VAX-MOUNT completion, following
increment 5's recommendation. Method: local field-by-field diffing of this
writer's own output against `real_vax_ods2.dsk` (already in the repo -- no
new lab session needed for DERIVATION), each fix validated on lab-2 (pod
**`vaxlab-7`**, RQ3 scratch unit, procedure identical to prior increments;
lab-1 and no other lab-2 pod touched).

### Fixes applied, in the order tried

1. **`wvol->mfd_fid.fid_seq` bug**: was hardcoded to 1 (the "newly-created
   file, first generation" convention) instead of `ODS2_FID_MFD` (4, what
   the MFD's OWN on-disk header actually self-declares, per [F2]). Every
   caller-created top-level file/dir's `fh2_backlink` is built from this
   struct, so it silently disagreed with the MFD header it pointed at --
   the same FID self-consistency class of bug Nankervis's `accesshead()`
   (access.c) rejects. **Tried alone on lab-2: MOUNT reproduced FILENUMCHK
   identically -- not sufficient alone**, but a real, kept fix (see
   `ods2_writer.c`'s `[F10]` comment).
2. **`fh2_fileowner`/`fh2_fileprot`/`fh2_reserved1`/`fh2_highwater`**: a
   full field-by-field diff of every one of the 13 real headers against
   this writer's own output (script below) found FOUR previously-zero
   fields that are consistently non-zero on every real sample: owner
   SYSTEM [1,4] (all 13), protection 0xFA00 (system/data files) or 0xBA00
   (directories), a constant 0x0000FE00 at offset 56 correlating exactly
   with "is one of the 10 traditionally-reserved files" (0 otherwise), and
   `highwater == hiblk + 1` (all 13, once derived correctly -- see
   `ods2_writer.c`'s `[F11]` comment for the full per-field derivation).
   **Tried together on lab-2: MOUNT reproduced FILENUMCHK identically --
   not sufficient either**, but real, oracle-grounded, and kept.
3. **INDEXF.SYS's own retrieval map, genuinely fragmented into 3 extents**
   (the leading candidate both increments 4 and 5 flagged but never
   actually tried): re-derived the exact real shape by decoding
   `real_vax_ods2.dsk`'s own FID1 header extents directly (not just citing
   the increment-3 addendum's summary) --
   `[(0,3), (altidxlbn,1), (ibmaplbn, ibmapsize+headers)]` -- and found
   the REAL reason INDEXF.SYS is fragmented at all: `BITMAP.SYS`'s own
   data (LBN 5-6) and `000000.DIR`'s own data (LBN 3-4) sit PHYSICALLY
   BETWEEN the home-block pair and the index file bitmap on the real
   volume -- this writer previously placed both AFTER the header area
   instead, which is why a single contiguous INDEXF.SYS extent "worked"
   internally (nothing else occupied that range) but didn't match reality.
   Reordered the writer's own physical layout to match (MFD data, then
   BITMAP.SYS data, then the index file bitmap + header area, then the
   alternate index header, then SECURITY.SYS -- see `ods2_writer.c`'s
   `[F12]` comment) and gave INDEXF.SYS a genuine 3-extent map via a new
   `write_fh2_header_ext()` (extent-array) primitive.
   **Lab-2 result: MOUNT's secondary status changed from
   `-SYSTEM-W-FILENUMCHK` to `-SYSTEM-W-BADIRECTORY, bad directory file
   format`** -- a real, reproducible state transition, proving the
   single-contiguous-extent simplification WAS a genuine contributor to
   FILENUMCHK (not proof it was the *only* one, given fixes 1-2 were
   already in the image at this point, but a real fix nonetheless).
4. **Directory entries not stored in ascending name order**: decoding the
   real fixture's OWN `[000000]` MFD directory block RECORD-BY-RECORD
   (not just via `strings`, as increment 4's `[F8]` pass did) showed its
   11 entries in strict ascending byte order by filename -- `000000.DIR,
   BACKUP.SYS, BADBLK.SYS, BADLOG.SYS, BITMAP.SYS, CONTIN.SYS, CORIMG.SYS,
   INDEXF.SYS, OVMXDIR.DIR, SECURITY.SYS, VOLSET.SYS` -- NOT this writer's
   own FID-creation-order insertion. `ods2_wvolume_dir_insert()` now finds
   the correct sorted position and shifts the tail right instead of always
   appending (`[F13]` in `ods2_writer.c`). **Lab-2 result: still
   `BADIRECTORY`, no change** -- real and oracle-grounded (now byte-exact
   with the real fixture's own entry order), but not (solely) the cause.
5. **Regression found while implementing #4**: the sorted mid-block
   insert's word-alignment pad byte (only present for odd-length names,
   e.g. `OVMXDIR.DIR`, 11 chars) is opened by a `memmove()` and was left
   holding stale pre-shift bytes instead of the `0xFF` empty-fill
   convention every other unused byte in the block follows. Fixed by
   explicitly `memset`-ing the vacated gap before the field writes.
   **Lab-2 result: still `BADIRECTORY`, no change** to the outcome, but a
   real byte-cleanliness bug regardless of whether it was load-bearing.

### Honest current status

`FILENUMCHK` is resolved (state-transition-confirmed on a real VAX, not
merely inferred). The new wall, `-SYSTEM-W-BADIRECTORY, bad directory file
format`, appears in the exact same MOUNT phase (immediately after
`QUOTAFAIL`, immediately before `BADSECSYS`) while VMS scans `[000000]` by
name for `QUOTA.SYS`/`SECURITY.SYS` -- i.e. it is very likely still about
`[000000]`'s own directory-file validity, but the specific defect survived
both the sort-order fix and the pad-byte fix, so it is NOT (solely) a
content-ordering or stray-byte issue in the directory data block itself.

**Diagnostic attempted and blocked**: tried `ANALYZE/DISK_STRUCTURE
$2$DUA3:` (both `/FOREIGN`-mounted and fully dismounted) on lab-2 to get a
direct, specific report instead of continuing to bisect blind --
consistently failed with `%ANALDISK-F-GETDVI, error getting device
characteristics, RVN 1 / -SYSTEM-F-WRONGACP, wrong ACP for device`,
apparently a lab/emulated-RQDX3 tooling limitation unrelated to this
volume's own format (a completely untouched real fixture would need to be
tried against the same command to confirm whether this is lab-wide or
volume-specific -- not done this increment, out of the remaining lab-trial
budget).

**Not yet tried / recommended next**: (a) confirm `ANALYZE/DISK_STRUCTURE`
even works in this lab config at all, against the known-good real fixture,
before spending further trials assuming it's usable; (b) a decisive splice
test in the style of increment 4's step 3 -- take the REAL fixture's own
complete, byte-exact `[000000]` FH2 header AND data block verbatim, graft
them into this writer's own volume (translating only the retrieval-pointer
LBN to point at this writer's own physically-placed MFD data), and see
whether `BADIRECTORY` disappears; if it does, the defect is provably still
in the MFD's header or data despite passing every check tried so far; if
it does NOT, the defect is elsewhere entirely (a different reserved file,
or the home block, or something in how MOUNT's quota/security phase itself
navigates to `[000000]`) and the search should move there. (c) Field-by-
field diff BITMAP.SYS's and INDEXF.SYS's headers again post-`[F12]`
reordering (not done this increment) in case the layout change introduced
a new, different discrepancy from the real fixture on top of resolving
the extent-count one.

### Reproduction

```
$ sim> detach rq3 / set rq3 rx50 / set rq3 format=raw / attach rq3 <scratch>.dsk
$ (VMS) MOUNT $2$DUA3: OVMXWRIT
%MOUNT-W-QUOTAFAIL, failed to activate quota file; volume locked
-SYSTEM-W-BADIRECTORY, bad directory file format
%MOUNT-F-BADSECSYS, failed to create or access SECURITY.SYS
-SYSTEM-W-BADIRECTORY, bad directory file format
```

Local field-diff script used for fixes 1-2 (re-derivable any time from the
fixture already in this repo, `tests/ods2/real_vax_ods2.dsk`, plus a fresh
`ODS2_WRITE_DUMP=/tmp/x.dsk ./bin/test_ods2_write` build): decode each of
the 13 real headers' `fh2_fileowner`/`fh2_fileprot`/offset-56 word/
`fh2_highwater` alongside the equivalent FID's header in the writer's own
dump (same `hdr_base + (fid - 1)` LBN formula both sides) and diff
field-by-field. No new lab session is needed to REPRODUCE this derivation
-- only to VALIDATE a candidate fix's effect on a real MOUNT.

## Addendum (increment 7, vms-0f3): splice-isolated BADIRECTORY to `dir_verlimit`; a SECOND directory-file defect remains

Increment 7's charter: run the splice diagnostic increment 6 recommended
(graft the real fixture's own `[000000]` MFD header+data, verbatim, into
this writer's own volume) to localize `BADIRECTORY`, fix what it finds,
and prove a real-VAX state transition. Lab-2 pod **`vaxlab-4`** (RQ3
scratch unit, procedure identical to prior increments; restored to its
original `cdrom`/ISO attach afterward; lab-1 and no other lab-2 pod
touched). All splicing/diffing done with local Python scripts operating
directly on `tests/ods2/real_vax_ods2.dsk` and a fresh writer dump -- no
new lab session needed for the diagnosis itself, only for validating each
candidate against a real MOUNT.

### Step 1: the prescribed splice -- decisive, and answers the isolation question

Took the real fixture's own complete FID4 (`000000.DIR`) FH2 header and
its first directory data block, byte-exact, and grafted them into this
writer's own volume: the header's retrieval pointer was rewritten to
point at a copy of that same real data placed in a completely unused
region of this writer's volume (LBN 797-798, far from anything the
writer's own layout touches for a 3-file volume), and the header
checksum recomputed over the patched bytes -- every other header field
(backlink, fid, fileprot, owner, idoffset/mpoffset/acoffset, recattr,
ident, filechar) is 100% the real fixture's own bytes. Confirmed offline
first (checksum recomputes correctly, map area decodes to the new LBN,
all 11 directory records decode in the same ascending order the real
fixture uses) before spending a lab trial.

**Real-VAX result: `%MOUNT-I-MOUNTED, OVMXWRIT mounted on _$2$DUA3:
(VAX1)` -- clean, with NO warnings at all** (no QUOTAFAIL, no BADSECSYS,
no BADIRECTORY). This directly answers increment 6's isolation question:
**the defect IS in this writer's own `[000000]` MFD header/data**, not in
SECURITY.SYS, not in INDEXF.SYS's map, not elsewhere in the volume-wide
layout -- all of those are apparently fine once `[000000]` itself is
real. `DIRECTORY/SIZE $2$DUA3:[000000]` on the spliced volume lists all
11 entries with correct sizes.

### Step 2: the same splice exposes a SECOND, independent defect

`DIRECTORY/SIZE $2$DUA3:[OVMXDIR]` on that same cleanly-mounted, spliced
volume -- i.e. `[OVMXDIR]`'s own header and directory data, 100% this
writer's own construction, untouched by the splice -- still fails:

```
%DIRECT-E-OPENIN, error opening $2$DUA3:[OVMXDIR]*.*;* as input
-RMS-E-FND, ACP file or directory lookup failed
-SYSTEM-W-BADIRECTORY, bad directory file format
```

This proves the SAME class of defect (BADIRECTORY) exists independently
in this writer's directory-creation path in general, not only in
`ods2_volume_format()`'s MFD-population loop -- i.e. fixing `[000000]`
alone would not have been sufficient even if this writer's own MFD
defect were fixed.

### Step 3: `dir_verlimit` field-confusion bug found, fixed, and confirmed real -- but NOT sufficient alone

Byte-by-byte decoding of the real fixture's own `[000000]` MFD block
(all 11 records) and `[OVMXDIR]` block (both records) found
`ods2_wvolume_dir_insert()` writing the wrong value into `dir_verlimit`
(offset+2 of each directory record): it wrote the caller's `version`
argument (the entry's OWN version number), but the real fixture shows a
DIFFERENT, per-name POLICY value there -- `0x0001` for all 10
reserved-file entries (their version is always 1, so this coincidentally
matched and hid the bug), and `0x7FFF` ("no limit set") for every
caller-created entry (`OVMXDIR.DIR` in the MFD, and `HELLO.TXT`/
`WORLD.TXT` in `[OVMXDIR]` itself) -- values that do NOT match the
version-number hypothesis. Fixed in `ods2_wvolume_dir_insert()`
(`src/vmsfs/ods2/ods2_writer.c`) and documented as `[F14]` in `ods2.h`:
`dir_verlimit = version` when `entry_fid.fid_num <= ODS2_RESFILES`, else
`ODS2_DIR_VERLIMIT_DEFAULT` (`0x7FFF`). Offline tests (`ctest -R ods2`)
stay green; the fixed writer's own `[000000]` and `[OVMXDIR]` blocks now
match the real fixture's `dir_verlimit` byte-for-byte in every record.

**This is a real, oracle-grounded, kept fix -- but it does NOT resolve
BADIRECTORY by itself.** Re-tested twice on lab-2 with the fix applied:
(a) the full unspliced writer output still shows `BADIRECTORY` on
`[000000]` at MOUNT time, identical to before the fix; (b) the fixed
writer's volume with the SAME `[000000]` real-data splice as step 1
mounts clean, but `[OVMXDIR]` still shows the exact same `BADIRECTORY`
as step 2. The `dir_verlimit` bug was real but not load-bearing for this
particular failure -- the same "real, kept, not sufficient alone"
pattern increments 4-6 already hit repeatedly with other fields.

### Step 4: the next candidate, found but NOT fixed this increment (effort-capped)

Field-by-field diffing this writer's own `[OVMXDIR]`/`[000000]` FH2
headers against the real fixture's (script logic: decode `fat_hiblk`/
`fat_efblk` as two hi/lo 16-bit words per `ods2_recattr_t`'s documented
layout, both sides) found:

```
real  FID11 (OVMXDIR.DIR): hiblk=1  efblk=2  (1 block allocated)
ours  FID11 (OVMXDIR.DIR): hiblk=1  efblk=1
real  FID4  (000000.DIR):  hiblk=2  efblk=2  (2 blocks allocated)
ours  FID4  (000000.DIR):  hiblk=1  efblk=1  (this writer's MFD is only
                                               1 block, a pre-existing,
                                               separately-flagged
                                               [OVMX-inferred] size
                                               simplification)
```

`OVMXDIR.DIR`'s real `efblk` (2) EXCEEDS its own `hiblk`/allocated size
(1) -- the same "+1, full-block" convention increment 6's `[F12]` already
found for `INDEXF.SYS`/`BITMAP.SYS`, generalizing to directory files too.
But `000000.DIR`'s real values (`hiblk==efblk==2`, no `+1`) do NOT fit
that same rule at face value with only ONE real sample of each shape to
compare (a 1-block-allocated directory vs. a 2-block-allocated one) --
not enough to safely generalize a rule without risking a regression on
the field that's already known to matter for MOUNT. **Not fixed this
increment, per the effort cap (isolate + fix ONE wall, do not chase
subsequent ones).** Flagged as the leading next candidate for increment
8, with the concrete numbers above and the recommendation to get a
THIRD real sample (a directory with >1 allocated block but a partially-
written last block, e.g. one holding enough entries to need a second
block with only one entry in it) to disambiguate hiblk/efblk's actual
rule before generalizing it into the writer.

## Addendum (increment 8, vms-0f3): `dir_rec` efblk/hiblk fixed -- FULL CLEAN MOUNT achieved, files list, new wall on plain-file content

Increment 8's charter: diagnose and fix the ONE remaining `[OVMXDIR]`
`BADIRECTORY` defect increment 7 isolated to this writer's own
directory-file construction, then prove a real-VAX state transition.

### Diagnosis: no new lab session needed -- direct byte decode of the fixture already in the repo

Increment 7 flagged, but did not pursue (effort-capped), a field-by-field
`fh2_recattr` diff of this writer's `[OVMXDIR]`/`[000000]` headers against
`tests/ods2/real_vax_ods2.dsk`'s own FID11/FID4, noting `OVMXDIR.DIR`'s
real `efblk` (2) exceeds its own `hiblk` (1) and flagging that a single
real sample wasn't enough to safely generalize a rule. This increment
re-derived both fields with a small local Python script doing a proper
struct-level decode (home block -> `hdr_base` -> FH2 header -> recattr ->
map-area format-1 extent decode), not a transcription, confirming exactly:

```
FID  4 (000000.DIR):  extent=[LBN 3, count 2]   hiblk=2  efblk=2  ffbyte=0
FID 11 (OVMXDIR.DIR):  extent=[LBN 31, count 1]  hiblk=1  efblk=2  ffbyte=0
```

FID11's `efblk=2` genuinely exceeds its own 1-block allocation -- not a
decode error. This matches the documented Files-11/RMS convention that
EFBLK/FFBYTE express an end-of-file *position*, not an allocation size:
when the last valid byte lands exactly on a block boundary, EFBLK is set
to the block *following* the last one with data (FFBYTE 0), even if that
following block was never separately allocated. FID4's `hiblk==efblk==2`
is the SAME rule applied to a file whose trailing block happens to
already be allocated -- not a contradiction once "efblk = (last block
containing data) + 1" is the invariant instead of "efblk == hiblk".
Every directory this writer itself creates (the MFD and every
caller-created directory, via `ods2_wvolume_create_dir()` and the
MFD-population path) is a single CONTIG block, so `efblk = hiblk + 1,
ffbyte = 0` generalizes cleanly to both without needing to reproduce the
real fixture's 2-block MFD (a separate, already-flagged, still-open
simplification).

### Fix

`write_fh2_header_ext()`'s `FH2_KIND_DIR` branch in `ods2_writer.c` (the
recattr-computation `else` arm for `total_count > 0`) now sets
`efblk = total_count + 1` for directories, keeping `hiblk = total_count`
and `ffbyte = 0`. Documented as `[F15]` in `ods2.h`. Offline `ctest -R
ods2` stays green (no existing test asserts directory hiblk/efblk, so no
regression risk there); `-Wall -Wextra` clean.

### Validation: lab-2 pod `vaxlab-9` (the SAME pod increment 2 originally
collected `real_vax_ods2.dsk` on), RQ3 scratch unit, procedure identical
to every prior increment; restored to its original `cdrom`/ISO attach
afterward; lab-1 and no other lab-2 pod touched.

```
$ MOUNT $2$DUA3: OVMXWRIT
%MOUNT-I-MOUNTED, OVMXWRIT mounted on _$2$DUA3: (VAX1)
```

**Completely clean -- zero warnings.** No `QUOTAFAIL`, no `BADIRECTORY`,
no `BADSECSYS`. This is the first fully-clean real-VAX `MOUNT` of an
all-OVMX-written volume across all 8 increments of this effort.

```
$ DIRECTORY $2$DUA3:[OVMXDIR]
Directory $2$DUA3:[OVMXDIR]
HELLO.TXT;1         WORLD.TXT;1
Total of 2 files.

$ DIRECTORY $2$DUA3:[000000]
Directory $2$DUA3:[000000]
000000.DIR;1        BACKUP.SYS;1        BADBLK.SYS;1        BADLOG.SYS;1
BITMAP.SYS;1        CONTIN.SYS;1        CORIMG.SYS;1        INDEXF.SYS;1
OVMXDIR.DIR;1       SECURITY.SYS;1      VOLSET.SYS;1
Total of 11 files.
```

Both directories, including the writer's own MFD, are now fully
traversable by a real VAX. **This meets vms-0f3's acceptance criterion's
directory-traversal half.**

### The file-content half: a new, separate wall found (NOT chased, per this increment's effort cap)

```
$ TYPE $2$DUA3:[OVMXDIR]HELLO.TXT
$
```

`TYPE` returns to the prompt with no output and no error message.
`DUMP` of the same file proves the bytes genuinely are on disk:

```
$ DUMP $2$DUA3:[OVMXDIR]HELLO.TXT
File ID (12,1,0)   End of file block 1 / Allocated 1
Virtual block number 1 (00000001), 512 (0200) bytes
 72657469 72772032 2D53444F 20584D56 4F206568 74206D6F 7266206F 6C6C6568 hello from the OVMX ODS-2 writer 000000
 00000000 ... 0000000A ................................ 000020
 (zero-fill through end of block)
```

The exact 34-byte string `ods2_wvolume_create_file()` was given
(`"hello from the OVMX ODS-2 writer\n"`) is present verbatim at the start
of the block. The likely cause: this writer stores plain-file content as
a raw byte stream, but the file's own `fh2_recattr` declares
`rattrib=0x02` (variable-length records, implied carriage control) --
under that record format, RMS expects each on-disk record to begin with
its own 2-byte little-endian record-length word, which this writer never
writes. `TYPE`'s record-oriented read likely misparses the first two
content bytes (`he` = `0x6568`) as a bogus record length and gives up
without emitting anything. **Not diagnosed further or fixed this
increment** -- charter was `[OVMXDIR]`'s `BADIRECTORY` specifically, and
that defect is resolved and lab-confirmed. Recommended as **increment 9's
target**: implement RMS variable-length-record framing (or determine
`ods2_wvolume_create_file()`'s intended record attribute is wrong and it
should mark files as `rattrib=0` "no record processing"/stream-LF
instead -- both are legitimate, undecided design choices, not yet
distinguished by any oracle observation this increment made).

### `ANALYZE/DISK_STRUCTURE`: confirmed lab-wide, not volume-specific

Per increment 6's open question: retried `ANALYZE/DISK_STRUCTURE
$2$DUA3:` on pod `vaxlab-4`, both against this writer's own (dismounted)
output AND -- the new check -- against the pristine, untouched, known-good
`real_vax_ods2.dsk` fixture attached fresh to the same pod. **Both fail
identically**:

```
%ANALDISK-F-GETDVI, error getting device characteristics, RVN 1
-SYSTEM-F-WRONGACP, wrong ACP for device
```

This settles increment 6's open question: `WRONGACP` is a **lab-wide
RQDX3/tooling limitation on this pod**, not specific to any OVMX-written
volume's format. `ANALYZE/DISK_STRUCTURE` is not usable as a diagnostic
on this lab config at all; do not spend further trials on it here.

## Addendum (increment 9, vms-0f3): RMS variable-length-record framing -- full read-back achieved, vms-600 acceptance met

Increment 9's charter: increment 8 left ONE wall -- `TYPE` of a real-VAX-
mounted, all-OVMX-written data file printed nothing, even though `DUMP`
proved the exact bytes were on disk.

### Diagnosis: no new lab session needed for the on-disk framing rule -- direct byte decode of the fixture already in the repo

`tests/ods2/real_vax_ods2.dsk`'s own FID12/FID13 (`HELLO.TXT`/`WORLD.TXT`)
are REAL data written by a real VAX `COPY` (of `SYS$MANAGER:SYSTARTUP_VMS.COM`
and `SYCONFIG.COM` respectively) -- i.e. genuine, already-collected oracle
evidence of exactly the on-disk shape this increment needed, requiring no
new lab-2 session to establish. A small local Python script fully decoded
both files' raw bytes as a stream of `{2-byte LE length}{content}` records:

```
FID 12 HELLO.TXT:  412 records decoded, final offset 17218 bytes
                    == (efblk=34-1)*512 + ffbyte=322 EXACTLY
FID 13 WORLD.TXT:   16 records decoded, final offset 720 bytes
                    == (efblk=2-1)*512  + ffbyte=208 EXACTLY
```

Both files decode CLEANLY end-to-end into their real, human-readable
source text (the actual `SYSTARTUP_VMS.COM`/`SYCONFIG.COM` boilerplate),
confirming three rules simultaneously:

1. Each record is a 2-byte little-endian length word followed by that
   many content bytes.
2. A single `0x00` pad byte follows whenever the content length is ODD,
   keeping the next record's length word on an even (word) offset. No
   other padding exists.
3. **Records MAY straddle a 512-byte block boundary.** An initial
   hypothesis (matching this increment's own task brief) was that RMS
   pads out to the next block whenever a record would not fit -- this
   was directly DISPROVED by the decode: `HELLO.TXT`'s real record 13
   starts at byte 492 of block 1 and ends at byte 39 of block 2, with no
   padding or gap at the boundary. A block-boundary-avoidance
   implementation would have been a genuine, oracle-contradicted bug.

`fh2_recattr`'s `rtype`/`rattrib` were ALREADY correct on this writer
(`ODS2_RTYPE_VAR`=2, `ODS2_RAT_CR`=0x02, set since increment 3) -- the
defect was purely that `ods2_wvolume_create_file()` wrote `data` as a raw,
unframed byte copy. A second, smaller defect was also found by this same
decode: `fat_rsize` is NOT a fixed 0 for data files as increment 3's
writer comment claimed (never itself re-checked against this field) --
the real values are 105 and 66, each exactly the LONGEST record actually
written to that file (`fat_maxrec` stays 0 in both real samples, matching
"no cap specified").

### Fix

`ods2_wvolume_create_file()` (`ods2_writer.c`) now treats caller-supplied
bytes as newline-delimited text lines -- `[OVMX-inferred]`, this writer's
own design choice for its own byte-buffer API, not read off any oracle --
and frames each line as one on-disk VAR record via a new two-pass
`ods2_var_frame_lines()` helper: pass 1 sizes the framed stream (and finds
the longest record, for `fat_rsize`) before data blocks are allocated;
pass 2 writes the identical framing into the now-allocated blocks. The
file's `fh2_recattr.fat_ffbyte`/`fat_efblk` are computed from the FRAMED
length, not the caller's raw `data_len`, and `fat_rsize` is patched
post-write to the longest record's length -- both documented as `[F16]`
in `ods2.h`.

`ods2_reader.c` gained `ods2_var_records_decode()` (decode a VAR-record
stream into newline-joined text, given the file's true valid byte count)
and `ods2_file_read_text()` (the single-extent convenience wrapper,
computing that byte count via the new `ods2_recattr_data_bytes()` inline
helper in `ods2.h`), so the round-trip test exercises the SAME decode a
real VAX's RMS performs, not a bypass of the on-disk framing.
`tests/ods2/test_ods2_write.c` was updated to use it, and gained a new
`MANYLINE.TXT` case (40 lines, 640 on-disk bytes, spanning 2 allocated
blocks with the boundary landing mid-record) to regression-guard finding
3 above. Offline `ctest -R ods2` stays green (4/4); `-Wall -Wextra` clean
on all four changed files.

### Validation: lab-2 pod `vaxlab-9` (the same pod increments 2/8 used), RQ3 scratch unit, procedure identical to every prior increment; restored to its original `cdrom`/ISO attach afterward; lab-1 and no other lab-2 pod touched

```
$ MOUNT $2$DUA3: OVMXWRIT
%MOUNT-I-MOUNTED, OVMXWRIT mounted on _$2$DUA3: (VAX1)

$ TYPE $2$DUA3:[OVMXDIR]HELLO.TXT
hello from the OVMX ODS-2 writer

$ TYPE $2$DUA3:[OVMXDIR]WORLD.TXT
genuine Files-11 bytes

$ TYPE $2$DUA3:[OVMXDIR]MANYLINE.TXT
line  0 text
line  1 text
...
line 39 text
```

All 40 `MANYLINE.TXT` lines printed exactly, including the ones whose
on-disk record straddles the block-1/block-2 boundary -- confirming
finding 3 above on the real hardware, not just in the offline decode.

`DIRECTORY/FULL` independently confirms the RECATTR fields this fix
computes, from VMS's OWN parse of the header this writer wrote (not an
OVMX self-report):

```
$ DIRECTORY/FULL $2$DUA3:[OVMXDIR]HELLO.TXT
...
Record format:      Variable length, maximum 0 bytes, longest 32 bytes
Record attributes:  Carriage return carriage control
...
Total of 1 file, 1/1 block.
```

`strlen("hello from the OVMX ODS-2 writer")` (the file's one real record,
trailing `\n` stripped) is exactly 32 -- VMS's own "longest 32 bytes"
matches this writer's computed `fat_rsize` exactly.

**This is full `vms-600` acceptance: a real VAX both `MOUNT`s an
OVMX-written ODS-2 volume cleanly (increment 8) AND reads its files'
actual content via `TYPE`/`DIRECTORY/FULL` (this increment) -- the
directory-traversal half and the file-content half are both closed.**
