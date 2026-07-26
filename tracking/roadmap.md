# OVMX Roadmap

## Phase 1: Syscall Layer (libvmssys) — COMPLETE

- [x] Direct Linux syscall wrappers (no glibc dependency)
- [x] Freestanding string utilities, snprintf, buffered I/O
- [x] Futex-based synchronization primitives
- [x] Math functions
- [x] Runtime initialization (crt0)
- [x] x86_64 assembly (syscall.S, crt0.S, sigreturn.S)
- [x] aarch64 assembly (syscall.S, crt0.S, sigreturn.S)
- [x] Kernel interface layer (vms_kif)
- [x] 7 test programs (syscall, string, snprintf, futex, stdio, math, crt)

## Phase 2: System Services & RTL (libvms) — COMPLETE

- [x] System services: assign, QIO, io_uring, event, time, process, memory
- [x] System services: logical names, AST, lock manager, mailbox, security
- [x] RTL: lib$ (VM, output, signal, datetime, misc)
- [x] RTL: str$ routines, mth$ routines, ots$ routines
- [x] VMS descriptor support (descrip.c)
- [x] VMS status code infrastructure (status.c, ssdef.h, stsdef.h)
- [x] Public headers: starlet.h, descrip.h, iodef.h, lnmdef.h, prcdef.h, rmsdef.h

## Phase 3: Filesystem & Logical Names — COMPLETE

- [x] VMS filesystem library (path translation, versioning, case, protection)
- [x] Logical name manager library (tables, translation, client interface)
- [x] Logical name daemon (vmslnmd)
- [x] Default logical name definitions
- [x] Record Management Services: sequential, relative, indexed files
- [x] RMS data structures: FAB, RAB, NAM, XAB
- [x] RMS parse and search operations
- [x] Process control blocks, ASTs, event flags, access modes

## Phase 4: DCL Shell — COMPLETE

- [x] Lexer (tokenization)
- [x] Recursive descent parser
- [x] Execution engine
- [x] 24 built-in commands (dcl_builtin.c)
- [x] Symbol management
- [x] Lexical functions (F$SEARCH, F$PARSE, etc.)
- [x] VMS-style file specification handling
- [x] I/O subsystem
- [x] Script execution (.COM files)
- [x] Optional readline support

## Phase 5: Kernel Modules — COMPLETE

- [x] vms.ko: module init, access control, AST, event flags, lock manager
- [x] vmsfs.ko: superblock, inode, file, directory, versioning
- [x] Standalone Makefiles for out-of-tree build
- [x] QEMU-based kernel module test infrastructure
- [x] 5 kernel test programs (access, ast, eflag, lock, vmsfs)
- [x] 62 test assertions across kernel modules
- [x] Test runner with QEMU boot and serial output capture

## Phase 6: Minimal Bootable Distro — COMPLETE

- [x] Static musl-gcc build mode (OVMX_STATIC)
- [x] Dockerfile.bootable: kernel + initramfs builder
- [x] init-wrapper.sh: PID 1 bootstrap (mount, module load, user setup)
- [x] ovmx_init: boot orchestrator
- [x] run-qemu.sh: QEMU launcher (x86_64 + aarch64)
- [x] Initramfs assembly with busybox
- [x] Kernel module inclusion in initramfs

## Phase 7: Multi-User & SSH — COMPLETE

- [x] vms_login: authentication against SYSUAF
- [x] vms_ssh_auth: SSH authorized key validation
- [x] SYSUAF database (sysuaf.dat)
- [x] Rights database (rightslist.dat)
- [x] sshd configuration (sshd_config.ovmx)
- [x] PAM integration for SSH
- [x] Docker compose with SSH on port 2222
- [x] VMS login scripts (LOGIN.COM, SYLOGIN.COM, STARTUP.COM)
- [x] System logical names configuration (sylogicals.conf)
- [x] Init script (S50ovmx)

---

## PIVOT (2026-07-25): from standalone compatibility to cluster interop

Phases 1-7 built a standalone VMS-compatible environment. The project has since
pivoted to its real north star — **cut VSI's legs out via cluster interop**: an
OVMX Linux node that joins a customer's live VMScluster so they migrate off VSI
node-by-node, zero downtime. See `docs/product-vision.md` for the full thesis.

Forward work is organized as **two co-required rails** (neither defers), tracked
as rd epics rather than linear phases:

### Rail A — Cluster interop (`vms-ci`) — the migration path IN
Clean-room RE of the VMScluster wire protocol against a SIMH VAX 7.3 reference
lab (`~/vax/cluster`). Ladder:
- [ ] `vms-ci.0` Clean-room wire-only rule recorded as project invariant
- [ ] `vms-ci.1` Distinct VAX node added to reference cluster + pristine formation capture
- [ ] `vms-ci.2` NISCA/SCS/MSCP dissector + written protocol spec
- [ ] `vms-ci.3` OVMX node appears as a member in real `SHOW CLUSTER`
- [ ] `vms-ci.4` MSCP-served disk across the OVMX boundary
- [ ] `vms-ci.5` Distributed Lock Manager participation (`$ENQ`/`$DEQ`) — data-integrity critical
- [ ] `vms-ci.6` Rolling evacuation demo (workload moves VMS→OVMX, cluster stays up)

### Rail B — VMS compatibility / image activation (`vms-913` + compat) — run their software
- [ ] `vms-913` VMS image activation (IMGACT.EXE, shareable images, INSTALL, system disk) — a co-equal pillar, NOT deferred
- [ ] `vms-801` Provable source compatibility with OpenVMS
- [ ] `vms-898` Authenticity / no-Unix-leaks — enforced as a build-failing invariant

The rails converge at `vms-ci.6` (evacuation needs both DLM and image activation).

### Reorientation (`vms-pivot`)
- [x] `vms-pivot.1` Reframe product vision + roadmap around cluster interop (this pivot)
- [ ] `vms-pivot.2` Cluster-node architecture design + stale-implementation assessment
- [ ] `vms-pivot.3` Re-triage the existing backlog against the two rails

### Legacy Phase 8+ backlog (to be re-triaged in `vms-pivot.3`)
Enhanced DCL (pipes/batch), DECnet-style networking, security hardening, FUSE
ODS-2 driver, Buildroot integration, VMS HELP database, extended RMS
(multi-key/journaling), job controller + quotas. These remain valid work but are
reprioritized against the two rails — some are load-bearing for a cluster node,
others defer.
