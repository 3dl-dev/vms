# Design Record: P4 — OVMX boots to a DCL prompt on NetBSD-vax under SIMH

> **Status:** DESIGN / DELIVERY PLAN for rd `vms-d59` (P4, the capstone of epic
> `vms-8e8`, "OVMX/NetBSD SYSKRNL — pluggable executive substrate to capture VAX
> as a first-class runtime"). This record is the backing documentation for the
> P4 rd tree: it maps every deliverable to its real rd item, states the
> done-condition each item must hit, and names the risks honestly. **Doc only —
> no code is changed here.**
>
> **Reads on top of** `docs/design-ovmx-netbsd-syskrnl.md` (the SYSKRNL
> feasibility + strategy), `docs/design-netbsd-executive-core.md` (rd `vms-bea` —
> one shared core, two kernel backends), and `docs/runtime-target.md` (Rule 9,
> substrate-neutral). It does not restate them.
>
> **Clean-room (CLAUDE.md Rule 8):** everything below is OVMX's own code cross-
> compiled and cross-tested. The only external surfaces touched are **public,
> documented** NetBSD kernel/libc APIs (`module(9)`, `cdevsw(9)`, `kmutex(9)`,
> `cv(9)`, `copyin(9)`/`copyout(9)`, `kmem(9)`, `rbtree(3)`, `queue(3)`) and the
> SIMH VAX emulator. No VSI/HPE source or binary is read, disassembled, or copied.

---

## 1. Goal, and the framing: foundation done, integration ahead

**Goal (P4, `vms-d59`):** `ovmx_init` → `LOGINOUT` → a **DCL prompt** on
**NetBSD-vax under SIMH**, with the OVMX executive test green against a **real
in-kernel `/dev/vms`** on that VAX — no per-process userspace fakes (INV-6),
no mocks.

The reason P4 is now an **integration** effort and not a research effort is that
every "will this even work?" unknown has already been answered **YES** by merged
work under `vms-8e8`:

| Foundation fact (merged) | rd | Evidence |
|---|---|---|
| Rule 9 generalized to substrate-neutral ("a real host kernel", not "Linux") | — | PR #364 |
| `vms_kif` transport seam split from Linux specifics | — | PR #370 |
| NetBSD/amd64 SYSKRNL: real `vms` pseudo-device, executive test green under QEMU | `vms-dd8` | PR #376 |
| NetBSD/**vax** boots on SIMH as a reusable cached-disk lab artifact | `vms-0041` | PR #379 |
| Shared-core / two-backend abstraction decided (`exec_kbackend.h`) | `vms-bea` | PR #384 |
| Kernel-backend shim + Linux backend landed | — | PR #388 |
| First facility (event flags) extracted to `src/kernel-core/` | — | PR #391 |
| Event flags on **both** kernels + cross-process proof on NetBSD/amd64 | `vms-4b4` | PR #393 |
| `libvmssys` cross-compiles to **elf32-vax**; ILP32 width audit | `vms-9dc` | PR #396 (`docs/audit-ilp32-vax-libvmssys.md`) |

So the hard unknowns — *does NetBSD boot on our SIMH-vax? is the executive
substrate-portable without duplicating facility logic? does `libvmssys` target
VAX at all?* — are all closed. What remains is **breadth** (every facility, not
just event flags), **width** (32-bit VAX, ILP32, non-IEEE float), and
**integration** (a full userspace, an init, and a boot). This record plans that
remaining work.

---

## 2. The two convergent tracks

P4 is orchestrated as **two convergent tracks** that share the same `libvmssys`
VAX base (`vms-9dc`, done) and meet at the boot capstone. This is the natural
2-seat parallelization: one track is kernel/executive-on-VAX; the other is
userspace-cross-build. Neither track can finish P4 alone; the capstone (`vms-d59`)
depends on both.

```
                         libvmssys elf32-vax + ILP32 audit  (vms-9dc, DONE)
                                        |
             ┌──────────────────────────┴───────────────────────────┐
             │                                                        │
   TRACK 1: EXECUTIVE-ON-VAX                            TRACK 2: USERSPACE CROSS-BUILD
   (kernel + facilities)                                (library build order + RTL width)
             │                                                        │
   Exec-core extraction (Linux):                        vms-30a  RTL width (3 ILP32 items)
     vms-5b2  D  ast+lnm+access  ─┐                     vms-84b  vmsprocess
     vms-a88  E  mbx+devtab       │                        │
     vms-846b F  proctab+RCU-lite │  each unblocks →     vms-1b2  libms
     vms-84a  G  lock manager     │                        │
             │                    │                     vms-271  vmslnm+vmsfs+vmsrms
   P4-A (vms-f8a): NetBSD backends + amd64 proofs           │
     vms-9bb  ast+lnm+access                             vms-1cb2 vmsdcl
     vms-d7a  mbx+devtab                                    │
     vms-ca7  proctab (+ proc-model/RCU-lite shim)       vms-5d1  ovmx_init/STARTUP + images
     vms-ff7  lock manager (rbtree+hash DLM)                │
             │                                             │
   P4-B (vms-476): executive on NetBSD-VAX                  │
     vms-20b9 B1 compile shared core+backend elf32-vax     │
     vms-f78bb B2 /dev/vms live (module or in-kernel)      │
     vms-4e7  B3 eflag cross-process vs REAL /dev/vms      │
             │                                             │
             └──────────────────┬──────────────────────────┘
                                │
                   P4-C (vms-c99): full userspace cross-builds
                                │
                    ┌───────────┴───────────┐
                    │   CAPSTONE  vms-d59    │
                    │  ovmx_init→LOGINOUT→   │
                    │  DCL prompt on         │
                    │  NetBSD-vax / SIMH +   │
                    │  gated boot-conformance│
                    └────────────────────────┘
```

Note the ordering fact that keeps the tracks honest: **Track 1's P4-A is gated on
the Linux-side exec-core extraction (Phases D–G, `vms-5b2/a88/846b/84a`)**, not
on VAX at all. You cannot give a facility a NetBSD backend until that facility
lives in `src/kernel-core/` behind the shim. P4-A mirrors the already-proven
event-flag pattern (`vms-4b4`, PR #393) facility-by-facility on **NetBSD/amd64**,
where the emulator is fast; P4-B then recompiles that same, now-complete core for
**elf32-vax** and lights it up on the slow VAX. The width port is deliberately
the *last* kernel step, so a width bug surfaces against known-good behavior.

---

## 3. Per-deliverable plan and done-conditions

Ground rule for every done-condition: **the proof runs against real state, never
a mock.** The amd64 proofs are cross-*process* (two processes, one `/dev/vms`,
shared kernel state — the INV-6 honesty bar from `vms-4b4`). The VAX proofs run
against the **real in-kernel `/dev/vms`** on the SIMH lab disk. The capstone
boots a **real OVMX image** on the **real NetBSD/vax SIMH lab** and asserts a DCL
prompt.

### 3.1 Track 1 · P4-A — every facility has a NetBSD backend (`vms-f8a`)

Extend the event-flag precedent to the whole executive surface. Each sub-item is
blocked by its Linux-side exec-core extraction phase and does two things: (a)
define the NetBSD backend of the shim for that facility's primitives, compiling
the **shared** `src/kernel-core/` logic into the NetBSD `vms` module — **no
reimplementation, no forked facility logic** (`vms-bea`); (b) prove it
cross-process on NetBSD/amd64.

| rd | Facility group | Blocked by (Linux extraction) | Done-condition |
|---|---|---|---|
| `vms-9bb` | ast + lnm + access | `vms-5b2` (Phase D) | Shared ast/lnm/access core compiles into the NetBSD module; amd64 cross-process proof green — two processes observe the same AST delivery / logical-name table / access-mode state through one `/dev/vms`. |
| `vms-d7a` | mailbox + device-table | `vms-a88` (Phase E) | Shared mbx/devtab core in the NetBSD module; amd64 cross-process proof — a mailbox written by P1 is read by P2; device table is shared, not per-process. |
| `vms-ca7` | process table | `vms-846b` (Phase F) | Shared proctab core + the **proc-model / RCU-lite shim** land on NetBSD; amd64 proof — process table entries created by one process are visible to another. |
| `vms-ff7` | lock manager (DLM) | `vms-84a` (Phase G) | Shared lock-manager core (rbtree + hash) compiles against NetBSD `rbtree(3)`; amd64 cross-process **DLM** proof — an ENQ in P1 blocks/grants against P2 through the real kernel lock DB. |

**P4-A done (`vms-f8a`):** all four green — the NetBSD `vms` module exposes the
*complete* executive facility set, every facility proven cross-process on
NetBSD/amd64 against a real `/dev/vms`. This is breadth parity with the Linux
`vms.ko`, still on the fast substrate.

### 3.2 Track 1 · P4-B — the executive builds + loads on NetBSD-VAX (`vms-476`)

Now take the complete, amd64-proven core to 32-bit VAX.

| rd | Step | Done-condition |
|---|---|---|
| `vms-20b9` | B1: compile for elf32-vax | The NetBSD backend + shared executive core (`exec_list`/`kmem`/`cv`/`kmutex` uses, all facility logic) **compile clean for elf32-vax**, width-clean — no 64-bit assumptions, audited against the ILP32 findings (`docs/audit-ilp32-vax-libvmssys.md`). |
| `vms-f78bb` | B2: `/dev/vms` live on VAX | The `vms` executive is **present and live on NetBSD-vax under SIMH**: `/dev/vms` opens. Preferred path is a **loadable `module(9)`**; **fallback** is compiling the driver into a **custom NetBSD/vax kernel** if `modules(9)` on the VAX port is too thin to carry a `cdevsw` module (see §4). Either way the done-condition is identical: a real in-kernel `/dev/vms` node. |
| `vms-4e7` | B3: eflag proof on VAX | **Event flags proven cross-process against the REAL `/dev/vms` on NetBSD-vax under SIMH** — the exact `vms-4b4` proof, re-run on the VAX. INV-6-honest: shared kernel state, no per-process fallback. |

**P4-B done (`vms-476`):** the OVMX executive builds and loads on NetBSD-VAX and
passes its first real cross-process facility test on the VAX. The kernel half of
the runtime exists on VAX.

### 3.3 Track 2 · P4-C — full OVMX userspace cross-builds for netbsd-vax (`vms-c99`)

Cross-build the entire userspace following the library dependency graph (the same
order CLAUDE.md documents), starting from the already-done `libvmssys` VAX base.
Before the graph can climb, three RTL width items deferred by the ILP32 audit must
be resolved.

**RTL width first (`vms-30a`)** — the three deferred ILP32 items:

1. **`vms_time_t` vs 64-bit `time_t`.** NetBSD/vax's `time_t` is 64-bit even on
   ILP32; OVMX's `vms_time_t` and its VMS 64-bit-quadword time semantics must be
   reconciled deliberately, not by accidental truncation. Done: a decided,
   documented width mapping, exercised by a test.
2. **`VMS_O_*` / futex constant namespaces.** The audit flagged Linux-numeric
   constant namespaces (open flags, futex ops) leaking into portable code. Done:
   these are abstracted behind the substrate layer so netbsd-vax gets NetBSD
   numeric values, not Linux ones.
3. **VAX non-IEEE floating point — HIGHEST-RISK RTL item (see §4).** The VAX FPU
   is F/D/G/H format, **not IEEE-754**. Any RTL that assumes IEEE bit layout,
   rounding, or `NaN`/`Inf` semantics is wrong on VAX. Done: the float-touching
   RTL is either VAX-format-correct or explicitly scoped, with a test that pins
   the behavior.

**Then the library build order** (each item = cross-builds + links for
netbsd-vax; done-condition = clean cross-compile + link, no unresolved width
issues):

| rd | Layer | Blocked by |
|---|---|---|
| `vms-84b` | `vmsprocess` (PCBs, ASTs, event flags) | `vms-9dc` |
| `vms-1b2` | `libvms` (system services + RTL) | `vms-30a`, `vms-84b` |
| `vms-271` | `vmslnm` + `vmsfs` + `vmsrms` | `vms-1b2` |
| `vms-1cb2` | `vmsdcl` (DCL shell) | `vms-271`, `vms-84b` |
| `vms-5d1` | `ovmx_init`/STARTUP + the OVMX images (LOGINOUT, DCL.EXE, …) link + activate | `vms-1cb2` |

**P4-C done (`vms-c99`):** the full OVMX userspace — through `ovmx_init` and the
activatable images — cross-builds and links for netbsd-vax. The userspace half of
the runtime exists on VAX.

### 3.4 Capstone — boot to a DCL prompt (`vms-d59`)

With P4-B (executive on VAX) and P4-C (userspace on VAX) both done, integrate:
`ovmx_init` runs as the boot process on NetBSD-vax, activates `LOGINOUT`, and
lands a **DCL prompt** — on the real in-kernel `/dev/vms`.

**Done-condition (ground-sourced, no mocks):** a **gated boot-conformance test**,
the VAX analog of `tests/qemu/test_boot_conformance.sh` / `test_persistent_boot.sh`,
boots a **real OVMX image on the real NetBSD/vax SIMH lab** (`tests/lab-vax/`,
`vms-0041`) and asserts, mechanically on the console transcript, that a **DCL
prompt** is reached (and that the executive test is green against the real
`/dev/vms`). It reuses the cached lab disk — **never reinstalls** (§4). Like the
other emulator tiers it is **CI-gated** (`if: != pull_request`, push/nightly) so
the slow SIMH job never blocks a PR.

`vms-d59` is blocked by `vms-0041` (done), `vms-476`, `vms-9dc` (done), `vms-c99`.

---

## 4. Risks, honestly

1. **VAX non-IEEE float (highest RTL risk).** The VAX has F/D/G/H floating
   formats, not IEEE-754. RTL that assumes IEEE bit-patterns, rounding modes, or
   special values is silently wrong on VAX and can pass a smoke test while
   producing wrong numbers. *Mitigation:* isolate float-touching RTL in `vms-30a`,
   pin behavior with a VAX-format test, and scope explicitly what OVMX guarantees
   in VAX float rather than assuming the host `gcc` gets it right by default.
2. **`modules(9)` on NetBSD/vax may be too thin.** The VAX port is not the place
   NetBSD's loadable-module machinery gets the most love; a `cdevsw` module may
   not load. *Mitigation:* `vms-f78bb` carries an explicit fallback — compile the
   `vms` driver **into a custom NetBSD/vax kernel**. The done-condition (a real
   in-kernel `/dev/vms`) is identical either way, so this is a build-path choice,
   not a redesign.
3. **SIMH-vax is SLOW.** Boot and test wall-clock on emulated VAX is long (the
   `vms-0041` lab boots in ~10 min under TCG-class emulation; a full OVMX boot +
   conformance run is longer). *Mitigation:* keep every fast proof on
   NetBSD/**amd64** (all of P4-A), do the minimum on VAX, and give VAX jobs a hard
   `timeout` wrapper (a QEMU/SIMH proof with no hard timeout has run unbounded
   before). The VAX is the *oracle of last resort*, not the routine loop.
4. **Shared-host disk constraint.** The dev/CI host is a **shared** machine and is
   ~96–98% full (measured: ~3.7 GB free on a 154 GB root). SIMH VAX installs are
   multi-GB. *Mitigation, mandatory for every VAX boot-test item (`vms-f78bb`,
   `vms-4e7`, `vms-d59`):* **reuse the cached `tests/lab-vax` disk, never
   reinstall** in the hot path; installs happen once into a mounted cache exactly
   as `vms-0041` established. And **CI-gate the heavy jobs** the way the other
   emulator tiers are gated (`if: != pull_request`) so they run on push/nightly,
   not on every PR, and never pile multi-GB installs onto a near-full shared host.

---

## 5. Definition of done (P4)

P4 (`vms-d59`) is **done** when **all** of the following hold:

- **P4-A (`vms-f8a`)**: `vms-9bb`, `vms-d7a`, `vms-ca7`, `vms-ff7` all green — every
  executive facility has a NetBSD backend built from the **shared** core (no forked
  facility logic) and is proven cross-process on NetBSD/amd64 against a real
  `/dev/vms`.
- **P4-B (`vms-476`)**: `vms-20b9` (elf32-vax compile) → `vms-f78bb` (`/dev/vms`
  live on VAX) → `vms-4e7` (event flags cross-process vs the REAL VAX `/dev/vms`)
  all green.
- **P4-C (`vms-c99`)**: `vms-30a` (3 RTL width items resolved, incl. VAX float) and
  the full build-order chain `vms-84b → vms-1b2 → vms-271 → vms-1cb2 → vms-5d1`
  cross-build + link for netbsd-vax.
- **Capstone (`vms-d59`)**: `ovmx_init` → `LOGINOUT` → a **DCL prompt** on
  NetBSD-vax under SIMH, and the **gated boot-conformance test** — booting a real
  OVMX image on the real cached SIMH-vax lab disk — asserts the DCL prompt and a
  green executive test against the real in-kernel `/dev/vms`. **No mocks, no
  per-process fallback (INV-6).**

**Final gate:** when the capstone lands, run **`/sweep`** (adversarial sweep —
security, bugs, dead code, antipatterns, test coverage, harness) over the P4
delta before P4 is closed, per release-gate practice.

---

## 6. What P4 does and does not claim

P4 delivers **VAX as a first-class OVMX runtime**: a real host kernel (NetBSD/vax)
exposing the VMS executive through `/dev/vms`, with a full OVMX userspace booting
to DCL on it. It does **not** claim OpenVMS-VAX authenticity of the *host* kernel
(NetBSD is the substrate, exactly as Linux is on x86_64) and it does **not**
weaken Rule 9: `tests/lab-vax` and the SIMH harness remain **build/test tooling**
that prove the runtime model on VAX — they are never themselves a runtime. The
authenticity surface OVMX owns (VMS identity, facility semantics, DCL, RMS) is the
**same shared code** running on the VAX backend, which is the entire point of the
`vms-bea` one-core/two-backends decision.
