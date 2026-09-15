# Building OVMX

## Prerequisites

```bash
# Debian/Ubuntu
sudo apt install cmake gcc make libc6-dev libreadline-dev flex bison

# For static builds
sudo apt install musl-tools

# For kernel modules
sudo apt install linux-headers-$(uname -r)

# For QEMU testing
sudo apt install qemu-system-x86

# For the build/test tooling containers (distro/Dockerfile.bootable,
# src/kernel/Dockerfile, tests/qemu/Dockerfile) — NOT an OVMX runtime (Rule 9)
sudo apt install docker.io
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | ON (OFF when cross-compiling) | Build test programs |
| `BUILD_TOOLS` | ON | Build the VMS tools (vms_login/LOGINOUT, vms_help/HELP.EXE, vms_authorize/AUTHORIZE, vms_mail, vms_monitor, vms_initialize, PRODUCT.EXE, mksysuaf, mkrightslist, …) |
| `BUILD_FUSE` | OFF | Build the optional dev-only FUSE ODS-2 driver (requires libfuse) — a convenience for inspecting volumes on the host; NOT the runtime path, which is the kernel-resident Files-11 ACP |
| `OVMX_STATIC` | OFF | Build all libraries as static and link statically (musl-gcc; suitable for initramfs) |
| `OVMX_IMGACT` | OFF | Build shareable images with `PT_INTERP=IMGACT.EXE` (musl, QEMU only; mutually exclusive with `OVMX_STATIC`) |
| `CMAKE_BUILD_TYPE` | — | Debug, Release, RelWithDebInfo |

## Development Build

Shared libraries, readline support, tests enabled.

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTS=ON \
  -DBUILD_TOOLS=ON

cmake --build build -j$(nproc)
```

**Outputs** in `build/`:
- Libraries: `libvms.so`, `libvmsprocess.so`, `libvmslnm.so`, `libvmsfs.so`, `librms.so`, `libvmssys.a`
- Executables: `vmsdcl`, `vms_login`, `vms_help`, `vms_authorize`, `vms_mail`, `vms_monitor`, `vms_initialize`, `ovmx_init` (deployed as `STARTUP.EXE`)
- Tests: `test_vmssys_*`, integration tests

## Static Build

Uses musl-gcc for fully static binaries (suitable for initramfs).

```bash
cmake -B build-static \
  -DCMAKE_C_COMPILER=musl-gcc \
  -DOVMX_STATIC=ON \
  -DBUILD_TOOLS=ON

cmake --build build-static -j$(nproc)
```

## Bootable QEMU VM

Builds a minimal Linux system with OVMX as the userspace.

```bash
# Build kernel + initramfs (outputs to dist/boot/)
docker build -f distro/Dockerfile.bootable -o dist .

# Boot with QEMU
./distro/boot/run-qemu.sh dist/boot/vmlinuz dist/boot/initramfs-ovmx.cpio.gz

# Custom memory (default 512M)
MEMORY=1G ./distro/boot/run-qemu.sh dist/boot/vmlinuz dist/boot/initramfs-ovmx.cpio.gz
```

The bootable image includes:
- A kernel.org kernel (6.12.103, pinned + integrity-checked), built from source
  with the OVMX modules (`vms.ko`) overlaid in-tree — not a stock Ubuntu kernel
  package
- Initramfs with static OVMX binaries and kernel modules
- `STARTUP.EXE` (`ovmx_init`) itself as `/init`, i.e. PID 1 — no wrapper script.
  `vmsfs.ko` has been retired; the ODS-2 filesystem codec is compiled into
  `vms.ko` and served through the Files-11 ACP.

## Base OS vs. Layered Products (kits)

OVMX is structured like real OpenVMS: a **base operating system** plus separately-installable
**layered products** (TCP/IP Services, DECnet, …). Each is packed as its own PCSI-equivalent
**kit** with a distinct product identity, and installed onto the target system by `PRODUCT
INSTALL` — which registers each product separately in `SYS$SYSTEM:VMS$PRODUCT_DATABASE.DAT`, so
`PRODUCT SHOW PRODUCT` lists them individually.

- **Kit format + packer:** `src/libvms/include/ovmx_kit_format.h` (the `OVMXKIT1` container) and
  `tools/ovmx_kit_pack.c` (the host packer). Product name shape is *vendor + arch-code + product*.
- **Installer + product DB:** `src/product/product.c` (`PRODUCT.EXE`), `src/product/ovmx_product_db.h`.
- **Base OS kit** — `OVMX X86VMS VMS` / `OVMX VAXVMS VMS`, packed as `ovmx-os.kit` by
  `distro/Dockerfile.bootable` and shipped by `tools/cut-release.sh`.
- **Layered-product kit** — packed by the *same* mechanism with its own identity and shipped as a
  separate artifact on its own release line:

  ```bash
  # (from a staging tree of the product's images/templates)
  ovmx_kit_pack pack ovmx-tcpip.kit <staging-dir> "X86VMS TCPIP"   # -> product "OVMX X86VMS TCPIP"
  # on the target system:
  $ PRODUCT INSTALL TCPIP        # registers a second product; SHOW PRODUCT lists OS + TCPIP
  ```

**TCP/IP Services** (`src/vmstcpip/`, rd epic `vms-67f`) is the first layered product, and
**bundles the OpenSSH port** into its kit (as real OpenVMS ships SSH inside TCP/IP Services).
Its IP engine is the substrate kernel's `AF_INET` stack; faithfulness lives in the userspace
product surface (`TCPIP$CONFIG`-equivalent, the `TCPIP$*` logicals, the `BGn:` device + sockets
veneer). Full design: `docs/design-tcpip-services-ovmx.md`. **DECnet** (`vms-30e`) follows the
identical layered-product kit pattern.

## Kernel Modules

The executive is a single kernel module, `vms.ko`. Its substrate-agnostic
facilities live in `src/kernel-core/`; `src/kernel/` holds the Linux glue (the
`/dev/vms` char device + backend primitives). For the distro kernel it is built
**in-tree** under `drivers/ovmx/` (see
[`adding-an-ovmx-kernel-module.md`](adding-an-ovmx-kernel-module.md)); the
standalone build below is out-of-tree against installed headers, used by the
QEMU test harness. It is not integrated into CMake. There is no separate
`vmsfs.ko` — the ODS-2 codec and Files-11 ACP are compiled into `vms.ko` (the
`vmsfs.ko` VFS mount was retired, vms-165).

```bash
# The VMS executive: access modes, ASTs, event flags, mailboxes, the process
# table, the device table, the lock manager, the logical-name manager, the
# ODS-2/Files-11 ACP, and the executive-resident cluster stack (SCS/CNXMAN/DLM)
make -C src/kernel

# Specify kernel version
make -C src/kernel KDIR=/lib/modules/6.8.0-50-generic/build
```

## Running Tests

### Unit / Integration Tests (CMake)

```bash
cd build && ctest --output-on-failure
```

Tests include:
- `test_vmssys_*` — 7 freestanding library tests (syscall, string, snprintf, futex, stdio, math, crt)
- Integration tests for logical names and DCL

### Kernel Module Tests (QEMU)

Boots a QEMU VM, loads vms.ko, runs test programs, captures serial output.

```bash
tests/qemu/run_tests.sh
```

Test programs (see `tests/qemu/` for the full, growing list), including:
- `test_kmod_access` — access control via ioctl
- `test_kmod_ast` — AST delivery
- `test_kmod_eflag` / `test_kmod_eflag_mproc` — event flag operations (single- and multi-process)
- `test_kmod_lock` / `test_kmod_lock_mproc` / `test_kmod_lock_sync` — lock manager
- `test_kmod_disk` — ODS-2/Files-11 ACP disk operations

### Integration Tests

```bash
tests/integration/test_logical_names.sh
tests/integration/test_dcl_basic.sh
```
