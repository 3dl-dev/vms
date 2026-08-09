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

---

# PART II — The full Option A design (vms-68f)

Operator 2026-08-08: "design it out. VMS faithful, all the rulings." Everything
above (problem, current state, options overview, and the **shipped B interim**) is
the frame; this part is the complete, implementation-ready A design and its
decomposition into QEMU-provable increments. Rulings honored throughout: Rule 8
(clean-room RE), Rule 9 (one runtime target), Rule 10 (match VMS or hide it), INV-6
(no per-process executive fakes; fail honestly without `/dev/vms`), the authenticity
program (a facility is not done until a QEMU//dev/vms test proves it, negctl-anchored),
and the continuation-identity model (record the model + observations, re-derive
runtime status; never freeze "A is done").

## A.1 The VMS model to reproduce

### A.1.1 Process address regions (P0 / P1 / S0S1 / P2S2)

A VMS process's virtual address space is split into regions, each with its own page
tables and growth direction:

| Region | VMS name | Contents | Lifetime |
|--------|----------|----------|----------|
| **P0** | Program region | The **image**: code, data, BSS, heap. Grows to higher addresses. | **Per-image** — created at activation, **deleted at image rundown.** |
| **P1** | Control region | Process-**permanent** state: user stack, the DCL/CLI code and data, process-permanent logical names, RMS process context, the image-activation context, exception vectors. Grows to lower addresses. | **Process lifetime** — survives activation and rundown. |
| **S0/S1** | System region | Executive, system-service vectors, PFN database, system-wide data. | System (node) lifetime; shared by all processes. |
| **P2/S2** | 64-bit regions | (Alpha/IA64/x86 only) 64-bit program / system space. | As P0/S0. |

> Source (public, Rule 8): *OpenVMS Internals and Data Structures* (IDSM), "Process
> Address Space" and "Memory Management Data Structures"; VSI *Programming Concepts*
> Vol. I, "Address Space". **Width note (Rule 5 / lab-Alpha):** the VAX had P0/P1/S0/S1
> only; Alpha/x86 added the 64-bit P2/S2 regions. A layout question about the 64-bit
> regions is a **lab-Alpha** (V8.4) question — reading a VAX's 32-bit region map as
> "what VMS does" reads an architecture limit as an OS rule.

**The load-bearing fact for A:** DCL lives in **P1** (process-permanent); the image
it runs lives in **P0** (per-image). "Run an image" = *map a P0*; "image exit" =
*delete that P0*, leaving P1 — and therefore DCL — intact and resumed. There is
exactly **one process** the whole time.

### A.1.2 The four access modes

```
0  Kernel      executive core, drivers, page-fault handler, inner half of $ services
1  Executive   RMS, file-layer of some services
2  Supervisor  DCL / the command language interpreter (CLI)
3  User        the activated image — user programs run here
```

> Source (public, Rule 8): IDSM, "Access Modes and the Processor Status Longword".
> In-tree constants already exist: `PSL_C_KERNEL..PSL_C_USER` = 0..3
> (`src/kernel/vms_ioctl.h`), and the executive already carries `current_mode` per
> process and gates mode-sensitive services on it.

Two consequences A must reproduce:

- **DCL runs in Supervisor; the image runs in User.** DCL activates the image and
  drops to User to enter it; the image cannot reach DCL's Supervisor-protected P1
  command-loop state. Rundown returns to Supervisor.
- **`$EXIT` / image rundown is a mode transition, not a process exit.** The image
  (User) requests `$EXIT`; the executive runs the image's exit handlers, tears down
  P0, and returns control to the CLI (Supervisor) — the outer half of its command loop.

### A.1.3 The activation → rundown sequence (what A must be)

```
DCL (Supervisor, P1) reads "RUN FOO"
  │
  ├─ SYS$IMGACT: map FOO.EXE into P0; resolve shareable images, relocate,
  │     build the image-activation context in P1 (so rundown knows what to free)
  ├─ SYS$IMGSTA: establish the image's condition/exit context; transfer to the
  │     image entry in USER mode
  │        ... image runs in User/P0; may DEFINE process-permanent logicals
  │            (they land in P1), $ENQ locks, set EF, do I/O — all on the ONE
  │            process's executive state ...
  ├─ image calls SYS$EXIT (from User)
  ├─ IMAGE RUNDOWN (Kernel): run the image's exit handlers, release image-scoped
  │     resources (user-mode channels, P0 locks, image ASTs, image temp logicals),
  │     DELETE the P0 region
  └─ return to DCL (Supervisor) at the point after activation — next prompt.
        Same PID throughout.
```

The return is the crux: **not** `wait()` on a child — a mode/return transition
**within one process** back to the CLI's saved supervisor context. The faithful
analogue is `swapcontext`/`longjmp` across a vms.ko-mediated mode transition, never a
process reap. Behaviorally confirmable on the labs (Rule 8, observation only): PID
from `F$GETJPI("","PID")` is identical before and after a `RUN`; a `DEFINE/PROCESS`
issued *inside* a RUN'd image is visible to DCL after the image exits.

## A.2 The Linux reality, and how vms.ko provides the model

Linux gives userland **one** flat address space and **two** rings a task sees (user +
the syscall entry). No P0/P1 split, no four-mode userland, no per-page four-mode
protection vector, and `execve()` *replaces* the caller. A must build the model in
`vms.ko` + an activation library, and be **honest** (Rule 10) about what is
*enforced* vs. *faithfully modeled*.

Core move: **turn IMGACT from a `PT_INTERP` that runs after `execve()` into a library
DCL calls in-process.** DCL does not fork; it maps the image itself and enters it,
then unmaps and resumes. `vms.ko` tracks region/mode state so the rest of the
executive (identity, EF, locks, ASTs, LNM) sees one process with a correct
current-mode and P0/P1 accounting.

### A.2.1 P0 as a per-image mmap arena inside DCL's process

At startup DCL reserves a **fixed P0 virtual window** — a large contiguous
`PROT_NONE` reservation (`mmap(NULL, P0_MAX, PROT_NONE, MAP_ANON|MAP_NORESERVE)`) —
recorded in P1 as the process's P0 base/limit. Activation maps **into** that window
with `MAP_FIXED`; rundown collapses it back. Nothing DCL owns ever lives there.

```
DCL process virtual layout (one Linux address space):
  [ P0 window ....................... ]  per-image; MAP_FIXED into the reservation
  [ P1: DCL/CLI code+data, user stack,]  process-permanent; loaded at DCL start
  [     LNM$PROCESS root, RMS ctx,    ]
  [     image-activation context      ]
  [ IMGACT-as-library + its heap      ]  process-permanent (it IS the activator)
  [ shareable images (LIBVMS$SHR, …)  ]  process-permanent, mapped once, shared
```

**Activation** (`imgact$activate()`, `SYS$IMGACT` as a library call):
1. `open()` the `.EXE`; validate the OVMX symbol-vector / ELF header.
2. Each `PT_LOAD`: `mmap(p0_base+voffset, filesz, prot, MAP_FIXED|MAP_PRIVATE, fd,
   off)`; zero the BSS tail. All addresses fall inside the P0 reservation.
3. Resolve shareable images (already-loaded ones reused — A.2.2), relocate, init the
   image's TLS block, run its constructors.
4. `VMS_IOCTL_P0_MAP`: register the P0 extent `[base,limit)` with the executive so it
   knows this process has an image mapped — for rundown accounting and `$GETJPI`
   region/working-set answers.

**Rundown** (`SYS$RUNDWN` as a library call, on the image's `$EXIT`):
1. Run the image's registered exit handlers (User, then inner-mode if any).
2. Ask `vms.ko` to release **image-scoped** executive state for this process:
   user-mode channels, P0-scoped locks, image-level ASTs, image temporary logicals.
   Process-permanent P1 state is untouched — the whole point.
3. `mmap(p0_base, P0_MAX, PROT_NONE, MAP_FIXED|MAP_ANON)` — collapse the window to a
   bare reservation, dropping the image's pages.
4. `VMS_IOCTL_P0_UNMAP`: executive marks the process image-less; current mode returns
   to Supervisor.
5. `swapcontext`/`longjmp` back to the CLI command loop.

"P0 deleted on rundown, P1 survives" is realized with `MAP_FIXED` into a reserved
window, and it is **genuinely enforced** at the memory level: after rundown the
image's pages are gone; a stale pointer into P0 faults.

### A.2.2 One address space, two runtimes: how the image's C runtime, TLS and BSS
coexist with DCL's

The objection that makes A look hard — DCL is itself a program with its own
libc/TLS/BSS; how does a second program's runtime live in the same address space?
**VMS's own shareable-image architecture is the answer, and it is what OVMX already
builds toward** (`docs/design-link-native-toolchain.md`, symbol-vector activation):

- **Everything is PIC and separately based.** DCL (P1) and the image (P0) are linked
  at **disjoint** ranges; the linker/activator assign the bases, so no text/data
  overlap by construction.
- **Shared code is a *shareable image*, mapped once.** `LIBVMS$SHR.EXE`, the
  C-runtime shareable, RMS, etc. are PIC shareable images mapped **once** and
  referenced by both DCL and the image through the OVMX symbol vector
  (`.vms$sv`/`.vms$imp`). There is **one** `sys$qio`, one malloc-arena policy — the
  VMS model where the RTL is a shareable image installed `/SHARED` and every image in
  the process binds to the same one. (This is exactly what B's separate process
  accidentally duplicated per image.)
- **Per-image mutable data is in P0; there is no second libc *instance*.** The image's
  `.data`/`.bss` live in its P0 segment (private, torn down at rundown). RTL *state*
  that must be process-wide (LNM cache, RMS process context, the `/dev/vms` fd) lives
  in the **shareable image's** data — process-permanent and deliberately shared,
  because it is the ONE process's state.
- **TLS.** The activated image gets a TLS block appended to the process's TLS vector
  (the DTV grows; the image's module gets a slot), initialized from its `PT_TLS`,
  freed at rundown. The thread pointer is unchanged — same thread. This is standard
  `dlopen`-of-a-PIE mechanics, which is what in-process activation fundamentally is:
  **`SYS$IMGACT` ≈ a VMS-flavoured `dlopen` + jump-to-entry in the current process**,
  with rundown ≈ a `dlclose` that also tears P0 down.

> Rule 8: the *mechanism* (PIC, symbol vectors, shareable images mapped once,
> per-image P0 data) is from public VSI *Linker Utility Manual* + *Programming
> Concepts* on shareable images/image sections, and OVMX's own
> `design-link-native-toolchain.md`. Where a byte layout is unpublished (the
> symbol-vector on-disk format) OVMX uses its own representation, already labeled a
> design choice — A does not change that.

### A.2.3 Access-mode enforcement without hardware rings

A splits "enforced" from "faithfully modeled" **explicitly** (Rule 10):

**(a) Current-mode tracked in `vms.ko`, enforced at the /dev/vms boundary — GENUINELY
ENFORCED.** The executive already carries `current_mode` and gates services on it
(`VMS_IOCTL_DCLAST` at kernel/exec requires CMKRNL/CMEXEC; `$SETPRV` widening requires
SETPRV). A makes the transitions real: activation sets current-mode **User** before
entering the image; rundown restores **Supervisor** (`VMS_IOCTL_SETMODE` exists).
Rule added: *lowering* mode (Super→User) is unprivileged; *raising* it
(User→Super/Exec/Kernel) requires the change-mode privilege VMS names
(CMEXEC/CMKRNL). So a User-mode image cannot promote itself to DCL's mode, and every
mode-sensitive service reads the now-correct tracked mode and refuses the image
exactly what VMS refuses a user-mode caller.

**(b) P0↔P1 memory protection — PARTIALLY enforced with mmap games, residue is a
faithful model.** True VMS gives P1 pages per-mode protection (User-no-write,
Supervisor-write). Linux cannot make a page "writable from supervisor code but not
user code" within one ring. In honesty order:
- *Enforced subset:* DCL's crown-jewel P1 structures — the saved command-loop /
  `swapcontext` buffer, the image-activation context, the LNM$PROCESS root — are
  `mprotect(PROT_READ)` **while the image runs** and restored to `RW` at rundown. A
  wild write from the image **faults** instead of corrupting DCL. Real, cheap, worth
  doing.
- *Faithful model, not enforced:* general "User mode cannot write arbitrary
  Supervisor data" is **not** reproduced page-for-page (that needs two rings). A
  states this plainly: mode is enforced at the **service boundary** and the
  **critical P1 pages** are protected; per-page four-mode memory protection is not
  claimed. This is strictly better than B (which enforced neither — the image was a
  whole separate process) and honest about the ceiling.

**(c) The negative control makes the enforced part provable** (§A.5): a User-mode
image attempting `$CMKRNL` or a `$SETPRV`-widen is **refused**, exactly as a
user-mode VMS image is — the anchor proving mode tracking is live, not decorative.

### A.2.4 IMGACT's role: interpreter → in-process activator library

| | B / today | A |
|--|-----------|---|
| Trigger | ELF `PT_INTERP`, after `execve()` | `imgact$activate()` called by DCL in-process |
| Process | new Linux process per image | **no new process** — same PID |
| Loads into | child's fresh address space | DCL's P0 window (`MAP_FIXED`) |
| Return to DCL | `waitpid()` on child exit | `swapcontext`/`longjmp` after rundown |
| Identity | shared PCB over a fork (B) / re-derived (today) | intrinsically one process |

The **loader body is unchanged** — the ELF/symbol-vector mapping, relocation, TLS and
constructor logic in `src/imgact/` is the same code, re-homed from a `_start` reading
`auxv` in a fresh process to a function taking `(path, argv, env)`, mapping into the
caller's P0, and returning a status. `STARTUP.EXE` (PID 1) and the `PT_INTERP` path
remain for the genuine `$CREPRC` cases (SPAWN/DETACHED) that *do* create a process;
RUN / foreign commands / DCL utilities stop using it.

## A.3 What A fixes over B

| Divergence | B (shipped) | A |
|-----------|-------------|---|
| `SYSTEM RUN AUTHORIZE` (SYSPRV) | fixed via shared PCB | fixed intrinsically (one process, one identity) |
| Process-permanent `DEFINE` in a RUN'd image flows back to DCL | **no** (dies with child) | **yes** — LNM$PROCESS is the one process's P1 state |
| `$GETJPI` self-identity / PID stable across RUN | reported same via shared PCB | **is** the same PID; nothing to reconcile |
| Per-image EF / lock / channel ownership + rundown release | approximated across two PIDs | correct — image-scoped released at real rundown, process-permanent kept |
| Accounting (CPU, page faults, working set) as one process | split across two PIDs, stitched | one process, one accounting record |
| RUN vs SPAWN vs RUN/DETACHED | same fork, different bookkeeping | **three real operations** |
| Ctrl-Y / `CONTINUE` re-enters the *same* image | re-enter a stopped child | re-enter the in-process image at its saved point (P0 still mapped) |
| DCL exit handlers re-run after image | child exit ≠ DCL exit handler | rundown returns to DCL's live P1 handler chain |

RUN/SPAWN/DETACHED becoming three real operations is the structural payoff:
`dcl_activate_image` stops forking and calls `imgact$activate()`; `cmd_spawn` and the
detached path keep `$CREPRC` (a real new process). The one semantically-wrong fork
(RUN) is removed; the semantically-correct ones (SPAWN, DETACHED, PIPE stages) stay —
VMS `PIPE` segments *are* subprocesses, so that fork is not a defect.

## A.4 Rulings and invariants honored

- **Rule 9 (one runtime target).** A is a `vms.ko` + activation-library capability on
  the real-kernel/QEMU path; the P0/P1 regions, mode tracking and rundown are
  executive facilities reached through `/dev/vms`. No CI substitute is built for the
  proof (§A.5); a lane that cannot boot `vms.ko` is "unproven here", not "proven".
- **INV-6 (no per-process fakes; fail honestly without `/dev/vms`).** The new ioctls
  (`P0_MAP`/`P0_UNMAP`, mode transitions, image-scoped rundown) have **no** userspace
  fallback. Absent `/dev/vms` the activator returns `SS$_NOSUCHDEV` and refuses to
  activate — it does **not** silently `dlopen` and pretend. A per-process fake here
  is the exact LARP class INV-6 kills. This is a design constraint on every increment
  below.
- **Rule 8 (clean-room RE).** Every VMS fact is from public docs (IDSM, *Programming
  Concepts*, *Linker Utility Manual*) or lab observation of tool output. No VSI/HPE
  source/binary referenced. Unpublished byte layouts (symbol vectors, P0/P1 bases)
  are OVMX's own, labeled — A invents no VMS-authentic byte format.
- **Rule 10 (match VMS or hide it).** §A.2.3 states exactly what is *enforced*
  (service-boundary mode, critical-P1 `mprotect`, P0 teardown) vs. *faithfully
  modeled* (general per-page four-mode protection). No unenforced mechanism is
  presented as enforcement.
- **Rule 5 (dual arch / width).** P0/P1 bases and the 64-bit P2/S2 question are
  architecture-dependent; 32-bit facts pin to lab-1/lab-2 (VAX 7.3), any
  width/quadword/64-bit-region question routes to lab-Alpha (V8.4). x86_64 primary.
- **Continuation-identity model.** This records *the model* and *observations*, not a
  frozen "A is done". Runtime status ("does RUN keep the PID today?") is re-derived by
  running the §A.5 suite, never remembered.
- **Test honesty / no branch-protection weakening.** The proof is the QEMU//dev/vms
  suite, negctl-anchored (§A.5); no declaration/census gate substitutes for it.

## A.5 The proof: QEMU//dev/vms, negctl-anchored

A facility is not done until a real-`/dev/vms` test proves it. On a booted OVMX under
QEMU, the A suite (increment vi) asserts:

**Positive (the property):**
1. `PID0 = F$GETJPI("","PID")`; `RUN AUTHORIZE` (exercise, `$EXIT`); `PID1 =
   F$GETJPI("","PID")` → **`PID0 == PID1`** (no new process for RUN).
2. Inside a RUN'd test image: `DEFINE/PROCESS FOO BAR`; image exits; back in DCL
   `SHOW LOGICAL FOO` → **`BAR`** (flowed back — the exact B failure this fixes).
3. `SHOW SYSTEM` / `$PROCESS_SCAN` shows **one** process across the RUN.
4. A P1 `LIB$GET_VM` region or a `$DCLEXH` exit handler established before RUN
   survives the image and is usable/invoked after rundown.

**Negative control (proves enforcement is live, not decorative):**
5. A test image that from **User** mode attempts `$CMKRNL` or a `$SETPRV`-widen is
   **refused** (`SS$_NOPRIV`/`SS$_NOCMKRNL`) — the same refusal VMS gives a user-mode
   image. If it *succeeds*, mode tracking is fake and the suite fails.
6. `SPAWN` and `RUN/DETACHED` **do** produce a new VMS PID — the negctl proving A did
   not "fix RUN" by making everything in-process.

Anchored on the `tests/qemu/facility_defects.sh` /
`facility_attribution_negctl.sh` convention (a red floor kept red, a green property
gone green). **Never** replaced by a string/census check.

## A.6 Risks and open questions (for operator review)

1. **Rundown completeness is the hard part, not P0 mapping.** Tearing down *exactly*
   the image-scoped executive state (user-mode channels, P0 locks, image ASTs, image
   temporary logicals) while preserving process-permanent P1 state is the real work;
   getting the split wrong leaks resources across images in one process — a bug B
   never had because the child's death was a sledgehammer. **Open:** which resource
   classes are image-scoped vs. process-permanent must be pinned to the oracle per
   class (observe what survives `RUN`), not guessed.
2. **RTL "run twice" assumptions.** Some C-runtime state assumes single entry
   (init-once flags, atexit lists, locale). The shareable-RTL model (§A.2.2) handles
   most (RTL ctors run once at process start), but per-image `atexit`/`__cxa_atexit`
   must be scoped to the image and drained at rundown. **Risk:** an image whose
   `atexit` the activator fails to drain corrupts DCL later.
3. **Crashing / non-cooperative images.** A forked child that segfaults never touched
   DCL; an in-process image that segfaults is in DCL's address space. Mitigation: the
   critical-P1 `mprotect` protects the crown jewels; a SIGSEGV handler converts an
   image fault into `$EXIT(SS$_ACCVIO)` + rundown — which is what VMS does (an ACCVIO
   in an image is a condition; the image runs down, DCL survives). **Open:** how
   faithfully to reproduce SYS$IMGSTA's condition/last-chance path vs. a pragmatic
   SIGSEGV→rundown. **Recommendation:** pragmatic first, faithful condition handling
   as a follow-on.
4. **Return mechanism: `swapcontext` vs `longjmp` vs a vms.ko mode-return.**
   `longjmp` after rundown is simplest and adequate; a vms.ko-mediated return is the
   most VMS-shaped but most work. **Recommendation:** `swapcontext` — a separate
   image stack in the P0 window and DCL's supervisor stack in P1 matches VMS's
   separate per-mode stacks and makes Ctrl-Y/`CONTINUE` re-entry natural.
5. **Enforcement ceiling honesty (Rule 10).** A does not get per-page four-mode memory
   protection; mode is enforced at the service boundary + critical-P1 pages, modeled
   elsewhere. **Operator question:** acceptable for 1.0, or is a two-process "shadow
   ring" (image in a seccomp/ptrace jail trapping privileged ops) wanted for stronger
   enforcement at the cost of re-introducing a second task? **Recommendation: no** —
   the service-boundary + critical-P1 model is faithful enough and keeps the
   one-process property, which is the entire point of A; a shadow ring re-opens the B
   problem.
6. **B stays as the RUN fallback until A lands and its suite is green.** The switch
   (increment iv/v) flips `dcl_activate_image` from fork to `imgact$activate()` only
   when the in-process path is proven on QEMU. No flag day; SPAWN/DETACHED/PIPE never
   change.

## A.7 Decomposition into implementation increments

Outcome-shaped, each QEMU-provable, each under the INV-6 constraint (no userspace
fake; fail honestly without `/dev/vms`). Dependency chain (i)←(ii)←(iii)←(iv)←(v)←(vi);
(iv) additionally needs (i)+(iii); (vi) needs all. Context hint: **Systems** for
(i)–(v), **QA** for (vi).

- **(i) vms.ko P0 region alloc/free.** `VMS_IOCTL_P0_MAP`/`P0_UNMAP`: the executive
  records a process's current P0 extent `[base,limit)` and reflects it in `$GETJPI`
  region/working-set answers; the P0 reservation window is established at process
  start. *Provable:* a QEMU test maps/unmaps a P0 extent and reads it back via
  `$GETJPI`; absent `/dev/vms`, the ioctl fails `SS$_NOSUCHDEV` (negctl).
- **(ii) P1 persistence + region model.** DCL reserves the P0 window and lays its
  process-permanent state (command-loop context, LNM$PROCESS root, RMS ctx) in P1;
  the executive distinguishes P0 (per-image) from P1 (process-permanent) for
  accounting and rundown. *Provable:* a P1 `LIB$GET_VM` region and a `$DCLEXH` exit
  handler survive a P0 map/unmap cycle.
- **(iii) Access-mode transition + boundary enforcement.** Activation sets
  current-mode User, rundown restores Supervisor; raising mode requires CMEXEC/CMKRNL,
  lowering is free; critical-P1 pages are `mprotect`-protected while an image is
  mapped. *Provable (negctl-anchored):* a User-mode image is **refused**
  `$CMKRNL`/`$SETPRV`-widen and a wild write to a protected P1 page faults — while the
  same from Supervisor succeeds.
- **(iv) In-process IMGACT activation library replacing the RUN fork.** Re-home the
  `src/imgact/` loader body as `imgact$activate(path, argv, env)`: map into P0,
  resolve/relocate/TLS/ctors, enter at entry in User mode via `swapcontext`.
  `dcl_activate_image` (`src/vmsdcl/dcl_cmd_process.c`) stops `fork()`/`execl()` and
  calls this. SPAWN/DETACHED/PIPE unchanged. *Provable:* `RUN FOO` runs FOO with
  `F$GETJPI("","PID")` unchanged across the call.
- **(v) Rundown returns to DCL.** `SYS$RUNDWN` library path: on the image's `$EXIT`,
  run image exit handlers, release image-scoped executive state via the executive
  (channels, P0 locks, image ASTs, image temp logicals), unmap P0, restore Supervisor,
  `swapcontext` back to the CLI loop. A crashing image → `$EXIT(SS$_ACCVIO)` +
  rundown (DCL survives). *Provable:* after `RUN` of an image that crashes or exits
  nonzero, DCL is alive at the next prompt with its P1 intact.
- **(vi) The QEMU proof suite.** The §A.5 suite: PID stable across RUN;
  `DEFINE/PROCESS` inside a RUN'd image visible to DCL after exit; SHOW SYSTEM shows
  one process; User-mode privilege escalation refused; SPAWN/DETACHED still make a new
  PID. Anchored on the `facility_defects` red-floor / green-property convention; never
  a census/string gate. **This increment is the definition of done for vms-68f.**

## A.8 Increment (v) landed state — the same-PID payoff proven, real-image loader flagged (vms-db2)

This records what increment (v) actually shipped, re-derivable by running the suite —
not a frozen "A is done". Runtime status is re-derived by the §A.5 QEMU suite.

### Shipped and proven

- **P1 is a real, registered control region.** `dcl_p1_init()`
  (`src/vmsdcl/dcl_cmd_process.c`) lays a page-aligned P1 control block at DCL
  startup, stores a process-permanent marker in its critical page, and registers
  `[base,limit)` via `vms_kif_p1_map()` — the call that had sat **UNWIRED** since
  increment (ii). `$GETJPI` now reports this process's P1 extent; the caller census
  proves the wiring (`vms_kif_p1_map`'s `OVMX-UNWIRED` declaration is retired).
- **The critical-P1 protection now guards DCL's own P1.** `dcl_activate_image()`
  hands `dcl_p1_critical_range()` (the real P1 critical page) to `imgact_activate()`
  instead of `NULL`, so §A.2.3(b)'s `mprotect` protects a live DCL datum while an
  in-process image runs.
- **THE PAYOFF, proven against real `/dev/vms`** (`tests/qemu/test_syssvc_imgact_inproc.c`,
  subject `testimg_inproc.c` mode 2): a value the in-process image stores into a
  writable cell DCL owns is **visible to DCL after the image runs down, at the same
  VMS PID and the same Linux PID** — the exact thing Option B could never do (its
  forked child's write landed in a separate, dead address space; line 149). This is
  the mechanism behind a process-permanent `DEFINE` flowing back to DCL: one address
  space, one process. Same-PID (Linux and VMS), P0-torn-down-at-rundown and the
  `mprotect` enforcement (a User-mode image's wild write to protected P1 faults →
  `SS$_ACCVIO`, DCL survives) are all asserted in the same negctl-anchored suite
  (`imgact-p1-not-protected`).

### The swapcontext tension — decision recorded (design §A.6.4)

Increment (iv) entered the image with a **direct call** on DCL's own stack because
`swapcontext`/`makecontext` (the design's preferred separate-P0-User-stack return) is
**not** part of the DECC$SHR universal set the VMS-native `DCL.EXE` links against, and
the fork replacement must link everywhere DCL does (musl-static / bootable / native-link).

**Decision: option (b).** The separate User-mode P0 stack and the eventual
Ctrl-Y/`CONTINUE` re-entry will be built with **already-exported primitives only** —
`setjmp`/`longjmp` (both DECC$SHR universals) plus a small per-arch **manual
User-mode stack switch** in the P0 window — **not** by adding the `ucontext` family to
DECC$SHR. Rationale: DECC$SHR is the C-RTL universal surface; the `ucontext` family is
not part of the OVMX C-RTL the self-hosting `DCL.EXE` binds to, so exporting it would be
a C-RTL expansion done to serve the activator rather than the RTL — the wrong reason to
grow the frozen vector (`docs/design-link-native-toolchain.md`, symbol-vector
positional freeze). `setjmp`/`longjmp` + a manual stack switch keep the fork replacement
linkable in every context and match VMS's separate per-mode stacks. **True Ctrl-Y/
`CONTINUE` *resume* additionally needs the image's full register context saved at the
interrupt point** (which `setjmp` cannot capture for later re-entry from a different call
chain); that is done by saving/restoring the interrupted register set with a per-arch asm
trampoline — the concrete follow-on this decision commits option (b) to.

### The flagged REMAINDER (fork fallback retained — no regression)

The in-process path still takes only **OVMX-marker / no-PT_INTERP / relative-reloc-only**
images (increment iv's class); every REAL image — one with a `PT_INTERP`, symbolic
imports, or the SysV auxv entry ABI — still returns `SS$_UNSUPPORTED` and
`dcl_activate_image()` **forks** it (design §A.6.6). Not yet done:

1. **The full real-image loader as an in-process library.** Re-homing `src/imgact/imgact.c`
   (a 55 KB freestanding, `-nostdlib`, self-relocating static-PIE with its own libc/
   syscall layer) into a library DCL calls in-process: `PT_INTERP` handling, the
   `.vms$sv`/`.vms$imp` **symbol-vector import binding to an already-resident
   `LIBVMS$SHR`** (so the image and DCL share ONE libvms — the prerequisite for a real
   `$CRELNM`/`DEFINE/PROCESS` by the image to flow back), `PT_TLS`, and the shareable
   dependency graph mapped once. This is the bulk of the increment and is deferred as a
   unit — attempting it hastily risks a silent LARP (an image that *looks* activated but
   does not truly share DCL's libvms state), which the authenticity invariants forbid.

   **LANDED (vms-db2, a proven sub-step of item 1 — the binding mechanism, in isolation):**
   The `.vms$imp` **import binding to an already-resident producer** now exists as library
   code and is proven genuinely-sharing in isolation. Two pieces shipped, both in
   `LIBVMS$SHR` (`src/libvms/syssvc/imgact_prodreg.c` + `include/imgact_prodreg.h`):
   - a **resident-producer registry** — `imgact_register_producer(soname, base, .vms$sv)` /
     `imgact_find_producer()` — the mechanism the design's §A.2.2 assumed but which did **not
     exist**: `IMGACT.EXE` keeps producer bases in its own private `static g_prods[]` and
     **discards** them at hand-off (no exported symbol, no `/proc/self/maps`, no
     `dl_iterate_phdr`), so in-process `libvms` had no way to find the `LIBVMS$SHR`/`DECC$SHR`
     the process already holds. This registry is that missing published table.
   - `imgact_bind_imports_resident(base, .vms$imp)` — re-homes `imgact.c`'s `bind_imports`
     as library code whose producer source is the **registry (resident)**, not a fresh
     `mmap`: for each import it GSMATCH+index-resolves (`ovmx_sv_resolve`) against the
     resident producer and writes the resolved **resident** address into the consumer's GOT
     cell. `imgact_activate()` now applies `.vms$rel` and calls it for a marker image that
     carries a `.vms$imp`; an import naming a non-resident producer returns `SS$_UNSUPPORTED`
     so the caller **forks** (never a private-copy bind — the LARP).
   - **Proof (`tests/qemu/test_imgact_bind.c`, negctl `consumer-import-not-bound-to-resident`):**
     a resident producer with shared internal state; a consumer that imports its universal by
     vector index and calls it through the bound GOT cell; the consumer's call reaches the
     **same** instance the test also mutates (counter 1 then 2 — genuine sharing, not a copy).
     This binding is pure userspace, so the proof needs **no** `/dev/vms` and runs in every
     environment; the P0-map/mode-transition/rundown that *wrap* a full activation stay proven
     separately against a real `/dev/vms` (`test_syssvc_imgact_inproc`).

   **LANDED (vms-db2, §A.8 remainder gap 1 — "publish the registry at runtime"):** the
   registry from the previous item was populated by nothing in the live boot path, so at
   runtime it stayed empty and a real image's `.vms$imp` imports had nothing to bind
   against. `imgact_publish_producers()` (`src/libvms/syssvc/imgact_prodreg.c` +
   `include/imgact_prodreg.h`) closes this: a `LIBVMS$SHR` universal that registers a whole
   producer list in one call. `IMGACT.EXE` (`activate_symbol_vector` in `src/imgact/imgact.c`)
   calls it once, after binding the executable's own imports and driving the C-RTL init:
   it resolves `imgact_publish_producers` from the resident `LIBVMS$SHR` symbol vector
   **by name** (no link-time dependency, no `DT_NEEDED` — IMGACT stays freestanding) and
   marshals its private `g_prods[]` across. Best-effort: a graph with no producer exporting
   the symbol (a non-`libvms` executable) publishes nothing and activates exactly as before.
   **Proof (`tests/qemu/test_imgact_publish.c`, negctl `publish-does-not-populate-registry`):**
   same anti-LARP shared-counter construction as `test_imgact_bind` above — publishing a
   resident producer is what lets a consumer's later import bind to it and reach the SAME
   instance the test mutates. Pure userspace, no `/dev/vms` needed. **No image class is
   flipped by this** — real images still fork; the IMGACT-side glue's own end-to-end proof
   (a real `LINK.EXE` image importing resident producers, activated in-process) rides on the
   native-link `DCL.EXE` runtime, which is blocked on the quarantined toolchain (`vms-0b8`).

   **STILL DEFERRED in item 1** (why real images still fork — no flip): (a) **entering a real
   `LINK.EXE` image** through its SysV auxv/stack `_start` ABI (not the `(a0,a1)` marker ABI)
   and **intercepting its `SYS$EXIT`** to return to DCL instead of terminating the process;
   (b) **`PT_TLS`** — sharing the resident `DECC$SHR`'s musl TLS with the in-process image
   (a TLS-bearing image is refused `SS$_UNSUPPORTED` today).
2. **The flip of REAL images to in-process** in `dcl_activate_image()`, gated on (1)
   being QEMU-proven per image class. Until then the fork stays for those classes.
3. **True Ctrl-Y / `CONTINUE`** for the in-process image (the option (a) asm follow-on above).
4. **Executive-mediated flows-back** ($SETEF / $CRELNM into LNM$PROCESS *by a real image*),
   which arrives with (1): a real image linked against the resident `LIBVMS$SHR` can call
   the services properly. This increment proved the address-space flows-back mechanism with
   a freestanding image's direct memory store; the service-level proof rides on (1).

`SPAWN` / `RUN/DETACHED` / `$CREPRC` / `PIPE` stages are untouched — they create genuinely
new VMS processes and always will (§A.3).

## Appendix — VMS documentation references

| Topic | Public reference (Rule 8) |
|-------|---------------------------|
| P0/P1/S0S1/P2S2 process address regions | IDSM, "Process Address Space"; VSI *Programming Concepts* Vol. I, "Address Space" |
| Access modes / PSL current & previous mode | IDSM, "Access Modes and the PSL" |
| SYS$IMGACT / SYS$IMGSTA / image initiation | *Programming Concepts*, "Image Initiation"; IDSM, "Image Activation and Exit" |
| Image rundown / SYS$RUNDWN / exit handlers | IDSM, "Image Exit and Rundown"; *System Services Reference* — `$EXIT`, `$DCLEXH`, `$RUNDWN` |
| Shareable images / image sections / symbol vectors | VSI *Linker Utility Manual*, "Shareable Images"; OVMX `docs/design-link-native-toolchain.md` |
| RUN vs SPAWN vs $CREPRC; PIPE = subprocesses | *DCL Dictionary* (`RUN`,`SPAWN`,`PIPE`); *System Services Reference* (`$CREPRC`) |
| Width / 64-bit regions | lab-Alpha (OpenVMS Alpha V8.4) — `tests/lab-alpha/README.md` |
