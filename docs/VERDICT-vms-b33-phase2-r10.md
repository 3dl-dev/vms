# PHASE 2 VERDICT, TENTH RUN — `vms-b33`: **GO**

**Run 2026-08-06 by the vms-b33 verdict-10 session.** Every figure below is command output taken
this session, pinned to a SHA. Re-derive it before trusting it.

> **The one-line result.** Half one (the executive) remains satisfied by execution, unchanged.
> Half two's clause — "the cited item must EXIST and be OPEN, machine-checked, not shape-checked" —
> no longer has a mechanism behind it to satisfy or fail: the citation-ledger apparatus that clause
> was written against is **torn down** (`vms-dc7`, operator ruling 2026-08-06), not fixed or
> reworded. This verdict rules that the clause is satisfied by the apparatus's absence rather than
> by anything it once checked, and states plainly that this is a **rewording of what the clause
> means**, not a claim that a stronger check now exists.

---

## 0. What changed since run 9

Run 9 (`docs/VERDICT-vms-b33-phase2-r9.md`, PR #101) found `vms-b33` NO-GO on exactly one ground:
`vms-72d`, an unanswered operator scope call between (A) accepting the ledger/`rd` disagreement as a
disclosed limit and rewording the clause, or (B) building real detection.

The operator did neither. On review, the citation-ledger mechanism itself was rejected outright: it
verified only that a source comment cited an rd item id resolving to "open" in a committed snapshot
— never anything about whether the labeled claim was true, and it could not see the snapshot go
stale between regenerations. That is the exact mechanism that let three `sys_lock.c` sites cite the
closed `vms-82a` while CI printed `PASS` (`vms-344`). Rather than choose (A) or (B), the operator
ordered the whole apparatus torn down (`vms-dc7`, PR #116, merged `3132b242`):

- Deleted: `tracking/rd-citations.tsv`, `tools/gen_rd_citations.py`,
  `tests/integration/lib/rd_citations.sh`, `test_rd_citations_fresh.sh` (itself a standing rule-7
  violation — it SKIPPED on every CI run by design and so never actually passed there),
  `test_rd_citations_partial_list.sh`.
- `vms-72d` and `vms-344` are both cancelled — see their close reasons. Neither tracks anything now.

Separately, and not part of the teardown: `vms-38c`'s runtime-attribution prototype merged to main
(PR #108, `0f7206f8`) — `tests/qemu/facility_attribution.sh`, which cross-checks a defect's real file
diff against which assertions actually redden in QEMU execution. This *reports* per-claim
attribution now; it does not yet gate on it (still blocked on `vms-2b2`'s handler-probe coverage).
It is not what turns this verdict GO and is not re-argued here — see `vms-38c`, `vms-d33`, `vms-2b2`
for its own status.

---

## 1. Blockers: all 29 re-verified closed, individually, again

Re-walked the DAG from `vms-14f`, resolving `vms-b33`'s 29 listed blockers one at a time:

```
vms-fa2 done   vms-9fc done   vms-1d9 done   vms-f42 done   vms-556 done   vms-e7d done
vms-f1f done   vms-2a8 done   vms-7fb done   vms-290 done   vms-2e5 done   vms-5b4 done
vms-0e4 done   vms-d89 done   vms-413 done   vms-ecf done   vms-f26 done   vms-e2b done
vms-279 done   vms-8cc done   vms-1e1 done   vms-32e done   vms-c13 done   vms-27e done
vms-c79 done   vms-004e done  vms-659 done   vms-d894 done  vms-35f done
```

No regressions since run 9.

---

## 2. HALF ONE — the executive: still SATISFIED, BY EXECUTION

Unaffected by the teardown or the attribution merge — neither touches `src/kernel/`. Re-confirmed on
CI run `31066242421`, tree `ce96d801` (an ancestor of current main `139ede64`, verified by
`git merge-base --is-ancestor`):

```
Kernel Executive (vms.ko via /dev/vms, QEMU)                                  -> success
Kernel Executive — Negative Control (proves the gate can fail)                -> success
Kernel Executive — Per-Facility Negative Controls (attribution)               -> success
```

Same job, same figures as runs 8 and 9 (42 defects, 45 passed / 0 failed, committed record matched
row for row). Not re-argued further.

---

## 3. HALF TWO — the clause is retired, not satisfied by a stronger check

### 3.1 There is nothing left to shape-check or machine-check

`origin/main` @ `139ede64`:

```
$ git ls-tree -r origin/main --name-only | grep -iE "rd.citation|gen_rd_citation"
(no output)
$ grep -c "rd_cite_check" tests/integration/test_userspace_service_register.sh
0
$ grep -c "rd_cite_check" tests/integration/test_kif_caller_census.sh
0
```

Both gates were run directly against the current tree this session and PASS, with the citation
section entirely absent from their output — replaced, in the register's case, by attribution lines
sourced from real QEMU execution (`tests/qemu/facility_attribution.sh`, PR #108):

```
MEASURED: assertion(s) in test_syssvc_ef_mproc.c were observed to change verdict when
          vms_ioctl_waitfr() was mutated -- this claim is paid by EXECUTION

PASS: every sys$ service declares, against an item, where its answer comes from.
```

### 3.2 What this verdict rules, plainly

The amended clause read: *"Every entry point the product does NOT reach must be honestly DECLARED,
and its cited item must EXIST and be OPEN — machine-checked, not shape-checked."* That machine-check
does not exist any more. This is **not** the operator choosing option (A) from run 9's text (accept
the gap as a disclosed limit) or (B) (build stronger detection) — it is a third outcome run 9 did not
enumerate: the operator decided the check itself was not worth having, at any fidelity, because it
never bore on whether the underlying claim was true.

**Ruling:** the clause is satisfied vacuously — there is no cited-item requirement left to violate,
and no ledger left to go stale. This verdict states that outright rather than implying the old
clause still holds against a mechanism that no longer exists. Any future re-introduction of a
citation requirement (e.g. `vms-38c`'s `proof=<file>` direction, tying an `OVMX-EXECUTIVE`
declaration to something the negctl manifest actually executes) would be a **new** clause, argued and
verified on its own terms — not a revival of this one.

**What is unchanged and still real:** the declaration-vs-call-graph consistency checks in both gates
(a service claiming USERSPACE that reaches the executive is red; a service claiming EXECUTIVE that
reaches nothing is red the other way) never depended on the citation ledger and are untouched,
confirmed by direct execution this session (§3.1).

---

## 4. Disclosed limits — carried forward, unchanged since run 8/9

1. **Phase 2 closing means WHAT IS WIRED IS PROVEN, not that the executive is complete.** 13 of 44
   entry points still have no product path (`vms-pv1`, `vms-as1`, `vms-dv1`, `vms-a86`) and gate
   Phase 3 (`vms-042`), not Phase 2.
2. **"Facility" here means the manifest's TU + suite decomposition.** `vms-2b2` measures a finer one
   (9 of 33 `vms_ioctl_*` handlers), deliberately wired to `vms-042`, not `vms-b33`.
3. **`vms-d894`'s literal criterion is unmet by operator ruling**, not by argument.
4. **The runtime-attribution instrument (`vms-38c`, PR #108) is live but report-only**, blocked on
   `vms-2b2` before it can enforce. It played no part in this verdict turning GO and is not a
   substitute for anything half two used to check.

---

## 5. What turns this NO-GO again

Nothing currently open does. If a future session wants to re-introduce a citation or attribution
requirement as part of Phase 2's clause, that is new scope requiring its own ruling — not a residual
of this verdict.

---

## 6. Method note worth keeping

**A CLAUSE WRITTEN AGAINST A MECHANISM DOES NOT SURVIVE THE MECHANISM'S DELETION BY DEFAULT.** Run 9
filed `vms-72d` assuming the fix would be a wording choice or a stronger check layered on the same
foundation. The operator instead removed the foundation. Closing `vms-b33` required noticing that
the clause's remaining text now refers to nothing, and saying so, rather than either (a) treating the
absence of a red as a PASS by default, or (b) treating the absence of the old mechanism as an
unresolved gap that blocks GO forever. Neither is correct; the clause is retired, and this document is
the record of that decision.
