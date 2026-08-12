# OVMX aarch64 system-emulation boot proof

rd `vms-f66` · the aarch64 sibling of `tests/qemu/` (Linux `vms.ko`) and
`tests/netbsd/` (NetBSD/amd64).

A **containerized** harness that boots a **real arm64 Linux kernel** under
**system-mode `qemu-system-aarch64 -machine virt`** and runs a genuine OVMX
freestanding **EM_AARCH64** artifact as **PID 1 (init)**. It asserts the guest
runs on a real arm64 machine (`uname().machine == "aarch64"`), that the OVMX
freestanding syscall/crt0 layer works there (`getpid`, `read`→`-EBADF`,
`write`), prints `OVMX_AARCH64_SYSMODE_PASS`, and powers the guest off cleanly.
Exit status carries the verdict.

```
docker build -f tests/qemu-aarch64/Dockerfile -t ovmx-aarch64-boot .
docker run --rm ovmx-aarch64-boot
```

## Why this exists — the aarch64 positioning gap

aarch64 is one of OVMX's first-class positioning arches, but until now it had
the *least* runtime proof of the four:

| Arch | Runtime proof before this |
|------|---------------------------|
| x86_64 | full boot-to-login under QEMU (`tests/qemu/`, `tests/uat/`) + the `vms.ko` executive |
| NetBSD/amd64 | boots under `qemu-system-x86_64` (`tests/netbsd/`) |
| NetBSD/vax | boots under SIMH |
| **aarch64** | **none** — only *user-mode* QEMU binfmt (the ~25 link-native/imgact arm64 CI jobs), never a system-mode boot |

Every existing aarch64 proof runs under **user-mode** QEMU (`docker run
--platform linux/arm64`): individual arm64 binaries are translated onto the
host's x86_64 kernel — there is no arm64 kernel, no EL1/EL0, no arm64 MMU. The
aarch64 branches that *would* do a real boot, in `distro/boot/run-qemu.sh` and
`tests/qemu/run_tests.sh`, were dead code: "this repo has no aarch64 QEMU host
to test against" (rd `vms-c83`).

This harness closes that gap one rung above the Alpha syscall/crt0 proof
(commit `c0a663dc`, "proven under qemu-alpha" — itself *user-mode*): it runs the
OVMX freestanding layer as init on a **genuinely system-emulated arm64 kernel**.

## What is real vs. emulated (be precise)

- **Real:** the arm64 Linux kernel (Debian arm64 `linux-image-arm64`, extracted
  from the official package), `qemu-system-aarch64` **system** emulation
  (CPU + MMU + PL011 UART), and the OVMX payload — a genuine **EM_AARCH64**
  static ELF built from the **real product sources** (`src/libvmssys/arch/
  aarch64/{crt0,syscall,sigreturn}.S` + the `vms_*.c` runtime), with the same
  flags the CMake `vmssys`/`vmssys_add_test()` targets use.
- **Emulated:** QEMU runs the arm64 guest under **TCG** (no KVM needed — and on
  an x86_64 host there is none for arm64). That is the same "system emulation on
  a foreign host" model `tests/netbsd/` and the SIMH VAX lab already use.
- **Not covered here (deferred):** the `vms.ko` executive on arm64. This proof
  is the freestanding userspace layer on a real arm64 kernel; it does **not**
  bring up `/dev/vms` on arm64. See "Blocker for a full aarch64 login" below.

## Anti-LARP: the negative controls

The one defect this class of test must never have is "secretly ran x86_64 and
printed an aarch64 PASS". Three independent guards:

1. **In-guest positive arch assertion:** the payload calls `uname(2)` and fails
   unless `.machine == "aarch64"`. A binary that ran on x86_64 prints `x86_64`.
2. **Wrong-arch init control (in the harness):** the same `/init`, built for
   **x86_64**, is fed to the *same* arm64 boot. A real arm64 kernel cannot exec
   an EM_X86_64 binary — it reports `Failed to execute /init (error -8)`
   (ENOEXEC) and panics with no working init. The PASS marker must be absent.
3. **Marker-teeth control (CI):** re-runs the harness keyed on a marker string
   the guest never prints (`-e PASS=…`); a correct harness then goes red,
   proving the grep is load-bearing.

## Files

| File | Role |
|------|------|
| `guest/init_aarch64.c` | the PID-1 proof program (links the real freestanding layer) |
| `build_init.sh` | cross/native-compile `/init` for `aarch64` or `x86_64`, pack a single-file initramfs, self-check the ELF machine |
| `fetch_kernel.sh` | extract a Debian arm64 kernel Image (never installs the package) |
| `Dockerfile` | assemble kernel + both initramfs images; entrypoint = `run_tests.sh` |
| `run_tests.sh` | boot positive + negative-control, verdict via exit status |

## This is tooling, not a runtime (CLAUDE.md Rule 9)

Booting a kernel in QEMU to **test** OVMX is exactly what `tests/qemu/` (Linux
`vms.ko`) and `tests/netbsd/` (NetBSD executive) do — it does not make Docker or
QEMU an "OVMX runtime". The Rule-9 gate
(`tests/integration/test_runtime_target.sh`) inspects only `src/**` C/H files,
so nothing here trips it.

## Blocker for a full aarch64 boot-to-login (deferred)

A full OVMX aarch64 boot-to-login (the x86_64 `tests/qemu/` equivalent) needs
the **`vms.ko` executive cross-built for arm64** against the target arm64
kernel's headers, then loaded so `/dev/vms` exists — without it, executive
services fail honestly with `SS$_NOSUCHDEV` (Rule 9's fail-honest path), so a
login shell would have no executive behind it. That cross-build + an arm64
initramfs carrying the VMS-native image graph (STARTUP/LOGINOUT/DCL, already
proven EM_AARCH64 under user-mode QEMU) is the next rung, tracked as follow-on
work to this bead. This proof deliberately stops at the freestanding layer,
which is what makes it cheap, real, and honest today.
