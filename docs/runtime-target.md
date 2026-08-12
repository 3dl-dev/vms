# Runtime target — one runtime model, and it is the real-host-kernel path

> **HARD INVARIANT.** Operator ruling, 2026-07-28; generalized to the
> substrate-neutral "one runtime model" form by operator ratification,
> 2026-08-12 (rd `vms-fff`, epic `vms-8e8`). This is the canonical,
> public statement of the rule the standing gate
> `tests/integration/test_runtime_target.sh` enforces. The gate checks the
> mechanics; this document carries the reasoning. If the rule is deleted, the
> gate is cargo cult — so the gate greps this file for it.

## The rule

**One runtime model: the real-host-kernel path.** The Docker *runtime* layer is
**dead**. OVMX runs on a real OS kernel that provides the VMS **executive** as an
in-kernel facility reached through `/dev/vms` (`vms_kif`). Two SYSKRNLs are
sanctioned, both exposing the identical `/dev/vms` contract: **OVMX/Linux**
(executive = `vms.ko`; x86_64/aarch64) and **OVMX/NetBSD** (executive = the
`vms` pseudo-device; initially VAX). Docker is **never** an OVMX runtime, never a supported
way to "run OVMX", and never a target whose limitations get designed around. This
holds on **both** SYSKRNLs. A userspace fake of any executive facility (INV-6)
is forbidden on either.

## The distinction is load-bearing — do not collapse it

- **Docker as a RUNTIME target — FORBIDDEN.** The root `Dockerfile` and
  `docker-compose.yml` (glibc product container, SSH on 2222) were deleted — CI
  no longer builds or runs them. Do not recreate them, do not document them as a
  way to run OVMX, do not add new CI jobs on them.
- **Docker/podman as BUILD or TEST tooling — FINE and expected.**
  `distro/Dockerfile.bootable` (builds the bootable QEMU image),
  `src/kernel/Dockerfile` (builds `vms.ko`), and `tests/qemu/Dockerfile` (QEMU
  test harness) *produce and test the real runtime*. The rule "ALL deps
  containerized — NEVER install on host" depends on these. Keep them.

## Why this keeps being forgotten, and the trap to avoid

Docker containers have no `/dev/vms`. Because CI runs in Docker, the kernel
executive is *unprovable in that environment* — and so OVMX historically grew
per-process userspace fakes (logical name tables, process table, event flags,
mailboxes) that report success while sharing nothing. **The architecture drifts
to fit the test harness.** Therefore:

- **Never add a silent userspace fallback for an executive facility.** If
  `/dev/vms` is absent, the correct behavior is to **fail honestly**
  (`SS$_NOSUCHDEV`, as `sys_lock.c` already does) — never to fake per-process
  success. A silent fallback is the exact LARP bug class the authenticity
  invariants exist to kill (INV-6).
- **An executive facility is not done until a test exercises it against a real
  `/dev/vms`.**
- The standing gate `tests/integration/test_runtime_target.sh` enforces the
  mechanical parts of this rule. Do not add allowlist entries to it to make it
  pass.
