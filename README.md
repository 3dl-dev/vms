# OVMX

OpenVMS-compatible environment for Linux. OVMX implements VMS system services,
the DCL command language, Record Management Services, and VMS-style kernel
extensions — packaged as a bootable Linux distribution.

## Features

- **System Services** — `sys$assign`, `sys$qio`, `sys$crmpsc`, `sys$enq`, `sys$setimr`,
  `sys$creprc`, `sys$asctim`, `sys$gettim`, mailboxes, event flags, ASTs, logical names
- **Runtime Library** — `lib$`, `str$`, `mth$`, `ots$` routines for memory, strings,
  math, signals, date/time
- **DCL Shell** — 24 built-in commands, symbol management, lexical functions, script
  execution, VMS-style file specifications
- **Record Management Services** — Sequential, relative, and indexed file organizations
  with FAB/RAB/NAM/XAB interface
- **Logical Name Manager** — Hierarchical name tables with daemon (`vmslnmd`)
- **VMS Filesystem** — Path translation, file versioning, ODS-2 protection bits
- **Kernel Modules** — `vms.ko` (access control, ASTs, event flags, locks) and
  `vmsfs.ko` (VMS filesystem)
- **Freestanding Syscall Layer** — `libvmssys` runs without glibc (x86_64 + aarch64)
- **Bootable Distro** — Static musl binaries, initramfs, QEMU boot
- **Multi-User SSH** — `vms_login`, SYSUAF authentication, `sshd` integration

## Quick Start

### Docker (easiest)

```bash
docker compose up --build
ssh system@localhost -p 2222    # password: MANAGER
```

### Local Build

```bash
# Prerequisites (Debian/Ubuntu)
sudo apt install cmake gcc make libc6-dev libreadline-dev flex bison

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```

### Bootable QEMU VM

```bash
# Build the kernel + initramfs
docker build -f Dockerfile.bootable -o dist .

# Boot it
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz
```

## Documentation

- [Architecture](docs/architecture.md) — component layers, boot sequence, data flow
- [Building](docs/building.md) — all build modes, CMake options, kernel modules

## Project Status

See [tracking/roadmap.md](tracking/roadmap.md) for the full phase plan.
Phases 1-7 are complete. The project is functional as a Docker container and
bootable QEMU VM with multi-user SSH access.

## License

GPL-2.0 — see [LICENSE](LICENSE).
