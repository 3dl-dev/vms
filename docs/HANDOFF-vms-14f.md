# HANDOFF — vms-14f, the executive-residency dispatch

**Rewritten 2026-08-04, after the round that took `vms-b33` from five blockers to one.** Read §1
first. Everything else is reference §1 sends you to.

Every number below was re-derived on 2026-08-04 by the command named beside it.

---

## 0. What changed since the 2026-08-03 revision

| That revision said | Actually |
|---|---|
| `vms-b33` is blocked by **five** items | **Blocked by one: `vms-d894`.** `vms-c79`, `vms-659`, `vms-35f`, `vms-004e` are closed, each verified by the orchestrator on *both* trees rather than accepted from its implementer. |
| closure = **35 open** | **34 open.** Six closed, five filed. The count is finally roughly flat instead of climbing. |
| "the eighth verdict should not be run until the four gates say what they mean" | The gates now say what they mean, and one of them says it because an execution said so. **The eighth verdict still should not be run** — see §3. |
| `vms-10c` is "transiently incomplete `rd list --json`", flaky ~1 in 4 | **Wrong mechanism, and the old wording is deleted rather than softened.** It is deterministic **cwd-dependence**. §4. |
| `vms-b1f` is "host-sensitive" | **Wrong mechanism, also deleted.** It is trailing-output-size sensitive. Fixed and merged. §4. |
| local ctest 59 tests, 2 expected failures | **63 tests, 1 expected failure.** `vms-008` was never a broken test — ctest was killing a passing one. |

---

## 1. Execution pointer — start here

The objective is **`vms-14f`**: *OVMX runs unmodified VMS software: executive-resident system
facilities, no facades.*

**State (re-derived 2026-08-04):** closure = **34 open** by DAG walk from `vms-14f`. Main is at
`cdef6bd`.

**`vms-b33` is blocked by exactly one item: `vms-d894`**, and there is one PR open against it.

**The single open PR is #74** — `work/vms-d894-execution-sourced-floor`, **35/35 green, mergeable
CLEAN**. It is rebased onto current main *(it was cut before #69 and #66 landed; merging it stale
would have reverted the census linkage basis and the register vocabulary pass — check this before
merging anything cut earlier in a round)*.

**Do this first: merge #74.** Then decide `vms-d894` on the evidence in §3, which is measured and
says the item does **not** close.

### The honest shape of what remains

| Category | Count | Note |
|---|---|---|
| Gate / harness machinery | **19** | Whether the measurement proves what it claims |
| Phase containers & rulings | 6 | `vms-14f`, `vms-b33`, `vms-042`, `vms-150`, `vms-cb5`, `vms-b8d` |
| End-of-epic sweeps | 5 | Cannot start until the phases close |
| **Real VMS defects** | **4** | `vms-b2e`, `vms-a30`, `vms-82a`, `vms-e60` (+`vms-2f8`) |

**Nineteen of thirty-four remaining items are machinery that measures the work rather than the
work.** That is the single most important fact in this document. It is not an accusation — the
facade history is why the tests are load-bearing here — but anyone picking this up should know they
are inheriting a verification program, not a VMS program, unless they deliberately change that.

The four real defects, for scale: `AUTHORIZE.EXE` grants ALL/SYSPRV from `getenv(USER)`; `MAIL`
picks whose mailbox to open from `getenv(VMS_USERNAME)`; the executive returns private lock status
numbers (100/108/116) instead of `ssdef.h` values; OVMX reports two different UICs for the DEFAULT
account. Those are holes in the clone. They are cheap next to the machinery.

---

## 2. What merged this round

Eight PRs. Every one CI-green by SHA before merge.

| Item(s) | PR | What it bought |
|---|---|---|
| `vms-659` (manifest half), `vms-d894` (partial) | #65 | `facility_defects.sh` stops saying PROVEN about a glob match; a count floor, honestly priced as not closing the item |
| `vms-659` (register half) | #66 | Register and its negctl say what they check; **zero controls touched**, every reworded string grep-checked against every `need`/`guard` fragment first |
| `vms-35f`, `vms-004e` | #67 | Citation check reads source text not paths, and reads what the row **says** not that a row exists |
| `vms-c79` | #69 | The census asks the **compiler** which calls it emitted |
| `vms-b1f` | #72 | The QEMU harness verdict is not a pipeline |
| `vms-008` | #73 | ctest stops killing a passing test |
| — | #68, #70 | QEMU job budgets; a wrong "skip" message. Not mine; noted because they moved main under me. |

**Closed with verified reasons:** `vms-c79`, `vms-659`, `vms-35f`, `vms-004e`, `vms-b1f`, `vms-008`.

**Filed:** `vms-871`, `vms-86a`, `vms-c13c` (all wired into `vms-150`).

**Closed as refuted:** PR #71. See §4.

---

## 3. `vms-d894` — the one blocker, and why it does not close

This is the decision the next session inherits. It is measured, not argued.

**What PR #74 builds.** `run_facility_negctl.sh` — the only thing in this program that executes
anything — now emits `tests/qemu/facility_negctl_observed.tsv`: one `RUN` row per defect, one `RED`
row per observed failing assertion. The static gates read it instead of guessing.

**It is live, not theoretical.** The first real record is committed: CI run `30940272725`, job passed
in 15m44s, tree-commit `6ffde90`, positive-control pass, **42 RUN rows and 272 observed failing
assertions**. `FACILITY_NEGCTL_REQUIRE_RECORD` is now `1`.

**What the gate said before and after, same tree:**

```
before:  OBSERVED: of those 25, 0 are PROVEN ABLE TO GO RED and 25 are NAMED ONLY
         NOT MEASURED: no execution record

after:   OBSERVED: of those 25, 25 are PROVEN ABLE TO GO RED and 0 NAMED ONLY.
         PASS: 42 of this manifest's 42 defect(s) are OBSERVED-EXECUTED
```

**"PROVEN" is now spendable only by a QEMU run that happened.** That was `vms-659`'s better option
and it is done.

**Deletion test 1 — delete a defect, leave the record:**

```
FAIL: REFUSING to certify ...: it records defect(s) the manifest [no longer has]   rc=1
```

and the PROVEN population drops back to **0** rather than holding a stale claim. A contradicted
record certifies nothing.

**Deletion test 2 — delete the defect AND its record rows AND lower the floor:**

```
OBSERVED: of those 25, 25 are PROVEN ABLE TO GO RED and 0 NAMED ONLY.
PASS: 41 of this manifest's 41 defect(s) are OBSERVED-EXECUTED
```

**Every record check goes quiet and certifies a smaller manifest as fully proven.** Both files are in
the same commit, the deleter owns both, and a regenerating CI run agrees with them.

**So the item's actual criterion — a floor sourced from something the deleter does not own — is
unmet.** What closes it is comparing against the **base branch's** record, which cannot be faked
without rewriting history. Needs `fetch-depth: 0` and a base-record extraction. Deliberately not
bundled: an untestable CI step that can red the only job which executes anything is the worse trade.

Price of deleting a defect is now a **measured 69 changed lines across 3 files** — measured by the
test, which searches for the cheapest deletable defect rather than asserting a number. It was ~21
lines / 3 files.

**The decision:** close `vms-d894` on the grounds that 69 lines and a visible three-file diff is
enough, or build the base-branch floor. That is a scope call. **Do not close it by asserting the
criterion is met — it is measurably not.**

---

## 4. Method — what this round paid for

The 2026-08-03 block still holds. New:

**THREE DEFECTS THIS ROUND WERE MISDIAGNOSED IN THE SAME WAY: A KILL READS AS A FAILURE.**
`vms-b1f` was filed "host-sensitive" (it is trailing-output-size). `vms-008` was carried as a known
failing test (ctest was killing a passing one). `vms-86a` I watched manufacture a red on my own PR.
A `timeout-minutes` kill, a ctest TIMEOUT and an infra cancellation **all** report
`conclusion: cancelled` or `Timeout` and render in `gh pr checks` as a plain `fail`. **Check
`.conclusion` and whether the assertion step actually ran before attributing any red to the diff.**

**A DISPROOF CAN BE WRONG IN THE DIRECTION THAT COMFORTS YOU.** `ctest --timeout 120` does **not**
override a per-test `TIMEOUT` property. My first `vms-008` disproof therefore passed, which would
have "proved" the timeout was fine. The real disproof needs a tree configured with the old property.
If a disproof tells you the thing you wanted to hear, check the disproof.

**MEASURE ONE ORACLE AT A TIME.** Three PRs' CI running concurrently took the per-facility negctl
job from 27m09s to >60m and it was killed by its own timeout — a red I manufactured by parallelism.
Series: **27m09s idle / 50m39s moderate / >60m killed**. Serialize the long jobs.

**DO NOT GARBAGE-COLLECT A TREE A BACKGROUND JOB IS STANDING IN.** I deleted a worktree under a
running negctl and got 16 "failures", every one carrying `getcwd() failed`. That run was void, not a
result, and it looked exactly like a finding.

**THE OBVIOUS HONEST FIX CAN COLLIDE WITH AN EXISTING DISCRIMINATOR.** PR #71 reworded a FAIL
headline to be truthful and CI refuted it: `F_CITE_TREE_UNRESOLVED="FAIL: the tree cites "` already
existed, so the reword made three distinct failure modes share a prefix. Closed rather than patched.
`vms-c13c` carries the constraint any replacement must satisfy.

**A BRANCH CUT EARLIER IN A ROUND CAN SILENTLY REVERT WHAT LANDED SINCE.** PR #74's branch predated
#69 and #66; `git diff main` showed 448 lines of census work as *removals*. Always read *which files*
differ before merging, not just whether it merges.

---

## 5. Environment

- **Disk is the binding constraint: 3.4 GB free, 98% used.** Build one image at a time. The ~48 GB of
  `vat-env-*` images are **not ours** — do not prune them.
- Local ctest is **63 tests, 1 expected failure**: `vms-2a1` (`test_libvms_protection`, 6 of 8
  subtests need unprivileged user namespaces, restricted by apparmor on 24.04). That is a genuine
  host restriction, not a budget.
- `docker`, not podman. `export PATH="$HOME/.local/bin:$PATH"` for `rd`.
- **`rd` run outside the repo returns a PARTIAL set, not an error.** 289 items from
  `~/projects/vms`, **159** from another directory, stable across runs. This is `vms-10c`'s real
  mechanism and it silently corrupts a regenerated ledger — genuinely-active items get written
  `closed`, with their true status still in column 3. `closed active` is self-contradictory and is
  the tell.
- `run_facility_negctl.sh` still runs in CI only.

---

## 6. What needs a human

- **`vms-d894`**: close on 69-lines-and-visible, or build the base-branch floor. §3.
- **`vms-b8d`**: is remote access next.
- **Whether `vms-1e1` (B) still holds.** It was ruled on the side condition that the citation check
  is machine-checked. `vms-004e` and `vms-35f` were attacks on exactly that check and are now fixed
  and merged, which arguably restores it — but re-affirming is a scope call.
- **Whether to keep paying for the machinery.** 19 of 34. §1.
