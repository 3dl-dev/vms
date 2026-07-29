# OVMX Executive Retrofit — dispatch plan (`vms-6b8`)

> **Status:** dispatch-ready decomposition. Parent `vms-6b8`, under the authenticity pillar
> `vms-898`. Root-cause analysis lives in `docs/design-authenticity-roadmap.md` §2.1–2.1.1.
>
> **Depends on PR #1** (`worktree-authenticity-inv1`) for CLAUDE.md **Project-Specific Rule 9** and
> the roadmap §2.1 analysis. This branch is cut from `main`, so those are not present here yet; this
> document is self-contained and does not require them to be read first.

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

## 4. The tree

```
vms-6b8  EPIC: executive retrofit
├── PHASE 0 — enabling (nothing else starts first)
│   └── vms-e4d   QEMU CI job loads vms.ko and proves executive assertions      [QA]
├── PHASE 1 — wire what already exists (cheap, real, no new kernel design)
│   ├── vms-EF1   two processes share a common event flag cluster              [Systems]
│   ├── vms-AST1  ASTs are delivered by the executive                          [Systems]
│   └── vms-PRV1  privileges/access modes are enforced by the executive        [Systems]
├── PHASE 2 — remove the harness that caused the drift
│   └── vms-71a   migrate Docker CI jobs to musl/QEMU; delete Dockerfile       [QA]
├── PHASE 3 — extend the executive (biggest tell first)
│   ├── vms-PT1   executive owns a process table every process can query       [Systems]
│   ├── vms-LNM0  DESIGN SPIKE: where do logical name tables live?             [Systems, gated]
│   ├── vms-LNM1  DEFINE/SYSTEM propagates across processes  (vms-d37)         [Systems]
│   ├── vms-DEV1  executive owns the device table                              [Systems]
│   └── vms-MBX1  named mailboxes connect two unrelated processes              [Systems]
└── PHASE 4 — retire the fakes
    └── vms-FAKE1 per-process userspace fakes deleted; gate forbids return     [Systems]
```

Consumers unblocked downstream (already wired in rd): `vms-853` (A1 SHOW SYSTEM), `vms-46b` (A4
login, via `vms-d37`), `vms-c17` (SPAWN), `vms-905` (broadcast).

**Independent of this tree — can dispatch immediately, in parallel:** `vms-b9f` (`SHOW DEVICE`
prints the host Linux mount table — an INV-4 leak on a first-two-minutes command). The *leak* fix
needs no executive; the *real* device table is `vms-DEV1`.

## 5. Dispatch notes

- **Phase 0 is a hard barrier.** Do not dispatch Phase 1 until `vms-e4d` is green — Phase 1 items
  have no way to prove themselves otherwise, and a "passing" Phase 1 item without it is exactly the
  silent-fake failure mode this epic exists to kill.
- **Phase 1 items are parallel-safe** (three disjoint facilities, separate files) once Phase 0
  lands. Ideal for concurrent worktree dispatch.
- **Phase 3 is mostly serial** — `vms-PT1` first (biggest tell, unblocks the most), and `vms-LNM0`
  must be ruled before `vms-LNM1` is dispatched.
- **`vms-LNM0` is operator-gated.** Logical-name translation sits on the hot path of *every file
  open*; an ioctl per translation is a syscall round trip. Kernel-side with a per-process cache and
  invalidation, or a shared mapping (the `MAP_SHARED` known-image DB is the in-tree precedent)?
  Decide before writing code — it is the one genuine design fork left in the epic.
- **Model tiers:** kernel/executive design → Opus; wiring and CI harness work → Sonnet; mechanical
  edits → Haiku.
- Each item carries its own done-condition and constraints in rd; `rd show <id>` is authoritative
  over this document if they ever diverge.
