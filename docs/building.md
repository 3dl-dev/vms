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
| `BUILD_TESTS` | ON | Build test programs |
| `BUILD_TOOLS` | ON | Build vms_login, vms_help, vms_ssh_auth |
| `BUILD_FUSE` | OFF | Build FUSE ODS-2 driver (not yet implemented) |
| `OVMX_STATIC` | OFF | Static linking with musl-gcc |
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
- Executables: `vmsdcl`, `vms_login`, `vms_help`, `vms_ssh_auth`, `ovmx_init`
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
# Build kernel + initramfs (outputs to dist/)
docker build -f distro/Dockerfile.bootable -o dist .

# Boot with QEMU
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz

# Custom memory (default 512M)
MEMORY=1G ./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz
```

The bootable image includes:
- Stock Ubuntu kernel (vmlinuz)
- Initramfs with busybox, static OVMX binaries, kernel modules
- `init-wrapper.sh` as PID 1: mounts filesystems, loads vms.ko + vmsfs.ko, launches ovmx_init

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

Built out-of-tree against installed kernel headers. Not integrated into CMake.

```bash
# Main VMS module (access control, ASTs, event flags, locks)
make -C src/kernel

# VMS filesystem module
make -C src/kernel/vmsfs

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

Test programs (5 total, 62 assertions):
- `test_kmod_access` — access control via ioctl
- `test_kmod_ast` — AST delivery
- `test_kmod_eflag` — event flag operations
- `test_kmod_lock` — lock manager
- `test_kmod_vmsfs` — VMS filesystem operations

### Integration Tests

```bash
tests/integration/test_logical_names.sh
tests/integration/test_dcl_basic.sh
```
