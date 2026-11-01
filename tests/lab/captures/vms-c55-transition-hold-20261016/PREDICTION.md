# Pre-registration: what the TRANSITION HOLD must do to the wire

Written and pushed BEFORE the fire, against the commit that introduces the
hold. The fix has an emitter-side observable that does not depend on a crash
happening, which is what carries the argument at a bugcheck rate of about
1 in 9.

## The baseline, counted off the fourteen runs this directory's README tabulates

Two committed crash captures plus the twelve case-runs of openvmx-site fires
`38811815171`, `38811829040`, `38811843112`, `38814006004`, counted by
`tools/cluster/transition_dlm_census.py emitter`:

| | OVMX cat-0x02 request IN a transition | outside | bugcheck |
|---|---|---|---|
| 3 crash runs | **3** (`02/04@step1` x3, `02/01@step2` x2) | 1 | YES x3 |
| 11 clean runs | **0** | 26 | no |

And over all 122 committed pcaps, real DEC-OUI senders only: **zero** non-`0x0d`
cat-0x02 frames at barrier steps 1–5, all 31 of them at steps 10/11/12 of 12
(plus one `02/08` at step 6).

## The predictions, hardest to fake first

**P1 (the emitter, categorical). No OVMX node originates a cat-0x02 frame of
any opcode other than `0x0d` while a transition window is open.** 0 of however
many fires, against 3 of 3 crash runs today. Count it with
`transition_dlm_census.py emitter <frames.jsonl>`: the `in-txn` column must be
0 for every case-run. A single in-window origination means the hold did not
reach the builder, and the change is wrong rather than unlucky.

**P2 (the hold really holds something, i.e. P1 is not vacuous). OVMX still
originates cat-0x02 requests OUTSIDE transitions**, at a rate comparable to
the 26 across eleven clean baseline runs. A fire in which OVMX emits NO lock
requests at all proves nothing about P1 — it would mean the workload never
reached the DLM, not that the hold worked. State the outside-count with P1
every time.

**P3 (no $ENQ is failed instead of waiting). No OVMX console carries a DLM
request failing with a timeout status** that the baseline runs did not also
carry, and both OVMX nodes still reach a logged-in DCL `$`. The hold deliberately
does not spend a rung of the retransmit ladder, so a transition must not turn
into a failed lock request. A node that stops reaching `$` falsifies the hold's
shape even if P1 holds.

**P4 (the shapes are unchanged). The set of (category, opcode) pairs OVMX
emits is the same as the baseline's** — `01/01, 01/02, 01/0b, 01/14, 02/01,
02/04, 04/00, 0b/05, 81/03, 81/05, 81/09, 82/01, 82/0d, 86/00, 8b/04` and the
two response shapes `81/08`/`81/0f` already recorded as unratcheted (rd
vms-59dd). This change emits NO new shape: it only moves WHEN two existing ones
go out. A new pair appearing means something else changed.

**P5 (the crash). ZERO LOCKMGRERR across at least 12 case-runs spanning
`b-first`, `all-at-once` and `page-order-throttled4`** — the three orders a
LOCKMGRERR has been seen in — against a baseline of 1 in 9 to 1 in 12. Twelve
clean case-runs is roughly one expected crash avoided; it is suggestive, not
proof, which is why P1 is the one stated first. The companion signal is the
console string `refusing a lock message (cat 0x02 op 0x19)`: present in 5 of 5
crashes and 0 of 30 clean case-runs to date. If it also stops appearing while
P1 holds, the VAX stopped being put in the state that made it send one.

**P6 (the instrument). Does OVMXB announce the build under test?** rd vms-4f4
says the `cases` input and a real Node B are mutually exclusive on the current
site branches. Any fire that still reports `OVMXB announced V0.7-7` measures
OVMXA only — which is enough for P1/P2/P4, because OVMXA is the emitter under
test and the node in all three crashes — and the claim must say so.

## What would falsify the change rather than merely fail to confirm it

* an in-window origination (P1) — the hold is not where the frames are;
* a run in which an OVMX node never reaches `$` while the baseline's did (P3)
  — the hold is wedging the lock path instead of delaying it;
* a LOCKMGRERR whose capture shows **no** OVMX cat-0x02 inside a transition —
  that would say the in-window traffic was a co-symptom, exactly as the zero
  PID cell turned out to be, and the hunt moves on with the hold kept on its
  own faithfulness merits.

## How to count it

```
tools/cluster/transition_dlm_census.py emitter <frames.jsonl>     # P1, P2
tools/cluster/transition_dlm_census.py census  <frames.jsonl>     # P4 (cat-02)
tools/cluster/capture_conformance.py <frames.jsonl> OVMXA --to VAXC
grep -c 'refusing a lock message' OVMX*.console.log               # P5 companion
grep -c 'BUG CHECK' VAXC.console.log                              # P5
```
