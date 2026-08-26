# Executive-Boundary AUDIT Tracer (vms-c08)

**Parent:** vms-040 (executive-boundary enforcement — the structural enabler under the
whole fabrication class). **Phase:** A/AUDIT (observe-only). **Sibling, post-1.0:**
vms-48e (the ENFORCE spike — CHMK/CHME interception model). This doc is Phase-A only.

## Why (the faithfulness problem)

OVMX images are musl-linked; musl issues **raw Linux syscalls** (clone/fork, openat,
socket, ioctl on devices). So an OVMX image has a live path straight to the Linux kernel
that **bypasses the executive** (`/dev/vms`). The executive is authoritative only *by
convention* — nothing makes a VMS-semantic op go *through* it rather than *around* it.
That is the exact reason green-CI ≠ VMS-correct: any facility can be faked by going
straight to Linux and reporting success (INV-6 violation avenue).

Real VMS has no such door: a user-mode image issues CHMK/CHME traps and the executive
dispatches; there is no raw-hardware path around it.

**Phase A does not close the door — it makes every use of the door VISIBLE.** A bypass
becomes a recorded *finding* instead of a silent cheat, so the GCC-port drive (vms-da0)
and the corpus-conformance drive produce *trustworthy* results: a corpus test that only
passes by issuing raw syscalls is now a flagged executive gap, not a green checkmark.

## Non-goals (explicit — do not build these here)

- **No blocking / enforcement.** The filter MUST let every syscall proceed unchanged.
  Behavior is identical with the tracer on or off. Blocking is vms-48e, post-1.0.
- **No rerouting** of syscalls through `/dev/vms`. (Not even achievable in Phase A —
  musl's own internals, malloc→mmap/stdio→write, issue raw syscalls that cannot be
  rerouted without forking libc. That is why enforcement is a later, selective phase.)

## Mechanism decision

**Chosen: seccomp `SECCOMP_RET_USER_NOTIF` + `SECCOMP_USER_NOTIF_FLAG_CONTINUE`,
with a small in-process supervisor thread.** Rationale vs alternatives:

| Option | Verdict |
|---|---|
| `SECCOMP_RET_LOG` (kernel logs to dmesg/auditd) | Rejected — coarse, needs `/proc/sys/kernel/seccomp/actions_logged` + root to read auditd, no structured args, hard to assert in a test. |
| `SECCOMP_RET_USER_NOTIF` + CONTINUE | **Chosen** — a supervisor gets a structured `seccomp_notif` per filtered syscall (nr + args + pid), records a finding, then `SECCOMP_USER_NOTIF_FLAG_CONTINUE` lets the kernel run the real syscall. Behavior unchanged; findings are structured and testable in userspace, no auditd. |
| `ptrace(PTRACE_SYSCALL)` supervisor | Rejected for Phase A — heavier (2 stops/syscall), perturbs timing, fights any debugger. |

**CONTINUE TOCTOU note (must be in the code comment):** `..._FLAG_CONTINUE` is documented
as unsafe for security *enforcement* because the args can change between notify and
execute. That is fine here precisely because this is AUDIT, not enforcement — we record
what was attempted and never gate on it. When vms-48e brings enforcement, it will NOT use
CONTINUE; it will resolve the syscall in the supervisor. Do not copy this pattern into an
enforcement path.

## What counts as a VMS-semantic syscall (the classifier)

The BPF program filters (returns USER_NOTIF for) the syscalls that in a faithful system
would trap to the executive. Everything else (pure compute / memory / thread —
mmap/mprotect/brk/futex/rt_sig*/clock_*/nanosleep/exit*) is `SECCOMP_RET_ALLOW` and never
notified, so overhead is near-zero.

Filtered set (seed — refine in review against the actual instances the GCC lane surfaces):
- **process create:** `clone`, `clone3`, `fork`, `vfork`, `execve`, `execveat`
- **file/volume:** `open`, `openat`, `openat2`, `creat`, `rename*`, `unlink*`, `mkdir*`,
  `chmod`/`fchmod*` (the vms-040 instance #3 class), `truncate`
- **network:** `socket`, `socketcall` (32-bit), `connect`, `bind`, `sendto`, `recvfrom`
- **device:** `ioctl`

**The one exemption that makes the whole thing work:** `ioctl` on the `/dev/vms` fd is the
*legitimate* executive path and must NOT be recorded as a bypass. seccomp-BPF cannot
dereference the fd→path, so the classifier cannot exempt by path in the BPF program. Handle
it in the SUPERVISOR: on an `ioctl` notification, read the caller's fd (via
`/proc/<pid>/fd/<n>` readlink, or compare against the `/dev/vms` fd number the supervisor
was told at install time) and DROP the finding if it targets `/dev/vms`. Every other
`ioctl` (and all non-ioctl filtered syscalls) is a finding.

## Install site

`src/imgact/imgact_xfer.c` — immediately before transferring control to the activated
image, after the executive handle (`/dev/vms`) is open, so the supervisor knows the fd to
exempt. The filter is installed with `SECCOMP_FILTER_FLAG_NEW_LISTENER`; the returned
listener fd is handed to the supervisor thread. Gate the whole thing behind an env/knob
(`OVMX_BOUNDARY_AUDIT=1`) so it is opt-in for the drives and CI and off by default until it
is proven — never change default runtime behavior in Phase A.

## Finding format + sink

One finding per recorded syscall, structured (JSON line is fine):
`{image, pid, syscall, key_args, count}` — coalesce repeats by (image,syscall,key_args) with
a count so a hot loop does not drown the log. Write to a path the harness picks up
(`$OVMX_BOUNDARY_AUDIT_LOG`, default a file under the run dir). This plugs into the
existing anti-cheat audit family (`tests/integration/test_runtime_target.sh` and the
`*_census*` tests) as a new **reporting** input — Phase A does NOT fail the build on
findings (that is a later, deliberate ratchet once the runtime path is audit-clean); it
makes them visible.

## Done-condition (must resist mocking — this is an anti-LARP instrument, so its OWN test
must be real)

Against a **real `/dev/vms`** (fail honestly per Rule 9 if absent — do NOT add a userspace
fake of the tracer), all four:

1. **Positive — bypass is caught.** A test image that issues a *raw* `openat` on a
   VMS-volume path (and a raw `socket`, and a raw `clone`) with `OVMX_BOUNDARY_AUDIT=1`
   produces a finding for each. Assert the finding names the syscall and the image.
2. **Negative — legitimate path is clean.** A test image that performs the *same logical
   operations through the executive* (`ioctl` on `/dev/vms`) produces **zero** findings —
   proving the `/dev/vms` exemption works and the tracer is not just logging everything.
3. **Behavior-transparent.** The image's observable result (exit status, output, files
   created) is byte-identical with the tracer on vs off — proving CONTINUE does not alter
   execution.
4. **Negative control on the gate itself** (the census/`negctl` pattern this repo uses):
   a deliberately-planted raw bypass MUST make the audit report non-empty; if the tracer
   is disabled or mis-installed, the negctl test fails — proving the instrument can detect
   a bypass and would not silently pass one.

Width note: seccomp syscall NRs are arch-specific (x86_64 vs aarch64 vs — later — alpha/vax
substrates). Phase A targets the x86_64 runtime; structure the syscall-NR table so a second
arch is a table add, not a rewrite. Do not claim multi-arch until a second arch's table is
proven.

## Scope for the first PR (one session)

Land, in order, stopping wherever the session ends with a *provable* increment:
1. The tracer as a standalone module (BPF program + USER_NOTIF supervisor + finding
   emitter) with a **unit proof** = done-condition items 1+2 driven by a tiny purpose-built
   test binary (not the full IMGACT path). This is the de-risking core.
2. Wire it into `imgact_xfer.c` behind `OVMX_BOUNDARY_AUDIT`, with an **integration proof**
   = a real activated OVMX image's raw syscall shows up as a finding (done-condition 1
   through the actual activation path) + the transparency check (3).
3. The negctl gate (4) + wire the report as a reporting input to the audit family.

Steps 1 is the minimum landable outcome; 2–3 are the same PR if the session has room, else
a fast follow filed as a child.
