# OpenVMX

OpenVMS-compatible environment for Linux. OpenVMX implements VMS system services,
the DCL command language, Record Management Services, and VMS-style kernel
extensions — packaged as a bootable Linux distribution.

OpenVMX is two named layers, GNU/Linux-style: **OpenVMX** is the VMS-compatible
product (login banner, `SHOW SYSTEM`, DCL — what a user touches), running on
**OVMX/Linux**, the Linux base layer underneath (kernel, boot, distro
tooling) — the rough equivalent of the VAX/Alpha hardware OpenVMS itself ran
on. See [Architecture](docs/architecture.md#product-and-kernel-layering) for
where the split falls.

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

### Bootable QEMU VM — the OpenVMX runtime

This is OpenVMX proper: it boots OVMX/Linux (kernel modules, init), which
hands off to the VMS-native toolchain (the `IMGACT.EXE` image activator and
`LINK.EXE`, on a musl userland).

```bash
# Build the kernel + initramfs
docker build -f distro/Dockerfile.bootable -o dist .

# Boot it
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz
```

### Local Build — libraries, DCL, tests

```bash
# Prerequisites (Debian/Ubuntu)
sudo apt install cmake gcc make libc6-dev libreadline-dev flex bison

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```

## Documentation

- [Architecture](docs/architecture.md) — component layers, boot sequence, data flow
- [Building](docs/building.md) — all build modes, CMake options, kernel modules
- [Install Guide](docs/install-guide.md) — `PRODUCT INSTALL` a kit onto a target volume (steps checked in CI against the real e2e gate)
- [Upgrade Guide](docs/upgrade-guide.md) — upgrade an installed system in place, preserving site config and user data (steps checked in CI against the real e2e gate)

## Project Status

See [tracking/roadmap.md](tracking/roadmap.md) for the full phase plan.
Phases 1-7 are complete. OpenVMX runs as a bootable QEMU VM — the VMS-native
runtime (image activation via `IMGACT.EXE`, linking via `LINK.EXE`, musl
userland) — with multi-user SSH access. The QEMU VM is the only OpenVMX
runtime; there is no Docker-based way to run OpenVMX (Rule 9).

## License

GPL-2.0 — see [LICENSE](LICENSE).
