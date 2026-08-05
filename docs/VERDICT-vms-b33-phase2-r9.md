# PHASE 2 VERDICT, NINTH RUN — `vms-b33`: **NO-GO**

**Run 2026-08-05 by the vms-b33 verdict-9 session.** Every figure below is command output taken this
session, pinned to a SHA. Re-derive it before trusting it — main moved multiple times while run 8 was
being written and has moved again since.

> **The one-line result.** Both halves of the closing clause that run 8 measured are now satisfied.
> `vms-344`'s fix (PR #88) landed on main and the citation ledger is fresh. The single remaining
> blocker is not a defect in the tree — it is `vms-72d`, an operator scope call raised by run 8 and
> still unanswered. This verdict does not decide it. NO-GO stands on that one ground alone.

---

## 0. What changed since run 8

Run 8 (`docs/VERDICT-vms-b33-phase2-r8.md`, PR #86, landed `06c6082`) found one blocker in two
halves: half one (the executive) satisfied by execution; half two (the machine-check) not, because
three `OVMX-EXECUTIVE` sites in `sys_lock.c` cited the CLOSED `vms-82a`, and the CI register gate
certified `PASS` over it. Run 8 filed `vms-72d` (the structural reason CI cannot see ledger/`rd`
disagreement) and left `vms-344`'s fix uncommitted in the shared tree, not run 8's to commit.

Since then: `vms-344`'s fix merged as PR #88 (`f874b043`, merged `2026-08-05T17:35:58Z`).

---

## 1. Blockers: all 29 re-verified closed, individually

Re-walked the DAG from `vms-14f`, resolving `vms-b33`'s 29 listed blockers one at a time rather than
trusting the item's own rollup:

```
vms-fa2 done   vms-9fc done   vms-1d9 done   vms-f42 done   vms-556 done   vms-e7d done
vms-f1f done   vms-2a8 done   vms-7fb done   vms-290 done   vms-2e5 done   vms-5b4 done
vms-0e4 done   vms-d89 done   vms-413 done   vms-ecf done   vms-f26 done   vms-e2b done
vms-279 done   vms-8cc done   vms-1e1 done   vms-32e done   vms-c13 done   vms-27e done
vms-c79 done   vms-004e done  vms-659 done   vms-d894 done  vms-35f done
```

No regressions since run 8. `vms-d894` remains closed by operator ruling, its literal criterion
accepted-unmet — not re-argued here.

---

## 2. HALF ONE — the executive: still SATISFIED, BY EXECUTION

Re-confirmed on current main. CI run `31052929533`, tree `9bb1d977ea94395f6d281e2a0a82c48c8c9e8c04`
(an ancestor-confirmed descendant of the `vms-344` merge commit `f874b043` —
`git merge-base --is-ancestor f874b043 9bb1d977` returns true):

```
Kernel Executive (vms.ko via /dev/vms, QEMU)                                  -> success
Kernel Executive — Per-Facility Negative Controls (attribution)               -> success
Kernel Executive — Negative Control (proves the gate can fail)                -> success
```

This is the same job that run 8 measured on `0830b59` and `dc10e1b` with identical figures (42
defects, 45 passed / 0 failed, committed record matched row for row). Nothing about half one has
changed; it is not re-argued, only re-checked for regression.

---

## 3. HALF TWO — the machine-check: the specific red is FIXED

### 3.1 The citation sites no longer cite a closed item

`origin/main` @ `b88bd6867b4a8776050fe070df6fdf27f1ac937f`:

```
src/libvms/syssvc/sys_lock.c:34: OVMX-EXECUTIVE: sys$enq  (vms-042) proof=tests/qemu/test_syssvc_lock_status.c
src/libvms/syssvc/sys_lock.c:38: OVMX-EXECUTIVE: sys$enqw (vms-042) proof=tests/qemu/test_syssvc_lock_status.c
src/libvms/syssvc/sys_lock.c:41: OVMX-EXECUTIVE: sys$deq  (vms-042) proof=tests/qemu/test_syssvc_lock_status.c
```

`vms-042` is `open` (`blocked`) in live `rd`. The three sites that certified a false statement in run
8 no longer do.

### 3.2 The ledger is regenerated and matches live `rd`, checked id by id

`tools/gen_rd_citations.py` cannot be run from a clean-tree worktree or with `--root` (`vms-10c`: both
resolve `rd`'s board from the working directory, and a partial `rd list` there produces a
self-contradictory `closed inbox` row rather than a clean red). Per run 8's method note, the harness
was taken out of the loop: every id was enumerated from a clean `origin/main` checkout, then resolved
against live `rd` **from the repo root**:

```
vms-042    -> blocked   vms-407  -> inbox    vms-642  -> inbox    vms-6aa  -> inbox
vms-846.3  -> inbox     vms-905  -> blocked  vms-911  -> inbox    vms-916  -> inbox
vms-a36    -> inbox     vms-a86  -> inbox    vms-afc  -> inbox    vms-afd  -> inbox
vms-as1    -> active    vms-d37  -> blocked  vms-dv1  -> inbox    vms-e0a  -> inbox
vms-f15    -> blocked   vms-f90  -> inbox    vms-mb1  -> blocked  vms-pt1  -> inbox
vms-pv1    -> active
```

All 21 (22 with `vms-042`, matching run 8's count) resolve to non-closing statuses. None are
`done`/`cancelled`/`failed`. The ledger's `open` column agrees with live `rd` on every row. Half two's
specific red — a citation certifying against a tracker that tracks nothing — is gone.

**Half two's specific defect is fixed.** This is not the same claim as "the machine-check clause is
satisfied" — see §4.

---

## 4. THE REMAINING BLOCKER: `vms-72d`, unanswered

`vms-344` fixed the instance. `vms-72d` is the mechanism run 8 filed alongside it: CI's only citation
check reads a committed ledger and believes it; the check that would compare that ledger against live
`rd` needs `rd` and does not run in CI. Nothing about fixing three citations changes that mechanism —
the next item that closes after being cited will reproduce exactly this red, invisibly, the same way
`vms-82a` did.

`rd show vms-72d` (checked this session): status `waiting`, no ruling recorded since run 8 filed it
at `2026-08-05T17:32:25Z`. It is listed in `rd gates` as an open, unranked p1 gate. The two options
and their costs are unchanged from run 8's text and are not re-argued here:

- **(A) Accept as a disclosed limit.** Cheap, honest, and requires rewording "machine-checked, not
  shape-checked" in `vms-b33`'s clause to what it actually buys today — a check against a ledger that
  can silently go stale, not a check against `rd`.
- **(B) Build detection without `rd`.** Not a CI check — the hard part is that a row goes stale when
  an item's status changes *outside the repo*, which no offline scan can observe. Enforcement has to
  move to closure time, or the grammar has to change so a settled claim cites `proof=<file>` rather
  than an open owner (the same fork `vms-344` raised structurally).

**This is a scope call, reserved to the operator, not decided by this verdict.** Weakening a stated
clause's wording (A) and changing a declaration's grammar (B) are both outside what an adversary
session self-certifies. Absent a ruling, `vms-b33` stays NO-GO on this one ground.

---

## 5. Disclosed limits — carried forward verbatim, unchanged since run 8

1. **Phase 2 closing means WHAT IS WIRED IS PROVEN, not that the executive is complete.** 13 of 44
   entry points still have no product path (`vms-pv1`, `vms-as1`, `vms-dv1`, `vms-a86`) and gate
   Phase 3 (`vms-042`), not Phase 2.
2. **"Facility" here means the manifest's TU + suite decomposition (7 kernel TUs, 25 suites).**
   `vms-2b2` measures a finer one — only 9 of 33 `vms_ioctl_*` handlers sit inside any mutation hunk.
   `vms-2b2` is deliberately wired to `vms-042`, not `vms-b33`. If the operator reads "facility" as
   "handler", re-wire it and this verdict is NO-GO on that ground too — a one-command change and the
   operator's call.
3. **The static gates price evasions in the low single digits of edits** (`vms-38c`, `vms-d33`,
   `vms-41b`), wired to `vms-150`/`vms-042` as veracity questions, not to `vms-b33`.
4. **`vms-d894`'s literal criterion is unmet by operator ruling**, not by argument.

---

## 6. What turns this GO

Exactly one thing: **`vms-72d` is ruled on** — either (A) or (B) above. Nothing else in the closure
blocks. Both halves of the executed clause are now satisfied; the remaining gap is a wording/grammar
decision reserved to the operator, not new engineering.

---

## 7. Method notes worth keeping (added this run)

**A FIX LANDING DOES NOT CLOSE THE ITEM THAT FILED IT, IF THE ITEM NAMED A MECHANISM.** `vms-344`
remains `waiting`, not `done`, after its PR merged — because the rd item was written to cover the
structural ruling (folded into `vms-72d`) as well as the instance. Checking `rd show` rather than
inferring status from "the PR merged" caught this; a session that stopped at "PR #88 is merged" would
have reported `vms-344` closed when `rd` says otherwise.

**RE-VERIFY THE CITATION FRESHNESS CLAIM BY THE SAME WORKAROUND EVERY TIME**, not by re-running the
harness. `vms-10c`'s cwd-resolution bug is unfixed; a worktree or `--root` invocation still produces
the self-contradictory refusal rather than a clean answer. The id-enumerate-then-resolve method from
run 8 is the only one that has worked twice now.
