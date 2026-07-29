# OVMX Executive Retrofit — dispatch plan (`vms-6b8`)

> **Status:** dispatch-ready decomposition. Parent `vms-6b8`, under the authenticity pillar
> `vms-898`. Root-cause analysis lives in `docs/design-authenticity-roadmap.md` §2.1–2.1.1.
>
> **COLD-START GATE:** this DAG must not be dispatched until PR #1 and PR #2 are merged to `main` —
> see §4 (`vms-pre`). Every item also carries its constraints inline, so items remain executable if
> the docs are missing; the docs carry the *why*.

## 1. The problem in one paragraph

On OpenVMS the **executive** owns system-wide state — logical name tables, the process list, the
device table, event flag clusters, mailboxes — and every process sees the same ones. OVMX behaves
instead as **N independent Linux processes, each privately simulating a whole VMS system.** State
that happens to be file-backed (queue DB, SYSGEN, SYSUAF, accounting, known images) is genuinely
shared and accidentally correct. State VMS keeps in system memory is **per-process fiction that
reports success**: `DEFINE/SYSTEM` succeeds and dies with the process; `SHOW SYSTEM` and `SHOW USERS`
can only ever see one process; `$CREMBX` is a socketpair named in `LNM$PROCESS_TABLE`, so the
canonical VMS IPC pattern cannot work at all.

## 2. Why it happened — this drives the ordering

`vms.ko` **already is** a real, half-built executive:

| Facility | ioctls (`src/kernel/vms_ioctl.h`) | Userspace wired? |
|---|---|---|
| Access modes / privileges | `0x01`–`0x04` | No |
| ASTs | `0x10`–`0x12` | No |
| Event flags **incl. VMS common clusters** | `0x20`–`0x27` | No |
| Lock manager `$ENQ`/`$DEQ` | `0x30+` | **Yes** — `sys_lock.c` via `vms_kif` |
| Logical names, process table, device table, mailboxes | none | — |

**No CI job ever loads `vms.ko`.** `persistent-boot` runs its QEMU script *inside a Docker
container*; there is no kernel-module job (`src/kernel/` is excluded even from static analysis).
Docker has no `/dev/vms`, and CI runs in Docker — so every executive facility that could not be
tested defaulted to a per-process userspace fake that reports success. **The architecture drifted to
fit the test harness.** That is why only `sys_lock.c` was ever wired.

**Consequence for sequencing:** the retrofit cannot start with executive code. Start with the CI job
that loads `vms.ko`, or every increment rots back into the userspace fake — that is the only path CI
exercises.

## 3. Standing constraints (apply to every item below)

1. **One runtime target** (CLAUDE.md Rule 9): real-kernel/QEMU, `vms.ko` as the executive via
   `/dev/vms`. Docker is **not** a runtime. Docker as *build/test tooling* is fine and expected —
   `distro/Dockerfile.bootable`, `src/kernel/Dockerfile`, `tests/qemu/Dockerfile` produce and test
   the real runtime. **Never collapse those two.**
2. **No silent fallback.** If `/dev/vms` is absent, **fail honestly** (`SS$_NOSUCHDEV`, as
   `sys_lock.c` already does). Never fake per-process success. Enforced by
   `tests/integration/test_runtime_target.sh` (arrives with PR #1).
3. **Not done until proven against a real `/dev/vms`.** A userspace unit test that never loads
   `vms.ko` does not close an executive item.
4. **Clean-room** (CLAUDE.md Rule 8) still applies to any VMS structure layout or constant.
5. **Purity** — VMS-authentic values/formats pin to the oracle with operator sign-off; never
   self-certify.

## 4. Cold-start prerequisite (`vms-pre`) — READ FIRST

**The DAG is gated on a human action.** Every item below references artifacts that, as of
2026-07-28, **do not exist on `main`** (verified with `git cat-file -e main:<path>`):

| Artifact | Where it lives |
|---|---|
| `docs/design-executive-retrofit.md` (this file) | PR #2, `worktree-executive-retrofit` |
| `tests/integration/test_runtime_target.sh` | PR #1, `worktree-authenticity-inv1` |
| CLAUDE.md **Rule 9** (one runtime target) | PR #1 |
| roadmap §2.1/§2.1.1 executive analysis | PR #1 (the file exists on `main`, the sections do not) |

An agent dispatched cold from `main` would be told to "obey Rule 9" and find no Rule 9 — then
re-derive the exact conclusion this epic exists to correct: that Docker is a live runtime and a
per-process fallback is acceptable. **`vms-pre` blocks the three DAG entry points until PR #1 and
PR #2 are merged.**

Belt and braces: every item also carries its constraints **inline**, so it is executable even if the
docs are missing. The docs carry the *why* — and an agent without the why will "fix" things back.

## 5. The tree

**Rigor tier: `heavy`** (Pass 0). blast = heavy (kernel + libvms + vmsprocess + vmsdcl + CI, >20
files, >3 packages); reversibility = standard (new ioctl contracts; Phase 4 deletes facilities);
adversarial = **heavy floor** (privileges/access modes are a security surface); coverage modifier
**+1** — no CI job loads `vms.ko`, so every touched path is uncovered. Full rigor: implementers,
reviewers, concurrent veracity adversaries, five sweeps, e2e.

```
vms-6b8  EPIC: executive retrofit                                    rigor: heavy
└── vms-pre   PREREQUISITE (human): merge PR #1 + PR #2 to main      [human]
    ├── PHASE 0 — enabling (hard barrier)
    │   └── vms-e4d   QEMU CI job loads vms.ko                       [implementer/QA, sonnet]
    ├── PHASE 1 — wire ioctls that ALREADY exist in vms.ko  (parallel-safe)
    │   ├── vms-ef1   two processes share a common event flag cluster [implementer, sonnet]
    │   ├── vms-as1   ASTs delivered by the executive                 [implementer, sonnet]
    │   ├── vms-pv1   privileges/access modes enforced by executive   [implementer, sonnet]
    │   ├── vms-vx1   VERACITY: Phase 1 cannot be faked  (concurrent) [veracity-adversary, opus]
    │   ├── vms-rv1   REVIEW: Phase 1 wiring                          [reviewer, sonnet]
    │   └── vms-rv2   SECURITY REVIEW: privilege surface              [sweeper-security, opus]
    ├── PHASE 2 — remove the harness that caused the drift
    │   └── vms-71a   migrate Docker CI jobs; delete Dockerfile       [implementer/QA, sonnet]
    ├── PHASE 3 — extend the executive (biggest tell first)
    │   ├── vms-pt1   executive owns a process table                  [implementer, opus]
    │   ├── vms-ln0   DESIGN RULING: where do LNM tables live?        [designer, opus, gated]
    │   ├── vms-d37   DEFINE/SYSTEM propagates across processes       [implementer, opus]
    │   ├── vms-dv1   executive owns the device table                 [implementer, opus]
    │   ├── vms-mb1   named mailboxes connect unrelated processes     [implementer, opus]
    │   ├── vms-vx2   VERACITY: Phase 3 cannot be faked  (concurrent) [veracity-adversary, opus]
    │   └── vms-rv3   REVIEW: Phase 3 structures                      [reviewer, opus]
    ├── PHASE 4 — retire the fakes
    │   └── vms-fk1   delete per-process fakes; gate forbids return   [implementer, sonnet]
    └── PARENT-LEVEL (depend on all implementation items)
        ├── vms-e2e   E2E: executive holds under a multi-process session [e2e-verification, opus]
        └── vms-sw1..sw5  sweeps: security / bugs / dead-code / antipatterns / test-coverage
```

Consumers unblocked downstream (wired in rd, and each annotated "SUBSTRATE-BLOCKED" so a cold agent
does not mistake them for display work): `vms-853` (A1 SHOW SYSTEM), `vms-46b` (A4 login, via
`vms-d37`), `vms-c17` (SPAWN), `vms-905` (broadcast).

**Independent — dispatch in parallel with anything:** `vms-b9f` (`SHOW DEVICE` prints the host Linux
mount table; INV-4 leak on a first-two-minutes command). Needs no executive.

## 6. Dispatch notes

- **`vms-pre` first, then Phase 0.** `vms-pre` is a HUMAN action (merge PR #1 + PR #2); no agent
  can close it. After that, do not dispatch Phase 1 until `vms-e4d` is green — Phase 1 items
  have no way to prove themselves otherwise, and a "passing" Phase 1 item without it is exactly the
  silent-fake failure mode this epic exists to kill.
- **Phase 1 items are parallel-safe** (three disjoint facilities, separate files) once Phase 0
  lands. Ideal for concurrent worktree dispatch.
- **Phase 3 is mostly serial** — `vms-pt1` first (biggest tell, unblocks the most), and `vms-ln0`
  must be ruled before `vms-d37` is dispatched.
- **`vms-ln0` is operator-gated. RULED — see `docs/design-logical-name-placement.md`.**
  Logical-name translation sits on the hot path of *every file open*; an ioctl per translation is a
  syscall round trip. Measured on the runtime target across 11 independent boots (n=5 trials/boot
  each, not a single run): the **observed range**, which has widened every round it has been
  checked and is not a proven bound, is ~72–83 µs per-boot mean for an executive ioctl round trip
  (a `CHKPRIV` proxy — a measured **lower bound**, not the real translate ioctl's ceiling, §4a)
  against ~0.75–1.01 µs per-boot mean for the in-process four-table translate it would replace —
  a per-boot ratio spanning **75.7×–104.5×** (union of within-boot trial brackets). A file open
  performs a mean of 1.83 translations that would have to reach the executive, giving a DERIVED
  cost-per-mean-open class of **~132–152 µs** — disqualifying on the unaccelerated QEMU runtime
  OVMX runs today (dev and CI); §1.4 records the separate, honestly-caveated projection for an
  accelerated runtime. **Ruling: the executive owns LNM$SYSTEM/GROUP/JOB; userspace reads them
  through a read-only `mmap()` on `/dev/vms`; all mutations go through ioctl. LNM$PROCESS stays
  per-process.** `vms-d37` must be built to that record, not to a per-translation ioctl. See
  `docs/design-logical-name-placement.md` §1.1 and §5 for which of these numbers are measured
  (always an observed range, never a single point estimate) and which are derived or projected —
  do not conflate the three, and do not requote them to more precision than stated there.
- **Model tiers:** kernel/executive design → Opus; wiring and CI harness work → Sonnet; mechanical
  edits → Haiku.
- Each item carries its own done-condition and constraints in rd; `rd show <id>` is authoritative
  over this document if they ever diverge.
