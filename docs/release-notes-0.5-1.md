# OVMX 0.5-1

**Completes the authenticity flip on all three substrates, and lands C++ first-light.**

0.5 shipped the Files-11 ODS-2 executive-ACP authenticity flip proven on the Linux
substrate (x86_64 and Alpha LP64). 0.5-1 closes the matrix and adds the first real
C++ program running as an OVMX image.

## What landed

- **The 3-substrate flip is complete.** The NetBSD/VAX executive now mounts and
  reads/writes genuine Files-11 ODS-2 over the executive ACP (`vms_blockdev_netbsd.c`,
  the faithful transliteration of the proven `vmsfs.kmod` path), proven end-to-end
  on real NetBSD/VAX under SIMH with an INV-6 on-disk hash-diff (real read *and*
  write, no false-pass). The `vms-d9c` VAX-boot gate goes green — no longer excluded.
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
