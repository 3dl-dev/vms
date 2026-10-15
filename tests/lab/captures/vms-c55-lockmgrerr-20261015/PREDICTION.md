# Pre-registration: what the body[20:24] fix must do to the wire

Written BEFORE the fire, against the commit that introduces it. The point is
that the fix has an observable that is not "did it crash this time" -- a crash
rate of roughly 1 in 9 cannot be moved convincingly by a handful of runs, but
the frame-level signal below is present in EVERY run and is 100% one-sided
today.

## The baseline, counted off the nine case-runs in this directory's fire

Three runs (openvmx-site `38779035174`, `38779039044`, `38779043209`) x three
cases, all on main with #1714/#1715/#1717 in. For each: how many cat-0x02
op-0x01 requests the OVMX nodes sent with `body[20:24]` zero, and what the real
VAX answered them at `body[34]`.

| case-run | OVMX op-01, zero cell | nonzero | answers |
|---|---|---|---|
| r1 all-at-once (**the LOCKMGRERR**) | 1 | 0 | `0xfe` x1 |
| r1 b-first | 0 | 0 | -- |
| r1 page-order-throttled4 | 4 | 0 | `0xf9` x3 |
| r2 all-at-once | 167 | 0 | `0xf9` x62, **`0xfe` x18** |
| r2 b-first | 6 | 0 | `0xf9` x3 |
| r2 page-order-throttled4 | 3 | 0 | `0xf9` x2 |
| r3 (all three) | 0 | 0 | -- |
| **total** | **181** | **0** | `0xf9` x70, **`0xfe` x19** |

So OVMX draws a `0xfe` from a real OpenVMS V7.3 directory in **19 of 89**
answers (21%) -- routinely, in runs that do NOT crash as well as in the one that
does. Of the whole committed corpus, a `0xfe` is answered to a zero cell 12
times and to a nonzero cell 0 times in 19,370.

## The predictions, in order of how hard they are to fake

**P1. No OVMX op-0x01 request carries a zero `body[20:24]`.** 0 of however many
go out, against 181 of 181 today. This is a property of the emitter and must
hold in every run, crash or not. If any zero appears, the executive read did not
reach the builder and the change is wrong, not unlucky.

**P2. No answer to an OVMX request carries `body[34] = 0xfe`.** 0, against 19 of
89 today. This is the one the fix is FOR: it is the real VAX's own verdict on
the frame, measured in its reply, and it does not depend on a crash happening.
If `0xfe` still appears against a nonzero cell, then the cell was not what drew
it, the corpus correlation (12/12 and 0/19,370) was a confounder, and this
change should be reconsidered rather than kept because the crash stopped.

**P3. The requests are otherwise unchanged.** The set of (category, opcode)
pairs OVMX emits is identical, no new builder runs, and the resource names,
modes, identity span and hashes are the same values for the same locks. Only
one cell moves.

**P4. ZERO LOCKMGRERR, and the one thing that would say more than that.** Across
at least three fires x the three orders a LOCKMGRERR has been seen in
(`b-first`, `all-at-once`, `page-order-throttled4`), 0 bugchecks. Weak on its
own -- 9 clean fires is consistent with luck at a 1-in-9 rate -- which is why P2
carries the argument. The stronger companion observation is the console string
`refusing a lock message (cat 0x02 op 0x19)`: it appeared in 4 of 4 crashes and
0 of 22 clean case-runs, so if the 02/19 ALSO stops appearing, the VAX stopped
reaching whatever state made it send one.

**P5. OVMXB announces the build under test.** Not a prediction about the fix --
a precondition on the instrument. All nine baseline runs carry `OVMXB announced
V0.7-7`, so no run was the build under test end to end. A fire that still says
that measures OVMXA only, and any claim from it must say so.

## What would NOT settle it

A run that crashes anyway is informative only if it has frames: with capture on,
the four-frame window around the bugcheck says whether the cell was nonzero when
the VAX died. A crash with a nonzero cell falsifies P2 and the diagnosis.

## How to count it

```
tools/cluster/dlm_pid_cell_census.py census  <the new frames.jsonl>
tools/cluster/capture_conformance.py <frames.jsonl> OVMXA --to VAXC
```
and for the two ratios above, the same two tallies this page was made from: OVMX
op-0x01 requests split on `body[20:24] == 0`, and the real VAX's `body[34]` on
the cat-0x82 op-0x01 frames addressed to an OVMX node.
