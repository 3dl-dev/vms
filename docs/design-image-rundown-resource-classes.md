# Image rundown: which resource classes are image-scoped vs process-permanent

Grounding for the SYS$RUNDWN image-scoped release the executive performs on
`VMS_IOCTL_IMAGE_RUNDOWN` (vms-68f increment v — `src/kernel/vms_access.c`,
`vms_lock.c`, `vms_devtab.c`, `vms_ast.c`). The design (`docs/design-in-process-
activation.md` Part II §A.6.1) flags this as *the hard part*: "which resource
classes are image-scoped vs. process-permanent must be pinned to the oracle per
class (observe what survives `RUN`), not guessed."

## Method and its ceiling (read first)

**This document is grounded in PUBLIC OpenVMS documentation (CLAUDE.md Rule 8's
second permitted source), NOT in live-lab observation.** It is therefore a
*design* note, not an entry under `docs/oracle/` (which is reserved for things
actually measured on the reference labs). The public documents cited below —
the VSI *OpenVMS System Services Reference*, *OpenVMS Programming Concepts*, and
*OpenVMS Internals and Data Structures* (IDSM) — publish the access-mode
ownership model and the rundown semantics at the level of detail the executive
change needs. No VSI/HPE source or binary was read (Rule 8).

**Flagged follow-up (the empirical confirmation this note does not carry):** a
live-lab pin of the *behavior* — e.g. on lab-1/lab-2 (VAX V7.3), issue
`$ASSIGN`/`$ENQ` from a running image and confirm via SDA that the channel/lock
is gone after the image exits while a channel DCL holds survives — is the
oracle-grade confirmation. It is tracked as a follow-up, not gated here (per the
"empirical-not-gate" / "pin to the oracle or make an item to revisit WITH the
method" rule). The rule this note grounds (below) is the *documented* SYS$RUNDWN
contract; the executive implements that contract, and the QEMU suite
(`tests/qemu/test_kmod_rundown.c`) proves the executive obeys it.

## The governing rule (documented)

On OpenVMS, most process resources are *owned at an access mode*. `SYS$RUNDWN`
takes an `acmode` argument and releases the resources owned at that access mode
**and all less-privileged (outer) modes**; resources owned at more-privileged
(inner) modes are untouched. Image exit (`SYS$EXIT` from a user-mode image)
drives image rundown at **user mode**, so it releases exactly the user-mode
(image) resources and leaves supervisor/exec/kernel-mode (process-permanent)
resources in place — which is why DCL (supervisor) resumes with its state
intact.

> Sources (public, Rule 8): *OpenVMS System Services Reference* — `$RUNDWN`
> ("performs rundown of the current image ... releases resources owned by the
> specified access mode and all outer access modes"), `$DASSGN`, `$DEQ`,
> `$DCLAST` (each documents its `acmode` and rundown behavior); IDSM, "Image
> Exit and Rundown"; *Programming Concepts*, "Image Initiation and Exit".

Access-mode numbering used throughout OVMX (`src/kernel/vms_ioctl.h`,
`PSL_C_KERNEL..PSL_C_USER` = 0..3): **inner = smaller number = more
privileged**. "Owned at access mode ≥ min_acmode" therefore means "at
min_acmode or *outer*", and image rundown passes `min_acmode = PSL_C_USER (3)`.

## Per-class disposition (what the executive does)

| Resource class | Executive object | Owned-at-mode recorded | Image rundown (USER) | Grounding |
|---|---|---|---|---|
| **Device channels** ($ASSIGN) | `struct vms_channel` | **new** `acmode`, stamped from `current_mode` at `$ASSIGN` | **released** if `acmode ≥ USER` (deassigned; implicit device ownership on that channel dropped) | `$DASSGN`/rundown: "channels assigned from user mode are deassigned at image rundown"; IDSM Image Rundown |
| **Locks** ($ENQ) | `struct vms_lock_entry` | **new** `acmode`, stamped from `current_mode` at `$ENQ` (distinct from the *lock* mode NL..EX) | **released** if `acmode ≥ USER` (dequeued; lkid becomes SS$_IVLOCKID) | `$DEQ` acmode semantics ("dequeues locks owned by the specified and outer access modes"); IDSM |
| **ASTs** ($DCLAST) | `struct vms_ast_entry` in `proc->ast[mode]` | already per-mode (`acmode`) | **flushed** for queues `mode ≥ USER` | `$DCLAST`/rundown: user-mode ASTs are flushed at image rundown |
| **P1 control region** | `proc->p1_base/p1_limit` (own lock) | n/a — process-permanent by definition | **untouched** (never named on the rundown path) | design §A.1.1: P1 is process-lifetime |
| **Identity / PID / name / privileges** | PCB fields | n/a | **untouched** | one process across a RUN (§A.1) |
| **Local event flags** (clusters 0/1) | `proc->ef.local[]` | n/a — process-permanent | **untouched** | Programming Concepts: local EF clusters are process-permanent |

### Deliberately NOT released here (flagged, not guessed)

Per the "do not guess" instruction, three classes the design's list mentions are
left to **process teardown** (unchanged from pre-vms-68f.v behavior — so there
is no leak *regression*; they still free at process death), pending either a
follow-up increment or a live-lab pin of their rundown class:

- **Mailbox channels** (`proc->mbx_channels`, `struct vms_mbx_chan`): released
  today only by `vms_mbx_release_all()` at process death. Stamping them with an
  acmode and image-scoping them is a mechanical follow-up; omitting it now does
  not regress anything (they never released at rundown before either).
- **A device the process `$ALLOC`ed**: an *explicit* allocation's rundown class
  (image vs process) is subtler than a channel's and is not pinned here, so the
  executive leaves an explicit allocation to process teardown rather than guess.
  Deassigning a user-mode channel still drops any *implicit* ownership resting on
  that channel (`device_release_channel()`), which is the common case.
- **Image temporary (user-mode) logical names**: `LNM$PROCESS` is not
  executive-resident yet (`src/kernel/vms_lnm.c` implements `LNM$SYSTEM`/GROUP/
  JOB; PROCESS is deferred — see design §"What B does NOT fix"), so there are no
  executive-resident image-scoped logicals to release. This becomes live only
  when `LNM$PROCESS` moves into the executive, and is the mechanism behind the
  "process-permanent DEFINE flows back to DCL" payoff (a separate increment).

### Exit handlers

`$DCLEXH` exit handlers run *before* rundown and are a userspace (image-library)
concern, not executive-resident state; the in-process activator
(`imgact_activate`) is where the image's exit-handler chain is walked. Not part
of this executive change.

## Why this is safe against the P0/P1 split

The rundown release runs **outside** `proc->mode_lock` and never touches
`proc->p1_base/p1_limit` (guarded by the disjoint `proc->p1_lock`). "P0/image
state dies at rundown, P1 survives" is thus true at the *resource* level for the
same structural reason it is true for the address extents (`vms_p1.c` header):
no code path on the rundown side can reach the process-permanent fields.
