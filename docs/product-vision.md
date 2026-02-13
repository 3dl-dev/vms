# Product Vision

## One-Liner

OpenVMS-compatible environment for Linux — VMS system services, DCL shell, RMS, and kernel modules packaged as a bootable distro.

## Key Decisions

| # | Question | Decision | Rationale |
|---|----------|----------|-----------|
| Q1 | Freestanding vs glibc for syscall layer? | Freestanding (no glibc) | VMS system services should not depend on Linux userspace; enables static linking for initramfs and maximum control over syscall interface |
| Q2 | Kernel modules vs pure userspace? | Both — kernel modules for VMS-specific semantics, userspace for portability | Access control, AST delivery, event flags, and filesystem versioning benefit from kernel support; userspace fallback keeps Docker mode viable |
| Q3 | Docker vs bare-metal deployment? | Both — Docker for development, QEMU bootable for production-like testing | Docker lowers the barrier to entry; bootable distro proves the full stack including kernel modules |
| Q4 | VMS compatibility vs Linux convenience? | VMS behavior wins when they conflict | The project's value proposition is VMS compatibility; deviating defeats the purpose |

Add rows as decisions are made. This table is the **source of truth** for product direction. If a calculation or spec contradicts a decision here, the conflict must be flagged explicitly — the user resolves it.

## Scope

### v1 (Now — Phases 1-7 Complete)
- Freestanding syscall layer (libvmssys) with x86_64 + aarch64
- VMS system services (13 service categories) and RTL (lib$, str$, mth$, ots$)
- Process management (PCBs, ASTs, event flags, access modes)
- Logical name manager with daemon
- VMS filesystem library (path translation, versioning, protection)
- Record Management Services (sequential, relative, indexed)
- DCL shell with 24 builtins, symbols, lexical functions, script execution
- Kernel modules (vms.ko, vmsfs.ko) with QEMU test infrastructure
- Bootable distro via Docker + QEMU
- Multi-user SSH with VMS authentication (SYSUAF, PAM)

### v2 (Later — Phase 8+)
- Enhanced DCL: pipes, redirection, batch execution
- FUSE ODS-2 driver for mounting disk images
- Rightslist integration into access checks
- Buildroot integration for reproducible distro builds
- VMS HELP content database
- Extended RMS: multi-key indexed files, journaling
- DECnet-style logical name networking
- Job controller and quota enforcement
