# OVMX Architecture

## Invariant: Clean-Room Cluster Interop

All VMScluster wire-protocol work in OVMX (SCS / NISCA / NISCS / MSCP / distributed lock
manager) is **clean-room**: it is derived **only** from (a) observing traffic on the SIMH
reference lab (`~/vax/cluster/`) and (b) public OpenVMS documentation and documented tool
output (SDA, SYSGEN, SYSMAN; the *OpenVMS Cluster Systems* manual, IDSM, `$SSDEF`/`$LCKDEF`).
We **never** disassemble, decompile, or copy VSI/HPE source or binaries, and never paste
leaked VMS source. This is the legal footing for interoperability reverse-engineering
(DMCA §1201(f), EU Software Directive Art. 6) and is a hard, non-negotiable project
invariant — see rule 8 in `CLAUDE.md`. Captured protocol specimens and their decode live in
`~/vax/cluster/captures/` (`RE-specimens-2026-07-26.md`).

## Component Layers

```
Layer 7 ─ System Integration
           SSH, PAM, init scripts
           [distro/rootfs/]

Layer 6 ─ Boot & Init
           Static binaries, initramfs, QEMU boot
           [ovmx_init, init-wrapper.sh, run-qemu.sh]

Layer 5 ─ Kernel Extensions
           VMS semantics in kernel space
           [vms.ko, vmsfs.ko]

Layer 4 ─ User Interface
           DCL shell, login, help, SSH auth
           [vmsdcl, vms_login, vms_help, vms_ssh_auth]

Layer 3 ─ File Services
           Record management, VMS filesystem, logical names
           [vmsrms, vmsfs, vmslnm]

Layer 2 ─ VMS Runtime
           System services and RTL
           [libvms: syssvc/ + rtl/]

Layer 1 ─ Process Management
           PCBs, ASTs, event flags, access modes
           [vmsprocess]

Layer 0 ─ Syscall Abstraction
           Freestanding, no glibc, direct Linux syscalls
           [libvmssys + arch/x86_64 + arch/aarch64]
```

## Dependency Graph

```
libvmssys (freestanding, static only)
  │
  ├── vmsprocess (+ pthread)
  │     │
  │     └── libvms (+ pthread, math)
  │           │
  │           ├── vmslnm (+ pthread) — per-process tables only;
  │           │     no daemon (deleted, vms-a4b); executive-resident
  │           │     placement is an open ruling, see vms-ln0
  │           │     │
  │           │     └── vmsfs
  │           │           │
  │           │           └── vmsrms
  │           │
  │           └── vmsdcl (+ vmsfs, vmsprocess, optional readline)
  │
  └── ovmx_init (+ vmsprocess, pthread)

tools/
  ├── vms_login  (+ libvms, standalone SHA-256)
  ├── vms_help   (+ libvms)
  └── vms_ssh_auth (+ libvms, standalone SHA-256)

kernel/ (out-of-tree, separate build)
  ├── vms.ko    (access, ast, eflag, lock)
  └── vmsfs.ko  (super, inode, file, dir, version)
```

## Boot Sequence

OVMX has exactly one runtime: the real-kernel/QEMU path (CLAUDE.md Rule 9).
`distro/Dockerfile.bootable` is build TOOLING that produces the kernel +
initramfs — it is not itself a runtime.

```
docker build -f Dockerfile.bootable -o dist .
./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz
  │
  ├── QEMU boots Linux kernel
  └── Kernel unpacks initramfs, runs /init (init-wrapper.sh)
        ├── Mount: proc, sysfs, devtmpfs, devpts, tmpfs
        ├── Load: vms.ko, vmsfs.ko
        ├── Generate /etc/passwd, /etc/group from sysuaf.dat
        └── exec /sbin/init (ovmx_init)
              ├── (no logical name daemon — deleted, vms-a4b; VMS has no such process)
              ├── Execute STARTUP.COM
              └── Launch login shell (vmsdcl)
```

## Data Flow: User Command Execution

```
User types command via SSH
  │
  ├── sshd authenticates via vms_ssh_auth + PAM
  ├── Spawns vms_login
  │     ├── Validates against sysuaf.dat
  │     ├── Executes SYLOGIN.COM (system-wide)
  │     └── Executes LOGIN.COM (per-user)
  │
  └── Launches vmsdcl (DCL shell)
        │
        ├── dcl_lexer.c    → tokenize input
        ├── dcl_parser.c   → parse to AST
        ├── dcl_exec.c     → evaluate AST
        │     │
        │     ├── Built-in? → dcl_builtin.c (SHOW, SET, DIR, COPY, etc.)
        │     ├── Symbol?   → dcl_symbol.c → resolve and re-parse
        │     └── External? → fork/exec with VMS-style status return
        │
        ├── File ops route through:
        │     vmsrms → vmsfs → vmslnm (logical name translation)
        │
        └── System calls route through:
              libvms (syssvc/) → vmsprocess → libvmssys → Linux kernel
                                                            └── vms.ko / vmsfs.ko
```

## Key Files by Component

| Component | Key Source | Key Header | Binary |
|-----------|-----------|------------|--------|
| libvmssys | `src/libvmssys/vms_runtime_init.c` | `vmssys.h` | libvmssys.a |
| vmsprocess | `src/vmsprocess/vms_pcb.c` | `include/vms/process.h` | libvmsprocess |
| libvms | `src/libvms/syssvc/sys_qio.c` | `include/starlet.h` | libvms |
| vmslnm | `src/vmslnm/lnm_table.c` | `include/vms/logical.h` | libvmslnm |
| vmsfs | `src/vmsfs/vmsfs_translate.c` | `include/vmsfs/filespec.h` | libvmsfs |
| vmsrms | `src/vmsrms/rms_core.c` | `include/rms/rms.h` | librms |
| vmsdcl | `src/vmsdcl/dcl_main.c` | `include/dcl/context.h` | vmsdcl |
| kernel | `src/kernel/vms_module.c` | `vms_internal.h` | vms.ko |
| vmsfs.ko | `src/kernel/vmsfs/vmsfs_super.c` | `vmsfs.h` | vmsfs.ko |
| ovmx_init | `src/ovmx_init/ovmx_init.c` | — | ovmx_init |
| vms_login | `tools/vms_login.c` | — | vms_login |
