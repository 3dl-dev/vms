# Design: the LNM surface goes through the executive (vms-96e2)

## Problem

The 0.2 demo `$ @SYS$UPDATE:PARTS_SETUP.COM` failed. `SYS$MANAGER:STARTUP.COM`
does `DEFINE/SYSTEM SYS$UPDATE SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSUPD]` at boot,
but OVMX's DCL surface and filespec resolution answered logical names from the
**in-process vmslnm manager** — a per-process store. So `SYS$UPDATE`, defined
in the STARTUP process, was invisible to a later login process, and
`@SYS$UPDATE:` could not resolve.

`#193` (vms-d37) made `LNM$SYSTEM` executive-resident: `sys$crelnm/$trnlnm/
$dellnm` route the SYSTEM table through `vms_kif` → the `/dev/vms` mmap arena in
`vms.ko`, so a name defined by one process is visible to every process on the
node. But nothing on the DCL/vmsfs path used it — they still used the
in-process manager.

## What vms-96e2 changes

The **vmslnm manager's `LNM$SYSTEM` table is now executive-backed**:
`lnm_create`, `lnm_delete` and `lnm_translate` route the SYSTEM table through
`vms_kif_lnm_define/_delete/_translate` — the same executive arena `#193`'s
`sys$` services reach. `LNM$PROCESS/JOB/GROUP` stay process-private (their
executive residency is the deferred half of vms-d37).

Because every existing consumer already uses this manager, they all become
executive-backed for SYSTEM with **no caller change**:

- DCL `DEFINE/SYSTEM` / `DEASSIGN/SYSTEM` (`lnm_create`/`lnm_delete`)
- DCL `SHOW LOGICAL <name>` and `F$TRNLNM` (`dcl_translate_logical` →
  `lnm_translate`)
- vmsfs filespec resolution (`vmsfs_resolve_device_r` → `lnm_translate`), which
  is what `@SYS$UPDATE:PARTS_SETUP.COM` traverses.

Boot seeding needed no new code either: `lnm_setup_defaults` (already called by
PID 1 / `ovmx_init` / `ovmx_provision`) calls `lnm_create(LNM$SYSTEM, …)`, which
now writes the executive. `SYS$UPDATE` and `SYS$SYSROOT` were added to that
baseline; the per-process terminal logicals (`SYS$INPUT/OUTPUT/ERROR/COMMAND`,
`TT`, `SYS$DISK`) were **moved to `LNM$PROCESS_TABLE`**, because a terminal
logical placed in the node-wide SYSTEM table would wrongly bind every process to
the first process's `/dev/tty`.

## Why the manager, not `sys$*`, at the DCL/vmsfs layer

The task specified routing through `sys$crelnm/$trnlnm/$dellnm`. That is exactly
right for DCL, but **vmsfs cannot call `sys$*`**: `libvms` links `vmsfs`, so
`vmsfs → libvms` is a link cycle. Both DCL and vmsfs already sit above `vmslnm`
and use it, and `vmslnm` sits above `vmssys` (where `vms_kif` lives). So the one
place that lets DCL **and** vmsfs reach the executive arena without a cycle and
without a second process-private store (split-brain) is the vmslnm manager
itself, routing SYSTEM through the shared `vms_kif` client. The SYSTEM arena is
identical to the one `sys$crelnm` uses — they are consistent by construction.

## The host-tooling fallback — REMOVED (vms-48ab)

vms-96e2 shipped a transitional fallback: under host **BUILD/TEST tooling**
(`ctest`, no `/dev/vms`), `lnm_create`/`lnm_delete`/`lnm_translate` fell back to
a process-local SYSTEM table so the ~21 host DCL/LNM/filespec tests that seed
and resolve `SYS$SYSDEVICE:` etc. did not have to be touched immediately. That
was disclosed in-code and in this doc, but it was still exactly the shape
CLAUDE.md Rule 9 / INV-6 forbids: a silent per-process fake standing in for an
executive facility. **vms-48ab removed it.**

`lnm_create`, `lnm_delete` and `lnm_translate`'s LNM$SYSTEM branches now return
`vms_kif`'s status unchanged, with no local-table branch at all — the same
honest `SS$_NOSUCHDEV`-with-no-executive behaviour `sys$crelnm/$trnlnm/$dellnm`
(`src/libvms/syssvc/sys_logical.c`) already had from day one (that
implementation never had a fallback to begin with; only this manager did).

### `lnm_setup_defaults` still needs to seed *something*, host-side

Removing the fallback surfaced a second-order gap the first pass of this fix
missed: `lnm_setup_defaults()` (called by PID 1, `ovmx_provision`, DCL.EXE's
`setup_session()`, and LOGINOUT.EXE) seeds the FILE-LOCATING baseline —
`SYS$SYSDEVICE`, `SYS$SYSROOT`, `SYS$SYSTEM`, `SYS$LIBRARY`, `SYS$SHARE`,
`SYS$MANAGER`, `SYS$UPDATE`, `SYS$HELP`, `SYS$SCRATCH`, `SYS$LOGIN` — entirely
into `LNM$SYSTEM`. With no executive, ALL of them now fail honestly too,
which broke host consumers that locate real files through them:
`test_libvms_sysuaf_uic_base`/`test_libvms_rightslist` (SYS$SYSTEM:SYSUAF.DAT
/ SYS$SYSTEM:RIGHTSLIST.DAT) and LOGINOUT.EXE's own `sysuaf_lookup()` in the
`login-native` CI job.

The fix, `lnm_seed_system_locating()` in `lnm_defaults.c`: try the SYSTEM
create as before; **only** on `SS$_NOSUCHDEV`, also define the same name at
**process** scope (`LNM$PROCESS_TABLE`). This is INV-6-preserving, not a
reintroduction of the removed fallback:

- It touches `lnm_setup_defaults` only — `lnm_create`/`lnm_translate`/
  `lnm_delete` themselves are unchanged, so an explicit `LNM$SYSTEM` lookup
  (what `test_system_table_no_fallback()` and `test_syssvc_lnm_system.c`'s
  no-executive branch assert) still honestly reports `SS$_NOSUCHDEV`.
- It never fires when the executive is present (the SYSTEM create already
  succeeded), so real multi-process SYSTEM-table semantics — a sysadmin's
  system-wide `DEFINE/SYSTEM` reaching every other process — are unaffected.
- A process-level `DEFINE` shadowing a system-level one is a real, disclosed
  VMS mechanism (`SHOW LOGICAL` reports it as `(LNM$PROCESS_TABLE)`, not
  SYSTEM), not a disguise of the executive-resident table — the same shape
  `dcl_filespec.c: dcl_translate_logical()`'s pre-existing SYS$DISK/SYS$LOGIN
  fallback already uses.

At OVMX **runtime** nothing changes: PID 1 pins `/dev/vms` (Rule 9), so the
executive path was always the whole story there. The removal is only visible
under host tooling, where `LNM$SYSTEM` operations (and anything that
transitively resolves through it, including `lnm_setup_defaults`'s baseline —
`SYS$SYSDEVICE`, `SYS$SYSTEM`, `SYS$LIBRARY`, `SYS$SCRATCH`, `SYS$LOGIN`,
`SYS$UPDATE`, etc.) now fail honestly instead of quietly working from a
process-local shadow. `tests/integration/test_runtime_target.sh` is unaffected
either way (it keys on returned VMS status, not on a `/dev/vms`-absence branch
in executable code, and there is now even less of one).

### Where the migrated host coverage went

- `tests/vmslnm/test_vmslnm.c`'s table-hierarchy/override scenario now targets
  `LNM$GROUP` (still process-private) instead of `LNM$SYSTEM`, so it keeps
  proving the same `lnm_translate` hierarchy logic without an executive. A new
  `test_system_table_no_fallback()` asserts `lnm_create`/`_translate`/`_delete`
  against `LNM$SYSTEM` return `SS$_NOSUCHDEV` on the host — the INV-6
  regression guard at the unit level.
- `tests/libvms/test_identity.c`'s SYS$WELCOME/SYS$ANNOUNCE "undefined →
  built-in" checks stayed host-side (any non-`SS$_NORMAL` status already fell
  through to the built-in banner in `ovmx_banner.h`, so they were never
  fallback-dependent). The "defined → overridden" checks became "a define that
  fails honestly does NOT change the banner" — proving INV-6 holds all the way
  through the DCL-consumer layer, not just at `lnm_create` itself.
- `tests/integration/test_parts_setup.sh` (vms-977) now seeds `SYS$UPDATE` with
  a plain process-scoped `DEFINE`, not `DEFINE/SYSTEM`: the gate is about
  `PARTS_SETUP.COM`'s own COPY/DEFINE/`:==` logic, which doesn't care what
  table `SYS$UPDATE` lives in, so a process-scoped define exercises the same
  logic without touching the executive. The real `DEFINE/SYSTEM SYS$UPDATE`
  line from `SYS$MANAGER:STARTUP.COM`, against a real executive, running this
  SAME committed `PARTS_SETUP.COM`, is proven end to end by
  `tests/qemu/test_parts_demo_e2e.sh`.
- **New**: `tests/qemu/test_syssvc_lnm_system.c` proves the vmslnm-MANAGER
  API's `LNM$SYSTEM` path (as distinct from the `sys$` API
  `test_syssvc_lnm_crossproc.c` already covered) against a real `/dev/vms`:
  manager-level create/translate/delete round-trip, table hierarchy
  (`LNM$FILE_DEV` finds a process override, `LNM$SYSTEM` itself is
  unchanged underneath it), consistency with `sys$trnlnm` (same arena), and
  the SYS$WELCOME/SYS$ANNOUNCE override scenarios (including the `@file`
  multi-line form) that could no longer be proven host-side. Its no-executive
  branch reproduces the INV-6 regression guard at the manager layer.
  Negative control: `lnm-manager-delete-noop`
  (`tests/qemu/facility_defects.sh`).

## Proof

`tests/qemu/test_syssvc_lnm_crossproc.c` (extended): two separate processes,
one real `/dev/vms`. The parent `DEFINE/SYSTEM`s `SYS$UPDATE` (as STARTUP.COM
does) and stages `PARTS_SETUP.COM`; the child — which never defines the name —
resolves it via `sys$trnlnm` (the SHOW LOGICAL / F$TRNLNM path) and resolves
`SYS$UPDATE:PARTS_SETUP.COM` via `vmsfs_to_linux_path` and **opens** it (the
`@`-command path). Cross-process visibility proves the name is executive-
resident; the executive-absent path still asserts honest `SS$_NOSUCHDEV`
(no fabricated success, INV-6). The full boot→login→`@SYS$UPDATE:` DCL chain is
tracked separately (vms-5dd).
