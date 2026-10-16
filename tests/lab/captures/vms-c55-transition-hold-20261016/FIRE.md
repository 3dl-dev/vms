# The fire, scored against PREDICTION.md — and what it does NOT settle

Written after the runs, against the pre-registration in this directory, which
was pushed with the commit and before the first dispatch.

## What was fired

| arm | ref | fires × cases | Node B |
|---|---|---|---|
| **the hold** | `work/vms-c55-transition-hold` | 4 × 3 = 12, then 2 × 3 = 6 more carrying the console diagnostic | built from the ref (vms `38833987086`, `38843471197`) |
| **the control** | `main` (`d7e1d264d`, this branch's exact base) | 2 × 3 = 6 | built from main (vms `38841415922`) |

Runs: openvmx-site `38836182130`, `38836196104`, `38836210245`, `38836224210`,
`38846164733`, `38846180242` (hold) and `38843411225`, `38843424946`
(control). All `preflight=true`, `capture=true`,
`cases=b-first,all-at-once,page-order-throttled4`.

This is the first scoring in which **Node B is the build under test** — rd
vms-4f4 put the `cases` input on site main alongside Node B staging from the
ref, so P6 is met and no run reports a stale banner.

## The scorecard

| | hold (18 case-runs) | control / main (6 case-runs) |
|---|---|---|
| **P1** OVMX cat-0x02 originated inside a transition | **0** | **0** |
| **P2** the same, outside a transition | 232+ | 28 |
| **P4** a new emitted (cat, op) pair | none¹ | none |
| **P5** LOCKMGRERR | **0** | **0** |
| op-0x19 refusal on any console | **0** | **0** |
| CN=3 | 18 of 18 | 6 of 6 |
| **P3** OVMXB reaches Username: | **no**, 17 of 18 | **no**, 6 of 6 |

¹ `8b/01` appears where the baseline list had only `8b/04`; it is a cat-0x0b
ANSWER with an existing grounding row (`cm_0b01_ans`), present because Node B
is a current build for the first time, and not an origination this change
touches.

## What the fire settles

**The hold is live, it fires on a real node, and it releases.** In
`38846164733` / b-first the OVMX/VAX node's console carries, once each:

```
%DLM, holding cross-node lock requests until the VAXcluster state transition completes
%DLM, the VAXcluster state transition completed; held cross-node lock requests resume
```

So the mechanism is exercised by real traffic rather than only by its tests,
and the node went on serving afterwards.

**The change causes no regression.** OVMXB never reaching Username: is
**present identically on main**, 6 of 6 against 17 of 18 — it is rd vms-97c2
(P0, still open, #1729 fixed it only partially), and rd vms-4f4 is why it
became visible: every earlier case-filtered run booted the deployed V0.7-7
image. The control is what attributes it; without it this fire would have read
as a regression caused by the hold.

**No peer was harmed.** 24 case-runs across two arms, zero bugchecks, zero
refusals, CN=3 everywhere.

## What the fire does NOT settle, said plainly

**That the LOCKMGRERR is fixed.** Two reasons, both measured:

1. **The control did not reproduce it either.** 0 crashes in 6 control runs on
   main, where the baseline rate is 1 in 9 to 1 in 12. So this round's 0-of-18
   on the hold side is consistent with the fix working *and* with the crash
   simply not firing. Eighteen clean runs is roughly 1.5–2 expected crashes
   avoided — suggestive, not proof, exactly as P5 was written.
2. **P1 is not a discrimination in these runs.** The control also shows 0
   in-transition originations, so the condition the hold guards against did not
   arise in the control arm. P1's categorical content is a property of the
   CODE, proven at R1 and R2 where the condition is forced (`test_dlm_requester`,
   `scenarios/dlm_requester.c`), not something these particular runs
   demonstrate. The one run whose console shows the hold firing is the only
   direct evidence from the wire that it acts at all.

So the honest claim is the one the change is defended on: **real VMS nodes do
not put lock traffic into a rebuild and OVMX did, in 3 of 3 crash captures and
0 of 11 clean ones; the hold removes that, harms nothing, and emits no new
shape.** Whether it is the LOCKMGRERR's cause is still open, and `vms-8cb5`'s
question — what `cat-0x02 op-0x19` IS — is still unanswered and still not
guessed.
