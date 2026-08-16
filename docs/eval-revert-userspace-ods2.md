# Evaluation: reverting the userspace ODS-2 adapter

Status: **evaluation only — nothing in this doc has been executed.** No PR was
closed, no branch deleted, no commit reverted. This is the classification +
plan for the conductor/operator to act on.

## Context

Operator ruling (2026-08-16): a filesystem must be a genuine ACP/XQP **in the
executive** (Rule 9 / INV-6) — not a userspace shim VMS components opt into.
`docs/design-ods2-runtime-flip.md` (still on main, unedited) records the
*opposite* architecture as ratified: "Architecture A1 (ratified,
2026-08-15): the LIVE userspace RMS/DCL/MOUNT path resolves genuine ODS-2
over a block device... A2 (kernel-native ODS-2 driver) is NOT adopted." That
line is now superseded by the operator's 2026-08-16 ruling and should be
flagged, not left standing as if still current (see "Doc hygiene" below). A
parallel effort is designing the real in-executive Files-11 ACP; this
evaluation exists so its foundation (the codec) survives the adapter's
removal.

## 1. Held branches — none merged, trivial to close

Verified via `git merge-base --is-ancestor origin/<branch> origin/main` (all
return false) and `git log origin/main..origin/<branch> --oneline` (all
nonzero):

| Branch | PR | Commits ahead of main |
|---|---|---|
| `work/vms-496-r3-dcl-ods2` | #615 | 1 |
| `work/vms-af7a-rms-working-copy` | #616 | 3 |
| `work/vms-d75-nonrms-writers` | #617 | 1 |
| `work/vms-6f5-r5-mount-ods2` | #618 | 1 |
| `work/vms-dca-search-ods2` | #619 | 1 |
| `work/vms-bd70-ods2-capstone` | (assembled) | 12 |

**Recommendation: close #615–#619 without merging; do not merge the
capstone.** Zero blast radius — none of this touched main's history. This is
the RMS/DCL/MOUNT/$SEARCH/non-RMS-writer reroutes onto `ods2_sysdisk_*` — the
part that would actually have rerouted live VMS components onto the userspace
adapter. It never landed, so there is nothing on main to undo for this layer.

## 2. Merged-to-main — classification

Confirmed live-call graph by `git grep` on `origin/main`: **nothing outside
`src/vmsfs/ods2_sysdisk.c`, `sysdisk.h`, `tests/ods2/*`, and the native-link
scripts calls `ods2_sysdisk_*`.** No RMS, DCL, MOUNT, or non-RMS-writer
consumer on main was ever rerouted — because the branches that would have done
that (§1) never merged. Every merged PR below is substrate that is either
codec (real, reusable) or adapter/prep that is **present but uncalled** —
dead code, not live-wired. That materially lowers the risk of reverting any
of it.

| PR | Commit | Files | Call |
|---|---|---|---|
| #598 | 9df82439 | `ods2/ods2_path.c`, `ods2.h` | **KEEP** — codec (path resolve) |
| #600 | d1ffaa4e | `ods2/ods2_writer.c`, `ods2.h` | **KEEP** — codec (block-backed writer) |
| #603 | 270c71b4 | `vmsfs_volume.c`, `volume.h`, `ovmx_init.c`, CI | **REVERT** — userspace per-process volume table |
| #605 | ae7adcbb | `ods2/ods2_writer.c` | **KEEP** — codec (verbatim RFM=FIXED write) |
| #607 | ffee4c26 | `ods2/ods2_writer.c` | **KEEP** — codec (long filenames) |
| #609 | 65f43645 | `tools/vmsfs_master.c` | **KEEP** — offline volume-building tool |
| #610 | f2811341 | `ods2_sysdisk.c`, `sysdisk.h` (read) | **REVERT** — adapter |
| #611 | 7f3a3606 | `ods2_sysdisk.c`, `sysdisk.h` (write), `ods2_writer.c`, `ods2.h`, `vmsfs_volume.c` | **SPLIT** — see §3, not a clean revert |
| #612 | 9a408236 | `ods2_sysdisk.c` (cross-process registration) | **REVERT** — adapter |
| #613 | e89ac160 | `ods2/ods2_writer.c`, `ods2.h` | **KEEP** — codec (multiversion dir_insert) |
| #614 | 7f0b3eea | `ods2_sysdisk.c` (flock broker) | **REVERT** — adapter (mechanism); concept carries forward, see §3 |
| #620 | e99fa283 | `mk_dcl.sh`, `mk_vmsrms_shr.sh`, `mk_decc_shr.sh`, `run_dcl_native.sh` | **REVERT** — native-link prep for reroutes that never landed |

### KEEP — the codec (do not touch)

`src/vmsfs/ods2/{ods2_reader.c,ods2_writer.c,ods2_bdev.c,ods2_path.c}`,
`src/vmsfs/include/vmsfs/ods2.h`, and their tests
(`tests/ods2/test_ods2*.c` for path/write_bdev/longname/multiversion/dirgrow/
initialize/bdev/security/write/real_image/unit, plus `test_ods2_master.sh`).
This is the byte-level Files-11 codec — the on-disk format read/write engine
— proven against real-VAX volumes across increments 1–9 (commits
51be7eb4…f1630407, pre-dating this session) and extended this session with
block-device backing, long names, multi-version directories, and verbatim
binary writes. It has no opinion about *how* a consumer reaches it (userspace
adapter vs. kernel ACP) — it is the substrate either architecture needs. This
is what the ACP design reuses.

`tools/vmsfs_master.c --ods2` (#609): an **offline** volume-building tool
(format + populate a genuine ODS-2 image from a host source tree, list it
back). Framed in its own comments as "R6-build" for the runtime flip, but its
actual function — mkfs+populate for genuine ODS-2 media — is architecture-
agnostic: an in-executive ACP still needs *something* to master the boot
disk as genuine ODS-2 in the first place. **KEEP**, but its header comments
reference the now-superseded "runtime flip" framing and should be relabeled
when the ACP design lands (doc hygiene, not a code change).

### REVERT — the userspace adapter

- **`src/vmsfs/ods2_sysdisk.c` + `include/vmsfs/sysdisk.h`** (#610 read,
  #612 cross-process env-var registration, #614 flock broker; write half
  split out, see §3). This is literally the thing the operator ruled against:
  a userspace "choke point" (its own header's word) that string-bridges a
  `/vms/...` Linux path to ODS-2 reads/writes, meant for RMS/DCL/MOUNT to
  call directly as a library. Confirmed unused by anything except its own
  tests.
- **`src/vmsfs/vmsfs_volume.c` + `include/vmsfs/volume.h`** (#603). Its own
  header says it outright: *"PROCESS-LOCAL BY CONSTRUCTION... an fd and an
  ods2_bdev_t are per-process, so this table is per-process, populated at
  process start."* That is exactly the wrong layer for an executive facility
  under Rule 9/INV-6 — each process would open its own fd and hold its own
  cache, the opposite of one authoritative kernel-resident mount table. It IS
  fail-honest (reads the real device, never fakes state) so it is not an
  INV-6 violation in the narrow sense, but it is the adapter's foundation and
  should go with it.
- **`ovmx_init.c`'s `register_system_volume()`** + its CMakeLists linkage +
  the CI step + `tests/qemu/test_ods2_boot_register.sh` (#603): PID 1 calls
  the above table at boot, gated behind an unused `ovmx.ods2reg` boot flag,
  silent by default, and fails honest today because the real boot disk isn't
  genuine ODS-2 yet. **Confirmed dormant**: no code path exports
  `OVMX_SYSDISK_DEV` (the env var `ods2_sysdisk.c`'s lazy cross-process
  registration reads) anywhere on main — so even the adapter's own
  cross-process design is only half-wired. Reverting this is safe and low
  effort.
- **`src/vmslink/{mk_dcl.sh,mk_vmsrms_shr.sh}`'s `ODS2_ADAPTER` blocks + the
  matching object-count guards in `run_dcl_native.sh`** (#620): compiles the
  adapter closure (and, incidentally, the codec objects) as internal,
  never-called objects into `DCL.EXE` and `LIBVMSRMS$SHR`, purely to
  pre-position for the R2/R3 reroute branches that are being abandoned (§1).
  Revert the `ODS2_ADAPTER` loops and restore the object counts (31→25 DCL
  objects).
- **`mk_decc_shr.sh`'s `pread`/`pwrite` addition to `DECC$SHR`'s vector**
  (#620): append-only, motivated solely by the adapter closure. Confirmed by
  `git grep` that nothing else on main calls `pread`/`pwrite` through this
  vector. Recommend reverting for hygiene, but flag it as the **lowest-risk
  item to simply leave** if the operator would rather not re-touch a strict
  symbol-vector script for a two-line append: it's inert, append-only
  (GSMATCH LEQUAL-compatible), and the real ACP will very likely need
  `pread`/`pwrite` exported from *somewhere* for a future kernel-backed
  block-device path anyway.

## 3. Where the revert is NOT clean — #611 mixes codec with adapter

**#611 does not split cleanly along the adapter/codec line and a wholesale
revert would destroy real codec capability.** Its diff to
`src/vmsfs/ods2/ods2_writer.c` + `include/vmsfs/ods2.h` adds two functions
that are **not adapter-specific**:

- `ods2_wvolume_open_bdev()` — reattach a writer to an *already-formatted*
  volume (reconstructs the free-block/free-FID bump-allocator watermark from
  the on-disk home block + bitmaps). Any consumer that opens a volume writer
  more than once per process lifetime needs this — including a kernel ACP
  that reopens the volume writer across mount/unmount or per-request.
- `ods2_wvolume_append_file()` — append verbatim bytes to an existing
  RFM=FIXED file (extends the FM2 extent map, updates FH2 EOF fields). This
  is core Files-11 write functionality (an `OPERATOR.LOG`-style append is
  exactly this, repeated) with zero dependency on the userspace path-bridge.

Both are pure `src/vmsfs/ods2/` codec functions — architecturally KEEP — but
they shipped **in the same commit** as the adapter's write half
(`ods2_sysdisk_create_file/_append_file/_mkdir` in `ods2_sysdisk.c`/
`sysdisk.h`) and the `vmsfs_volume.c` `O_RDWR`-first change. Worse: **the
only test coverage for `ods2_wvolume_open_bdev`/`_append_file` is
`tests/ods2/test_ods2_write_adapter.c`**, which is itself an adapter test
(drives the reattach/append primitives *through* `ods2_sysdisk_*`). A
wholesale `git revert` of #611 would silently drop tested, reusable codec
functionality and leave it untested even if manually re-added later.

**Recommendation for #611 (not a single revert commit):**
1. Keep `ods2_wvolume_open_bdev()` / `ods2_wvolume_append_file()` in
   `ods2_writer.c`/`ods2.h` — reclassify KEEP.
2. Revert the adapter-half additions in `ods2_sysdisk.c`/`sysdisk.h`
   (`ods2_sysdisk_create_file/_append_file/_mkdir` and the write-adapter
   header block).
3. Reassess (not urgent either way) the `vmsfs_volume.c` `O_RDWR`-first
   open — harmless to leave (nothing currently writes through a registered
   handle post-revert) or trivial to revert alongside `vmsfs_volume.c`
   wholesale per §2.
4. **Someone needs to write a codec-level test** for
   `ods2_wvolume_open_bdev`/`_append_file` that exercises them directly
   (format → close → open_bdev → append_file → read back), independent of
   `ods2_sysdisk.c`, before `test_ods2_write_adapter.c` is deleted with the
   rest of the adapter's tests — otherwise this capability regresses to
   untested-but-present. This is real follow-up work, not a revert-button
   click.

`#614`'s flock broker is architecturally REVERT (userspace `flock` is not
where an executive ACP's write serialization belongs — the design doc itself
calls it a stand-in for "an executive Distributed Lock Manager resource," not
VMS-authentic), but the *concept* — the one SYS$DISK block device has no
transaction manager and concurrent writers will tear it — is a real problem
the ACP will still have to solve, just with a kernel-side lock instead of
`flock(2)`. Worth a one-line pointer in the ACP design doc, not worth keeping
the userspace mechanism.

## 4. Recommended plan (ordered)

1. **Close #615–#619 without merging; abandon `work/vms-bd70-ods2-capstone`.**
   No main-history impact. (Not operator-reserved — these never merged.)
2. **On main, revert in newest-first order** to avoid conflicts with the
   file-disjoint codec commits interleaved between them:
   - #620 (e99fa283) — native-link adapter-prep, clean revert.
   - #614 (7f0b3eea) — flock broker, clean revert (touches only
     `ods2_sysdisk.c` + its own test/doc section).
   - #612 (9a408236) — cross-process registration, clean revert (same file).
   - #611 (7f3a3606) — **hand-split per §3**, not a single `git revert`.
   - #610 (f2811341) — read adapter, clean revert.
   - #603 (270c71b4) — volume table + PID-1 registration + CI step, clean
     revert.
3. **Leave untouched:** #598, #600, #605, #607, #609, #613 and every
   pre-session ODS-2 increment commit (51be7eb4…f1630407, af70c277, 145018c6)
   — the codec, its provenance, and the real-VAX validation.
4. **Doc hygiene (small, separate change):** `docs/design-ods2-runtime-flip.md`
   should gain a superseded-by notice pointing at the operator's 2026-08-16
   ruling and the new ACP design doc once it exists, rather than being left
   to read as still-ratified guidance ("A2... is NOT adopted"). Not a code
   revert; flagging so it doesn't mislead the next reader.

## 5. Blast radius / what actually breaks

Because none of the reroute branches merged, **reverting the adapter breaks
nothing that currently runs**: `ctest`, the QEMU boot path, and native-link
all currently exercise the adapter only through its own dedicated tests
(`ods2_sysdisk`, `ods2_sysdisk_ensure`, `ods2_write_adapter`,
`ods2_write_broker`, `ods2_volume`, `test_ods2_boot_register.sh`) and the
inert PID-1/DCL.EXE/LIBVMSRMS$SHR linkage — all of which get removed
together with the code they test. `facility_defects_floor.txt` needs no
floor bump (the #603 commit message already confirms the boot test carries
no negctl anchor). No other in-flight work references `ods2_sysdisk_*` or
`vmsfs_volume` (confirmed by repo-wide grep on `origin/main`).

**Pre-existing, unrelated context worth the ACP designer knowing:** there is
already an **in-kernel** `struct vmsfs_volume` + ODS-2-ish core
(`src/kernel/vmsfs/`, `src/kernel-core/vmsfs/`, `src/kernel-netbsd/vmsfs/` —
`vmsfs_alloc.c`, `vmsfs_dirscan.c`, `vmsfs_bio.h`, "substrate-neutral ODS-2
core") backing the `vmsfs.ko` module, using the "bespoke VMFS/VFH2" format
the design doc calls "demoted." This is a **different type with the same
name** as the userspace `vmsfs_volume` library target being reverted here —
no functional collision (separate build, separate translation units), but a
naming collision worth a rename if both survive review, and it is likely the
more relevant starting point (already kernel-resident) for the real ACP than
anything in this evaluation's scope.

## 6. Operator-reserved calls

- **Reverting merged main history** is, per the source-of-truth hierarchy and
  the general caution around undoing shipped work, worth a one-line operator
  confirmation before execution — even though every commit identified here
  is dead code with no live callers. This doc is the go/no-go input; the
  operator ratified the *direction* (2026-08-16) but has not separately
  signed off on rewriting main's history to enact it. Recommend: proceed on
  the plan above unless the operator prefers "leave dormant, let the ACP
  design's own PRs naturally supersede/delete this code as it lands" instead
  of standalone revert commits — both reach the same end state; the
  standalone-revert path is faster and gives a clean `git log` for the next
  reader, the leave-dormant path is zero-effort now but leaves ~1,400 lines
  of dead/misleading (self-describes as "the choke point every consumer
  reroutes onto") userspace-adapter code sitting on main until the ACP PRs
  happen to touch it.
- The #611 hand-split (§3) is a judgment call about which lines are codec vs.
  adapter — reasoned above from the commit's own description and call graph,
  but it's a manual patch, not a `git revert -m`, so it's worth a second pair
  of eyes (or the ACP designer's sign-off that `ods2_wvolume_open_bdev`/
  `_append_file` are indeed what they'd want kept) before executing.
