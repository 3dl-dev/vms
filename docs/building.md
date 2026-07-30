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

# For Docker
sudo apt install docker.io docker-compose-v2
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

## Docker Container

Full runtime with SSH access.

```bash
docker compose up --build
```

- Builds OVMX inside Ubuntu 24.04 container
- Installs openssh-server, configures SYSUAF users
- SSH available on port 2222: `ssh system@localhost -p 2222` (password: `MANAGER`)
- Runs `ovmx_init` as PID 1

## Bootable QEMU VM

Builds a minimal Linux system with OVMX as the userspace.

```bash
# Build kernel + initramfs (outputs to dist/)
docker build -f Dockerfile.bootable -o dist .

# Boot with QEMU
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz

# Custom memory (default 512M)
MEMORY=1G ./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz
```

The bootable image includes:
- Stock Ubuntu kernel (vmlinuz)
- Initramfs with busybox, static OVMX binaries, kernel modules
- `init-wrapper.sh` as PID 1: mounts filesystems, loads vms.ko + vmsfs.ko, launches ovmx_init

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
