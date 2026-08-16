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
CMake arch branch. All of `libvmssys`'s C sources cross-compile for alpha.
Remaining for a full port: `struct vms_stat` / `vms_sigaction` Alpha layouts in
`vms_types.h` (deliberately left undefined so they fail loud, not silently
wrong), and an `imgact` EM_ALPHA arch header.
