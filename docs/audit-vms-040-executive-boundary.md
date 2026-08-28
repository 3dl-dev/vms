# Audit — vms-040 Phase A: where OVMX userspace can bypass the executive

Status: **Phase A (audit) complete.** This document enumerates and categorizes
every place an OVMX userspace image can reach VMS-facility state with a **raw
Linux syscall / libc call** instead of going through the executive (`vms.ko` via
`/dev/vms`). It is **input to the operator's scope-gate on Phase B** (the
enforcement mechanism), not an implementation. No code was changed.

- **Item:** vms-040 (executive-boundary enforcement).
- **Baseline audited:** `origin/main` @ `f613190e` (audited in a clean worktree;
  the shared checkout's working branch is stale and was **not** used).
- **Method / clean-room:** static reading + grep of the shipped image set. No
  VSI/HPE source or binary was consulted; findings are derived only from OVMX's
  own source and public VMS documentation. Enumeration was fanned out across
  four facility areas and each finding re-read for classification.

---

## 1. Why this audit exists

OVMX's core invariant is that **the executive is authoritative** for VMS
facilities — locks/`$ENQ`, event flags, ASTs, the process table/`$GETJPI`,
mailboxes, logical names, RMS/Files-11 over the ACP, device allocation,
identity/privileges. The invariant holds *by enforcement* only where a facility
physically cannot be done any other way.

But OVMX images are **musl-linked** and can issue **raw Linux syscalls
directly** (`openat`/`read`/`write`/`mmap`/`kill`/`futex`/`flock`/…). Wherever a
VMS facility is implemented as a raw Linux call instead of an executive call,
the executive is authoritative **only by convention** — nothing stops the image
from doing the facility itself, per-process, and reporting success while
sharing nothing. That is the exact fabrication/LARP class the project exists to
kill (INV-6, Rule 11).

The existing standing gate `tests/integration/test_runtime_target.sh` enforces
this **at the source level** (it greps for silent-fallback prose and fake
patterns). It cannot see a musl image issue a raw syscall **at runtime**. Phase
B closes that gap structurally (authoritative-*by-enforcement*). The 1.0 goal
in `docs/release-roadmap-to-1.0.md` states it directly: *"authenticity enforced
by the executive, not by convention."*

---

## 2. The runtime model (what routes where)

OVMX userspace has **two** downward interfaces:

1. **The Linux substrate — raw syscalls.** `src/libvmssys/vms_syscall.h`
   provides typed wrappers (`vms_sys_openat`, `vms_sys_mmap`, `vms_sys_kill`,
   `vms_sys_futex`, …) over the arch trampolines; musl provides the same set to
   images through its normal libc entry points (`open`/`read`/`fork`/`flock`/…).
   These go **straight to the host kernel** and the executive never sees them.

2. **The executive channel — `ioctl()` on the `/dev/vms` fd.** Every executive
   facility is transported as an `ioctl` on the per-thread `/dev/vms` descriptor
   (`src/libvmssys/vms_kif.c`, `KIF_CALL(VMS_IOCTL_*)`). This includes the whole
   `vms_kif_*` family: locks (`_enq`/`_deq`/`_convert`), event flags, ASTs, the
   process table (`_getjpi_*`/`_setprn`/`_register`), logical names (`_lnm_*`),
   mailboxes (`_mbx_*`), device table, **and Files-11 file I/O over the ACP**
   (`_acp_assign`/`_access`/`_readvb`/`_writevb`/`_acpcontrol`/`_fileop`).

Two architectural facts make the boundary tractable for enforcement (see §6):

- **The ACP file-I/O path is `ioctl`-based, not `read`/`write`-based.**
  `vms_kif_acp_readvb`/`writevb` are `VMS_IOCTL_ACP_READVBLK`/`WRITEVBLK` on the
  `/dev/vms` fd. So legitimate Files-11 I/O and raw file I/O are *different
  syscalls* (`ioctl` vs `read`/`write`), not the same syscall on a different fd.
- **The ODS-2 backing block device is opened kernel-side only.** Only the
  vmsfs kernel module (`filp_open`, `src/kernel/vmsfs/*`) and the boot-time
  MOUNT open the backing device. **No userspace image ever holds a backing-store
  fd** — an image that does raw file I/O is hitting a VFS-mounted path, not the
  raw volume, so it can be denied the backing store entirely without breaking
  the legitimate ACP path.

### What legitimately stays a raw Linux syscall

An image *must* use the Linux kernel to run at all. The following are the
**substrate** and are not bypasses:

- Memory: `mmap`/`munmap`/`mprotect`/`brk`/`madvise` (P0/P1 windows, heap, TLS).
- Thread/TLS bring-up: `arch_prctl`/`set_tid_address`/`set_robust_list`, and
  `futex` **when used for pthread mutex/cond over process-local data**.
- The `execve`/`clone` that is the *real mechanism under* an executive-authorized
  `$CREPRC`/`SPAWN`/image activation (when paired with a `vms_kif_register*`).
- Console/terminal and socket byte transport on fds the process was handed.
- Time (`clock_gettime`/`nanosleep`), signals plumbing, and reads of host
  telemetry (`/proc/*`) for MONITOR-style display.
- Explicitly host-side staging: the tmpfs **boot-bridge** that stages a
  first-hop image whose *bytes were read from the volume over the ACP* — this
  is Linux `execve` reality, not a Files-11 read bypass.
- Genuinely-Linux-only operations with no VMS-executive equivalent: `mount(2)`
  in the setuid mount helper; block writes by `INITIALIZE`/`ANALYZE` (VMS's own
  `INIT`/`ANALYZE` write raw blocks below RMS); build-host packaging/seed tools.

### Classification rubric used below

- **GENUINE-BYPASS** — a VMS facility the executive should own, done as a raw
  Linux call or as per-process fake state. Enables per-process fake success /
  unenforced protection. **This is the Phase-B target set.**
- **LEGIT-SUBSTRATE** — an inherently-Linux call not tied to executive facility
  state (the list just above).
- **BORDERLINE** — disclosed/gated, or VMS-faithfully process-local, or a
  fallback that fails honestly rather than faking. Not the primary Phase-B
  target but relevant to a complete fence.

---

## 3. Findings by facility

Counts are **GENUINE-BYPASS sites** unless noted. Positive results (facilities
already fully executive-routed) are called out because they define the *reachable
floor* for Phase-B enforcement.

### 3.1 File I/O — should be RMS/`$QIO` over the ACP

This is the largest bypass surface. Three sub-populations:

**(a) The core `$QIO` gap.** `src/libvms/syssvc/sys_qio.c` — ordinary `$QIO`
file I/O never reaches the ACP. `qio_validate_and_classify` maps the channel to
a raw fd (`vms$$chan_to_fd`) and `sys$qio(w)` transfers with raw
`read`/`write`/io_uring; **there is not a single `vms_kif_acp_*` call in the
file** (only mailbox/BG-device `$QIO` is executive-routed, and terminal `$QIO`
consults the executive for identity but still transfers on the raw fd). `$QIO`
to a disk file is the most common low-level I/O path in VMS code, and it is
entirely raw-fd. — `sys_qio.c:89,127,395,413-426,513-568` — **GENUINE-BYPASS.**

**(b) DCL commands never migrated by the ACP flip.** TYPE/COPY/DELETE/CREATE/HELP
*were* migrated (they route through `dcl_rms_read_open`/`write_create` or the ACP
with an honest executive-absent gate). These were not:

| Command | file:line | raw call |
|---|---|---|
| LIBRARY (.TLB/.HLB/.OLB — whole file) | `src/vmsdcl/dcl_library.c:92,126,188,347,490,641,692,870,1062` | `fopen`/`fread`/`fwrite` |
| BACKUP (savesets + sources) | `src/vmsdcl/dcl_backup.c:145,166,247,336,397` | `fopen` |
| EDIT (EDT load/save) | `src/vmsdcl/dcl_editor.c:89,112` | `fopen` |
| SEARCH | `src/vmsdcl/dcl_cmd_file.c:2781` | `fopen` |
| APPEND | `src/vmsdcl/dcl_cmd_file.c:3095,3101` | `fopen` |
| PURGE (no ACP gate, unlike sibling DELETE) | `src/vmsdcl/dcl_cmd_file.c:2928` | `opendir`+`unlink` |
| DIFFERENCES / SORT | `src/vmsdcl/dcl_cmd_misc.c:89,94,198,247` | `fopen` |
| FTP local file | `src/vmsdcl/dcl_cmd_misc.c:3094,3118` | `fopen` |
| SPAWN `/OUTPUT=` (raw qualifier, never resolved) | `src/vmsdcl/dcl_cmd_process.c:2177` | `open(O_CREAT\|O_TRUNC)` |

All **GENUINE-BYPASS.** PURGE is a live inconsistency worth flagging: DELETE in
the same file gates on `rms_executive_absent()`; PURGE never checks and never
calls the ACP `$ERASE` path at all.

**(c) Utilities storing VMS-facility data on the raw Linux fs or the retired
`/vms` literal path:**

| Facility | file:line | raw call | note |
|---|---|---|---|
| **MAIL / VMSMAIL** message store | `tools/vms_mail.c:153,164,216,312,352,452,496` | `mkdir`/`fopen`/`unlink` | Whole store (maildir, MAIL.IDX, message files) on raw Linux fs; `deliver_message()` writes **another user's** mailbox with no ACP-mediated access check |
| SYSMAN STARTUP_LIST | `tools/vms_sysman.c:103,187,191,194,223,269` | `fopen`/`mkdir` | Hard-codes the retired `/vms/...` literal path; no CURRENT/ACTIVE-vs-ACP split (the exact defect HELP.EXE was fixed for under vms-4ac). PARAMETERS in the same file *is* ACP-backed. |
| TCPIP$CONFIG current config | `tools/vms_tcpip_config.c:53,73` | `fopen` | Retired `/vms/...SYSEXE/TCPIP$*.DAT` literal path |
| Queue-manager DB | `src/vmsqueue/vmsqueue.c:222-281` | `vmsfs_to_linux_path(/vms…)`+`fopen` | Retired passthrough |
| Known-images DB (INSTALL) | `src/install/install.c:47-94` | `fopen(/vms/SYS0/…)` | Retired passthrough |

All **GENUINE-BYPASS.** The `/vms`-literal cases are both a bypass **and**
probably non-functional at runtime (that mount is retired), so they degrade
silently rather than fabricate — but SYS$MANAGER/config data still never goes
near the executive.

**Positive / not-primary-path:**
- **RMS + vmsfs (`src/vmsrms/`, `src/vmsfs/`): 0 genuine bypasses on the primary
  path.** All 46 raw POSIX file-I/O call sites are behind the `rms_acp_absent()`
  probe or the `fd>=0` discriminator (ACP handles carry `fd=-1`), i.e. the
  documented, non-silent legacy-defer / netbsd-vax `#else` path, or are
  LEGIT-SUBSTRATE (boot-bridge staging with ACP-sourced bytes, `/proc/mounts`
  read, the standalone ODS-2 volume-format tool codec that runtime RMS never
  links). **vms-040 note:** those fallback syscalls still live in the shipped
  image binary and are issuable at runtime — the `fd`-gate is *by convention*,
  which is exactly what a Phase-B fence would make structural.
- **LOGINOUT (`tools/vms_login.c`) and AUTHORIZE (`tools/vms_authorize.c`) — the
  crown jewels — are clean.** SYSUAF/RIGHTSLIST load/save route through
  `ovmx_sysuaf_*`/`ovmx_sysuaf_enum` over RMS/ACP; identity is read/stamped via
  `vms_kif_getjpi_self`/`vms_kif_setident` (executive-owned), and AUTHORIZE's
  privilege gate explicitly deleted the old `geteuid()==0`/`getenv("USER")`
  bypass.

**Borderline:** `sys$assign` on an arbitrary filename does `open()` directly
(`sys_assign.c:534-540`); `$GETUAI`/`$SETUAI` touch real file-backed SYSUAF but
without an executive interlock (`sys_uai.c:23-36`).

### 3.2 Process control — should be `$CREPRC`/`$DELPRC`/`$GETJPI` via the executive process table

| Site | file:line | pattern | class |
|---|---|---|---|
| `dcl_exec_utility` (every SYSGEN/MAIL/AUTHORIZE/… launch) | `src/vmsdcl/dcl_cmd_misc.c:458,461` | `fork`+`execv` with **no** `vms_kif_register*` | GENUINE-BYPASS |
| LINK's compile-and-link `cc` | `src/vmsdcl/dcl_cmd_misc.c:2757,2759` | `fork`+`execvp("cc")` | GENUINE-BYPASS |
| **PIPE** — each pipeline segment | `src/vmsdcl/dcl_cmd_process.c:2405` | `fork`+`execl`, zero registration → **N untracked processes per PIPE** | GENUINE-BYPASS |
| `$CREPRC` priv/UIC | `src/libvms/syssvc/sys_process.c:650-661,812-824` | child copies parent PCB privs/UIC and stamps `getpid()` into a **local** struct; no `vms_kif_setprv`/`setident` | GENUINE-BYPASS |
| `$FORCEX` / `$SUSPND` / `$RESUME` | `sys_process.c:1115-1189` | `kill(pid, SIGUSR1/SIGSTOP/SIGCONT)` on a **raw Linux PID**, no executive resolution; `$FORCEX` silently discards `prcnam` | GENUINE-BYPASS |
| `$SETPRI` | `sys_process.c:1204-1237` | discards `pidadr`/`prcnam`, `setpriority` on self, returns `SS$_NORMAL` | GENUINE-BYPASS |
| Queue job submission | `src/vmsqueue/vmsqueue.c:361-421` | flat-file write; "submitted job" ties to no real executive process | GENUINE-BYPASS |
| **IMGACT.EXE initial exec** | `src/imgact/imgact.c` (whole activate flow) | maps ELF with raw `mmap`/`mprotect`; **never** calls `vms_kif_enter_image`/`p0_map`/`p1_map`/`image_rundown` | GENUINE-BYPASS (nuanced — see below) |

**Nuance on IMGACT.EXE (highest blast radius).** In the shipped **fork-per-image
model** (`docs/design-in-process-activation.md` Option B, the interim), DCL's
`dcl_activate_image_inner` *does* call `vms_kif_register_continue` before
`execve` (`dcl_cmd_process.c:1759`), so the process **identity** is known to the
executive. What IMGACT.EXE does **not** do is record the image's P0/P1 extents
or perform the executive mode transition — those primitives are only exercised
by the **in-process** re-activation path (`src/libvms/syssvc/sys_imgact.c`
`imgact_activate`, which correctly calls all four), which is the Option A / 1.0
target, not the default. So for the common case (every new process's first
exec) the executive has the PCB but not the image-activation records. Classify
as a genuine gap in executive-owned image state, distinct from a "hidden
process."

**Legit-substrate contrast (mediated by the executive):** `$RUN/DETACHED` →
`sys$creprc` (`dcl_cmd_process.c:1138`); `STOP` → `sys$delprc` (`:2601`); SPAWN
(`:2069`) forks but the child calls `vms_kif_setprn`/`vms_kif_getjpi_self` and
reports back over a pipe, self-exiting `126` if unregistered (INV-6 honest
fail); RUN foreign-command activation (`:1740`) is the fork *under*
`register_continue`. `$CREPRC`'s own `fork`+`pipe` (`sys_process.c:720`) is
plumbing for the child to report its executive-assigned VMS PID — the fork is
substrate; the identity write into a **local** PCB (row above) is the bypass.

### 3.3 Locks — should be `$ENQ`/`$DEQ` (`vms_kif_enq/deq/convert`)

- Queue-manager DB locking: `flock(fd, LOCK_EX/UN)` instead of `$ENQ`/`$DEQ`
  (`src/vmsqueue/vmsqueue.c:97-109`) — **GENUINE-BYPASS.**
- **Positive:** `src/libvms/syssvc/sys_lock.c` is fully `vms_kif_enq/deq/convert`
  routed. The `pthread_mutex` uses in `sys_misc.c`/`sys_mailbox.c`/`sys_time.c`
  protect **process-local** PCB structures (priv_lock, chan_lock, timer_mutex)
  — legit intra-process synchronization, **not** a `$ENQ` substitute.

### 3.4 Event flags / ASTs / mailboxes / device table — positive

Clean and executive-routed: `sys_event.c` (`vms_kif_setef/…`), `sys_ast.c`
(`vms_kif_dclast/setast/deliverast`), `sys_mailbox.c` (`vms_kif_mbx_*`; the
`pipe()` uses there are internal fd bridges paired with the real executive
channel, not a substitute), `sys_device.c`. DCL's `dcl_mbx.c` likewise uses
`vms_kif_mbx_assign/read/write`. The AF_UNIX-socketpair mailbox implementation
this replaced is gone.

### 3.5 Logical names — LNM$SYSTEM routed; LNM$JOB/GROUP/PROCESS not

- `sys$crelnm`/`dellnm`/`trnlnm` still resolve **LNM$JOB, LNM$GROUP and
  LNM$PROCESS** through a `static logical_table[]` + `pthread_mutex` — process
  private (`src/libvms/syssvc/sys_logical.c:94-95,171-237,336-363`), labeled
  `OVMX-LOCAL`. Job/group logicals are cross-process on real VMS. **GENUINE-BYPASS.**
  (LNM$SYSTEM *is* executive-routed via `vms_kif_lnm_*`.)
- Minor DCL env-var residuals feeding process context: `getenv("VMS_DEFAULT_DIR")`
  (`dcl_main.c:121,204`), `getenv("VMS_ROOT")` (`:426`), the
  `setenv/getenv("VMS_FOREIGN_CMD")` foreign-command hand-off
  (`dcl_cmd_process.c:1914`, `lib_output.c:189`). The old identity-via-env
  facade (`VMS_PRIVILEGES`/`VMS_USERNAME`) has been removed; a stale dead
  `setenv("VMS_USERNAME")` with a misleading "facade" comment remains at
  `tools/vms_login.c:276` (its last reader now uses `vms_kif_getjpi_self`).

### 3.6 Exit status — `$STATUS`/`$EXIT`

DCL keeps `$STATUS`/`$SEVERITY` as a process-local symbol
(`ctx->last_status`, `src/vmsdcl/dcl_exec.c:140-148`) and maps child
`waitpid`/`WEXITSTATUS` locally to `SS$_NORMAL`/`SS$_ABORT`
(`dcl_cmd_misc.c:469`, `dcl_cmd_process.c:1766` et al.). **BORDERLINE** — the CLI
is where real VMS keeps `$STATUS` too, so process-local is faithful *for the DCL
that ran the image*. It compounds §3.2 only for the untracked-child cases
(dcl_exec_utility / LINK / PIPE), where no completion code is ever recorded in a
PCB another process could `$GETJPI`.

### 3.7 Identity / privilege / reference-monitor / memory — fabricated success

| Facility | file:line | pattern | class |
|---|---|---|---|
| `$CHKPRO` protection check | `src/libvms/syssvc/sys_security.c:151-187` | decides in-process via `getuid`/`getgid` SOGW compare — checker and checked are the same process; no executive reference monitor | GENUINE-BYPASS |
| **vmssshd identity soft-fail** | `src/vmsssh/vmssshd.c:437-461` | when `vms_kif_setident()` is **refused by the executive**, logs `%OVMX-W-NOIDENT` and proceeds with a local PCB holding a UIC + privilege mask | GENUINE-BYPASS (INV-6 — the exact anti-pattern the project forbids) |
| `$CRMPSC` named global section | `src/libvms/syssvc/sys_memory.c:218-254` | discards `gsdnam`, `mmap(MAP_SHARED)` anonymous — two processes can never share the "same" section | GENUINE-BYPASS |
| `$DGBLSC`/`$PURGWS`/`$LKWSET`/`$ULWSET` | `sys_memory.c:186-195,287-342` | return `SS$_NORMAL` with no action (no registry, no `mlock`) | GENUINE-BYPASS (fabricated success) |
| `$SNDOPR` seq / `$BRKTHRU` | `src/libvms/syssvc/sys_operator.c:413-414,564-573` | per-process `static` counter; broadcast reaches only a tty this process can open | GENUINE-BYPASS |
| `$GETSYI` cross-node | `sys_misc.c:130` | discards `csidadr`/`nodename`, answers local `uname()` | BORDERLINE (disclosed) |

**Positive:** `sys_uai.c`'s SYSPRV gate routes through `vms_kif_getjpi_self`
(no fallback); privilege *mutation* `sys$setprv` routes through `vms_kif_setprv`.

### 3.8 Device allocation — `$ALLOC`

`vms_kif_alloc`/`dalloc` are **unwired** (no product caller — `vms_kif.h`
census). SET TERMINAL is the live symptom: characteristics live only in the
per-process `ctx->terminal` struct, applied via raw `tcsetattr`/`ioctl` on this
process's own tty, with no `vms_kif_ttsetmode`/`$QIO IO$_SETMODE` — no other
process/executive sees the setting (`src/vmsdcl/dcl_cmd_set.c:293,465` →
`dcl_terminal.c:110-132`). **GENUINE-BYPASS** (textbook per-process fake). The
legacy `flock`-on-a-table terminal allocator (`dcl_terminal.c:244-263`) is
effectively dead debris (its reader was replaced by `vms_kif_procscan`).

---

## 4. Summary counts per facility

| Facility | GENUINE-BYPASS sites | Borderline | Executive-routed? |
|---|---:|---:|---|
| File I/O (`$QIO`/RMS/ACP) | ~24 (1 core `$QIO` + ~10 DCL cmds + MAIL 7 + SYSMAN 5 + TCPIP 2 + queue/install 2 + `$ASSIGN`) | 3 | Partial — TYPE/COPY/DELETE/CREATE/HELP + RMS primary path routed; the rest not |
| Process control (`$CREPRC`/`$DELPRC`/`$GETJPI`) | 8 (DCL fork ×3, `$CREPRC` local ×2, `$FORCEX`/`$SUSPND`/`$RESUME`/`$SETPRI`, queue submit, IMGACT records) | — | Partial — `$CREPRC`/`$DELPRC`/SPAWN mechanism mediated; per-process state + signal-ops raw |
| Locks (`$ENQ`/`$DEQ`) | 1 (queue `flock`) | — | Yes — `sys_lock.c` routed |
| Event flags / ASTs / mailboxes / device table | 0 | — | **Yes — fully routed** |
| Logical names | 3 (LNM$JOB/GROUP/PROCESS) + env residuals | — | Partial — LNM$SYSTEM routed |
| Exit status (`$STATUS`) | 0 | 5 | Faithfully DCL-local |
| Identity / priv / reference monitor / memory | 8 (`$CHKPRO`, vmssshd soft-fail, `$CRMPSC`/`$DGBLSC`/`$PURGWS`/`$LKWSET`/`$ULWSET`, `$SNDOPR`/`$BRKTHRU`) | 1 | Partial — SYSUAF/identity via LOGINOUT/AUTHORIZE clean |
| Device allocation (`$ALLOC`) | 1 (SET TERMINAL `ctx->terminal`) | — | No — `$ALLOC` unwired |

**Crown jewels clean:** LOGINOUT and AUTHORIZE (SYSUAF over RMS/ACP; identity
via `vms_kif_setident`). **RMS/vmsfs primary path clean** (POSIX only as an
honest, gated fallback).

---

## 5. Highest-risk fabrication-enabling bypasses (ranked)

1. **`sys_qio.c` — ordinary `$QIO` file I/O never reaches the ACP.** Widest
   surface: `$QIO`-to-a-disk-file is the most common I/O path in VMS code and is
   entirely raw-fd; Files-11 protection/allocation/FID semantics do not apply.
2. **IMGACT.EXE initial exec records no executive image state** (P0/P1/mode) —
   runs on *every* new process; only the not-yet-default in-process path is
   faithful.
3. **MAIL's total bypass of Files-11/ACP for message storage** —
   `deliver_message()` writes into another user's mailbox directory with no
   ACP-mediated access check; no VMS protection model applies to mail at all.
4. **DCL PIPE — N untracked Linux processes per pipeline** (`dcl_cmd_process.c:2405`),
   invisible to `$GETJPI`/SHOW SYSTEM; worse than the SPAWN pattern beside it.
5. **vmssshd identity soft-fail** — proceeds with a local privileged PCB *after
   the executive explicitly refused* the identity (INV-6 / Rule 11 violation).
6. **DCL LIBRARY (whole facility)** + PURGE/SEARCH/APPEND/BACKUP/EDIT — a cohort
   of file commands the ACP flip never reached (PURGE is inconsistent with its
   own sibling DELETE).
7. **`sys_memory.c` global-section / working-set stubs** — `$CRMPSC` named
   sections and `$LKWSET`/`$PURGWS` return `SS$_NORMAL` for work never done.
8. **`sys_process.c` `$FORCEX`/`$SUSPND`/`$RESUME`/`$SETPRI`** — raw `kill`/
   `setpriority` on Linux PIDs, next to the already-fixed `$DELPRC`/`$WAKE`.
9. **LNM$JOB/GROUP/PROCESS** still process-private; **SET TERMINAL** per-process.

---

## 6. Phase-B enforcement options (input to the operator's scope-gate)

Phase B makes the executive authoritative **by enforcement**. The audit surfaces
one decisive architectural fact and one decisive sequencing fact:

- **Architecture is favorable for file I/O.** Legitimate Files-11 I/O is
  `ioctl(/dev/vms)`, not `read`/`write`; the backing device is kernel-only, so an
  image never legitimately holds a backing-store fd. File-I/O bypass can be
  fenced **without** a per-syscall path inspector.
- **Enforce-after-migrate, not enforce-first.** Most bypasses reuse syscalls that
  images *also* need legitimately (`read`/`write` for console+sockets, `futex`
  for pthreads, `kill` for signals, `openat` for tmpfs staging). Turning on a
  filter *before* the ~40 bypass sites are migrated to the executive would break
  the product. The fence is a **backstop that locks in migration**, not the fix
  itself. The bulk of Phase B is migration (the operator's standing "excise
  fabrication" mission); the mechanism below prevents regression.

### Option A — seccomp-BPF syscall-number filter per OVMX image
A classic seccomp filter installed by IMGACT/PID1 for each image.
- **Catches cleanly:** syscalls with **no** legit image use — notably `flock`
  and file-record `fcntl(F_SETLK…)` (kill the queue lock bypass; force `$ENQ`).
- **Cannot cleanly separate** "read a Files-11 file" from "read the console"
  (both are `read`), or `kill`-for-`$FORCEX` from a legit signal — seccomp-BPF
  cannot dereference pointers (the `openat` path) and fd numbers are dynamic.
- **Verdict:** necessary but insufficient alone; a coarse floor.

### Option B — seccomp `USER_NOTIF` (unotify) broker
Trap the ambiguous syscalls to a supervisor that inspects args (paths, fds) and
allows / denies / redirects (e.g. redirect a Files-11 `openat` to the ACP).
- **Powerful:** can police file I/O by path and process ops by target.
- **Cost:** reintroduces a second task and per-syscall latency. Note the
  in-process-activation design (`docs/design-in-process-activation.md` §A.6.5)
  already **rejected a per-image "shadow ring"** for mode enforcement because it
  re-opens the two-process problem. **Distinguish** a per-image shadow ring
  (rejected) from a single system-wide boot-time broker (a different trade) —
  the operator should rule on whether *any* broker is acceptable.

### Option C — mount / filesystem namespace containment (cheap, complementary)
Run each OVMX image in a mount namespace where the ODS-2 backing store **and the
retired `/vms` tree** are simply absent. Raw `openat` on them then fails
`ENOENT` naturally — no per-syscall filtering.
- **Closes structurally:** all the `/vms`-literal bypasses (SYSMAN, TCPIP,
  queue, install) and any attempt to reach the backing store, **once** the
  legitimate consumers are on the ACP. Combine with Option A for a broker-free
  file-I/O fence.
- **Limitation:** does nothing for process/lock/logical/memory bypasses that
  don't touch the filesystem.

### Option D — eBPF-LSM / policy hook in `vms.ko`
Since OVMX already ships a kernel executive, `vms.ko` (or an eBPF-LSM) can gate
file/process operations by OVMX policy in-kernel — the **most VMS-shaped**
(the executive *is* the reference monitor, which also fixes `$CHKPRO`).
- **Cost:** most work; couples to kernel/LSM version; must be carried across
  both sanctioned SYSKRNLs (Linux `vms.ko` and NetBSD's vms pseudo-device).

### Recommended shape (for the operator to accept/modify)
**Migrate, then fence, in this order:** (1) migrate the file-I/O cohort (§3.1)
and LNM$JOB/GROUP (§3.5) to the ACP/executive; (2) turn on **Option C + Option A**
as a broker-free fence for file I/O and the lock bypass; (3) migrate the
process/identity/memory bypasses (§3.2, §3.7, §3.8) to the executive; (4) decide
**Option B vs Option D** for the residual process/signal-op enforcement that
number-based seccomp can't police. Keep the existing source-grep gate as the
regression tripwire throughout.

### Scope-gate questions reserved to the operator
1. **Blast radius for 1.0:** fence *file I/O only* (C+A, after migration), or
   also process/lock/memory (needs B or D)?
2. **Broker tolerance:** is *any* `USER_NOTIF` broker acceptable, given the
   shadow-ring rejection — and does a single system-wide broker read differently
   from a per-image one?
3. **LSM route:** willing to host policy in `vms.ko`/eBPF-LSM (most faithful,
   most work, per-substrate) for the reference-monitor properties (`$CHKPRO`)?
4. **Sequencing confirmation:** endorse enforce-**after**-migrate (safe backstop)
   over enforce-first (breaks product)?
5. **Cross-substrate parity:** must the Phase-B mechanism hold identically on the
   NetBSD-VAX substrate, or is Linux-first acceptable for 1.0?

---

## Appendix — audit coverage

Areas read (tests excluded): `src/libvmssys/` (the substrate + `/dev/vms`
client), `src/libvms/` (system services + RTL), `src/vmsdcl/`, `src/vmsrms/`,
`src/vmsfs/`, `src/imgact/`, `src/ovmx_init/`, `src/ovmx_provision/`,
`src/vmsqueue/`, `src/vmsssh/`, `src/apps/`, `src/install/`, `tools/`. Ground
truth for the boundary: `src/libvmssys/vms_syscall.h`,
`src/libvmssys/vms_kif.h`/`.c`, `src/kernel/vms_ioctl.h`,
`tests/integration/test_runtime_target.sh`,
`docs/design-in-process-activation.md`, `docs/release-roadmap-to-1.0.md`.
