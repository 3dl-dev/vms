# HANDOFF — vms-14f, the executive-residency dispatch

**Originally written 2026-08-01. Rewritten 2026-08-02, and rewritten again 2026-08-03 after the round
that produced the SEVENTH Phase 2 verdict.** Read §1 first. Everything else is reference §1 sends you
to.

Every number below was re-derived on 2026-08-03 by the command named beside it. Where a figure comes
from a run I did not repeat, it says so.

---

## 0. What changed since the 2026-08-02 revision — read this before trusting anything you remember

| The 2026-08-02 revision said | Actually |
|---|---|
| main is at `eff4fe3` | **main is at `f04b3d6`**, attesting product tip **`8e8be98`**. It travelled `eff4fe3` → `a4ead89` (#48) → eight more merges → `8e8be98`. Re-derive it; do not quote this line back later. |
| "`vms-cb5` — **PR #48 OPEN**… Verify CI, then merge" — the document's single named loose end | **#48 was merged at `2026-08-02T02:42:42Z`.** That revision was committed at `2026-08-02T02:30:05Z`. **The only loose end it named was gone twelve minutes after it was written**, and every agent that read it started by chasing something that did not exist. |
| closure = **23 open** by DAG walk from `vms-14f` | **35 open**, measured today by the same walk (`rd list --json`, transitive `blocked_by`, no cycles). Two of the old 23 closed with #48 (`vms-f39`, `vms-f42d`); **fourteen** members have been filed since that revision was committed. |
| "`vms-b33` is UNBLOCKED and `active`. The fifth Phase 2 verdict is the next thing to run." | Runs five, six and seven have all been run and all returned **NO-GO**. `vms-b33` is **`blocked` again**, by five items that did not exist when that sentence was written. |
| `libssh-dev` is **absent**, local ctest is 39/41, and the `vms-ecf` gate honestly refuses on this host | **`libssh-dev` 0.10.6-2ubuntu0.4 is installed.** `src/vmsssh` configures, the build-derived gates can compile every product source, and that refusal is gone. Local ctest is **59 tests** on today's main. |

### And a constraint on HOW this round could be run, which shaped everything

The guard hooks around worktrees are now tight enough to change the dispatch pattern, and a session
that does not know this will burn a cycle discovering it:

- **An agent isolated in an `agent-*` worktree cannot touch the shared checkout.** Measured here: a
  command whose `cd` lands in `/home/baron/projects/vms` before running `git` is refused outright,
  and so is any command the hook cannot statically prove stays inside the worktree — pipelines,
  `for` loops, `$(…)` substitution and `>` redirects inside a compound command all get refused. Every
  `rd` call has to be issued plain and separately. Budget for it; it roughly doubles the tool calls in
  any measurement pass.
- **Reported but NOT verified by me, so treat as a lead rather than a fact:** that a session whose cwd
  lands in a *non*-`agent-*` worktree is blocked for all tool use, and that non-isolated subagents
  inherit that cwd and inherit the block. I could not test either from inside an isolated worktree. If
  you hit it, that is what it is.

The practical consequence for orchestration: measurement and merging must happen in the shared
checkout by a non-isolated session, and implementation must happen in isolated worktrees. Do not plan
a round that needs one agent to do both.

---

## 1. Execution pointer — start here

The objective is **`vms-14f`**: *OVMX runs unmodified VMS software: executive-resident system
facilities, no facades.*

**State (re-derived 2026-08-03):** closure = **35 open** by DAG walk from `vms-14f`, counting the epic
itself, no cycles. Main is at **`f04b3d6`** (product `8e8be98`).

**`vms-b33` is `blocked`, and by exactly five items** — every one of them a *two-edit escape from a
static gate*, every one filed on 2026-08-03 by the seventh verdict's adversary:

| Blocker | Pri | What it buys, for two edits |
|---|---|---|
| `vms-c79` | p1 | `if (0) { (void)vms_kif_chkpriv(0); }` inside a function that IS reached → census `44/32/12` PASS |
| `vms-004e` | p1 | cite an id that has never existed, plus the row the generator itself emits → census, register and freshness all rc=0 |
| `vms-659` | p1 | a suite whose whole body is two `printf`s joins the "PROVEN able to go red" set |
| `vms-d894` | p2 | delete a control for 20 lines across 2 files; the gate prints a smaller number as a PASS |
| `vms-35f` | p1 | a **directory** named `vms-<alnum>` satisfies the floor whose entire job is that you cannot get there by deletion |

**The next thing to run is not an eighth verdict.** Seven have been run; all seven returned NO-GO; in
all seven **every facility attacked survived**. The blocker has been *the gate the verdict rests on*
since run 3. Run 7 finally said what that means (§3), and the fix is a design call, not another
measurement pass. Fix the four gates' vocabulary and the five escapes first.

`vms-150` (Phase 3 veracity) is blocked by nine open items: `vms-a30`, `vms-95f`, `vms-38c`,
`vms-d33`, `vms-05e7`, `vms-ed8`, `vms-41b`, `vms-35f`, `vms-10c`.

---

## 2. What this objective actually delivers, and what it does not

**The epic's title overpromises relative to its closure. Say so out loud rather than discovering it at
the end.** Nothing this round changed that, and the honest limit got sharper, not softer.

What the closure delivers is the *substrate*: a VMS executive that really is the executive. Process
table, device table, event flags, identity and privileges, terminal characteristics and the lock
manager live in `vms.ko` and are proven **cross-process** — A writes, B reads — because that is the
only test a per-process fake fails. PID 1 refuses to boot without it; the per-call "executive absent"
fallbacks were **deleted rather than corrected**, so there is no degraded mode to drift back into.

What it does **not** deliver:

- **Running unmodified VMS images.** That is the toolchain and image-activation work — `vms-ade`,
  `vms-913` — a different epic.
- **Remote access.** SSH is excluded from Phase 3. `vms-b8d` exists to *decide* whether it is next.

**THE LIMIT THAT MUST STAY VISIBLE, and it is now the item's own clause text.** On 2026-08-02 the
operator ruled `vms-1e1` **(B)**: `vms-b33`'s *closing condition* governs, not its *clause*. The item
had carried two non-equivalent tests — the clause quantified over EVERY facility, the closing
condition over every WIRED facility — and with **13 of 44 entry points unwired they answer
oppositely**, so under the clause reading `vms-b33` could never close. The amended clause is:

> Every facility `vms.ko` claims AND THAT THE PRODUCT REACHES is exercised from userspace through
> `vms_kif` against a REAL `/dev/vms`, proven cross-process (A-writes/B-reads), with a negative
> control proving each gate can go red. Every entry point the product does NOT reach must be honestly
> DECLARED, and its cited item must EXIST and be OPEN — machine-checked, not shape-checked.

Three things about that ruling you must carry:

1. **13 of 44 entry points still have no product path.** Today's census on main is `44 / 31 / 13`
   with floors `33/33` opcodes and `5/5` selectors (printed in `8e8be98`'s own commit body).
   **Phase 2 closing means what is wired is PROVEN, not that the executive is complete.** Anyone
   quoting a Phase 2 GO as "the executive is complete" is misquoting it. The four unwired families —
   `vms-pv1`, `vms-as1`, `vms-dv1`, `vms-a86` — gate Phase 3 (`vms-042`), not Phase 2.
2. **(B) is only in force while the citation check is machine-checked**, which is `vms-8cc`. That was
   the binding side condition, and it landed this round (§3). But `vms-004e` and `vms-35f` are both
   attacks on exactly that check, so the side condition is currently **weaker than the ruling
   assumes**. That is a live consideration for whether (B) still holds, not a settled one.
3. **The undo is one command:** `rd dep add vms-b33 vms-pv1` (and `vms-as1`, `vms-dv1`, `vms-a86`),
   then restore the clause text from `vms-1e1`'s history. That returns reading (A) and re-blocks
   `vms-b33`.

---

## 3. What merged this round, and the seventh verdict

### The merges

Eight PRs closing `vms-b33` blockers landed between 2026-08-02T17:15Z and 2026-08-03T22:56Z. **Every
SHA below was checked against `gh api repos/3dl-dev/vms/commits`**, and every one shows `success` on
both **Build & Test** and **Kernel Executive — Per-Facility Negative Controls (attribution)**.

| Item(s) | PR | SHA on main | What it bought |
|---|---|---|---|
| `vms-279` | #49 | `ca193e4` | Floors the negative-control manifest against its own deletion. Before: 37 of 42 defects deletable individually, **32 of 42 in one edit** by exact set-cover, with the coverage drop printed only as a NOTE. Now anchored in the suite sources. |
| `vms-e2b` | #50 | `2d80e3f` | Census reachability derived from `compile_commands.json` and read **after preprocessing**. All six measured buys closed — including the 13-additive-edit file of fabricated callers that was in no CMakeLists and `#include`d by nothing. |
| `vms-c19` | #51 | `f5a321e` | Register universe = the **build union glob**, not where the files sit; parenthesized declarators bind. |
| `vms-8cc` | #52 | `47a6678` | A cited rd id must **exist and be open**, not merely be well-formed. Mechanism, shaped by rd being nostr-backed and unreachable from CI: a committed derived ledger `tracking/rd-citations.tsv`, a sourceable checker CI reads, and a host-side freshness test. |
| `vms-c13` | #55 | `35896b5` | A call counts only if its **enclosing function is reached** — a product call graph over the preprocessed build set, roots = `main()` plus every function prototyped in a compiled header. All five dead-function buys went rc=0 → rc=1. Pristine unchanged at `44/31/13`. |
| `vms-fab` | #54 | `8552c29` | The register's 88 declarations went from **9 cited ids — 7 CLOSED, carrying 85 sites — to 20 ids, all OPEN**. 33 sites to 12 existing live owners, 52 to 6 new narrow ones, **zero resolved by deleting a declaration**. |
| `vms-a85` | — | inside `8552c29` | Two census controls had gone **vacuous** because `vms-fab` removed the last closed citation in the tree; control 34 was certifying the evasion it existed to catch. **No separate SHA on main** — fixed as `a42a3d9` on `work/vms-fab-citations` and squashed into #54. |
| `vms-27e` `vms-d79` `vms-d98` | #53 | `c73726a` | Ledger hardening. The freshness test no longer routes through the generator it validates; the generator's `ABSENT:`/`CLOSED:` stderr, which used to be piped into the report **unread**, is now parsed and reds on contradiction; the checker derives and prints its own floor by independently rescanning `src/` and `tools/`. |
| `vms-32e` | #57 | `8e8be98` | The register checks its citations too — `49 → 62` controls, coverage `27 → 39`. |

`8e8be98` is green on **all 35 checks**. Its printed cardinals, which are today's baseline: register
`88 services / 21 exec / 60 state / 67 USERSPACE / 14 PARTIAL / 7 EXECUTIVE / 87 protos / 88 exported
/ 0 symonly`, citations `88 sites, 20 ids, 20 open, 0 closed`; register negctl `62 controls, coverage
39/39`; census `44/31/13`; census negctl `43 controls`; `rd_citations_fresh` `22 rows, 22 open, 16
self-controls`.

*(Main also moved through four SCS-layer merges — #56 `d9b69cc`, #58 `62aff14`, #59 `6a5674a`, #60
`31d497e`. Those belong to `vms-187`, a different dispatch. They matter here only because they change
the test count under you; see §5.)*

### The seventh verdict: NO-GO

Measured by an independent adversary on `d9b69cc` and on PR head `3efb610`. **Five escapes, every one
at two edits** — they are `vms-c79`, `vms-004e`, `vms-659`, `vms-35f`, `vms-d894`, tabulated in §1.
Plus:

- **`vms-38c`, unchanged at two edits.** Its paying call is now best written
  `if (0) { (void)sys$wflor(0u,0u); }` — i.e. the `vms-c79` trick applied to the register.
- **`vms-41b`, RE-PRICED CHEAPER at two edits across two files — and it links clean.** It was filed
  earlier on 2026-08-03 as `vms-c13`'s own disclosed residual, at *three* edits across three files. It
  got cheaper on measurement, not more expensive.

**The sentence these four gates have actually earned, and the only one a verdict may use — quoted
verbatim:**

> they price evasions in the low single digits of edits and print the price; they do not execute
> anything, and every PROVEN / every / cannot line in their output is a string relation over
> declarations the tree makes about itself.

Three items reached that independently — `vms-d33`, `vms-38c`, `vms-41b` — *before* the adversary
demonstrated it. That convergence is the reason to believe it rather than argue with it.

---

## 4. What must NOT be lost in the fix

Read this before touching any of the four gates. The seventh verdict is a narrow finding and it is
very easy to over-correct into deleting something that works.

**`tests/qemu/run_facility_negctl.sh` genuinely does the thing.** It injects every defect in the
42-entry manifest one at a time, rebuilds `vms.ko` and the suites inside the container, boots QEMU
against a real `/dev/vms`, and asserts the complete red set is **EXACTLY** the set the manifest names
— no missing member and no extra member — with a pristine positive control run first, so a harness
that fails indiscriminately cannot pass it. **That CI job was `success` on every one of the eight
merge SHAs in §3**, checked individually. It is the one instrument in this program that executes
anything.

**The defect is that the STATIC gates borrowed its vocabulary.** The census, the register, the
manifest selftest and the freshness test say "PROVEN", "every" and "cannot" about relations they
compute over declarations. Saying **"declared"** and **"anchored"** where they mean declared and
anchored is a one-line honesty fix per gate and **should not wait for runtime attribution**. Do it
first; it costs nothing and it makes every later claim true.

**THE TRAP, named for the next round: do not special-case `if (0)`.** The next form is
`if (never_true_global)`, and after that `if (argc < 0)`. **Enumerating syntactic forms is the losing
side of this game.** `vms-c13` already showed the winning side once — it replaced "is there a call
site" with "is the enclosing function reached", a *semantic* question — and `vms-c79` exists only
because that call graph still credits a branch the compiler deletes. The answer is a reachability
question the compiler can already answer, or the question moves to runtime. It is not a bigger regex.

---

## 5. Method — the part that transfers

The most valuable thing in this document. Each was paid for. The first block is from earlier rounds
and still holds; the second was earned on 2026-08-03.

**Two legal answers, never three** (Rule 10). Match VMS, or make the condition unreachable. The
illegal third — a reasonable-looking handler for a condition VMS never faces — is the defect behind
nearly every facade found here. **It always looks like diligence.**

**The decisive test for a facility is A-writes / B-reads** (Rule 11). A per-process fake passes every
single-process test perfectly.

**Delete emphatic claims; do not correct them.** Every round that deleted ended clean; every round
that reworded shipped a new false claim. This covers runtime output — a sentence OVMX prints is a
claim. **`vms-32e` and `vms-c13` both deleted a false "shared with the userspace service register"
sentence this round; neither rewrote it.**

**Never write a "cannot"/"only"/"never"/"every" you have not tried to break by execution.**

**A green result only means something if the mutation would otherwise have changed behaviour.**

**A cardinal must be derived-and-printed or absent.**

**Run `nm` on the built artifact.** A facade is invisible in source review and obvious in the symbol
table.

### Earned 2026-08-03

**A GREEN ON A STALE BASE IS NOT EVIDENCE, and this was measured twice.** PR #53 was `35/35` green
while sitting on base `44870ac`, which predates #55. **I re-derived the gap by configuring both trees:
`44870ac` enumerates 56 ctest tests; `d9b69cc`, the tree it was about to land in, enumerates 58.** A
branch can be green on a suite that is missing the very tests written to catch it. The rule: before
merging, `update-branch` and re-run — and when a queue has a required order (#55 → #54 → #53 here), a
green earned out of order proves nothing about the merged tree.

**A CONTROL CAN GO VACUOUS WHEN THE TREE CHANGES UNDER IT, and the *correct* fix is what disarms it.**
`vms-fab` did exactly the right thing — repoint 85 declaration sites off closed items — and thereby
broke five register controls keyed on the literal `(vms-5b4)` (the seds became no-ops, so the
mutations never landed and the expected red never fired) and two census controls keyed on `vms-fb9` as
their closed-item exemplar. **The controls were keyed on a fact that is SUPPOSED to change.** Both
were fixed by changing the controls' **AIM**, never by touching an assertion; both now **synthesize
their fixture inside the sandbox** against a synthetic id no product file cites.

**AND SEVEN MORE CAN REDDEN PARTLY ON THEIR OWN FIXTURE WITHOUT ANYONE NOTICING.** When `vms-32e`
wired the register to the citation checker, nine fictional-service controls citing the closed
`vms-5b4` were expected to break. The two GREEN ones did. **The seven RED ones did not fail** —
`expect_red` required only that their named fragment appear somewhere in the output, so each was
reddening *partly on its own fixture*, one edit away from reddening *only* on it. The remedy is a
**forbidden-fragment list**: a control that reds for a second, unrelated reason must fail, not pass
quietly.

**PROVE NON-VACUITY BY DISPROOF, SHOWING EXACTLY ONE CONTROL FAIL.** The pattern that worked four
times in `vms-32e` and again in `vms-a85`: delete the mechanism the control exists to exercise, run
the whole suite, and show the failure count is **one**, and that the one is the control in question.
"62 passed" proves nothing by itself; "delete the 10-line `_cs_dups` block → 61 passed, 1 FAILED,
exactly the duplicate-row control" proves the control is load-bearing *and* that nothing else was
quietly depending on it.

**UNDER-FIRING IS A FINDING, NOT A FAILURE.** `vms-c13`'s first design cut made address-taken
functions **roots**, and was bought at the same two edits by a dead table. Making file-scope objects
*nodes* instead fixed that — but the same change initially **redded on correct code**, a function
reached through a live callback table. A GREEN control was added to bound the over-firing. Test a new
disqualifier for over-firing as hard as for under-firing; a gate that reds on correct code is the one
the next person weakens.

**VERIFY AGAINST THE CHEAPER ORACLE BEFORE BRIEFING — INCLUDING WHEN THE ORACLE IS YOUR OWN MERGE
QUEUE.** Recorded because it is mine. I briefed the run-7 adversary describing the register as already
checking its citations. **Those numbers were PR #57's, not main's — #57 was still open.** The
adversary caught the mismatch itself and measured *both* trees. An orchestrator running a merge queue
holds a picture of the tree that is ahead of the tree, and it does not announce itself.

**GO TO THE LAB BEFORE DECLARING SOMETHING UNPINNABLE.** Escalate what only the operator knows — not
what you can measure.

---

## 6. Environment and harness facts — read before running anything

**Disk is the binding constraint on this host and it is tight.** Measured 2026-08-03: `/dev/sda1`,
154 G total, **8.0 G available, 95% used**. Build one image at a time, `docker rmi` immediately, and
prune *dangling* layers only. This round was run under a standing "no builds, no container images"
constraint for exactly this reason.

**Do not reclaim space by deleting the big images — they are not ours.** `docker images` shows nine
tags `192.168.2.43:30500/vat-env-v49` … `v57`, 9.3–9.71 GB each as reported (that column double-counts
shared layers, so the true footprint is smaller than the naive ~86 GB sum). Nothing in OVMX builds or
consumes them. The OVMX-owned images on this host are `ovmx-test:latest` (126 MB),
`192.168.2.43:30500/ovmx-vaxlab:2` and `:3` (201 MB each) and `simh-build:24.04` (595 MB).

**`libssh-dev` is now installed** (0.10.6-2ubuntu0.4). `CMakeLists.txt:379-385` adds `src/vmsssh` when
`libssh/libssh.h` is found, so every product source compiles and the build-derived gates no longer
refuse on this host — that was the `vms-ecf` "BROKEN SYMBOL SCAN: 126 of 127 product source file(s)
compiled" refusal the previous revision documented. Corollary worth knowing: `tests/vmsssh`
contributes only the `term_mapping` unit test, and there is **no `vmsssh_integration` ctest in this
tree**, so the old "39 vs 41" arithmetic is dead and quoting it will mislead you.

**Local ctest is 59 tests on today's main.** Measured by configure-only enumeration
(`cmake -S <tree> -B /tmp/… -DBUILD_TESTS=ON -DBUILD_TOOLS=ON`, then `ctest -N`; no build, no
container, temp trees deleted). Counts at four points, all measured the same way:

```
44870ac  56 tests   <- the stale base PR #53 was green on
8552c29  56 tests
d9b69cc  58 tests   <- the tree run 7 reported against
8e8be98  59 tests   <- today's main (#60 added one)
```

**Two ctest failures are expected, and both are host-shaped rather than regressions.** Run 7 reported
`58 tests, 2 failed, rc=8` on its merged tree. **I did not re-run ctest this round** (no builds), so
that pair is carried from run 7 and not re-measured. Both tests do exist in today's enumeration:

- `terminal_identity_negctl` (#12) — ctest `TIMEOUT 120` against roughly 138 s actual. **`vms-008`.**
- `test_libvms_protection` (#35) — 6 of 8 subtests need unprivileged user namespaces, restricted on
  this host. **`vms-2a1`.**

**`test_rd_citations_fresh` is flaky about 1 in 4, and it SKIPS in CI.** Cause: `gen_rd_citations.py`
derives open-ness from `rd list --json`, which is **transiently incomplete** — run 7 saw 167 items
from one cwd and 282 from another seconds apart. **`vms-10c`**, and it is a correctness defect in the
ledger generator, not merely an unstable gate. *I did not reproduce it:* two consecutive
`rd list --json` dumps here both returned 288 items with identical id sets, which is what a 1-in-4
flake looks like when you get lucky. The skip is deliberate and honest —
`tests/integration/test_rd_citations_fresh.sh:407-427` exits **77** when `rd` is absent so ctest
reports Skipped and never Passed — but **under rule 7 a skipped test is a failing test**, and CI is
precisely where it always skips. That is the real ceiling on the whole citation mechanism, and the
merge commits say so rather than claiming otherwise.

**What I did verify about the ledger, by hand, standing in for the test CI cannot run:** all 22 rows
of `tracking/rd-citations.tsv` read `open`, and all 22 ids are present in rd's live open set. No row
is stale today.

**CI `Build & Test` needed `timeout-minutes` 20 → 35** (`.github/workflows/ci.yml:20`, with the reason
in the comment directly above it). `userspace_service_register_negctl` went **504 s → 692 s (+37%)**
as `vms-32e` took the register negctl from 49 to 62 controls and added a per-control citation check;
`terminal_identity_negctl` held within 1% across the same two runs, so the runner was not the
variable. Main's job runs about 17 minutes and the 20 m cap left under three minutes of margin — it
was killed mid-ctest with **no assertion failure**, which is the failure mode to recognise before you
go hunting for a broken test.

**The per-facility negative control job is separate and slower:**
`kernel-executive-facility-negative-controls`, `timeout-minutes: 60`, running
`tests/qemu/run_facility_negctl.sh` (`.github/workflows/ci.yml:586-595`). The manifest it drives holds
**42 defects** (`tests/qemu/facility_defects.sh:255`).

**Reproduce with `docker`, not `podman`, and use your own image tag** — parallel agents race a shared
one. The negative control is the same build with `--build-arg NEGATIVE_CONTROL=1`.

**`vms-b1f` still stands and still matters:** `tests/qemu/run_tests.sh` decides its verdict with
`echo "$OUTPUT" | grep -q …` under `set -o pipefail`; past one pipe buffer of trailing output, `grep`
exits first, `echo` takes SIGPIPE, and the harness prints "KERNEL MODULE TESTS FAILED" and exits 1 on
a run with **zero failures**. It is host-sensitive — CI on main exits 0. **Judge local runs by the
`FINAL RESULTS` line and the SUITE banners.** It also makes `run_facility_negctl.sh` refuse to run on
workshop at all, because that driver's first step is a pristine positive control which exits 1 through
this bug. **The driver is behaving correctly** — refusing to certify when its own control fails is the
property `vms-ecf` added on purpose. Do not loosen it to work around a harness bug. CI is that job's
only home.

---

## 7. Open items filed this round

Seven were filed on 2026-08-03 and wired into the `vms-14f` closure. All seven are open and `inbox`.

| Item | Pri | Blocks | What it is |
|---|---|---|---|
| `vms-c79` | p1 | `vms-b33` | The census counts a branch the compiler deletes. Preprocessing kills `#if 0`; it does not kill `if (0)`. |
| `vms-004e` | p1 | `vms-b33` | The citation completeness loop asks whether an id has a **row**, never what the row **says**. |
| `vms-659` | p1 | `vms-b33` | `facility_defects`' "PROVEN able to go red" set is a **glob match against the manifest's own attribution claim**; neither coverage nor selftest runs anything. |
| `vms-35f` | p1 | `vms-b33`, `vms-150` | `rd_cite_scan_tree` greps with `-Hn`, so a **directory name** matching `vms-<alnum>` fabricates a citation and satisfies floor 1. *The item's own original "fails safe" disclosure was wrong and has been deleted rather than corrected.* |
| `vms-d894` | p2 | `vms-b33` | Nothing floors the **size** of the defect manifest. Deleting a control costs 20 lines across 2 files and prints a smaller number as a PASS. |
| `vms-41b` | p2 | `vms-150` | Prototype a dead function in a compiled header and the census makes it a **root**. Filed at 3 edits as `vms-c13`'s disclosed residual; **re-priced this round at 2 edits / 2 files, and it links clean.** |
| `vms-10c` | p1 | `vms-150` | `gen_rd_citations` derives open-ness from a transiently incomplete `rd list --json`. §6. |

**Two more were filed on 2026-08-03 and are deliberately NOT in this closure** — they came out of the
`vms-187` SCS dispatch, and are recorded here only so nobody re-files them: `vms-819d` (p2, parent
`vms-898`: the authenticity negctl gates take 5+ minutes each and `ctest --timeout` does not reap
them) and `vms-2c6` (p1, unwired: QEMU CI harnesses exit nonzero while their own accounting reports
everything passed — the `vms-b1f` shape, seen from another dispatch).

**One cosmetic note:** `vms-35f` carries a malformed priority field — rd reports it as `1`, not `p1`.
Harmless, but it sorts oddly in any `--json` consumer, so do not read it as a distinct tier.

---

## 8. What still needs a human

- **`vms-b8d`** — the ruling on whether remote access is buildable faithfully yet. Typed `decision`;
  it escalates rather than deciding. An agent must not self-approve it.
- **`vms-c9e`** — a GitHub branch-protection admin action no agent can take.
- **Whether `vms-1e1` (B) still holds.** It was ruled on the binding side condition that the
  `OVMX-UNWIRED` citation is machine-checked. `vms-004e` and `vms-35f` are attacks on precisely that
  check. Re-affirming (B), or invoking the one-command undo in §2, is a scope call and not an agent's.
- **Any VMS constant or message value** not settleable at the lab. Pin it to the oracle
  (`/data/training/vax`, or public VSI docs) or escalate. **Never self-certify. Green CI is not
  evidence of VMS correctness.**

---

## 9. Honest accounting

The closure was 17 open on 2026-08-01, 23 on 2026-08-02, and is **35 today**. Eight PRs merged this
round, every one CI-green by SHA, and **the number still went up by twelve.**

**That is the honest outcome, and it is the same shape as the last two rounds.** Every merge in §3
closed a real, measured, reproduced purchase of a gate — the manifest's set-cover deletion, the
census's fabricated-caller file, the shape-only citation, the source-spelling universe, the dead
function's call site, the 85 declarations citing closed items. The round's adversary then bought each
surviving gate for two edits, on a tree where all of that had already landed.

**What moved is not the count. It is what the gates are allowed to say.** Before this round the four
static gates printed "PROVEN" and "cannot" about string relations. After it, three items reached
independently — and an adversary demonstrated — that **they price evasions and print the price, and
nothing more.** That sentence is now the ceiling on what a Phase 2 verdict may claim, and it is a
smaller true claim replacing a larger false one. **A smaller number that is true is the deliverable
here.**

Two things are worth stating plainly against the temptation to read seven NO-GOs as stalling:

1. **Every facility attacked has survived, across all seven runs.** Process table, lock manager, event
   flags, device table, identity, terminal characteristics — each was attacked by injecting a
   per-process facade **into the kernel** that keeps shared storage, shared lock and every write and
   kills only cross-process resolution, and each reddened named assertions at two or three layers. The
   substrate is real. The blocker has never been the substrate.
2. **`run_facility_negctl.sh` is real and was green on every merge.** The thing that executes works.
   What got refuted is the thing that only reads.

The eighth verdict should not be run until the four gates say what they mean and the five two-edit
escapes are closed. Running it now would produce an eighth NO-GO for the seventh reason, and the
record already contains that.
