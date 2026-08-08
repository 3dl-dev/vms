# In-process image activation — the fork-per-image model, and how to leave it

Status: **Option B shipped as the interim** (vms-4d7, this PR). **Option A is the
1.0 target.** Operator decision 2026-08-08: ship B now, do A before 1.0.

## The problem in one paragraph

On OpenVMS, running an image — `RUN`, a foreign command, a DCL utility, a
CLI-dispatched image — does **not** create a process. The image is mapped into
the **current** process's P0 space and runs there with the process's PID, UIC,
username, privileges and process-permanent logical name table; when the image
exits, image rundown returns control to DCL **in the same process**. Only
`SPAWN`, `RUN/DETACHED` and `$CREPRC` create new processes. OVMX instead
`fork()`s + `execve()`s a fresh Linux process for **every** image, with
`IMGACT.EXE` wired as the `PT_INTERP` interpreter (`src/imgact/imgact.c:8`). So
every activated image runs in a **different** process than the DCL that
activated it. That single divergence is the root cause of a family of bugs.

## Current state (what OVMX does today)

Every image activation is a Linux `fork()` + `execve()`:

| Site | File:line | What it activates |
|------|-----------|-------------------|
| `dcl_activate_image()` | `src/vmsdcl/dcl_cmd_process.c:1091` | `RUN` and foreign commands — **image activation** |
| `cmd_spawn()` | `src/vmsdcl/dcl_cmd_process.c:1314` | `SPAWN` — a genuinely new process |
| pipeline stage | `src/vmsdcl/dcl_cmd_process.c:1515` | each `PIPE` stage — a new process |

The forked child `execve()`s the target image; the kernel loads `IMGACT.EXE`
(the static-PIE `PT_INTERP`, `src/imgact/imgact.c`), which maps and starts the
real image. The child is a new Linux thread-group, so before this work it
auto-registered a **brand new** executive PCB and **derived its own privilege
mask** from `capable(CAP_SYS_ADMIN)` (`src/kernel/vms_module.c`,
`vms_proc_register`). `CAP_SYS_ADMIN` maps to the enforced set
(`CMKRNL|CMEXEC|SETPRV|WORLD`) — which **never includes SYSPRV**.

### What the fork model breaks

- **Privileges do not persist across activation.** SYSTEM's SYSPRV is stamped
  onto its DCL by LOGINOUT (`vms_kif_setident`), but the forked image derived a
  fresh mask without it — so `$ RUN AUTHORIZE` as SYSTEM was refused. This is the
  reported bug (vms-4d7).
- **`$GETJPI` self-identity is wrong.** The image reports a different VMS PID,
  and (pre-fix) a different UIC/username, than the DCL a user thinks they are in.
- **Process-permanent logical names don't survive.** An image's
  `DEFINE`/`$CRELNM` into `LNM$PROCESS` goes into the child's table and is gone
  when control returns to DCL.
- **Accounting / process-permanent state** (image counts, connect time, the
  process's event-flag clusters and lock IDs) belong to the child, not the
  process the user is in.

## The VMS-faithful target

Activating an image runs it **in the current process**: no new PID, no new PCB,
same P0/P1 split, same privileges, same `LNM$PROCESS`, same event flags and
locks; image rundown returns to DCL's command loop. There is exactly one VMS
process for an interactive session and everything it `RUN`s, until it explicitly
`SPAWN`s or `$CREPRC`s.

## The hard part on Linux

A Unix process has **one** address space and **one** C runtime. VMS activates an
image by mapping it into P0 **alongside** the still-resident DCL in P1/supervisor
mode; the two coexist because they are separated by **address region (P0 vs P1)
and processor access mode (kernel/exec/super/user)**. Linux offers neither:

- No P0/P1 region split — a mapped image and DCL would share one flat address
  space and one heap/BSS/TLS. Two musl C runtimes cannot both own `environ`,
  `errno`, `malloc` arena, stdio, and the TLS block.
- No supervisor/user mode split inside a process — nothing keeps the activated
  image from scribbling on DCL's state, which the VMS access-mode split prevents
  by hardware.
- `execve()` is the only clean "run a different image" primitive, and it
  **replaces** the caller — the opposite of "run it and come back."

## Options

### Option A — true in-process activation (1.0 target)

DCL maps the target image into its **own** address space and runs it without
`execve()`, recovering control on image exit (a `longjmp` back into DCL's command
loop, image rundown freeing P0). The `IMGACT` logic becomes a **library inside
DCL**, not a separate `PT_INTERP` process.

- **Gets everything right**: same process, same PID/UIC/privileges, `LNM$PROCESS`
  and process-permanent logicals defined by an image survive back to DCL,
  `$GETJPI` self is correct, accounting is one process.
- **Key architectural finding (operator, 2026-08-08):** A almost certainly
  requires **`vms.ko` to implement real P0/P1 process address regions and the
  access-mode split** (kernel/exec/super/user). The image maps into P0 while
  DCL's command loop lives in P1/supervisor within one process; without a
  region+mode boundary enforced below userspace, DCL and the image corrupt each
  other's C runtime and state. This is a substantial executive addition — the
  same "make the MMU, not a convention, the boundary" discipline `vms_lnm.c`
  already invokes for system space (design §2.4), extended to per-process space.
- **Cost**: high. A second C-runtime coexistence story (or a freestanding image
  ABI that shares no libc state with DCL), image rundown/unmap, and the vms.ko
  region/mode work. Not a single-PR change.

### Option B — shared-PCB interim (SHIPPED, this PR)

Keep the Linux `fork()` + `execve()`, but make the executive treat DCL **and**
its image-activation child as **one VMS process**: the fork is invisible to VMS.

- **Mechanism.** DCL, in the forked child of an image activation only
  (`dcl_activate_image`, `src/vmsdcl/dcl_cmd_process.c:1091`, before `execve`),
  calls `vms_kif_register_continue()` → `VMS_IOCTL_REGISTER_CONTINUE`. The
  executive (`src/kernel/vms_module.c`, `vms_proc_continue_identity`) reads the
  **parent's** PCB from the task hierarchy (`current->real_parent`, which a
  process cannot forge) and **shares** its VMS PID, UIC, username and both
  privilege masks onto the child. The PCB is keyed on the thread group and
  survives `execve`, so the activated image inherits the continued identity; its
  own first `kif` call adopts the existing row.
- **Distinguished from new processes.** `SPAWN` / `RUN/DETACHED` / `$CREPRC`
  create genuinely new VMS processes and use plain `VMS_IOCTL_REGISTER` (new PID,
  derived identity). Only image activation continues the activator. The signal is
  authoritative, not declared: the child names only the *relationship*
  ("continue my parent"); the identity comes from the parent's row.
- **Security preserved.** The parent's **current** masks are copied, so a
  context that `setident`'d DOWN to an unprivileged identity cannot regain a
  privilege across activation — the reduction now survives the fork. INV-6
  holds: `/dev/vms` absent → the register path fails honestly, no per-process
  fake. Proven both directions in `tests/qemu/test_syssvc_identcont.c`
  (admitted-with-SYSPRV / refused-after-setident-down), anchored by the
  `register-continue-identity-dropped` negative control.

**What B fixes:** SYSPRV (and the whole identity) now persists across `RUN` —
SYSTEM can `RUN AUTHORIZE`. `$GETJPI` self reports DCL's VMS PID/UIC/username.

**What B does NOT fix (residual gaps only A closes):** the child is still a
separate Linux process with its **own** address space and its **own** executive
sub-state:

- **Process-permanent logical names an image defines do not flow back to DCL.**
  `LNM$PROCESS` is not executive-resident at all today (`src/kernel/vms_lnm.c`
  implements only `LNM$SYSTEM`; GROUP/JOB/PROCESS are deferred), so an image's
  `DEFINE` lands in its own userspace `vmslnm` table and is lost at image exit.
  Even once `LNM$PROCESS` becomes executive-resident and keyed by the shared VMS
  PID (which B's shared PID would then make resolve to one table — a noted bonus
  path), a **process-permanent** logical the image `DEFINE/PROCESS`s only
  survives back to DCL if the two share the table, which B's separate address
  spaces still complicate.
- **Event-flag clusters, lock lists and channels are per-child**, not shared with
  DCL: the child gets its own `ef`, `locks`, `channels` in its PCB row.
- **Accounting is split**: image-activation counts accrue to the child row, not
  the one continuous process VMS would show.
- The two rows **share a VMS PID** (correct — same VMS process) but are two Linux
  tasks; anything that must be genuinely one address space (a C-runtime-level
  side effect an image leaves for DCL to read) cannot cross the boundary.

Only A — one address space, image rundown returning to DCL — closes these.

## Recommendation

Ship **B now** (done): it removes the reported authenticity break (SYSTEM can
`RUN AUTHORIZE`) and the visible `$GETJPI`/privilege divergences with a small,
well-fenced, security-preserving executive change, and it does so without
entrenching anything A must tear out — B's `REGISTER_CONTINUE` is the executive
learning "these two tasks are one VMS process," a fact A also needs.

Do **A before 1.0**, and treat its real cost as the **vms.ko P0/P1 region +
access-mode work**, not the DCL-side mapping/`longjmp`. The mapping is
mechanical; the boundary that lets an image and DCL coexist in one process
without corrupting each other is the executive feature, and it is the same
MMU-enforced discipline the project already committed to for system space.
Sequence A behind that executive capability; until it exists, B is the faithful
interim and its residual gaps above are the acceptance criteria for A.
