# LIB$SPAWN / SYS$CREPRC subprocess pipeline for OVMX — design record

Status: **design only, no implementation**. Written for the GCC lane (epic `vms-da0`)
ahead of its predicted F2 wall — the GCC driver's `cpp → cc1 → as → ld`/`collect2`
pipeline, which on real VMS is built entirely on `LIB$SPAWN`/`SYS$CREPRC`, never
Unix `vfork`/`execve`. Routed per the main/ACP conductor's contract note on
`vms-da0` (2026-08-20 17:27Z log): *"LIB$SPAWN (compiler spawning as/ld) = new
executive/process territory, likely MINE; route design to conductor for 3-way
gate."* This doc is that routed design.

Clean-room (Rule 8): derived only from the VSI OpenVMS RTL Library (LIB$)
Routines Reference Manual (`LIB$SPAWN`), the OpenVMS System Services Reference
Manual (`$CREPRC`, `$DCLEXH`, `$CREMBX`/mailbox QIO, `$WAITFR`), the DCL
Dictionary (SPAWN, RUN), and the DEC C RTL Reference Manual's documented
`vfork()`/`exec()` behavior under OpenVMS (VMS has no native `fork()`; the CRTL
implements the BSD `vfork`/`exec` pair *in terms of* `$CREPRC`, not a Linux-style
copy-on-write fork — this is public, documented VMS host behavior, not VSI
internals). Everywhere OVMX represents something VMS does not publish
byte-for-byte, it is labelled **OVMX design choice** below, never presented as
VMS-authentic. No VAX/Alpha lab observation was needed for this doc — process
creation is behavioral, not width-sensitive at the *contract* level, so public
docs are sufficient; a lab-verifiable claim is marked where one exists.

---

## 1. What LIB$SPAWN / SYS$CREPRC do on real VMS

**`$CREPRC`** (System Services Reference) creates a process. The two shapes:

- **Subprocess** (default): stays in the creator's job tree; the creator is the
  process's *parent* and can `$WAITFR`/`$SYNCH` on it, and it is deleted
  automatically if the job's top process exits.
- **Detached process** (`PRC$M_DETACH`): not part of any job tree, owns no
  controlling terminal, and outlives its creator — `RUN/DETACHED` and system
  startup procedures use this shape.

`$CREPRC` takes an image name, `SYS$INPUT`/`SYS$OUTPUT`/`SYS$ERROR` equivalence
names, an optional privilege mask and quota list (inherited from the creator
when omitted), an optional process name (`PRCNAM` — duplicate names within a UIC
group are rejected, `SYSTEM-F-DUPLNAM`), base priority, UIC, and status flags.
On success it returns the new process's **VMS process ID** — a value the
*executive* assigns, meaningful system-wide (`$GETJPI`, `SHOW SYSTEM`,
`$DELPRC` can all resolve it), not a host OS PID.

**`LIB$SPAWN`** (RTL Library Reference) is the higher-level routine DCL's own
`SPAWN` command and most VMS programs use. Per the public reference, it creates
a subprocess that runs a **CLI** (normally DCL) and:

- If a `command-string` is supplied and no `input-file`, the subprocess CLI
  executes that one command and terminates. Otherwise it reads commands
  interactively from `SYS$INPUT` (the given input file, or the parent's
  `SYS$INPUT`).
- `output-file` becomes the subprocess's `SYS$OUTPUT`.
- `CLI$M_NOWAIT` in the flags argument makes `$CREPRC` create the subprocess
  and return immediately; **without it, the caller `$HIBER`nates until the
  subprocess completes** (the documented default wait behavior), and the
  completion status is returned in the `completion-status-address` argument.
- `event-flag-number` and `ast-address`/`ast-routine-argument` let the caller
  ask to be notified of subprocess **completion** asynchronously instead of (or
  in addition to) blocking — the flag is set and/or the AST is queued when the
  subprocess CLI exits, which is what lets a `/NOWAIT` spawn later `$SYNCH` or
  simply be interrupted by an AST rather than polling.
- **Symbol table and logical names**: the spawned subprocess is a normal child
  CLI process — it does **not** automatically inherit the parent's *local*
  (process-private) symbol table or logical name table unless `CLI$M_KEYPAD`-
  adjacent inheritance switches are used; it does inherit the *job-wide*
  logical name table (`LNM$JOB`) and any `LNM$SYSTEM`/`LNM$GROUP` names, because
  those tables are shared by table, not copied per-process. This is the
  standard VMS process/job logical-name-table relationship (Programming
  Concepts Manual), not something specific to `LIB$SPAWN`.
- **CLI relationship**: `LIB$SPAWN`'s `cli-name`/`table-name` arguments let a
  caller ask for a non-default CLI or a private command table; absent that, the
  subprocess runs whatever CLI the creating process itself runs under (on
  OVMX, and on stock VMS, that is DCL).

Underneath, `LIB$SPAWN` is documented to invoke `$CREPRC` to create the
subprocess and start it running the CLI image, then arranges the `SYS$INPUT`/
`SYS$OUTPUT` redirection and the wait/notification behavior described above.
That composition — `$CREPRC` as the primitive, `LIB$SPAWN` as the packaged
"run a CLI command as a subprocess" convenience atop it — is the structural
fact this design preserves.

**`$DCLEXH`** registers an exit handler that runs when the *calling* process
exits (`$EXIT`) — relevant here because a genuine subprocess-pipeline driver
(the GCC driver, MMK) commonly registers one to clean up temp files/mailboxes
if a stage fails and the driver process itself has to unwind.

---

## 2. OVMX current state

This is **not** a "does OVMX spawn subprocesses at all" gap — it already does,
genuinely, in three separate places, with real fork/exec of real images and an
honest-failure boundary. The gap is narrower and more structural: the three
paths don't agree with each other, and the one most directly relevant to the
GCC pipeline (`LIB$SPAWN`) bypasses the executive process-table registration
the others use.

### 2a. `sys$creprc` — real, executive-registered (`src/libvms/syssvc/sys_process.c:609`)

Implemented for both subprocess and `PRC$M_DETACH` shapes. `fork()`s, and the
**child** — before doing anything else that could fail — registers itself in
the executive's process table over `/dev/vms` (`vms_kif_setprn` if a name was
given) and reports back over a `O_CLOEXEC` pipe both the executive-assigned
VMS process ID and a status. `$CREPRC`'s `pidadr` is therefore that
**executive-assigned** VMS PID (not `getpid()`), which is what makes it
resolvable later by `$GETJPI`/`SHOW SYSTEM`/`$DELPRC` — the exact property the
`vms-2b8` fix exists to guarantee (see the long comment at
`sys_process.c:660-700`). `PRC$M_DETACH` correctly `setsid()`s + double-forks
so the reporting task is the one that actually runs the image, matching VMS's
"creator cannot wait on a detached process" semantics. The compat register
(`docs/compat/facilities/sys-process.yaml:17`) rates it `implemented, real`.

### 2b. `lib$spawn` — real, but NOT executive-registered (`src/libvms/rtl/lib_misc.c:194`)

As of `vms-98c`, this genuinely `fork()`s and `execl()`s the **real, shipped
`SYS$SYSTEM:DCL.EXE`** image (resolved through the same VMS filespec
translator `PROVISION`/`JOB_CONTROL` use) — not `/bin/sh`, which is what it did
before `vms-98c` and is exactly the facade class Rule 9/INV-6 exist to kill.
`SYS$INPUT`/`SYS$OUTPUT` redirection, single-command (`-c`) vs. interactive
subprocess CLI, and `CLI$M_NOWAIT` are all real. **Honest failure**: if
`SYS$SYSTEM:DCL.EXE` cannot be resolved or is not executable, it returns
`SS$_NOSUCHFILE` and creates nothing — no fallback interpreter, ever. This
needs **no `/dev/vms`** today, because it never touches the executive.

That last fact is also the gap. Three parameters are accepted and silently
discarded (`lib_misc.c:275`, `(void)prcnam; (void)efn; (void)astadr;
(void)astprm;`):

- **`prcnam`** — never applied. The child never registers under the requested
  name (no `vms_kif_setprn` call), unlike `sys$creprc`'s child.
- **`efn`/`astadr`/`astprm`** — the documented completion-notification path
  for `/NOWAIT` is entirely unwired. A `/NOWAIT` `lib$spawn` returns
  `SS$_NORMAL` with `*status` **left unwritten** — there is no way for a
  caller to later learn the subprocess finished short of `waitpid()`-style
  polling the driver has no VMS-legal way to express, because `lib$spawn`
  gave it no VMS process ID to `$GETJPI`/`$SYNCH` against in the first place.
- **Consequence**: because the subprocess is never entered into the executive
  process table, `pid` (returned to the caller) is a bare Linux PID with no
  VMS meaning — `$GETJPI`/`SHOW SYSTEM`/`$DELPRC` cannot resolve it. A
  `lib$spawn`ed process is invisible to the rest of the VMS process-management
  surface that `sys$creprc`-created processes are visible to. This is the
  concrete divergence between OVMX's two "create a process" paths.

### 2c. DCL `SPAWN` builtin — a *third*, independent path (`src/vmsdcl/dcl_cmd_process.c:1801`)

`cmd_spawn()` does **not** call `lib$spawn` or `sys$creprc`. It re-execs
`/proc/self/exe` (the `vmsdcl` binary itself) directly, with its own
`/OUTPUT=file` and `/NOWAIT` handling duplicated from scratch, and its own
`%DCL-E-CREPRC`/`%DCL-I-SPAWNED` message text. Functionally it produces a
similar user-visible result to `LIB$SPAWN` (a DCL subprocess running one
command or interactively), but it is a third, divergent implementation with
no relationship to either `sys$creprc`'s executive registration or
`lib$spawn`'s `SYS$SYSTEM:DCL.EXE` resolution path. A program that calls
`LIB$SPAWN` and a user who types `SPAWN` at the DCL prompt get subprocesses
created by two different mechanisms with two different fidelity profiles.

### 2d. What's already built and directly reusable for the GCC pipeline

The MMK exec-drive investigation (`docs/design-mmk-exec-drive-ovmx.md`, item
`vms-b23`) hit exactly the completion-notification gap above from a different
angle — a persistent `/NOWAIT` DCL subprocess driven over mailboxes — and it
named three prerequisites. All three are now landed on `origin/main`:

| Prerequisite | Facility | Status |
|---|---|---|
| Async AST delivery to a hibernating process | `$HIBER` interruptible by a cross-process AST | **implemented** — `vms-feb`, `docs/design-async-ast-delivery-ovmx.md`, executive-resident (`src/kernel-core/vms_ast.c`), proven against real `/dev/vms` |
| Cross-process mailbox write-attention AST | `$QIO IO$_SETMODE\|IO$M_WRTATTN` | **implemented** — `vms-9003` (`src/kernel-core/vms_mbx.c`) |
| Non-blocking mailbox read | `$QIO IO$_READVBLK\|IO$M_NOW` | **implemented** — `vms-5df` (`src/kernel-core/vms_mbx.c:725`, `src/libvms/syssvc/sys_qio.c:199`) |
| DCL reading `SYS$INPUT`/writing `SYS$OUTPUT` over a mailbox (not just a file) | `dcl_mbx.c` bridge | **implemented** — `vms-786`, proven end-to-end against the real shipped `DCL.EXE` by `tests/qemu/test_syssvc_mbx_dcldrv.c` |
| Bidirectional, multi-message mailbox command/response between two genuinely separate processes | `$CREMBX`/`$QIO WRITEVBLK`/`READVBLK` cross-process | **implemented** — `vms-e0b`, proven by `tests/qemu/test_syssvc_mbx_cmdresp.c` |

This means the full "persistent subprocess + two mailboxes + async
notification" transport `design-mmk-exec-drive-ovmx.md` scoped as *design A*
(and marked BLOCKED pending exactly these facilities) now has all of its
named executive prerequisites in place. That design's own status header is
therefore stale relative to current `main` and should be re-checked before
anyone assumes design B's synchronous-batch workaround is still necessary —
noted here as an observation, not something this doc's scope resolves.

### 2e. Net assessment

**OVMX spawns real subprocesses today**, with a genuine honest-failure
boundary and no `/bin/sh` fallback anywhere in the three paths. What it does
**not** have is *one* VMS-faithful creation primitive that both (a) registers
the child in the executive process table the way `$CREPRC` does and (b)
delivers the `LIB$SPAWN`-documented completion notification (EF/AST) for
`/NOWAIT`. That gap is exactly what the GCC driver's pipeline needs, because a
pipeline stage's completion status is the signal the next stage (or the
driver's own error handling) depends on.

---

## 3. Backfill design

### 3a. Principle: one creation primitive, `LIB$SPAWN` as documented — a thin CLI-packaging layer over `SYS$CREPRC`

Per §1, VMS's own layering is `LIB$SPAWN` → `$CREPRC` + wait/notify wiring.
OVMX should match that layering exactly, which also resolves §2's
triplication:

1. **`lib$spawn` stops doing its own `fork()`/`execl()`.** It resolves
   `SYS$SYSTEM:DCL.EXE` (or the requested `cli-name`) exactly as today, builds
   the `SYS$INPUT`/`SYS$OUTPUT` equivalence-name arguments, and calls
   `sys$creprc()` with those, `prcnam` passed straight through (currently
   discarded — this alone fixes the invisibility gap), and `stsflg` derived
   from `CLI$M_NOWAIT` (clear → wait for completion, matching `$CREPRC`'s own
   default-subprocess-with-wait shape via a `$CREPRC`-side or `LIB$SPAWN`-side
   wait loop — see 3b).
2. **DCL's `SPAWN` builtin (`cmd_spawn`) calls `lib$spawn()`** instead of its
   own re-exec. This deletes the third code path rather than adding a fourth,
   and gives every DCL-visible spawned subprocess the same executive
   registration and `$GETJPI`/`SHOW SYSTEM` visibility a program-level
   `LIB$SPAWN`/`$CREPRC` caller gets — currently only true of `RUN`
   (`dcl_cmd_process.c:1137`, which already calls `sys$creprc` directly, per
   the comment at `dcl_cmd_process.c:2172`).
3. **`sys$creprc` is otherwise unchanged.** It already does the hard part (the
   creation handshake, PID assignment, detached-process double-fork). This
   backfill is additive on top of it, not a rewrite.

This is an **OVMX design choice** in exactly one place: real VMS's `$CREPRC`
uses a genuine kernel-mode process-creation primitive with no `fork()`
equivalent to reason about; OVMX's is `fork()`-based with a post-fork
executive-registration handshake (already true of `sys$creprc` today, and
already labelled as such in its own header comment). `LIB$SPAWN` calling
`sys$creprc()` as a normal libvms-internal call, rather than duplicating its
handshake, is not a new representation choice — it is removing a duplicate.

### 3b. Completion notification: EF + AST for `/NOWAIT`, matching the documented contract

`LIB$SPAWN`'s `efn`/`astadr`/`astprm` need a real completion signal to attach
to. Two facilities already exist to build this from, both executive-resident
and already 3-way-gated (§4):

- The async AST delivery machinery from `vms-feb` (`vms_ast_notify_arrival`,
  interruptible `$HIBER`) is exactly the "notify a waiting/hibernating process
  when something happens elsewhere" primitive `LIB$SPAWN`'s AST argument
  needs — the "something" here is process exit rather than a mailbox write.
- `sys$creprc`'s child-registration handshake already reports back over a
  pipe when the child registers; nothing today reports back when the child
  **exits**. The backfill's genuinely new executive-resident piece is a
  **process-exit notification**: when a subprocess created via `$CREPRC`
  terminates, the executive (which already tracks the process table entry,
  `src/kernel-core/vms_proctab.c`) queues a completion event — an AST if one
  was armed (mirroring the mailbox write-attention AST's queue/pending-mode
  logic), and/or sets the requested event flag — to the *parent*. A
  synchronous (`CLI$M_NOWAIT` clear) `lib$spawn` then simply does what
  `$CREPRC` subprocess semantics document today for its own caller: `$HIBER`
  until that event fires, rather than the current implementation's direct
  `waitpid()` (which only works because today's `lib$spawn` still holds a
  real Linux child — once it goes through `$CREPRC`'s handshake, the direct
  parent/child Linux relationship is preserved for the *subprocess* shape
  specifically, so `waitpid()` remains available as the underlying wait
  **mechanism**; the exit-notification event is what's new, needed for the
  EF/AST-driven `/NOWAIT` case where the caller isn't blocked in `waitpid()`
  at all).
- **Labelled OVMX design choice**: the exact representation of "queue an exit
  AST/EF to the parent process's executive-resident AST queue" is OVMX's own
  (VMS's real mechanism is kernel-internal and unpublished at that level);
  the *observable contract* — EF gets set, AST routine gets queued and later
  dispatched, both only for a `/NOWAIT` spawn that asked for them — is what's
  pinned to the public `LIB$SPAWN` reference.

### 3c. GCC driver pipeline mapping: `cpp → cc1 → as → ld`/`collect2`

The GCC driver (`gcc.c`) on a VMS host does not use Unix `vfork`/`execve`
directly for its pipeline — as documented publicly (DEC C RTL Reference,
`vfork()`), the CRTL's own `vfork`/`exec` pair is itself implemented in terms
of `$CREPRC` on VMS, because VMS has no native `fork()`. The forcing-function
framing from `vms-da0` (§2a of `docs/design-gcc-vms-oracle-lane.md`: OVMX
drives native RMS rather than honoring the CRTL's Unix-shim for file I/O)
extends identically to process creation: **OVMX's authored VMS-host layer for
the GCC driver should call the `LIB$SPAWN`/`$CREPRC` primitives above
directly, not route through musl's `vfork`/`posix_spawn`/`clone` on the
Linux host underneath OVMX's DECC$SHR.** Doing the latter would reinstate
exactly the "OS calls go through POSIX, force nothing VMS-authentic" failure
mode `design-gcc-vms-oracle-lane.md §2` rejected for the base pick.

Two shapes for the pipeline, both buildable on the backfilled primitive:

- **Default: sequential, temp-file handoff.** Each stage
  (`cpp`, then `cc1`, then `as`, then `ld`/`collect2`) is a synchronous
  `LIB$SPAWN` (or a direct `$CREPRC` + `$HIBER`/wait for tighter control over
  privileges/quotas than `LIB$SPAWN` exposes) with `SYS$OUTPUT` for one stage
  wired as `SYS$INPUT`... no — VMS has no anonymous-pipe file redirection
  primitive matching Unix `pipe(2)`, so each stage's output is a genuine RMS
  scratch file (`SYS$SCRATCH:`-style temp naming, matching how DCL command
  procedures traditionally chain compile/link phases), read back in as the
  next stage's input file argument. This needs nothing beyond §3a/§3b: create,
  wait, check `$STATUS`, if odd feed the temp file forward, if even stop the
  pipeline and report through `$DCLEXH`-registered cleanup (deleting
  temp files) — the standard `-save-temps`-shaped behavior, made the *default*
  transport rather than an option, because it needs no new mailbox plumbing.
- **Stretch: mailbox-pipe fan-out for concurrent stages** (e.g. `-pipe`
  behavior, `cc1` and `as` running concurrently with `cc1`'s stdout streamed
  into `as`'s stdin). This is not new work — it's the exact machinery §2d
  lists as landed for the MMK exec-drive: `$CREMBX` two mailboxes, `/NOWAIT`
  spawn both stages with those as `SYS$INPUT`/`SYS$OUTPUT` (now real per the
  `dcl_mbx.c`-style bridge generalized from DCL specifically to any
  RMS-record-oriented child image), write-attention AST + interruptible
  `$HIBER` for backpressure instead of DCL's specific command/response
  framing. Deferred until the sequential default is proven, since GCC does
  not require `-pipe` to produce correct output — it's a performance
  optimization on real VMS too.

### 3d. Fail-honest boundary (Rule 9 / INV-6)

Once `lib$spawn` routes through `sys$creprc`, it inherits `sys$creprc`'s
`/dev/vms` dependency for the registration handshake — a **new** dependency
`lib$spawn` does not have today (§2b: it currently needs no executive at all,
because it never registers anything). That is the correct direction: a
`lib$spawn`ed process that the rest of VMS process management cannot see was
never fully honest, it was just non-obviously so. With no `/dev/vms`, the
registration handshake must fail exactly as `sys$creprc`'s already does
(reported back over the handshake pipe as a real status, not silently
degraded) — `lib$spawn` propagates that failure status rather than falling
back to an unregistered fork/exec. No per-process fake registration, no
"looks connected, shares nothing" EF/AST notification path if the executive
that would deliver it is absent.

---

## 4. Shared executive/process layer → 3-way convergence gate

Every piece of new work above lands in shared, not per-arch, code:

- `src/libvms/syssvc/sys_process.c` (`sys$creprc`, unchanged logic but now the
  sole creation path for `lib$spawn` too)
- `src/libvms/rtl/lib_misc.c` (`lib$spawn`, rewritten to call `sys$creprc`)
- `src/vmsdcl/dcl_cmd_process.c` (`cmd_spawn`, rewritten to call `lib$spawn`)
- `src/kernel-core/vms_proctab.c` + `src/kernel-core/vms_ast.c` (new
  process-exit notification queuing, alongside the existing AST-queue and
  process-table code both already shared kernel-core modules per
  `docs/design-async-ast-delivery-ovmx.md`)

This is exactly the shared kernel-core/executive surface CLAUDE.md's
[[ilp32-width-proof]] invariant gates: VAX (ILP32) inherits it through the
NetBSD-VAX SYSKRNL build, Alpha (LP64) through its vms.ko recompile, x86_64
through CI. The process-exit notification's on-the-wire shape (whatever
carries "this VMS PID's subprocess exited, status=X" across the `/dev/vms`
ioctl boundary) is a struct whose field widths matter exactly the way the
existing `vms_mbx_wrtattn_reg` write-attention registration does — so this
backfill is a **3-way-gated shared-core change**, and per the `vms-da0`
conductor contract, routes through the main/ACP conductor for the
convergence gate before merge, same as any other shared executive change.

---

## 5. Ladder (suggested, not binding — sequencing is implementation's call)

1. **B0** — `lib$spawn` calls `sys$creprc()` instead of its own fork/exec;
   `prcnam` applied; `DCL SPAWN` calls `lib$spawn()` instead of re-exec.
   Deletes the triplication (§3a). No new executive facility — pure reuse of
   what `sys$creprc` already has. Fixes `$GETJPI`/`SHOW SYSTEM` visibility for
   both paths.
2. **B1** — process-exit notification (EF/AST) in the executive for
   `/NOWAIT` `$CREPRC`/`LIB$SPAWN` completions (§3b). New shared kernel-core
   work, 3-way gated.
3. **B2** — GCC driver's VMS-host process-creation hook calls
   `LIB$SPAWN`/`$CREPRC` directly (§3c default sequential/temp-file shape).
   Consumer of B0+B1, lives in the GCC-lane's own authored VMS-host layer,
   not in `src/libvms`/`src/vmsdcl`.
4. **B3 (stretch, deferred)** — mailbox-pipe concurrent-stage transport
   (§3c stretch), generalizing the MMK exec-drive machinery beyond DCL.

---

## 6. Open items flagged, not resolved here

- `docs/compat/facilities/lib.yaml:24` rates `lib$_cli_spawn` `implemented,
  real` with no qualifier about the EF/AST/`prcnam` gaps this doc found —
  worth a compat-register pass once B0/B1 land (or a `notes:` caveat now);
  not fixed in this doc per its design-only scope.
- `docs/design-mmk-exec-drive-ovmx.md`'s "BLOCKED as scoped" status header
  predates `vms-feb`/`vms-9003`/`vms-5df`/`vms-786` landing (§2d) — its
  design-A prerequisites now appear closed; someone should re-open that
  record and check whether design A is actually buildable now, separate
  from this doc's GCC-driven backfill.
