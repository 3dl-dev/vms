# cross-alpha — OVMX on Alpha (AXP) build + proving ground

Containerized `alpha-linux-gnu` cross toolchain plus the harness that proves
OVMX's freestanding layer boots on an Alpha. Build/test tooling only (Rule 9):
never a runtime.

## Contents

- `Dockerfile` — Debian trixie + `gcc-alpha-linux-gnu`, `libc6-dev-alpha-cross`,
  `qemu-user` (`qemu-alpha`), `qemu-system-misc` (`qemu-system-alpha`),
  `genisoimage`, and a kernel build chain.
- `ovmx-alpha-selftest.c` — a PID-1 self-test built on OVMX's own
  `src/libvmssys/arch/alpha/{crt0,syscall}.S`. Prints a banner and checks
  `getpid`, `getuid`, and the a3→negative-errno normalization.
- `boot-ovmx-qemu.sh` — builds a Linux/Alpha kernel with the self-test as its
  built-in initramfs and boots it under `qemu-system-alpha`. Asserts the
  self-test's WORKING line. `./boot-ovmx-qemu.sh` → `PASS`/`FAIL`.

## The proving ground: qemu-system-alpha, and why not AXPbox

The DS10's compute stack is a 21264 (EV6/EV67) CPU + Tsunami (21272) core logic
running OSF/1 PALcode. `qemu-system-alpha -M clipper` emulates exactly that and
boots a real Linux/Alpha kernel — the kernel names it: *"Booting GENERIC on
Tsunami variation Clipper."* This is the interim proving ground (operator ruling
2026-08-10): OVMX runs here as PID 1, which de-risks the DS10 on the axis that
decides whether OVMX runs — CPU ISA, core logic, PALcode, real Linux boot.

Known caveat, kept honest: qemu's clipper is not a full DS10. It differs on
peripherals (its CMD646 IDE vs the DS10's ALi M1543C — irrelevant to OVMX's
freestanding syscalls) and, notably, **cannot boot OpenVMS**. So it models the
DS10's Linux path, not the whole machine. We lean on it until a *dual-boot*
faithful emulator (a patched AXPbox, or Charon-AXP) or the DS10 itself is
available.

**AXPbox is not the proving ground.** It is VMS/Tru64-faithful but its
Linux/OSF-1 path is incomplete. Driving it directly (tracked in rd `vms-054`)
found three independent gaps: SRM disk-read callbacks corrupt kernel-sized
reads (worked around by writing the kernel to the raw boot area with
`swriteboot` + aboot `b -`); SMP init crashes with an unexpected exception at
vector 440 (worked around with `nosmp` — the config comments already warn SMP
and the icache are AXPbox's known bug areas); and once the kernel switches to
OSF/1 PALcode, no kernel console path (`console=srm`, `console=ttyS0`, even
`earlyprintk`) reaches the SRM serial console — leaving the running kernel
invisible. Making AXPbox a dual-boot DS10 stand-in means patching its emulator
source (OSF/1 console + timer callbacks); that is its own effort, not this one.

## Alpha port status (rd `vms-054`)

The freestanding layer is ported and committed: `src/libvmssys/arch/alpha/`
(`crt0.S`, `syscall.S`, `sigreturn.S`), the `__alpha__` syscall table in
`vms_syscall.h` (numbers derived from the toolchain's `<asm/unistd.h>`), and a
CMake arch branch. All of `libvmssys`'s C sources cross-compile for alpha. `struct vms_stat` /
`struct vms_sigaction` Alpha layouts landed in `vms_types.h` (rung A1, rd
vms-40b) -- verified field-for-field against the alpha-linux-gnu kernel
headers under qemu-alpha, with `vms_sys_fstat`/`vms_sys_rt_sigaction` wired
to the correct stat64/5-arg-rt_sigaction ABI. Remaining for a full port: an
`imgact` EM_ALPHA arch header.

## The executive on Alpha: vms.ko + /dev/vms (rd vms-89dd, rung A4)

On the OVMX/Linux-Alpha substrate (operator ruling 2026-08-16) the VMS
executive on Alpha is **`vms.ko` recompiled for the Linux/Alpha kernel** and
reached via `/dev/vms` -- NOT a NetBSD SYSKRNL port (that is the VAX path).
Two scripts prove it, both build/test tooling (Rule 9), never a runtime:

- `build-vmsko-alpha.sh` -- cross-compiles the executive `vms.ko` and the
  ACP-bearing filesystem `vmsfs.ko` (the genuine kernel-resident ODS-2 codec,
  epic vms-208) for the Linux/Alpha kernel (6.6.52, `alpha-linux-gnu-`).
  **Result: both modules cross-compile CLEAN** -- every kernel-core facility
  (locks / event flags / AST / mailboxes / process table / device table /
  logical names) and every ODS-2 codec source builds with zero width or
  endianness warnings. Alpha is little-endian + LP64 like x86_64, and the
  codec reads on-disk fields through byte-wise `le16/le32` helpers, so the
  ODS-2 work converged onto Alpha without change. (The one and only warning is
  a pre-existing large-stack-frame note in `ods2_writer.c`, not Alpha-specific.)
  Produces `elf64-alpha` `vms.ko`/`vmsfs.ko`, vermagic `6.6.52`.

  NOTE: `make modules` (not just `vmlinux modules_prepare`) is required so the
  kernel emits `Module.symvers`; an external module's modpost resolves the
  vmlinux-exported symbols against it.

- `boot-vmsko-qemu-alpha.sh` + `ke-init-alpha.c` -- boots the cross-compiled
  `vms.ko` under `qemu-system-alpha -M clipper` (the DS10 compute stack) with a
  static PID-1 init that loads the module, confirms `/dev/vms`, and runs the
  SAME cross-process (A-writes/B-reads) executive suites the x86_64 Kernel
  Executive CI job runs. **Result:** `/dev/vms` comes up on Alpha;
  `test_kmod_eflag_mproc` proves common event flag clusters cross-process
  **13/13** (including local-flag isolation, the anti-shared-memory
  discriminator); `test_kmod_lock_mproc` proves cross-process lock state
  **15/16** -- EX blocks EX/CR, cross-process `$GETLKI` sees the peer's queued
  request, CR+CR compatibility, `$DEQ`. The one deterministic failure is the
  cross-process **blocking-AST DELIVERY** leg: `VMS_IOCTL_DELIVERAST` returns
  `-EAGAIN` (no AST pending) on Alpha where x86_64 delivers it. The queued
  incompatible request IS visible cross-process, so this is specifically the
  async AST-delivery path, not lock state sharing. Code pointers for the
  follow-up: `vms_ioctl_deliverast` (`src/kernel-core/vms_ast.c:263`, the
  `!enabled || empty pending` -> `-EAGAIN` branch) and `notify_blocking_asts`
  (`src/kernel-core/vms_lock.c:611`, called from :991/:1184). Tracked for the
  executive owner; every other cross-process facility converged on Alpha.
