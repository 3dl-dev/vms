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

## The host-tooling fallback (transitional, disclosed)

At OVMX **runtime** the executive is always present (PID 1 pins `/dev/vms`,
CLAUDE.md Rule 9), so the executive path is the whole story. Under host
**BUILD/TEST tooling** (`ctest`) there is no executive; rather than red-line the
~21 host DCL/integration tests that seed and resolve `SYS$SYSDEVICE:` etc., the
SYSTEM path falls back to the process-local table **only when `vms_kif` reports
`SS$_NOSUCHDEV`/unavailable**. This mirrors the pre-existing per-process
behaviour of the whole manager; it is a strict improvement (the executive path
is added, not removed) and it is disclosed in-code, never silent.

This fallback is the transitional piece. The end state is executive-only
resolution with those host tests migrated to the QEMU path (tracked as
vms-96e2's follow-up). It does not trip `tests/integration/test_runtime_target.sh`
(no `/dev/vms`-absence branch in executable code; it keys on the returned VMS
status), and it adds no `vms_kif_open()` presence test.

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
