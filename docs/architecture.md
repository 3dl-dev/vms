# OpenVMX Architecture

## Product and Kernel Layering

Following the GNU/Linux naming convention, this repo builds two named
layers, not one:

- **OpenVMX** (`OVMX_PRODUCT_NAME`) — the VMS-compatible **product**: what a
  human logs into and what identifies itself on VMS-facing surfaces (login
  banner, `SHOW SYSTEM`, `MONITOR`, DCL). This is Layer 4 and above in the
  component-layer diagram below.
- **OVMX/Linux** (`OVMX_SYSKRNL_NAME`, with the slash — exactly like
  "GNU/Linux") — the **SYSKRNL** layer (the Linux kernel underneath): kernel,
  boot sequence, and distro build tooling. Roughly the equivalent of the VAX/Alpha hardware
  OpenVMS itself ran on, except here the "hardware" is a Linux distribution.
  This is Layers 0-3 and the boot path (Layer 6).

The two identities are printed in that order at boot: `src/ovmx_init/ovmx_init.c`
prints the OVMX/Linux SYSKRNL banner first, then hands off through
STARTUP.EXE to the OpenVMX product identity a user actually sees. Single
source of truth for both: `src/libvms/include/ovmx_identity.h`
(`OVMX_PRODUCT_NAME`, `OVMX_SYSKRNL_NAME`).

The bare, un-slashed token `OVMX` survives in a handful of places this
layering deliberately does **not** touch — the `OVMX$_` VMS facility-code
prefix, the SCS/cluster wire node-name fallback, the IMGACT ELF-note owner,
and `OVMX_*`/`ovmx_*` source identifiers generally. These are machine-read,
not brand, and are pinned by `tests/integration/test_frozen_identity_tokens.sh`.

## Invariant: Clean-Room Cluster Interop

All VMScluster wire-protocol work in OVMX (SCS / NISCA / NISCS / MSCP / distributed lock
manager) is **clean-room**: it is derived **only** from (a) observing traffic on the SIMH
reference lab (`/data/training/vax/cluster/`) and (b) public OpenVMS documentation and documented tool
output (SDA, SYSGEN, SYSMAN; the *OpenVMS Cluster Systems* manual, IDSM, `$SSDEF`/`$LCKDEF`).
We **never** disassemble, decompile, or copy VSI/HPE source or binaries, and never paste
leaked VMS source. This is the legal footing for interoperability reverse-engineering
(DMCA §1201(f), EU Software Directive Art. 6) and is a hard, non-negotiable project
invariant — see rule 8 in `CLAUDE.md`. Captured protocol specimens and their decode live in
`/data/training/vax/cluster/captures/` (`RE-specimens-2026-07-26.md`).

## Component Layers

```
Layer 7 ─ System Integration
           SSH, init scripts
           [distro/rootfs/]

Layer 6 ─ Boot & Init
           Static binaries, initramfs, QEMU boot. PID 1 IS the ovmx_init
           binary, deployed as STARTUP.EXE — there is no wrapper script.
           [STARTUP.EXE (ovmx_init), run-qemu.sh, Dockerfile.bootable]

Layer 5 ─ The Executive (kernel-resident)
           The whole VMS executive in kernel space, reached through
           /dev/vms: locks, event flags, ASTs, access modes, mailboxes,
           the process table, the device table, the logical-name manager,
           the Files-11 ODS-2 ACP, and the executive-resident VMScluster
           stack (SCS / CNXMAN / DLM).
           [vms.ko = src/kernel (Linux glue) + src/kernel-core (facilities)]

Layer 4 ─ User Interface
           DCL shell (HELP is a DCL built-in), login, help image
           [vmsdcl, vms_login, vms_help]

Layer 3 ─ File & Name Services (user-space clients of the executive)
           RMS record management; ODS-2 filespec translation / codec
           helpers; a logical-name client veneer. The ODS-2 volume I/O and
           the logical-name tables themselves live in the executive
           (Layer 5); these libraries call in over /dev/vms.
           [vmsrms, vmsfs, vmslnm]

Layer 2 ─ VMS Runtime
           System services and RTL
           [libvms: syssvc/ + rtl/]

Layer 1 ─ Process Management
           PCBs, ASTs, event flags, access modes
           [vmsprocess]

Layer 0 ─ Syscall Abstraction
           Freestanding, no glibc, direct Linux syscalls
           [libvmssys + arch/x86_64 + arch/aarch64 + arch/alpha + arch/vax]
```

## Dependency Graph

```
libvmssys (freestanding, static only)
  │
  ├── vmsprocess (+ pthread)
  │     │
  │     └── libvms (+ pthread, math)
  │           │
  │           ├── vmslnm (+ pthread) — client veneer over the executive
  │           │     logical-name manager (src/kernel-core/vms_lnm.c, in
  │           │     vms.ko); no daemon (VMS has none). The tables are
  │           │     executive-resident and shared cross-process via /dev/vms.
  │           │     │
  │           │     └── vmsfs — ODS-2 filespec translation + codec helpers
  │           │           │      (the volume I/O itself is the executive ACP)
  │           │           │
  │           │           └── vmsrms
  │           │
  │           └── vmsdcl (+ vmsfs, vmsprocess, optional readline)
  │
  └── ovmx_init (+ vmsprocess, pthread)

tools/
  ├── vms_login  (+ libvms, standalone SHA-256)
  └── vms_help   (HELP.EXE — thin wrapper over the shared DCL help engine
                  src/vmsdcl/dcl_help.c; HELP is primarily a DCL built-in)

kernel/ (the executive — src/kernel Linux glue + src/kernel-core facilities;
         built in-tree via drivers/ovmx/ for the distro kernel, and
         out-of-tree against installed headers for the QEMU test harness)
  └── vms.ko    access modes, ASTs, event flags, mailboxes, the process
                table, the device table, the LOCK MANAGER, the logical-name
                manager, the Files-11 ODS-2 ACP, and the executive-resident
                VMScluster stack (SCS / CNXMAN / DLM). Reached via /dev/vms.
                (On NetBSD/VAX the same src/kernel-core builds as the
                vms.kmod module — see docs/building-multiarch.md.)
```

## Boot Sequence

OpenVMX has exactly one runtime: the real-kernel/QEMU path (CLAUDE.md Rule 9).
`distro/Dockerfile.bootable` is build TOOLING that produces the kernel +
initramfs — it is not itself a runtime.

```
docker build -f distro/Dockerfile.bootable -o dist .
./distro/boot/run-qemu.sh dist/boot/vmlinuz dist/boot/initramfs-ovmx.cpio.gz
  │
  ├── QEMU boots the Linux kernel
  └── Kernel unpacks the initramfs and runs /init directly — which IS
      STARTUP.EXE (the ovmx_init binary), PID 1. There is NO wrapper
      script and NO busybox (init-wrapper.sh is retired): PID 1 does the
      bootstrap itself (vms-9b7, vms-2f0). BOOTSTRAP ONLY — it does not
      install, INITIALIZE or provision. PID 1 does NOT read SYSUAF and is
      NOT SYSTEM; it holds only what the executive derived from root's
      credentials at registration (UIC [0,0], empty username).
        ├── mount the Linux base layer (proc, sysfs, devtmpfs, devpts, tmpfs)
        ├── ovmx_boot_load_module("vms") → finit_module(2) loads vms.ko;
        │     executive_attach opens /dev/vms (the executive I/O + device table)
        ├── opcom_kmsg_start() -- a detached thread that reads /dev/kmsg and
        │     reformats vms.ko's own printk records as bare
        │     "%OVMX-<S>-<IDENT>, text" lines (started right after the module
        │     load so init-time records are replayed, not missed). SYSKRNL
        │     (Linux-kernel-layer) lines (module-taint warnings, hrtimer, ...)
        │     are RE-STYLED too, as "%SYSKRNL-<S>-KERNEL, text" -- not
        │     suppressed, since they carry real operator-relevant information.
        │     BOTH facilities go to SYS$MANAGER:OPERATOR.LOG ONLY -- this
        │     bridge never opens /dev/console at all (routing EITHER facility's
        │     kmsg lines to the console broke the oracle-pinned boot sequence
        │     twice -- PR #365, rounds 1 and 2). Routine INFO-level
        │     device/bus-probe chatter is dropped as operator-worthless. See
        │     the design subsection below and design-opcom-executive-logging.md.
        ├── mount the pre-installed ODS-2 system disk over the executive
        │     Files-11 ACP ($ASSIGN + IO$_ACCESS/READVBLK) -- NO /vms
        │     passthrough (retired). If there is no valid installed system disk
        │     PID 1 HALTS honestly (%OVMX-F-EXECINIT) rather than provisioning
        │     one -- a booting VMS system finds its disk or stops.
        ├── lnm_setup_defaults + init_search_paths (SYS$SYSTEM:, SYS$SHARE:)
        └── exec SYS$SYSTEM:PROVISION.EXE — the startup process (where PID 1
              │  used to exec DCL.EXE). Every shareable .EXE is activated by
              │  IMGACT.EXE (src/imgact/), the static-PIE image activator named
              │  as the PT_INTERP of the image: it maps the image and resolves
              │  its .vms$sv symbol vector against the shareable images. (See
              │  docs/design-imgact-vms-activation-context.md.)
              ├── vms_kif_establish_system() → the executive stamps SYSTEM
              │     [1,4]/ALL onto THIS process from constants vms.ko owns
              │     (VMS_SYSTEM_UIC [1,4], VMS_PRV_M_SYSTEM_ALL) -- no
              │     username/uic/privs args for this process to have supplied.
              ├── provision home directories + system-tree ownership. This is
              │     PROVISION.EXE's ONE SYSUAF read (sysuaf_read_line/
              │     sysuaf_parse_line, vms-9b7) -- home-directory provisioning,
              │     also the "does SYSUAF have a SYSTEM account at all" check.
              └── exec DCL.EXE on SYS$MANAGER:STARTUP.COM — SAME PROCESS.
                    exec(2) preserves the executive's SYSTEM identity, so
                    STARTUP.COM / SYSTARTUP_VMS.COM run under SYSTEM, exactly
                    as OpenVMS (STARTUP runs as SYSTEM).
                    │
                    └── SYSTARTUP_VMS.COM → @SYS$STARTUP:JOB_CONTROL_STARTUP.COM
                          RUN/DETACHED/PROCESS_NAME=JOB_CONTROL (vms-47b's
                          mechanism) creates JOB_CONTROL.EXE
                          (src/ovmx_job_control/ovmx_job_control.c) as a
                          DETACHED process — NOT PID 1's child — with
                          /INPUT /OUTPUT /ERROR pointed at the physical
                          console (/dev/console). JOB_CONTROL owns the
                          console session from here on (vms-8d2):
                            └── fork/exec SYS$SYSTEM:LOGINOUT.EXE
                                  (tools/vms_login.c) on the console,
                                  forever, with retry/backoff on repeated
                                  failure. LOGINOUT is SYSUAF's FIRST
                                  reader for an authenticated identity,
                                  matching OpenVMS. Login shells (vmsdcl)
                                  launch under the authenticated session.

STARTUP.EXE (PID 1) returns from run_startup() once STARTUP.COM has finished —
by which point JOB_CONTROL already owns the console — and then waits
(reaping anything reparented to it, answering SIGTERM) rather than exiting,
because Linux's PID 1 cannot exit without panicking the kernel. It contains
no login loop of its own; grep src/ovmx_init/ovmx_init.c finds none.
```

### Executive kernel messages → the operator surface (vms-32a)

Full design: `docs/design-opcom-executive-logging.md` (two-vocabulary model,
lab-Alpha oracle citations, the OVMX/SYSKRNL facility+ident choices under
Rule 8, the route-by-default operator-ruling correction of 2026-08-12, and
the three-round path to the OPERATOR.LOG-only destination, PR #365).

`vms.ko`/`vmsfs.ko` speak only through `printk` (`pr_info`/`pr_warn`/
`pr_err`) — there is no kernel-to-user push channel and this item adds none
(`/dev/vms`'s `file_operations` stay ioctl + mmap only). Two independent
pieces close the gap between "the kernel module said something" and "a VMS
operator can see it":

- **`src/ovmx_init/opcom_kmsg.c`** — a detached pthread, started early in
  `bare_metal_init()`, that reads the standard `/dev/kmsg` device (seek to
  start, then poll + follow) and reformats each record as a bare
  `%FACILITY-<S>-<IDENT>, text` line (boot-time vocabulary shape — no OPCOM
  banner). `vms:`/`vmsfs:`-prefixed records (vms.ko/vmsfs.ko's own) wear
  the `OVMX` facility; everything else that clears the severity bar —
  SYSKRNL (Linux-kernel-layer) lines, including the kernel's own generic
  module-taint warning — is RE-STYLED, wearing `SYSKRNL`. **Both
  facilities go to `SYS$MANAGER:OPERATOR.LOG` ONLY; this bridge never
  opens `/dev/console` at all.** `tests/qemu/test_boot_conformance.sh` pins
  the exact ordered console facility+ident sequence against the OpenVMS
  oracle, produced entirely by the boot orchestrator
  (`ovmx_init`/`PROVISION.EXE`/`SYSTARTUP_VMS.COM`) — never by a kernel
  module's printk. Two earlier cuts (console for `OVMX`-facility lines;
  then also for `SYSKRNL` lines) each broke that pinned sequence in turn
  (PR #365, rounds 1 and 2); the definitive fix routes everything to the
  log. Routine INFO-level device/bus-probe chatter is dropped as genuinely
  operator-worthless. (The taint warning happens to also start with
  `vms: `, because Linux substitutes the loading module's own name into
  it, so it is classified alongside vms.ko's own lines under `OVMX` rather
  than `SYSKRNL` — a disclosed simplification, not a defect: content and
  severity both stay the kernel's real ones, and the destination is the
  same either way.)
- **`src/libvms/syssvc/sys_operator.c`** (`sys$sndopr`) — writes the actual
  OPCOM records to `SYS$MANAGER:OPERATOR.LOG`, now in the oracle-exact
  shape: an eleven-`%` boxed banner (`%%%%%%%%%%%  OPCOM  DD-MMM-YYYY
  HH:MM:SS.ss  %%%%%%%%%%%`) followed by `Request N, from user U on N`,
  where `N` (the node) is `ovmx_node_name()` — the real configured SCSNODE
  — not a hardcoded literal.

### SYSUAF.DAT — one format, one reader, one writer (vms-9b7)

`src/libvms/include/sysuaf.h` is the single definition of the SYSUAF text
format: the `|` separator, field order/count, the octal UIC radix, and the one
`SYSUAF_LINE_MAX`. Every accessor derives from it — `sysuaf_parse_line`,
`sysuaf_format_record` (which **refuses** an over-length record rather than
letting a reader silently truncate it), and `sysuaf_read_line` (which **reports**
an over-length line). This replaced five independent hand-rolled parsers that
carried three different line limits and two writer format strings; the
disagreement between a 512-byte reader (PID 1) and a 1024-byte writer is what let
a long SYSTEM row split across reads and halt the boot with
`%OVMX-F-EXECINIT, no SYSTEM record`. The FLAGS field is a comma-separated list
of UAI flag **names** in the file and the `UAI$M_*` longword through
`$GETUAI`/`$SETUAI`; `sysuaf_flags_to_mask`/`sysuaf_mask_to_flags` are the only
conversion, so the file has one answer and the API has one answer.

## Data Flow: User Command Execution

```
User types command via SSH
  │
  ├── sshd (OpenSSH port, src/vmsssh) authenticates
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
        │     vmsrms (RMS) → the executive Files-11 ODS-2 ACP over /dev/vms
        │       ($ASSIGN + IO$_ACCESS/READVBLK/WRITEVBLK — no /vms
        │       passthrough). Logical names resolve against the executive
        │       LNM; vmslnm is the client veneer, vmsfs the filespec / ODS-2
        │       codec helper.
        │
        └── System services route through:
              libvms (syssvc/) → vmsprocess → libvmssys → the executive
                via /dev/vms (vms.ko)
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
| executive (Linux glue) | `src/kernel/vms_module.c` | `vms_internal.h` | vms.ko |
| executive (facilities) | `src/kernel-core/*.c` (locks, EF, AST, mailbox, proctab, devtab, LNM, ODS-2 ACP, cluster) | `src/kernel/vms_acp.h`, … | (into vms.ko / vms.kmod) |
| image activator | `src/imgact/imgact.c` | `src/imgact/arch/<a>/imgact_arch.h` | IMGACT.EXE |
| ovmx_init (PID 1) | `src/ovmx_init/ovmx_init.c` | — | STARTUP.EXE |
| vms_login | `tools/vms_login.c` | — | vms_login |
