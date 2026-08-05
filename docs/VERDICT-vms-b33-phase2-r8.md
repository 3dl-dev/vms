# PHASE 2 VERDICT, EIGHTH RUN — `vms-b33`: **NO-GO**

**Run 2026-08-05 by the vms-b33 verdict session.** Every figure below is command output taken this
session, pinned to a SHA. Re-derive it; main moved twice while this was being written.

> **The one-line result.** The half of the clause that needed *execution* is **satisfied, by
> execution**. The half that needed a *machine-check* is **not**: the machine-check runs in CI
> against a committed ledger that presently contradicts `rd`, and it does not merely miss the
> disagreement — **it prints `PASS` over it.** One blocker, not five.

---

## 0. What is being judged

`vms-b33`'s closing condition, as written on the item:

> all children closed, and the Phase 2 veracity adversary has shown — with command output, not
> assertion — that a deliberately injected defect in each wired facility turns CI red.

and its operative clause as amended 2026-08-02 under the `vms-1e1` ruling (B):

> Every facility `vms.ko` claims **and that the product reaches** is exercised from userspace through
> `vms_kif` against a REAL `/dev/vms`, proven cross-process (A-writes/B-reads), with a negative
> control proving each gate can go red. **Every entry point the product does NOT reach must be
> honestly DECLARED, and its cited item must EXIST and be OPEN — machine-checked, not shape-checked.**

Two halves. They are judged separately below because they answer differently.

### SHAs this verdict is pinned to

| Thing | SHA | Note |
|---|---|---|
| Executive / negative-control evidence | **`0830b59`** | CI run `31023091917`, job green |
| Citation evidence | **`cf4ce33`** | current `origin/main` at time of measurement |
| Intervening merge | `dc10e1b` (#85, `vms-187` SCA) | **executive paths untouched** — see §4 |

---

## 1. Blockers: all 29 closed

Verified by DAG walk from `vms-14f`, resolving each blocker's status individually rather than
trusting the item's own summary:

```
vms-fa2 done   vms-9fc done   vms-1d9 done   vms-f42 done   vms-556 done   vms-e7d done
vms-f1f done   vms-2a8 done   vms-7fb done   vms-290 done   vms-2e5 done   vms-5b4 done
vms-0e4 done   vms-d89 done   vms-413 done   vms-ecf done   vms-f26 done   vms-e2b done
vms-279 done   vms-8cc done   vms-1e1 done   vms-32e done   vms-c13 done   vms-27e done
vms-c79 done   vms-004e done  vms-659 done   vms-d894 done  vms-35f done
```

**`vms-d894` closed 2026-08-05 by operator ruling** — recorded explicitly as a *scope decision*, not
as the criterion being met. Its literal criterion (a floor sourced from something the deleter does
not own) remains unmet and is accepted as such. This verdict does not re-open it.

**Verdict 7's five escapes are all closed**: `vms-c79`, `vms-004e`, `vms-659`, `vms-35f`, `vms-d894`.
That is the substantive difference between run 7 and run 8, and it is why this NO-GO rests on one
finding rather than five.

---

## 2. HALF ONE — the executive: **SATISFIED, BY EXECUTION**

Source: CI run `31023091917`, job *Kernel Executive — Per-Facility Negative Controls (attribution)*,
tree `0830b59`, 15m46s, **conclusion `success`**.

### The positive control ran first, and it is not vacuous

```
--- positive control: pristine image, every suite green ---
  ok: pristine image: all 27 suites rc=0, and ZERO failing assertions, against a real /dev/vms
```

A harness that fails indiscriminately cannot reach the rest of this job.

### Every defect was injected, booted, and attributed

42 defects. Each one produced the same five-line shape. Taking the last as the specimen:

```
--- negative control: opcom-header-host-login-name ---
  inject:   injected 'opcom-header-host-login-name' into /src/repo/src/libvms/syssvc/sys_operator.c
  ok: harness exited 1 (nonzero)
  ok: vms.ko still loaded and /dev/vms present (this is a facility defect, not an absent executive)
  ok: the facility's own suite(s) went red: test_syssvc_ident
  ok: no suite outside [test_syssvc_ident] failed -- the failure is attributable to '...'
  ok: the red set is EXACTLY the 2 assertion(s) the manifest names (observed 2)
```

The `vms.ko still loaded` line is what separates this from a re-run of the executive-absent control:
the module is live and the guest is up; only the facility is broken. The `no suite outside` line is
the attribution claim. The `EXACTLY` line is the set equality that round 1 of `vms-e7d` got wrong.

### Coverage, and it is execution-sourced rather than declared

```
PASS: every src/kernel/*.c translation unit is named by some defect's targets declaration
PASS: 25 derived suite(s) are each NAMED by some defect's suites_red glob
OBSERVED: of those 25, 25 are PROVEN ABLE TO GO RED and 0 NAMED ONLY.
PASS: all 42 defect(s) anchored by 273 marker(s) across 25 in-scope suite source(s)
PASS: 42 defect(s) >= floor 42 recorded in tests/qemu/facility_defects_floor.txt
PASS: 42 of this manifest's 42 defect(s) are OBSERVED-EXECUTED
```

`PROVEN` is spendable only by a QEMU run that happened — that was `vms-659`'s fix and it is live.

### The record cannot be fabricated upward or go stale quietly

```
--- the execution record this run observed vs the one committed ---
  observed: 42 defect(s) executed, 274 failing assertion(s) recorded
  ok: the committed record matches this run EXACTLY, row for row

==========================================================
 Facility negative controls: 45 passed, 0 failed
==========================================================
```

### Declared non-claim, restated rather than left implied

```
SCOPE: this gate does NOT cover 6 translation unit(s) under src/kernel/vmsfs/
       or the suite(s) [test_kmod_vmsfs test_kmod_vmsfs_blkdev].
  CONSEQUENCE, STATED PLAINLY: this gate proves nothing about vmsfs.ko.
```

`vmsfs.ko` is a filesystem driver, reached through no `/dev/vms` ioctl; CI job 3c covers it. Correct
exclusion, and it is printed by the job rather than living in a comment.

**Half one is met.** A deliberately injected defect in each wired facility turns CI red, names the
facility, and reddens nothing else — shown with command output, not assertion.

---

## 3. HALF TWO — the machine-check: **NOT SATISFIED. This is the blocker.**

The clause requires that a declaration's cited item **exist and be open**, and that this be
**machine-checked, not shape-checked**. Measured on `origin/main` @ `cf4ce33`:

### 3.1 One cited id on main is closed

Enumerated every `OVMX-<TOKEN>:` citation under `src/` and `tools/` in an `origin/main` checkout,
then resolved each against live `rd` **from the repo root** (the only cwd where `rd` returns the
complete board — `vms-10c`):

```
cited ids on origin/main: 22
  *** CLOSED: vms-82a (done)
--- sites citing the closed id ---
34: * OVMX-EXECUTIVE: sys$enq (vms-82a) proof=tests/qemu/test_syssvc_lock_status.c
38: * OVMX-EXECUTIVE: sys$enqw (vms-82a) proof=tests/qemu/test_syssvc_lock_status.c
41: * OVMX-EXECUTIVE: sys$deq (vms-82a) proof=tests/qemu/test_syssvc_lock_status.c
```

21 of 22 are open. One is not, at three sites, and all three are `OVMX-EXECUTIVE` — the strongest
claim the grammar has.

### 3.2 The gate does not miss this. It certifies it.

The committed ledger CI reads still carries the pre-closure answer:

```
origin/main tracking/rd-citations.tsv:19 -> vms-82a   open   inbox
live rd                                  -> vms-82a   done
```

Running the register gate against an `origin/main` checkout exactly as CI does — no `rd`, ledger
only:

```
PASS: every sys$ service declares, against an item, where its answer comes from.
REGISTER_RC=0
```

**Three services are declared against an item that tracks nothing, and the gate reports each one as
declared against an item.** That is not a gap in coverage; it is a gate returning the wrong answer
with the word `PASS` in front of it.

### 3.3 Why CI cannot see it, structurally

Two checks that were meant to cover each other:

- `tests/integration/lib/rd_citations.sh` resolves ids against the **committed ledger**, with grep
  and awk. Its header states why: `rd` is nostr-backed and unreachable from CI. Disclosed design
  constraint, not a bug.
- `tests/integration/test_rd_citations_fresh.sh` is the check that the ledger still **agrees with
  live rd**. It needs `rd`. **In CI it does not run.**

So in CI nothing ever compares the ledger to `rd`, and the ledger is simply believed. The row went
stale because `vms-82a` closed in round 9 (PR #78) and nothing regenerated the ledger; regeneration
is manual and is forced only by a test that cannot run in CI.

This is filed as **`vms-72d`** (mechanism, `p1`, under `vms-898`), distinct from **`vms-344`** (the
specific red). The falsifiable claim left for whoever fixes it: *on a tree where the ledger disagrees
with `rd` about any cited id, some gate that runs in CI must go red.* Today none does.

### 3.4 State of the fix

`vms-344`'s repoint (`vms-82a` → `vms-042`, plus a comment block recording that repointing is the
honest local fix and **not** the answer to the structural question) exists as **uncommitted work in
the shared working tree**, written 16:27Z by a concurrent session. It is not mine to commit. With it
applied, from the repo root:

```
gen_rd_citations: 22 cited id(s) from 101 declaration site(s) -- 22 open, 0 closed, 0 absent
self-controls: 24 passed, 0 failed
rd citation ledger freshness: PASS
```

The gate's own negative controls confirm it is not vacuous — `floor control T: a CLOSED id outside
the caller's own ids is still resolved (not just present) (rc=1)` passed, so a closed citation *does*
red the gate when the ledger is accurate. The failure in §3.2 is the ledger, not the check.

---

## 4. Disclosed limits — to be repeated verbatim in any future GO verdict

1. **Phase 2 closing means WHAT IS WIRED IS PROVEN, not that the executive is complete.** 13 of 44
   entry points still have no product path. The four unwired families gate Phase 3 (`vms-042`), not
   Phase 2: `vms-pv1`, `vms-as1`, `vms-dv1`, `vms-a86`.
2. **"Facility" here means the manifest's TU + suite decomposition (7 kernel TUs, 25 suites).**
   `vms-2b2` measures a finer one: only 9 of 33 `vms_ioctl_*` handlers sit inside any mutation hunk.
   Eight of the 24 uncovered are unwired entry points (legitimately exempt under the `vms-1e1`
   ruling while honestly declared), and the measure is line-level rather than behavioural, so some of
   the remaining ~16 are covered in effect. **`vms-2b2` is deliberately wired to `vms-042`, not to
   `vms-b33`** — adopting the finer decomposition mid-verdict would be moving the goalposts. If the
   operator reads "facility" as "handler", re-wire it and this verdict is NO-GO on that ground too.
   That is a one-command change and it is the operator's call.
3. **The static gates price evasions in the low single digits of edits** (`vms-38c`, `vms-d33`,
   `vms-41b`). All are wired to `vms-150`/`vms-042` as veracity questions, not to `vms-b33`.
4. **`vms-d894`'s literal criterion is unmet by operator ruling**, not by argument. Deleting a defect
   costs a measured 69 changed lines across 3 files.
5. **This verdict's executive evidence is from `0830b59`, one merge behind current main.** `dc10e1b`
   (#85, `vms-187` SCA) landed during the run. Its diff touches `src/vmsscs/`, `tests/vmsscs/` and
   `tools/cluster/` and **no path under `src/kernel/`, `src/libvms/` or `tests/qemu/`** — verified by
   `git diff --name-only`. The negative-control evidence therefore carries forward. The citation
   evidence in §3 is already at `cf4ce33` and includes #85's new sources.

---

## 5. What turns this GO

Exactly two things, in order:

1. **`vms-344` lands on main.** The three `sys_lock.c` citations stop naming a closed item, and the
   ledger is regenerated from the repo root. This removes the false certification. The fix is written
   and uncommitted in the shared tree.
2. **`vms-72d` is ruled on.** Either CI gains a way to detect ledger/`rd` disagreement without `rd`,
   or the operator accepts as a disclosed limit that CI cannot — in which case "machine-checked, not
   shape-checked" must be reworded to what it actually buys, because today it claims more than it
   delivers. **That is a scope call and it is the operator's, not the adversary's.**

Nothing else in the closure blocks. Half one is done and was done by execution.

---

## 6. Method notes worth keeping

**A GATE THAT PASSES IS NOT EVIDENCE THE PROPERTY HOLDS — CHECK WHAT IT READ.** The register gate
prints `PASS: every sys$ service declares, against an item`. It was true of the ledger and false of
the tree. The whole finding in §3 is one `grep` of the ledger row next to one `rd show`.

**RUNNING A GATE FROM THE WRONG CWD PRODUCES A RED FOR THE WRONG REASON, AND IT LOOKS LIKE A
FINDING.** Three attempts to reproduce the citation red on a clean `origin/main` tree — a `git
archive` extraction, then a `git worktree`, then the generator with `--root` — all failed with
`vms-10c`'s partial-board refusal rather than the citation error, because `rd` and the generator both
resolve the board from the *working directory*, and `--root` sets that too. The refusal is correct
behaviour (round 10's fix: refuse rather than write a self-contradictory `closed inbox` row). The
measurement that worked was the one that stopped using the harness: enumerate the ids from the clean
tree, resolve them with `rd` **from the repo root**. *When the instrument's cwd is the confound, take
the instrument out of the loop.*

**RE-DERIVE "MAIN" PER MEASUREMENT, NOT PER SESSION.** `origin/main` moved from `3ab3205` to
`cf4ce33` between two commands in this run. Every SHA in this document was read at the moment of the
measurement it labels, and the executive/citation evidence is deliberately pinned to *different*
SHAs with the diff between them checked rather than assumed away.

**A SHARED WORKING TREE IS NOT YOUR TREE.** `sys_lock.c`, `ci.yml`, `tests/libvms/CMakeLists.txt`
and `tracking/rd-citations.tsv` all changed 2–3 minutes into this session, mid-read, from a
concurrent session. The session-start `git status` snapshot showed none of them. `stat -c %y` before
concluding anything about who owns an edit — and release a claim rather than race it.
