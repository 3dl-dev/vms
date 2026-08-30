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
  `sys$creprc`, `sys$asctim`, `sys$gettim`, mailboxes, event flags, ASTs, logical names —
  kernel-resident in the executive (`vms.ko`), reached through `/dev/vms`
- **Runtime Library** — `lib$`, `str$`, `mth$`, `ots$` routines for memory, strings,
  math, signals, date/time
- **DCL Shell** — the DCL command language: 50+ built-in verbs, symbol management,
  lexical functions, script execution, VMS-style file specifications (see
  [the command reference](docs/dcl-commands.md))
- **Record Management Services** — Sequential, relative, and indexed file organizations
  with the FAB/RAB/NAM/XAB interface; file-share and record locking arbitrated by the
  distributed lock manager
- **Files-11 ODS-2** — Genuine ODS-2 volumes read and written over the executive ACP —
  `INITIALIZE` writes a real volume, `MOUNT` parses the home block/SCB, and RMS resolves
  through FID → file header → extents — not a host-filesystem passthrough
- **Authentication** — Binary SYSUAF with the Purdy password hash; DCL login
  (`vms_login`) and OpenSSH-based multi-user access
- **Cluster participation** — A real distributed lock manager (6-mode `$ENQ`/`$DEQ`,
  blocking ASTs, deadlock detection, lock value blocks) running cross-node over the SCS
  wire; cluster membership resident in the executive, so `SHOW CLUSTER` and `$GETSYI`
  read the real member block; RMS file/record locking behind the DLM
- **TCP/IP Services** — VMS-faithful IP layered product: the `TCPIP$CONFIG`
  configuration plane, `TCPIP$` logicals, `BGn:` socket transport, `PING`, and an
  inetd-style auxiliary server
- **Logical Name Manager** — Hierarchical name tables; `LNM$SYSTEM` is executive-resident
  and visible node-wide
- **Kernel Module** — `vms.ko` (the VMS executive: access control, ASTs, event flags,
  the distributed lock manager, mailboxes, and the Files-11 ODS-2 ACP)
- **Freestanding Syscall Layer** — `libvmssys` runs without glibc (x86_64, Alpha, VAX,
  aarch64)
- **Bootable Distro** — Static musl binaries, initramfs, QEMU boot

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

Start at the [Documentation Index](docs/index.md) for the full map. Key entries:

- [Architecture](docs/architecture.md) — component layers, boot sequence, data flow
- [Building](docs/building.md) — all build modes, CMake options, kernel modules
- [Install Guide](docs/install-guide.md) — `PRODUCT INSTALL` a kit onto a target volume (steps checked in CI against the real e2e gate)
- [Upgrade Guide](docs/upgrade-guide.md) — upgrade an installed system in place, preserving site config and user data (steps checked in CI against the real e2e gate)
- [DCL Command Reference](docs/dcl-commands.md) — the built-in verb set
- [System Services API](docs/api-system-services.md) — the `sys$` reference
- [Compatibility Surface](docs/compatibility-surface.md) — per-surface status register
- [Release Roadmap to 1.0](docs/release-roadmap-to-1.0.md) — milestone ladder and roadmap of record

## Project Status

OpenVMX ships on a milestone ladder toward 1.0. The current release is **V0.6**
(cluster correctness): a genuine VMScluster participant — the distributed lock
manager runs the full ladder over the SCS wire, RMS file/record locking reaches
the real arbitrator, and cluster membership lives in the executive. Earlier
milestones delivered the executive / DCL / RMS / kernel foundation (**0.3**),
faithful install-and-boot (**0.4**), and the authenticity flip to genuine
Files-11 ODS-2 over the executive ACP with binary SYSUAF/Purdy login (**0.5**).
See [the release roadmap to 1.0](docs/release-roadmap-to-1.0.md) for the full
milestone ladder and current state.

First-class release architectures are **x86_64, Alpha (LP64), and VAX** (on the
NetBSD substrate); **aarch64** is also supported. Every release is co-released
across these architectures through a shared build/release gate.

OpenVMX runs as a bootable QEMU VM — the VMS-native runtime (image activation via
`IMGACT.EXE`, linking via `LINK.EXE`, musl userland) — where `vms.ko` provides the
VMS executive and userspace reaches it through `/dev/vms`. The QEMU / real-kernel
path is the only OpenVMX runtime; there is no Docker-based way to run OpenVMX
(Rule 9).

## License

GPL-2.0 — see [LICENSE](LICENSE).
