# OVMX 0.5-1

**Advances the Alpha login gate and lands C++ first-light.**

0.5 shipped the Files-11 ODS-2 executive-ACP authenticity flip proven on the Linux
substrate (x86_64 and Alpha LP64). 0.5-1 hardens the Alpha authentic-login gate and
adds the first real C++ program running as an OVMX image.

## What landed

- **ODS-2 executive ACP: proven on x86_64 and Alpha (LP64); VAX runtime still on the
  VFS/POSIX path.** x86_64 and Alpha LP64 boot and run RMS over the executive ACP. On
  NetBSD/VAX the executive ACP codec is built and unit-proven, but it is **not yet
  wired into the VAX runtime** — the VAX image set builds with `OVMX_HAVE_ACP`
  undefined and boots via the Files-11 VFS/POSIX path. Converting the VAX runtime onto
  the ACP is tracked as `vms-d5d` / `vms-049` (targeting V0.5-2+). The `vms-d9c`
  VAX-boot gate is green — it boots to `PROVISION.EXE` via that current VFS/POSIX path.
- **Alpha authentic login, gated green-by-SHA.** `SYSTEM`/`MANAGER` authenticate by
  Purdy against the genuine binary `$UAFDEF` SYSUAF over the ODS-2 ACP on Alpha LP64,
  and a standing dispatch-tier CI gate boots `qemu-system-alpha` cold and proves
  login-to-`$` every cut.
- **C++ first-light.** A complete C++ program — global constructors, `std::string`/
  iostream, and `throw`/`catch` exception handling — runs to `exit 0` as an OVMX
  image, on genuine TLS/init_array/eh_frame machinery (multi-module TLS combined
  block, `.init_array` bucketing + init-priority, `.eh_frame`/`__register_frame`
  before constructors, DECC$SHR musl universals, classic GD/LD→LE relaxation).
  Proven across x86_64, Alpha LP64, and VAX ILP32 on the 3-way convergence gate.
- **Executable-own `.vms$wimp` binding** — an executable's own weak-by-name imports
  now bind at activation (`resolve_wimp_for` runs for `g_exe`, not just producers).

## Honest scope

C++ *first-light* runs; a full in-guest compiler (cc1 compiling) is the next rung —
blocked on a known executable-TLS-loader gap in the interp-driven musl path
(`vms-c07`, forward, post-1.0 self-hosting northstar; gates nothing shipped). The
NetBSD/VAX install-faithfulness capstone (install onto a blank target → boot alone)
is `vms-d5d`/`vms-4834` (V0.5-2), pending a NetBSD block-read width fix.
