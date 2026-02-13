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

## Phase 8+: Future — PLANNED

- [ ] Enhanced DCL: pipes, command procedures, batch execution
- [ ] Networking: DECnet-style logical names, cluster emulation concepts
- [ ] Security hardening: privilege enforcement, audit logging
- [ ] FUSE ODS-2 driver (BUILD_FUSE option exists, not yet built)
- [ ] Buildroot integration (configs and package definitions exist in distro/)
- [ ] VMS HELP content database
- [ ] Extended RMS: multi-key indexed files, journaling
- [ ] Process management: job controller, quota enforcement
