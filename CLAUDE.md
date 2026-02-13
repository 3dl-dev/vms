# OVMX - Claude Onboarding

OpenVMS-compatible environment for Linux. Implements VMS system services, DCL shell,
RMS file system, and kernel modules — packaged as a bootable distro.

## Project Status

- **Roadmap & phases**: `tracking/roadmap.md` (Phases 1-7 complete)
- **Current focus**: `tracking/status.md`
- **Ideas & debt**: `tracking/backlog.md`

## Quick Reference

```bash
# Development build (shared libs, readline, tests)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_TOOLS=ON
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure

# Static build (musl)
cmake -B build-static -DCMAKE_C_COMPILER=musl-gcc -DOVMX_STATIC=ON -DBUILD_TOOLS=ON
cmake --build build-static -j$(nproc)

# Docker container (SSH on port 2222)
docker compose up --build
ssh system@localhost -p 2222    # password: MANAGER

# Bootable QEMU VM
docker build -f Dockerfile.bootable -o dist .
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz

# Kernel modules (needs kernel headers)
make -C src/kernel
make -C src/kernel/vmsfs

# QEMU kernel module tests
tests/qemu/run_tests.sh
```

## Source Layout

| Directory | What |
|-----------|------|
| `src/libvmssys/` | Freestanding syscall library (no glibc). x86_64 + aarch64 assembly. |
| `src/libvms/` | VMS runtime: system services (`syssvc/`) and RTL (`rtl/`) |
| `src/vmsprocess/` | Process control blocks, ASTs, event flags, access modes |
| `src/vmslnm/` | Logical name tables, translation, daemon (`vmslnmd`) |
| `src/vmsfs/` | VMS filesystem: path translation, versioning, protection bits |
| `src/vmsrms/` | Record Management Services: sequential, relative, indexed files |
| `src/vmsdcl/` | DCL shell: lexer, parser, executor, builtins, symbols, scripts |
| `src/kernel/` | Linux kernel module `vms.ko` (access, AST, eflag, lock) |
| `src/kernel/vmsfs/` | Linux kernel module `vmsfs.ko` (superblock, inode, file, dir) |
| `src/ovmx_init/` | PID 1 boot orchestrator |
| `tools/` | `vms_login`, `vms_help`, `vms_ssh_auth` |
| `distro/rootfs/` | Root filesystem: VMS configs, SSH, PAM, init scripts |
| `distro/boot/` | `run-qemu.sh`, `init-wrapper.sh` |
| `tests/` | Integration, QEMU kernel, libvmssys, conformance tests |

## Key Conventions

- **Language**: C11 with GNU extensions (`_GNU_SOURCE`), compiled with `-Wall -Wextra`
- **Naming**: VMS-style APIs (`sys$`, `lib$`, `str$`, `mth$`, `ots$`), prefixed C names
- **Status codes**: VMS status codes via `ssdef.h` / `stsdef.h` (odd = success, even = error)
- **Architecture**: x86_64 primary, aarch64 supported
- **License**: GPL-2.0
- **libvmssys**: Freestanding — compiled with `-ffreestanding -fno-builtin`, no glibc

## Library Build Order (Dependency Graph)

```
libvmssys          (freestanding, no deps)
  └─ vmsprocess    (+ pthread)
       └─ libvms   (+ pthread, m)
            ├─ vmslnm   (+ pthread)
            │    └─ vmsfs
            │         └─ vmsrms
            └─ vmsdcl   (+ vmsfs, vmsprocess, optional readline)
```

## When Working on This Project

1. Read `tracking/status.md` for current focus
2. Read `tracking/roadmap.md` for phase context
3. After completing work, update `tracking/status.md`
4. When a phase completes, update `tracking/roadmap.md`
5. Add discovered issues/ideas to `tracking/backlog.md`
6. Never delete history from tracking files — move items between sections
