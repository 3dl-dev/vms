# Design record: the ODS-2 runtime flip (epic vms-5eb, rungs R2/R3/R5/R6)

Status: **IN PROGRESS — read-path foundation landed; the atomic flip is NOT
complete and this branch does NOT boot-to-login on a genuine ODS-2 SYS$DISK.**
This record exists so the conductor can re-plan: driving the four "flip" rungs
surfaced two load-bearing dependencies the epic's rung decomposition did not
account for. See "Newly-surfaced blockers" below.

Architecture A1 (ratified, epic note 2026-08-15): the LIVE userspace
RMS/DCL/MOUNT path resolves genuine ODS-2 over a block device via the
`src/vmsfs/ods2/` library; the bespoke `vmsfs.ko` VMFS/VFH2 format is demoted;
the `vmsfs_to_linux_path -> /vms -> POSIX` passthrough is retired for SYS$DISK.
A2 (kernel-native ODS-2 driver) is NOT adopted. FLAG-1 = clean break (no
migrator).

---

## 0. Progress since this record (update 2026-08-15)

The two "newly-surfaced blockers" §4 flagged are now LANDED as additive
foundations, and the first flip rung's builder primitive is landed:

- **B1 (block-backed WRITER) — LANDED.** `ods2_wvolume_format_bdev()` +
  `ods2_wvolume_create_file_raw()` (verbatim RFM=FIXED, for binary `.EXE`) +
  long-filename support (fi2_filename + fi2_filenamext, up to 86 chars) are in
  `src/vmsfs/ods2/ods2_writer.c` (tests `ods2_write_bdev`, `ods2_longname`).
- **B2 (userspace mounted-volume table) — LANDED.** `src/vmsfs/vmsfs_volume.c`
  + `include/vmsfs/volume.h` (`vmsfs_volume_register/_handle/_unregister`);
  PID 1 `register_system_volume()` (`src/ovmx_init/ovmx_init.c:506`) already
  registers the boot device DKA0: additively (fails honest today because the
  boot disk is still VMFS, not DECFILE11B). Test `ods2_volume`.
- **R6-build (boot-master → genuine ODS-2) — builder primitive LANDED,
  default NOT yet flipped.** `tools/vmsfs_master.c` gained an ADDITIVE `--ods2`
  master + list mode (env `OVMX_MASTER_ODS2`), mirroring how INITIALIZE gained
  `--ods2` (vms-6ef). It lays the host source tree onto a genuine DECFILE11B
  volume via `ods2_wvolume_format_bdev` + `create_dir`/`create_file_raw`/
  `dir_insert`, and `--ods2 list` reads it back over the real block device via
  `ods2_bdev_*`. Test `ods2_master` (`tests/ods2/test_ods2_master.sh`) drives
  the real binary end-to-end incl. the fail-honest NOTODS2 path. **The default
  remains VMFS** so boot is unaffected — the atomic group must flip the default
  (Dockerfile.bootable / callers to `--ods2`, or `OVMX_MASTER_ODS2=1`) TOGETHER
  with R6-kernel + R6-mount + R2 + R3 + R5 (§5 atomicity).
  - File-content policy chosen: EVERY regular file is written VERBATIM
    (`create_file_raw`), not just binaries — this keeps the whole volume
    byte-identical to today's POSIX `/vms` reads (the A1 read-path property).
    **OPEN R2 decision:** whether RMS should instead see `.COM`/`.TXT` as
    RFM=VAR records (real-VMS record format) vs verbatim stream bytes. Not
    taken here; the verbatim default is the conservative byte-faithful choice.

- **SYS$DISK read ADAPTER — LANDED (update 2026-08-15, this branch).**
  `src/vmsfs/ods2_sysdisk.c` + `src/vmsfs/include/vmsfs/sysdisk.h`
  (`ods2_sysdisk_owns_path` / `_resolve_file` / `_read_file` / `_list_dir`),
  built into the `vmsfs_volume` static library, test `ods2_sysdisk`
  (`tests/ods2/test_ods2_sysdisk.c`, 26 assertions, green). This is the ONE
  bridge R2/R3/R5 all consume but that B2 did not yet offer: it turns a
  resolved `"/vms/A/B/.../NAME.EXT;ver"` Linux path — the exact string every
  `/vms` consumer already computes via `vmsfs_to_linux_path` — into an
  `ods2_bdev` directory-component walk + file resolve/read/list over the
  registered SYS$DISK volume handle (`vmsfs_volume_handle(SYSDISK_DEVICE)`).
  Proven: verbatim binary read-back byte-identical (the A1 property; via
  `create_file_raw`), `;ver`/highest version selection, directory listing, and
  every fail-honest edge (no volume → `SS$_DEVNOTMOUNT`, non-`/vms` →
  `SS$_BADPARAM`, missing file → `SS$_NOSUCHFILE`, undersized buffer →
  `SS$_DATACHECK`). Additive; **flips no live path** (boot unaffected). Rule 8:
  adds no on-disk fact — only sequences the reader's existing primitives.
  **This is the READ half only.** The reroute of each consumer ONTO it, and the
  write half (below), remain in the atomic group.

- **⚠ NEWLY CONFIRMED (2026-08-15) — boot-to-login is NOT read-only on
  SYS$DISK.** A full trace of PID 1 → STARTUP → LOGINOUT → `Username:` found
  exactly two GUARANTEED per-boot writers to SYS$DISK, and — critically — both
  bypass RMS entirely (plain `fopen`/`mkdir` on the `/vms` tree via
  `vmsfs_to_linux_path`), so §4's "B1 = RMS `$PUT`/`$CREATE`" framing was
  incomplete:
  1. **OPERATOR.LOG append** — `src/ovmx_init/opcom_kmsg.c:258-270`,
     `fopen(SYS$MANAGER:OPERATOR.LOG, "a")` on the PID 1 `/dev/kmsg` follower
     thread, every boot once the disk is mounted.
  2. **LASTLOGIN rewrite + mkdir** — `src/libvms/rtl/ovmx_accounting.c:158-199`
     (+ `ensure_dir` :43-51), temp-file write + `rename()` of
     `SYS$MANAGER:LASTLOGIN/<USER>`, plus `mkdir` of the dir, on every
     interactive login (called from `tools/vms_login.c:145`, before the shell).
  SYSUAF/RIGHTSLIST/SYSGEN-params/VMS$PHASES/VMS$VMS/all images are read-only
  in this window (SYSUAF is consulted, never written at login). **Implication:**
  the atomic flip's write path is (a) an ODS-2 runtime WRITER over the LIVE
  registered volume — append-to-existing-file (OPERATOR.LOG), create-file
  (LASTLOGIN temp), and mkdir — which needs the volume opened O_RDWR (today
  `vmsfs_volume_register` opens O_RDONLY) and the `ods2_wvolume_*` block writer
  driven against the mounted device, AND (b) rerouting these two NON-RMS
  `fopen`/`mkdir` callers, not only RMS `$CREATE`/`$PUT`. The `ods2_wvolume_*`
  block-backed create/alloc/dir_insert/grow primitives already exist (§0 B1);
  what is missing is the runtime "append a record to an existing file" +
  O_RDWR-handle + non-RMS-caller reroute. This is the true long pole.

- **CROSS-PROCESS lazy SYS$DISK registration — LANDED (update 2026-08-15,
  this branch). The atomic flip's MISSING SUBSTRATE, surfaced while wiring the
  reroute.** `vmsfs_volume.c`'s mounted-volume table is process-local by
  construction (an fd + `ods2_bdev_t` are per-process); only PID 1 registers
  DKA0: (`register_system_volume()`). But the flip's LIVE consumers — RMS in
  the LOGINOUT process, DCL in the shell image, every RUN'd image — execute in
  OTHER processes whose tables start EMPTY, so `vmsfs_volume_handle(SYSDISK_
  DEVICE)` returns NULL there and every rerouted read/write would fail
  `SS$_DEVNOTMOUNT` — the reroute could NEVER reach login. No landed rung
  covered this (§4 B2 named "every consumer moves to the volume handle" but
  provided no cross-process registration path). Closed by a static
  `sysdisk_handle()` in `ods2_sysdisk.c`: returns the registered handle if
  present, else lazily registers SYS$DISK in the calling process from the boot
  device path PID 1 exports in `OVMX_SYSDISK_DEV`, then returns it. Every
  adapter entry point (read + write) now routes through it. Fail-honest: no /
  bad channel → NULL → `SS$_DEVNOTMOUNT`, table stays empty (Rule 9 / INV-6).
  Additive; **flips no live path** (`OVMX_SYSDISK_DEV` is exported by nobody
  yet — that wiring is part of the atomic group's R6-mount step). Test
  `ods2_sysdisk_ensure` (`tests/ods2/test_ods2_sysdisk_ensure.c`) drives the
  adapter as a fresh RMS/DCL process would (never registers itself): proves
  lazy register, byte-identical read-back, idempotence, and both fail-honest
  edges. **OVMX design choice (labelled, Rule 8):** the VMS-faithful refinement
  is to translate the `SYS$SYSDEVICE` logical → boot device and resolve its
  backing store; a later rung substitutes that inside `sysdisk_handle()`
  without touching a single consumer.

**⚠ RE-PLAN — the two remaining long poles are BIGGER than §3/§4 framed
(update 2026-08-15, grounded by reading every reroute site).**

1. **R2-RMS record I/O is a POSITIONED-I/O reroute, not a call-swap.** RMS's
   sequential/relative/indexed engines (`rms_seq.c`, `rms_rel.c`, `rms_idx.c`)
   are built ENTIRELY on a POSIX `fd` + `lseek`/`read`/`write` at arbitrary
   offsets — 60+ sites: append-at-`SEEK_END`, random `lseek(rec_offset)`,
   in-place status-byte deletes, record rewrites, `SEEK_CUR` cursors. The
   SYS$DISK adapter offers only whole-file `read_file`/`append_file`/
   `create_file`/`list_dir` — NO positioned read/write and NO fd. The doc's
   "reroute record I/O off `fab->_linux_fd`" is therefore a substantial
   sub-project, not a line edit. Two honest routes, both bigger than a rung:
   (a) **per-open working copy (checkout/checkin):** on `$OPEN`/`$CREATE` of a
   SYS$DISK file, `ods2_sysdisk_read_file` the whole file into a private
   working fd (`memfd`/tmpfile), keep `fab->_linux_fd` pointing at THAT so
   seq/rel/idx stay byte-for-byte unchanged, and on `$CLOSE` write the working
   copy back as a new ODS-2 version via the write adapter. Honest (the store is
   genuine ODS-2; the working fd is a per-channel window, like a VMS RMS
   buffer), lowest-risk, but rewrites the whole file on any write and needs the
   `.rms_meta` indexed sidecars (`rms_core.c:532/:600`, `rms_idx.c:516/:568`)
   moved off `/vms` too. (b) **positioned bdev I/O:** extend the adapter with
   `ods2_sysdisk_pread/pwrite(fid, off, …)` mapping file offset → FM2 extent
   LBN, and swap the fd for a `{volume, fid, offset}` cursor in
   `rms_read_exact`/`rms_write_exact`. More faithful, much larger. **Route (a)
   is the recommended first cut.** Either way R2 is its own multi-session pole.

2. **No partial reroute keeps the suite green — the flip is genuinely atomic
   with the DEFAULT-FLIP and the TEST FIXTURES.** Debug ctest and the boot
   image run RMS/DCL against a POSIX `/vms` tree with NO ODS-2 volume
   registered. The moment a consumer is rerouted "use the adapter when
   `ods2_sysdisk_owns_path()`", every existing RMS/DCL test that touches a
   `/vms` path fails `SS$_DEVNOTMOUNT` (fail-honest is mandatory — a POSIX
   fallback is the forbidden LARP). So the reroute CANNOT land incrementally
   green: R6-build's default flip to genuine ODS-2, PID 1 exporting
   `OVMX_SYSDISK_DEV`, AND every RMS/DCL test fixture switching to an ODS-2
   backing volume must co-land with R2/R3/R5. This is the real content of §5's
   "atomic" and is why this branch's progress is red-in-CI until the whole
   group lands — expected, not a regression.

Remaining atomic group (still must land together, boot-gated): R6-kernel,
R6-mount, R2-RMS (record-I/O working-copy model above), R3-DCL, R5-MOUNT,
**the write-half reroutes (Wr)**, test-fixture switch to ODS-2, Retire, Proof.
Current-state anchors:

| Rung | Anchor (current) |
|------|------------------|
| R6-build (flip default) | `tools/vmsfs_master.c` `--ods2` DONE; flip caller in `distro/Dockerfile.bootable` (mastering step ~line 713) to `--ods2` / `OVMX_MASTER_ODS2=1` |
| R6-kernel | `src/kernel/vmsfs/vmsfs_super.c:387` (`VMSFS_HOME_MAGIC` check) — under A1 the SYS$DISK kernel MOUNT is RETIRED, so this is home/SCB parse for boot-device VALIDATION only, not a POSIX mount |
| R6-mount | `src/ovmx_init/ovmx_init.c` `bare_metal_init` (stop `mount(...,"vmsfs")` at `ovmx_boot_linux.c:158`; rely on `register_system_volume()` volume handle — which must open O_RDWR for the write half); `tools/vms_mount_helper.c` |
| **read adapter** | **DONE — `src/vmsfs/ods2_sysdisk.c` (`ods2_sysdisk_read_file`/`_resolve_file`/`_list_dir`/`_owns_path`); test `ods2_sysdisk`. R2/R3/R5 reroute their read/list sites onto this.** |
| **cross-process registration** | **DONE — `sysdisk_handle()` in `src/vmsfs/ods2_sysdisk.c` lazily registers SYS$DISK per-process from `OVMX_SYSDISK_DEV`; all adapter entry points route through it; test `ods2_sysdisk_ensure`. R6-mount must have PID 1 EXPORT `OVMX_SYSDISK_DEV=<boot dev>` so children inherit the channel.** |
| R2-RMS | `src/vmsrms/rms_core.c` `resolve_filename` (:413), `rms_impl_open` read-open at `:702` (O_RDONLY); drop `rms_validate_path_boundary` (:207) for SYS$DISK; `$SEARCH` `opendir`/`readdir` (`rms_search.c:173/:186`) → `ods2_sysdisk_list_dir`. **Record I/O off `fab->_linux_fd` = the POSITIONED-I/O sub-project (see RE-PLAN #1): recommended first cut = per-open working copy — on open `ods2_sysdisk_read_file` → `memfd`/tmpfile → `fab->_linux_fd`; seq/rel/idx (`rms_util.c:15/:34` + `lseek` in rms_seq/rel/idx) stay UNCHANGED; on `$CLOSE` write back via the write adapter. Move `.rms_meta`/idx sidecars (`rms_core.c:532/:600`, `rms_idx.c:516/:568`) off `/vms` too.** Write-open (`rms_impl_create` `:808` O_CREAT) → Wr. |
| R3-DCL | `src/vmsdcl/dcl_cmd_file.c` `dir_collect` (:280)/`cmd_directory` (:648, `vmsfs_to_linux_path` at :686/:690) → `ods2_sysdisk_list_dir`; `src/vmsdcl/dcl_cmd_set.c` `cmd_set_default` (:97) → `ods2_sysdisk_resolve_file`/`_list_dir` to validate the dir |
| R5-MOUNT | `src/vmsdcl/dcl_cmd_misc.c` `cmd_mount` (:2211) — validate home via `ods2_home_parse` + SCB via `ods2_scb_parse`, reject non-ODS-2 |
| **Wr (write half)** | NEW long pole. (a) `vmsfs_volume` O_RDWR handle + a `ods2_sysdisk_write`/`_append`/`_mkdir` twin driving `ods2_wvolume_*` over the LIVE mounted device; (b) reroute the two guaranteed non-RMS boot writers — `opcom_kmsg.c:258` OPERATOR.LOG append, `ovmx_accounting.c:158`/`:43` LASTLOGIN write+mkdir — plus RMS `rms_impl_create`/`$PUT`, off `vmsfs_to_linux_path`+`fopen`/`open(O_CREAT)` onto it |

---

## 1. The boot data-flow as it exists today (what the flip must replace)

The genuine ODS-2 library (`src/vmsfs/ods2/`) is real and validated against a
real OpenVMS VAX volume, but at runtime it is wired into **exactly one** place:
`tools/vms_initialize.c` as an opt-in `--ods2` / `OVMX_INIT_ODS2` branch. The
entire boot spine runs on the bespoke VMFS format and reaches files by POSIX:

```
host tree /system-stage/vms  (SYS0/SYSCOMMON/{SYSEXE,SYSLIB,SYSMGR,SYSHLP}, USERS, SYSTMP)
   │  tools/vmsfs_master.c  "master ... OVMXSYS ... 64"   (bespoke VMFS/VFH2, vmsfs_ondisk.h)
   ▼
/boot/ovmx-distrib.img       (distro/Dockerfile.bootable:714; round-trip-verified)
   │  QEMU -drive file=...,if=virtio   (distro/boot/run-qemu.sh)
   ▼
/dev/vda   (guest block device)
   │  PID 1 ovmx_init: load vmsfs.ko; mount("/dev/vda","/vms","vmsfs")
   │        (src/ovmx_init/ovmx_boot_linux.c:156; vmsfs.ko checks hb_magic==VMSFS_HOME_MAGIC
   │         at src/kernel/vmsfs/vmsfs_super.c:387)
   ▼
/vms == SYSDISK_MOUNT ; DKA0: ; SYS$SYSTEM = /vms/SYS0/SYSCOMMON/SYSEXE  (ovmx_layout.h)
   │
   ├─ RMS/DCL/IMGACT/LOGINOUT/INSTALL/provisioning ALL reach files by translating VMS
   │   filespecs -> /vms/... via vmsfs_to_linux_path, then open(2)/opendir(2)/read(2)/write(2)
   └─ DCL MOUNT of other disks: cmd_mount -> vms_mount_helper "mount ... vmsfs" (+ readlabel)
```

`/dev/vms` is the `vms.ko` **executive** char device (locks/EF/AST) — it is NOT
the system disk. The system disk is a **block** device (`/dev/vda`) whose
contents `vmsfs.ko` presents as an ordinary POSIX tree at `/vms`. That POSIX
tree is the substrate the whole system reads today.

## 2. What this branch delivers (landed, tested, atomic-safe)

The read-path FOUNDATION that R2/R3/R5 all require but the library did not yet
offer over a block device: **block-backed ODS-2 path resolution + content
read** — `src/vmsfs/ods2/ods2_path.c`, declared in
`src/vmsfs/include/vmsfs/ods2.h`:

- `ods2_bdev_dir_find()` — find `{name, version, fid}` in a directory header.
- `ods2_bdev_resolve_dir()` — walk MFD (FID 4) -> component -> component to a
  directory's header + FID (a "SYS0" component is the on-disk `SYS0.DIR`).
- `ods2_bdev_resolve_file()` — resolve `[dir.sub]NAME.EXT;ver` to a file
  header + FID (version 0 = highest).
- `ods2_bdev_read_file()` / `ods2_bdev_read_file_text()` — read a file's
  content by pread-ing its FM2 retrieval-pointer extents (multi-extent
  supported); the `_text` variant decodes RFM=VAR records to newline-joined
  text.

Rule 8: adds NO on-disk format facts — it only sequences directory traversal +
extent reads the reader already implements from its cited Files-11 sources.
Name matching (`SYS0`->`SYS0.DIR`, `LOGIN.COM;3`) is VMS filespec semantics.

Proof: `tests/ods2/test_ods2_path.c` (ctest `ods2_path`, green) builds a
genuine ODS-2 volume carrying the real system-disk hierarchy shape
(`[000000]->[SYS0]->[SYS0.SYSCOMMON]->[SYS0.SYSCOMMON.SYSEXE]` with files),
lays it on a real loop-image fd, and drives the resolver over that fd:
resolves the directory through a real MFD->SYS0->SYSCOMMON->SYSEXE FID chain
(never POSIX opendir), lists its real records, resolves files by highest and
exact version, and reads their content back byte-for-byte — verified against
`ods2.h` structs, never POSIX `stat`/`opendir`. This is also exactly the
`ods2_volume_format` + `ods2_wvolume_create_dir/create_file/dir_insert`
sequence the R6 boot-master builder will use, so it doubles as a builder proof.

This branch flips **no live path**: it is purely additive (a new library TU +
its unit test + this doc). Boot is unaffected; nothing is half-flipped on the
runtime path.

## 3. Remaining work to complete the flip (in dependency order)

R6-build  `tools/vmsfs_master.c` `do_master`/`emit_tree`/`write_header`/
          `write_dir_data`/`write_home` -> rebuild on `ods2_volume_format` +
          `ods2_wvolume_create_dir/create_file/dir_insert` (pattern proven in
          `format_volume_ods2`, `tools/vms_initialize.c:365`, and in
          `test_ods2_path`); add `ods2` to `tools/CMakeLists.txt:293`.
R6-kernel `src/kernel/vmsfs/vmsfs_super.c:387` magic/checksum/geometry -> parse
          the ODS-2 home/SCB (kernel analog of `ods2_home_parse`/`ods2_scb_parse`).
          The one piece the userspace library does not cover.
R6-mount  fstype `"vmsfs"` at `ovmx_boot_linux.c:158`, `ovmx_boot_netbsd.c`,
          `tools/vms_mount_helper.c:148`; and `read_vmsfs_label` -> ODS-2 home
          block. INITIALIZE default flip (`vms_initialize.c:575`).
R2-RMS    `rms_core.c` `resolve_filename`/`rms_impl_open` -> resolve via
          `ods2_bdev_*` (this branch's `ods2_path.c`) instead of
          `vmsfs_to_linux_path`+`open(2)`; drop `rms_validate_path_boundary`
          for SYS$DISK. Reroute record I/O off `_linux_fd`
          (`rms_record.c`/`rms_seq.c`/`rms_rel.c`/`rms_idx.c`) and `$SEARCH`
          off `opendir` (`rms_search.c`) onto the bdev volume. `ovmx_link_rms_io.c`
          is a pure RMS client — converts transparently.
R3-DCL    `cmd_directory`/`dir_collect` (`dcl_cmd_file.c`) -> list via
          `ods2_bdev_resolve_dir` + `ods2_bdev_list_dir` (real names/versions/
          FIDs; `/FULL` can finally emit File ID). `cmd_set_default`
          (`dcl_cmd_set.c`) -> validate the directory against the real MFD.
R5-MOUNT  `cmd_mount` (`dcl_cmd_misc.c`) -> validate home block via
          `ods2_home_parse` + SCB via `ods2_scb_parse`, REJECT a non-ODS-2
          device, set the volume label from the real home block. Optionally
          teach `sys_device.c` `fill_dvi_item` DVI$_VOLNAM from the home block.
Retire    `vmsfs_ondisk.h`, `format_volume` in `vms_initialize.c`, the bespoke
          halves of `vmsfs_master.c`, and the `vmsfs_master_roundtrip` test wiring.
Proof     negctl anchors `rms-open-uses-posix-passthrough` +
          `mount-is-getcwd-logical-alias` (+ `facility_defects_floor.txt` bump);
          `tests/dcl/test_set_default_root.sh` asserts real-ODS-2 behaviour;
          boot-smoke + install-e2e green BY SHA on a genuine ODS-2 SYS$DISK.

## 4. Newly-surfaced blockers (NOT in the epic's rung plan — conductor call)

These two dependencies were discovered while mapping the flip. Neither is
covered by R1/R4/R8 (which delivered the block-backed reader, INITIALIZE, and
in-memory writer completeness). Both gate a boot-to-login flip. They should be
filed as rungs and sequenced before R2/R5 can land green.

**B1 — Block-backed ODS-2 WRITER (create/put/erase/allocate over a block
device via pwrite).** `src/vmsfs/ods2/ods2_writer.c` operates on a caller-owned
**whole-volume in-memory image**; there is no way to create a file, append a
record, extend an allocation, or maintain a directory *on a mounted block
device*. But under A1, RMS `$CREATE`/`$PUT`/`$ERASE`, indexed writes, and any
boot-time write to SYS$DISK (SYSUAF last-login updates, accounting, operator
log, temp files, STARTUP-authored files) must write real ODS-2 blocks. Without
B1 the write path has no honest backing: the only alternatives are (a) keep a
POSIX write fallback — the exact silent-fallback LARP the authenticity
invariants forbid (INV-6, CLAUDE.md Rule 9) — or (b) fail every write, which
will not reach login. B1 is a Rule-8, oracle-grounded effort comparable to the
original writer (R3/R4/R8) and is the critical long pole.

**B2 — System-wide dismantling of the POSIX `/vms` passthrough + a userspace
mounted-volume model + a new PID-1 boot mount.** Today `vmsfs_to_linux_path ->
/vms` is called "at the point of each syscall" by RMS, DCL, IMGACT, LOGINOUT,
INSTALL, and provisioning — not just RMS/DCL. A1 requires the system disk to
stay a raw ODS-2 block device that userspace reads via `ods2_bdev`, which means:
(i) PID 1 registers `/dev/vda` into a userspace mounted-volume table
(device -> open fd + cached `ods2_bdev_t`) instead of `mount("/dev/vda","/vms",
"vmsfs")`; (ii) the device table (`src/vmsfs/vmsfs_device.c`, today device ->
mount-point string) gains a per-device fd/volume handle (the RMS mapper
identified this as the natural home); (iii) **every** `/vms` consumer, not only
the four flip rungs, moves to the volume handle. R2/R3/R5 as written flip
RMS/DCL/MOUNT but implicitly assume this substrate exists. The atomic group is
therefore larger than R2+R3+R5+R6: it must also carry B2, or boot breaks the
moment `/vms` stops being a POSIX tree.

## 5. Atomicity assessment

R2/R3/R5/R6 cannot land in a state that boots to login without B1 and B2. This
branch deliberately stops at the tested, additive read foundation rather than
land a half-flip that breaks boot (the constraint the dispatch and CLAUDE.md
Rule 9 both impose). Recommended sequencing: file B1 and B2 as rungs; land B1
(block-backed writer) and B2 (volume-handle substrate + boot mount) as
additive foundations the way R1/R4/R8 were; then the true atomic group
(R2+R3+R5+R6, all consuming B1/B2 + this branch's `ods2_path.c`) lands and is
gated on a real QEMU boot-to-login on a genuine ODS-2 SYS$DISK.

### 5.1 Write-path concurrency: the single-writer serialization broker (vms-49d)

The write half (§0, the `ods2_sysdisk_create_file` / `_append_file` / `_mkdir`
adapter over the block-device-backed writer) has a concurrency hazard the read
half does not. At boot **three independent processes write the one SYS$DISK
with no transaction manager**: PID 1's OPERATOR.LOG append (opcom), the login
LASTLOGIN write, and RMS `$PUT`. Each adapter entry point opens a per-call
writer with `ods2_wvolume_open_bdev`, which **reconstructs the free-block and
free-FID watermarks by reading the on-disk bitmaps**, then mutates and flushes.
Two such spans interleaving on the same device tear the volume two ways: both
`open_bdev` calls read the *same* watermark and allocate the *same* LBN/FID, and
two `dir_insert` read-modify-writes of one parent directory block clobber each
other (last writer wins, dropping the other entry). Because the watermark is
read *inside* `open_bdev`, the critical section must begin *before* it.

**This must land before the write-half reroutes** (NonRMSWriters `vms-d75`,
R2-checkin `vms-af7a`) — those make the three boot writers real, so the
serialization has to exist first. `vms-49d` is that additive broker.

**Mechanism (chosen): `flock(fd, LOCK_EX)` on the volume block-device fd**, held
across the whole `open_bdev -> read-modify-write -> flush -> close` span of each
write entry point, released after. `flock` is a real OS-level, *inode*-
associated advisory lock: two different processes — each with its own open file
description on the device, exactly the boot case, since every process lazily
registers SYS$DISK for itself via `sysdisk_handle()` — contend on it and are
serialized. It is INV-6 clean: a kernel lock on the shared object, never a
per-process fake. It is placed at the adapter entry points (`ods2_sysdisk.c`),
not down inside the writer, precisely because the watermark read must be inside
the critical section. Proven by `tests/ods2/test_ods2_write_broker.c`, which
drives fork()'d, barrier-released children (the real cross-process case; threads
share one open file description and would not exercise the lock) and asserts
byte-exact survival of every concurrently-written file plus complete directory
listings — and which fails if the lock is neutered.

**VMS-authentic path (possible future refinement, NOT built now):** the faithful
mechanism is an executive Distributed Lock Manager resource — `sys$enq` of an
EX-mode lock on a per-volume resource name — which additionally generalizes to
the clustered / MSCP-served case where the volume is written from more than one
node. `flock`-on-device is the minimal correct *single-node* broker for the
additive write half; a later rung can substitute a DLM lock at the same seam
without touching any consumer.
