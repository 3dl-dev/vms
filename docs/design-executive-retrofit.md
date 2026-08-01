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
2. **The executive is integral — its absence is made unreachable, not handled.** *(Corrected
   2026-07-29; this constraint previously said "if `/dev/vms` is absent, fail honestly with
   `SS$_NOSUCHDEV`, as `sys_lock.c` already does". That rule is **superseded**, and every item
   written under it is wrong on this point.)*

   Apply the governing rule — *what would VMS do?* On OpenVMS, SYSBOOT loads the executive before
   any process exists; **VMS is never in the state where a running system has no executive**. So
   the deliverable is not an error path. It is a guarantee:

   - **Boot is fatal.** `src/ovmx_init/ovmx_init.c` (`executive_attach`) refuses to bring the
     system up unless `vms.ko` loads and `/dev/vms` opens. No image can ever run without the
     executive.
   - **The executive is pinned.** PID 1 holds the `/dev/vms` descriptor for the life of the
     system. `vms.ko`'s `file_operations` carry `.owner = THIS_MODULE`, so that open descriptor
     holds a module reference and `rmmod vms` fails with `EBUSY` while OVMX runs. Mid-life loss is
     **prevented**, not responded to.
   - **Therefore per-call fallbacks are deleted, not corrected.** `sys_lock.c`'s `ensure_kif_open()`
     status return and `vms_kif.c`'s absent-fd guard are gone. Do not reintroduce a status for a
     condition no caller can observe.
   - **`SS$_NOSUCHDEV` keeps its real meaning** — a caller named a VMS device that does not exist.
     Only the "the executive is missing" uses were wrong. Classify each site; never blanket-replace.

   Why the old rule was itself a defect: `SS$_NOSUCHDEV`-on-absence encoded "OVMX runs, minus the
   executive", and no such system exists. A handled-but-impossible-on-VMS state is the same class of
   lie as a per-process fake that reports success — it just fails more politely. Enforced by
   `tests/integration/test_runtime_target.sh`.

   **The fail-stop boot turned out to be VMS-authentic, not an OVMX invention.** Pinned to the
   oracle (`~/vax/cluster`, OpenVMS VAX V7.3, node VAX2, 2026-07-29): renaming
   `SYS$COMMON:[SYS$LDR]EXCEPTION.EXE` aside and cold-booting `B/R5:10000000 DUA0` produces

   ```
   %EXECINIT, error loading system file - EXCEPTION.EXE R0 = 00000910
   ?06 HLT INST
           PC = 871306A6
   >>>
   ```

   and **the machine halts** — no degraded boot, no bugcheck, no crash dump. Capture archived at
   `~/vax/cluster/captures/vax2-execinit-missing-exception-2026-07-29.log`. Four properties are
   reproduced deliberately, because they are the authenticity tells: the facility is **EXECINIT**,
   not SYSBOOT; there is **no severity letter and no mnemonic**; the image is a **bare filename**;
   and the status is printed raw as `R0 = ` + 8 hex digits.

   This also **disproves an earlier wave's `%SYSBOOT-F-LDFAIL`** — the complete VAX 7.3 SYSBOOT
   message set (`HELP/MESSAGE/FACILITY=SYSBOOT`, ~48 entries) contains no such mnemonic. The
   independently useful corroboration is `SYSBOOT-E-I/O error reading file`, whose shipped Help
   Message text states that if the error occurred reading a system loadable image, *"SYSBOOT
   terminates the bootstrap operation"*.

   Two things remain genuine **OVMX design choices**, labelled per CLAUDE.md Rule 8 and never
   presented as VMS-authentic: the `%OVMX-I-EXECINIT` detail line carrying the underlying Linux
   error (VMS prints nothing more — `?06 HLT INST` comes from the VAX console firmware, which OVMX
   has no analogue of), and reporting `/dev/vms` failing to *open* in the same shape without an
   `R0`, since a VMS executive has no device node and VMS is never in that state.

   **Not silently "fixed" here:** the oracle's `R0 = 00000910` decodes via `F$MESSAGE` to
   `%SYSTEM-W-NOSUCHFILE`, while in-tree `ssdef.h` defines `SS$_NOSUCHFILE` as 2696 (0xA88). That
   drift is real and tracked (vms-556 / vms-c90, alongside `SS$_NOSUCHDEV` 2680 vs the oracle's
   2312) and needs **operator sign-off** — a VMS constant is never self-certified. `ovmx_init.c`
   uses the observed value only to reproduce an observed console line, and says so at the
   definition.
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
- **`vms-ln0` is operator-gated.** Logical-name translation sits on the hot path of *every file
  open*; an ioctl per translation is a syscall round trip. Kernel-side with a per-process cache and
  invalidation, or a shared mapping (the `MAP_SHARED` known-image DB is the in-tree precedent)?
  Decide before writing code — it is the one genuine design fork left in the epic.
- **Model tiers:** kernel/executive design → Opus; wiring and CI harness work → Sonnet; mechanical
  edits → Haiku.
- Each item carries its own done-condition and constraints in rd; `rd show <id>` is authoritative
  over this document if they ever diverge.
