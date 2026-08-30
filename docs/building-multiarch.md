# Building and Booting the Alpha and VAX Images

[`docs/building.md`](building.md) and [`docs/getting-started.md`](getting-started.md)
cover the primary **x86_64** bootable image. This guide covers the other two
first-class release architectures:

- **Alpha (LP64)** — OVMX cross-built for `alpha-linux-gnu`, running on a
  Linux/Alpha kernel with `vms.ko` as the executive, booted under
  `qemu-system-alpha`.
- **VAX (ILP32, NetBSD)** — OVMX cross-built for `vax--netbsdelf`, running on
  **NetBSD/vax** (a second SYSKRNL) with the executive as a NetBSD kernel
  module, booted under **SIMH** (there is no `qemu-system-vax`).

Both are cross-built inside containers — like the x86_64 path, **no
cross-compiler or emulator needs to be installed on your host.** All commands
are run from the repository root.

> **Oracle labs are not build targets.** `tests/lab-alpha/` runs *real OpenVMS
> Alpha V8.4* on AXPbox and `tests/lab/` runs *real OpenVMS VAX V7.3* on SIMH.
> They are **comparison oracles only** — they do not build or boot OVMX. This
> guide documents the OVMX build/boot paths under `tools/cross-alpha/`,
> `tools/cross-vax/`, and `tests/lab-vax/`. Do not confuse `tests/lab-vax/`
> (OVMX on NetBSD/vax) with `tests/lab/` (the OpenVMS VAX oracle).

---

## Architecture: one executive model per SYSKRNL

The two arches reach the VMS executive differently, and the difference drives
everything below:

| | Alpha | VAX |
|---|---|---|
| SYSKRNL | Linux/Alpha kernel | **NetBSD/vax** |
| Emulator | `qemu-system-alpha -M clipper` (DS10-class: EV6 + Tsunami/21272) | **SIMH** (MicroVAX 3900 / KA655) |
| Executive | `vms.ko` recompiled for Linux/Alpha, served at `/dev/vms` | `vms.kmod` — a NetBSD kernel module, served at `/dev/vms` |
| Width | LP64 | ILP32 (elf32-vax) |
| PID 1 | `STARTUP.EXE` (via a Linux/Alpha initramfs) | `ovmx_init` / `STARTUP.EXE` |
| Toolchain file | `tools/cross-alpha/toolchain-alpha-linux.cmake` | `tools/cross-vax/toolchain-vax-netbsd.cmake` |

VAX uses NetBSD as its base layer because NetBSD/vax is still actively
maintained and there is no `qemu-system-vax`; Alpha uses a Linux/Alpha kernel
booted on `qemu-system-alpha`. In both cases the VMS executive is a real
kernel-resident module reached through `/dev/vms` — the same authenticity
contract as x86_64 (CLAUDE.md Rule 9).

---

## Alpha (LP64)

### Prerequisites

Only Docker. The cross toolchain (`gcc-alpha-linux-gnu`, `libc6-dev-alpha-cross`,
`qemu-system-alpha`, `qemu-user`, `genisoimage`, and the kernel build chain)
lives in the container built from `tools/cross-alpha/Dockerfile` (image tag
`ovmx-cross-alpha:latest`).

### Build + boot to DCL (the release gate)

The friendliest entry point is the release-acceptance driver, which handles the
container build, the cross build, disk mastering, and the QEMU boot for you. It
maintains its own build cache under `.boot-cache/alpha-gate`:

```bash
# Build the cache and boot the Alpha image; exit 0 iff a real DCL "Username:"
# prompt is reached.
tools/cross-alpha/run-boot-alpha.sh gate

# Boot and additionally authenticate SYSTEM/MANAGER.
tools/cross-alpha/run-boot-alpha.sh login

# Boot, log in, and run the shared DCL/SHOW acceptance battery.
tools/cross-alpha/run-boot-alpha.sh acceptance
```

Other modes: `build`, `boot`, `validate [N]`. Force a rebuild with
`FORCE_BUILD=1`. This driver is release tooling and is deliberately **not**
wired into the per-PR CI (see the per-PR gates below).

### What the build does under the hood

`tools/cross-alpha/build-alpha-bootimage.sh` cross-builds the LP64 userland,
masters a VMSFS/ODS-2 system disk, and bakes an initramfs whose `/init` is
`STARTUP.EXE`. The userland configure line is:

```bash
cmake -S . -B build-alpha \
  -DCMAKE_TOOLCHAIN_FILE=tools/cross-alpha/toolchain-alpha-linux.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON -DOVMX_STATIC=ON
cmake --build build-alpha -j$(nproc)
```

The executive `vms.ko` for Linux/Alpha is cross-compiled separately by
`tools/cross-alpha/build-vmsko-alpha.sh` (produces an `elf64-alpha` module).

### The raw QEMU boot

The underlying boot (assembled for you by the drivers) is:

```bash
qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
  -kernel vmlinux-boot -append "console=ttyS0 panic=-1" \
  -drive file=diskA.img,format=raw,if=virtio \
  -nographic -no-reboot
```

To exit QEMU: `Ctrl-A` then `X`.

### Lower-level Alpha drivers

| Script | Proves |
|--------|--------|
| `tools/cross-alpha/boot-ovmx-qemu.sh` | Freestanding `crt0`/syscall backend (`OVMX syscall trampoline + crt0: WORKING on Alpha`). |
| `tools/cross-alpha/boot-alpha-image.sh` | Full OVMX/Alpha system boot to `Username:` (BOOT A) and IMGACT capability (BOOT B). |
| `tools/cross-alpha/boot-vmsko-qemu-alpha.sh` | `vms.ko` loads, `/dev/vms` appears, cross-process executive suites run. |
| `tools/cross-alpha/run-syssvc-tests-alpha.sh` | System-service suite over ODS-2 fixture disks. |
| `tools/cross-alpha/boot-alpha-probe.sh` | Diagnostic probes (`PROBE=provision\|contention\|exec`). |

### Alpha CI gates (`.github/workflows/ci.yml`)

Per-PR jobs run when the `alpha_activation` path filter matches; each builds the
`ovmx-cross-alpha` image, then runs:

| Job | Command |
|-----|---------|
| `alpha-boot-mount` | `tools/cross-alpha/run-module-gp-activation-alpha.sh gate` |
| `alpha-crtl-rms-n7` | `tools/cross-alpha/run-module-gp-activation-alpha.sh crtl-rms-gate` |
| `alpha-activation-selftest` | `tools/cross-alpha/run-module-gp-activation-alpha.sh selftest` |

Scheduled / dispatch jobs run the heavier `run-boot-alpha.sh login` and
`... acceptance` boots.

---

## VAX (ILP32, NetBSD)

### Prerequisites

Only Docker. The cross toolchain (`vax--netbsdelf` gcc 13.3.0 + binutils 2.42)
lives in the container built from `tools/cross-vax/Dockerfile` (image tag
`ovmx-cross-vax:latest`, also published as
`ghcr.io/3dl-dev/ovmx-cross-vax:<source-hash>`). The emulator is SIMH
(MicroVAX 3900 / KA655), pulled/built by the lab tooling. Pins: NetBSD/vax
**10.1**, open-simh, anita.

### Build + boot OVMX to PID 1 and disk mount

The VAX boot layers OVMX onto a cached stock-NetBSD/vax system disk. First
build the lab image and lay down the NetBSD disk cache once, then run the OVMX
boot drivers:

```bash
# 1. Build the VAX lab container (SIMH + NetBSD install/boot harness).
docker build -f tests/lab-vax/Dockerfile -t ovmx-vax-lab tests/lab-vax

# 2. Once: create the cached stock-NetBSD/vax system disk (under .boot-cache/lab-vax/).
tests/lab-vax/run-local.sh install

# 3. Boot OVMX: ovmx_init (STARTUP.EXE) as PID 1, vms.kmod loaded (/dev/vms live),
#    OpenVMX banner, vmsfs.kmod loaded, ODS-2 system disk (DUA0:) mounted.
tests/lab-vax/run-boot.sh prove

# Boot past the installed-system gate to PROVISION.EXE (mastered system volume):
tests/lab-vax/run-boot.sh sysboot

# Boot, log in SYSTEM/MANAGER, and run the shared DCL/SHOW acceptance battery:
tests/lab-vax/run-boot.sh acceptance
```

Negative-control modes (`negctl`, `sysboot-negctl`) assert that milestones
which *should not* appear (e.g. an ODS-2 MOUNT with no ODS-2 volume present) do
not. Force rebuilds with `FORCE_CROSS_BUILD=1` (rebuilds `ovmx_init` /
`vms.kmod` / `vmsfs.kmod`) and `FORCE_SYSVOL_BUILD=1` (rebuilds the five boot
images).

Inside SIMH the guest is booted single-user (required for securelevel-0
`modload`) via the ROM console command:

```
>>> B/R5:2 DUA0
```

The console driving is handled for you by `tests/lab-vax/drive_boot_vax.py`
(built on `tests/lab-vax/vaxharness.py`).

### What the build does under the hood

- `tools/cross-vax/build-vms-module-vax.sh` — cross-compiles the whole
  executive as a NetBSD `vms.kmod` for **elf32-vax** (`-Werror`), and asserts
  ILP32/endian-cleanliness via a generated width-audit TU plus an `objdump`
  "file format elf32-vax" check. This is the fast **compile/width gate**, not a
  boot (env: `NBSRC=/nbsrc`, `TARGET=vax--netbsdelf`).
- `tools/cross-vax/build-boot-images-vax.sh` — cross-builds and link-proves the
  five boot images: `STARTUP.EXE` (ovmx_init), `PROVISION.EXE`, `DCL.EXE`,
  `JOB_CONTROL.EXE`, `LOGINOUT.EXE`.
- `tools/cross-vax/build-vax-modular-kernel.sh` — builds a
  `GENERIC + options MODULAR` NetBSD/vax kernel (the stock vax GENERIC omits
  `MODULAR` and so cannot `modload`), installed as `/netbsd`.

The CMake cross-build uses the toolchain file:

```bash
cmake -S . -B build-vax \
  -DCMAKE_TOOLCHAIN_FILE=tools/cross-vax/toolchain-vax-netbsd.cmake
cmake --build build-vax -j$(nproc)
```

Per-facility OVMX boot proofs (each boots the cached disk under SIMH and
exercises one executive facility cross-process): `run-devvms.sh` (`/dev/vms`
PING), `run-eflag.sh`, `run-access.sh`, `run-proctab.sh`, `run-mbx.sh`,
`run-vmsfs.sh`.

### VAX CI gates (`.github/workflows/ci.yml`)

| Job | Command | Notes |
|-----|---------|-------|
| `netbsd-vax-vms-crosscompile` | `tools/cross-vax/build-vms-module-vax.sh` | Per-PR elf32-vax width/compile gate (fast). |
| `netbsd-vax-simh` | `run-local.sh install` / `smoke` / `negctl` | Proves *stock NetBSD/vax* boots (`uname -srm` == `NetBSD 10.1 vax`). |
| `netbsd-vax-devvms` | `tests/lab-vax/run-devvms.sh` | OVMX `/dev/vms` PING. |
| `netbsd-vax-boot` | `run-boot.sh prove` then `run-boot.sh negctl` | OVMX `ovmx_init` PID 1 + DUA0: mount. |
| `netbsd-vax-sysboot` | `run-boot.sh sysboot` / `sysboot-negctl` | OVMX boot to PROVISION.EXE. |
| `vax-dcl-acceptance` | `run-boot.sh acceptance` | OVMX login + DCL battery. |

The per-PR gate is the fast `netbsd-vax-vms-crosscompile` compile/width check;
the boot jobs are scheduled / dispatch.

---

## Release cuts

Each arch has a release-cut script that drives its toolchain file directly:

```bash
tools/cut-release-alpha.sh   # -> dist/alpha-gate-proof/... (CI job cut-release-alpha-gate)
tools/cut-release-vax.sh     # (CI job cut-release-vax-gate)
```

---

## Which script for which job

| I want to… | Alpha | VAX |
|------------|-------|-----|
| Fastest per-PR sanity check | (activation gates) `run-module-gp-activation-alpha.sh gate` | `build-vms-module-vax.sh` (compile/width) |
| Boot to a DCL prompt | `run-boot-alpha.sh gate` | `run-boot.sh prove` (+ `sysboot`) |
| Log in + run the DCL battery | `run-boot-alpha.sh acceptance` | `run-boot.sh acceptance` |
| Prove the executive `/dev/vms` | `boot-vmsko-qemu-alpha.sh` | `run-devvms.sh` |
| Cut a release artifact | `cut-release-alpha.sh` | `cut-release-vax.sh` |
