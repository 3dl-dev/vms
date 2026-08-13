# Asynchronous AST delivery + interruptible $HIBER (vms-feb)

Status: implemented (executive-resident, cross-process, proven against real /dev/vms).
Facility: OVMX executive (vms.ko via /dev/vms). Coordinates with the executive-gap
epic (vms-6b8). Keystone prereq for MMK's exec-drive (spine #4, vms-b23).

## Clean-room provenance (CLAUDE.md Rule 8)

All AST-delivery, `$HIBER`/`$WAKE`, and access-mode semantics below are derived
ONLY from public VSI OpenVMS documentation and observed behaviour — never from
VSI/HPE source or binaries:

- **VSI OpenVMS Programming Concepts, "Using Asynchronous System Traps"** — an
  AST is delivered to a process ASYNCHRONOUSLY when it becomes deliverable (the
  process is at or below the AST's access mode and AST delivery is enabled at
  that mode); a more privileged AST is delivered over a less privileged one;
  `$SETAST(0)` disables delivery at the current mode without losing queued ASTs.
- **VSI OpenVMS System Services Reference, `$HIBER` / `$WAKE` / `$SCHDWK`** —
  `$HIBER` places the process in a wait; it is resumed by a `$WAKE` (or a
  scheduled wake), and **an AST can be delivered to a hibernating process; the
  AST executes and the process then continues to hibernate unless the AST (or
  another agent) issued a `$WAKE`**. A `$WAKE` that precedes the `$HIBER` is
  remembered (the wake-pending bit), so that `$HIBER` returns immediately.
- **VSI OpenVMS I/O User's Reference, mailbox driver** — `IO$_SETMODE|IO$M_WRTATTN`
  arms a one-shot write-attention AST delivered to the reader when another
  process writes the mailbox (the notification MMK's `send_cmd_and_wait` waits on).

Where OVMX makes a representation choice not published byte-for-byte by VSI, it
is LABELLED as an OVMX choice below, never presented as VMS-authentic.

## The gap this closes

Before vms-feb the AST *queue* existed (per-process, 4 access modes, executive-
resident in `src/kernel-core/vms_ast.c`) but there was no *asynchronous
delivery*: a queued AST was drained only when the process itself called
`sys$setast(1)`, and `sys$hiber` was a bare `pause()`. So a process that armed a
write-attention AST on a mailbox and then `$HIBER`ed waiting for it was **never**
interrupted when another process wrote the mailbox — the AST sat in the queue and
`$HIBER` blocked forever. That is exactly the `$HIBER`/`$WAKE` + write-attention
pattern `tests/corpus/tier3-mmk/build_target.c` uses (`sys$wake(0,0)` inside the
AST, `sys$hiber()` in `send_cmd_and_wait`), so MMK's exec-drive deadlocked.

## Design: split the wait (executive) from the dispatch (userspace)

An AST routine address is only valid in the process the AST is queued to, so the
executive cannot *run* it — dispatch stays in userspace (`vms$$deliver_pending_asts`
in `src/libvms/syssvc/sys_ast.c`, draining the queue through `VMS_IOCTL_DELIVERAST`,
unchanged). What becomes executive-resident (Rule 9 / INV-6 — cross-process,
through `/dev/vms`, no per-process fake) is the three things a per-process fake
could never carry across the process boundary:

1. **The hibernate wait** — `VMS_IOCTL_HIBER` (`vms_ioctl_hiber`, vms_proctab.c)
   blocks the caller in the executive until either a `$WAKE` is pending for it OR
   an AST is deliverable to it (`vms_ast_has_deliverable`, the same
   at-or-outside-current-mode bound `VMS_IOCTL_DELIVERAST` enforces).
2. **The wake state** — a sticky single bit `wake_pending` on the process
   (matching VMS's wake-pending semantics), set by `VMS_IOCTL_WAKE`
   (`vms_ioctl_wake`) and consumed by `VMS_IOCTL_HIBER`.
3. **The AST-arrival notification** — `vms_ast_notify_arrival` (vms_ast.c): every
   path that queues an entry into a process's AST queue ($DCLAST, a mailbox
   write-attention write in vms_mbx.c, a lock completion/blocking AST in
   vms_lock.c) broadcasts that process's `hiber_wq` AFTER the entry is on the
   queue, so a `$HIBER` waiter wakes, re-tests, and drains.

`sys$hiber` (`src/libvms/syssvc/sys_process.c`) becomes a loop:

```
for (;;) {
    woken = vms_kif_hiber();        // block: $WAKE pending OR an AST deliverable
    vms$$deliver_pending_asts();    // run deliverable ASTs here; one may $WAKE
    if (woken) return SS$_NORMAL;   // released by a $WAKE -> $HIBER ends
    // released by an AST that did NOT $WAKE -> re-hibernate (per VMS)
}
```

This reproduces the VMS rule exactly: an AST that does not `$WAKE` runs and the
process keeps hibernating; only a `$WAKE` (its own, or one the AST issued) ends
`$HIBER`; and a `$WAKE` preceding the `$HIBER` (sticky bit) makes it fall
straight through.

### Locking (lost-wakeup freedom)

`hiber_wq` is paired with `hiber_lock`. `vms_ioctl_hiber` holds `hiber_lock`,
tests `wake_pending || vms_ast_has_deliverable(...)`, and only then
`exec_cv_wait`s (which atomically drops `hiber_lock`). Every enqueue path drops
`ast_state->lock` and THEN calls `vms_ast_notify_arrival`, which takes
`hiber_lock` to broadcast — so the broadcast is issued under the very lock the
waiter parked on, and cannot be lost against a waiter mid-park. Lock order is
`hiber_lock` OUTER → `ast_state->lock` INNER (waiter side only); the enqueue side
never holds both, so there is no inversion. `mbx->lock`/`res->lock` → `hiber_lock`
are fresh edges (nothing takes those under `hiber_lock`).

### $WAKE targeting (OVMX choice, Rule 8)

`sys$wake(NULL)` / `vms_pid == 0` is a self-directed wake (no privilege) — the
MMK case. A non-zero `pidadr` names a **VMS PID**, resolved by the executive
(SS$_NONEXPR if none; SS$_NOPRIV without GROUP/WORLD for another process),
consistent with how `$DELPRC`/`$GETJPI` target by VMS PID — NOT the raw Linux pid
the old `SIGCONT` shim used. VMS gates `$WAKE` of another process on GROUP/WORLD;
OVMX pins it to `vms_proc_may_read` (the same predicate), stated here rather than
implied.

## Proof (Rule 7 / veracity)

`tests/qemu/test_syssvc_hiber_ast.c`, against a real `/dev/vms`: process A arms a
write-attention AST (whose routine `sys$wake`s self) and `sys$hiber`s with NO
explicit `$SETAST` drain; an unrelated re-exec'd process B assigns the mailbox by
name and writes it; the test asserts A's `$HIBER` returns and the AST ran (a
bounded coordinator wait, so a deadlock is a named FAIL, not a QEMU-wide hang). A
second scenario proves the sticky wake (a `$WAKE` before `$HIBER` returns
immediately). Negative control `hiber-ast-not-delivered`
(`tests/qemu/facility_defects.sh`) removes the arrival broadcast: the AST is
still queued but `$HIBER` never wakes → deadlock → the suite reddens, while
`test_syssvc_mbx_wrtattn` (explicit `$SETAST` drain, never hibers) stays green.

## Scope / deferred

- Self-directed `$WAKE` and single-node cross-process `$WAKE`-by-VMS-PID are
  implemented and (self-wake) proven. `$WAKE` by process NAME (`prcnam`) is not
  resolved — a caller naming its target by string is redirected to itself, the
  same limitation the other `prcnam`-taking process services carry.
- Upward access-mode PREEMPTION (a kernel-mode AST trapping into a process
  running in user mode) is NOT introduced here and remains the labelled OVMX
  containment choice already documented in `vms_ast.c` (delivery is bounded below
  by the caller's current mode). vms-feb adds *when* a deliverable AST wakes a
  hibernating process, not a new preemption model.
- This unblocks MMK's `send_cmd_and_wait` (vms-b23) at the AST/$HIBER layer;
  `IO$M_NOW` (vms-5df) and the DCL-mailbox wiring (vms-786) are still needed for
  the full MMK drive.
