# MMK exec-drive on OVMX — design record (vms-b23, self-host spine #4)

Status: **BLOCKED as scoped** — the mailbox + write-attention-AST drive MMK's
`build_target.c` actually uses cannot be completed with the three facilities
this item was filed against (`vms-98c` lib$spawn, `vms-e0b` mailbox IPC,
`vms-9003` write-attention AST). Wiring the seam exposes **three further
executive/DCL gaps** the three prereqs each proved a *primitive* for but never
proved *in composition*. This record states the gaps with evidence, gives the
one achievable interim design, and tees up the transport decision.

Clean-room (Rule 8): all of this is derived from MMK's own public `sp_mgr.c` /
`build_target.c` and the OpenVMS System Services / I/O User's Reference. The
OVMX transport choices below are labelled as OVMX design, never VMS-authentic.

---

## What MMK requires (the protocol, unedited freeware)

`tests/corpus/tier3-mmk/build_target.c` drives builds through ONE persistent DCL
subprocess:

- `sp_open(&spctx, &ini, echo_ast, 0)` — spawn a persistent DCL (`CLI$M_NOWAIT`),
  SYS$INPUT/SYS$OUTPUT redirected to a command + a result mailbox; arm a
  **write-attention AST** (`echo_ast`) on the result mailbox.
- `send_cmd_and_wait()` — `sp_send` the resolved command line + three
  end-of-command marker commands, then:
  ```c
  do { sys$hiber(); } while (!command_complete);
  ```
- `echo_ast` (fired by the write-attention AST when DCL writes output) drains the
  result mailbox with a **non-blocking** `sp_receive` loop, echoing output and,
  on the `MMK____status=<hex>` marker, setting `command_complete` and
  `sys$wake()`-ing the hibernating main line.

That is: **persistent DCL + two mailboxes + async write-attention AST +
`$HIBER`/`$WAKE` + non-blocking drain.**

## The three gaps (evidence)

1. **No asynchronous AST delivery.** OVMX delivers ASTs only when a process
   calls `sys$setast(1)` and *drains* the executive queue
   (`src/libvms/syssvc/sys_ast.c:154` `deliver_pending_asts`). A process sitting
   in `$HIBER` is never interrupted to run a queued cross-process
   write-attention AST, so `command_complete` is never set and the wait loop
   deadlocks. `src/kernel-core/vms_ast.c:233` names signal-driven delivery a
   *"future enhancement"*; `sys$hiber` is a bare `pause()`
   (`src/libvms/syssvc/sys_process.c:448`). `vms-9003` proved the AST is
   **queued** cross-process and **drained on an explicit `sys$setast(1)`** — it
   did not prove delivery to a *blocked* process.

2. **No non-blocking mailbox read.** `echo_ast`'s drain loop needs
   `IO$M_NOW` (return an error when the mailbox is empty). OVMX mailbox reads
   **always block**: `IO$M_NOW` is dropped in `qio_mailbox_op`
   (`src/libvms/syssvc/sys_qio.c:200`) and `vms_kif_mbx_read` "blocks until a
   message is queued" (`src/libvmssys/vms_kif.c:1734`). The `while(OK(sp_receive))`
   drain would block forever after the last record instead of terminating.

3. **DCL cannot use a mailbox as SYS$INPUT/SYS$OUTPUT.** DCL reads commands with
   `fgets(stdin)` and writes with `printf(stdout)` — fd-based
   (`src/vmsdcl/dcl_main.c:762`). OVMX mailboxes are **entirely executive-resident
   with no fd** (`src/libvms/syssvc/sys_mailbox.c:133` sets `fd = -1`;
   read/write route through `exec_chan` ioctls). `lib$spawn` redirects a child's
   SYS$INPUT/SYS$OUTPUT with `freopen(<linux path>)`
   (`src/libvms/rtl/lib_misc.c` child path) — a mailbox has no such path, so a
   spawned DCL cannot read/write the mailboxes at all without a bridge.

None of these is a test-harness artifact (CLAUDE.md Rule 9): they are real
missing executive/DCL capabilities. Faking the drive over them would be exactly
the INV-6 facade the authenticity invariants exist to kill.

## Two designs

### A. VMS-faithful (mailbox + write-attention AST) — BLOCKED

Needs, as new prerequisite work:

- **Async AST delivery** — when the executive queues a user-mode AST into a
  process that has delivery enabled, signal that process (the `SIGRTMIN+mode`
  path `vms_ast.c` already names) and have libvms drain on the signal; make
  `$HIBER` AST-interruptible. Executive + libvms change; own design cascade +
  QEMU proof.
- **Non-blocking mailbox read** — honour `IO$M_NOW` through
  `qio_mailbox_op` → `vms_kif_mbx_read` → the executive read path, returning an
  empty-mailbox status. Executive + libvms change.
- **DCL-over-mailbox bridge** — in `lib$spawn`'s child, when SYS$INPUT/SYS$OUTPUT
  name a mailbox (`MBAn:`), assign it and pump mailbox↔stdio around the DCL
  grandchild (DCL stays fd-based). Userspace, but only *useful* once (1)+(2)
  land.

This is the transport the item asked for. It is **multiple items of executive
work**, not a wiring step.

### B. Interim: synchronous per-command DCL batch — ACHIEVABLE, host-testable

Uses only facilities that already work (`vms-98c` synchronous `lib$spawn` needs
**no executive** — "running a real child image needs no /dev/vms"). OVMX
`ovmx_mmk_sp.c` accumulates the `sp_send` stream; the `send_cmd_and_wait` wait
point (one tagged OVMX seam) runs *ini-setup + this command* as one temporary
`.COM` through a **synchronous** `lib$spawn` with SYS$OUTPUT to a file, and reads
`$STATUS` back from the `MMK____status=` marker in that file. `echo_ast` /
`$HIBER` / the write-attention AST are bypassed on OVMX.

- **Real, not a facade:** a real DCL runs the real `TCC`/`LINK` foreign commands
  and produces a real image; `$STATUS` is the real command status read from real
  output.
- **Testable on the host** (ctest, same path as `toolchain-mmk-parse`, no
  `/dev/vms`): MMK.EXE reads a real `descrip.mms`, drives `TCC MMKSPINE.C` →
  `LINK MMKSPINE.OBJ`, and the built `MMKSPINE.EXE` activates + runs + prints its
  expected output (independent oracle).
- **Deviation (Rule 5, disclosed):** the *transport* is a synchronous `.COM`
  batch, **not** MMK's persistent-mailbox subprocess. State that does not persist
  across separate `send_cmd_and_wait` calls: fine for independent build steps
  (compile, link); the ini setup is idempotent and replayed each batch. Labelled
  an OVMX design choice; superseded by design A once the executive gaps close.

## Recommendation / decision teed up

1. **Reclassify vms-b23**: it is not spine #4's final wiring step. File its true
   prerequisites — **async AST delivery**, **non-blocking mailbox read (IO$M_NOW)**,
   **DCL SYS$INPUT/SYS$OUTPUT over a mailbox** — and make vms-b23 (design A)
   depend on them.
2. **Operator/conductor call**: ship design B now as the interim exec-drive so
   spine #4's *goal* (MMK actually builds + activates an image) lands and spine
   #5 (`vms-fe4`) unblocks — accepting the disclosed synchronous-batch transport
   — OR hold spine #4 until design A's executive prerequisites are built. This is
   a transport-authenticity trade the operator owns (Rule 5; product/authenticity
   posture is reserved).

Absent an answer, the honest default is: do **not** silently ship either a
non-functional mailbox+AST stub dressed as working, nor design B without the
disclosed-deviation sign-off. The stub in `ovmx_mmk_sp.c` therefore stays
honest (`SS$_UNSUPPORTED` outside `/NOACTION`) and this record carries the plan.
