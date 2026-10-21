# vms-194 (D2/D3): the DRIVEN one-variable RELEASE arm

**Registered before the pod exists.** rd vms-194's own ruling is that body[30]
and body[32] of a `cat-0x02 op-0x03` release must NOT be guessed: *"the
determinant comes from a DRIVEN one-variable capture (the method that grounded
the mode byte, docs/cluster-protocol-spec.md 4(f).1, the ac4-* captures) --
release locks on a real VAX one variable at a time and read body[30]."* This is
that arm, pre-registered, with its predictions written down before any capture
exists so the reading cannot be fitted to the answer.

## What is already measured, and what is not

`tools/cluster/dlm_deq_census.py --selftest` over every committed pcap:

| | |
|---|---|
| real op-0x03 senders' `body[30]` value set | **{0, 1}** over 9,858 frames, 29 captures, both lab VAXes |
| ...`0` appears | 954 times |
| ...`1` appears | 8,904 times |
| OVMX's own `body[30]` | **{5}** over 11 frames -- a lock MODE, out of the real envelope |
| real releases with `body[32] == body[30]` | **9,858 of 9,858** |
| OVMX's `body[32]` | `0` on 11 of 11 |

So the census bounds what OVMX may write and says NOTHING about what selects 0
from 1. The codec calls body[30] "the mode being released" and that name is
**wrong**: a real node releasing an EX lock does not put 5 there.

D1 (the envelope: a release opens no transaction) is already fixed and
host-proven -- PR #1852, `vms_dlm_body_is_notification` admits op-0x03 on the
census's 0-of-9,858 / 0-answers evidence. **This arm's captures are also D1's
acceptance evidence**: every OVMX release in them must read `body[4:8] = 0`.

## The driver

`RMDEQ.MAR` in this directory: `RMDEQ "<r><v><n><name>"`, one lock op per
capture. It acquires, waits ten seconds with no traffic of its own, RELEASES,
and then hibernates so SDA can read what the release left. The quiet gap is
what makes the release's frames readable without pairing logic.

It is modelled line-for-line on `RMHOLD.MAR` (committed, assembles, and already
drove the rd vms-380 and rd vms-c2b arms), so the only new things in it are the
timer wait and the `$DEQ`.

## The topology, and why it is this one

A healthy 2-node REAL cluster, both nodes real OpenVMS VAX V7.3, and **no OVMX
node in the first six arms**. The question is what a REAL node puts in those
cells; an OVMX node in the cluster adds nothing and risks perturbing the
mastering. VAX2 holds a NULL-mode lock on each test resource first, so VAX2
masters the tree and **every VAX1 release crosses the wire** -- the same device
4(f).1 used to make the request cross.

Arms V2/S2 below repeat two readings with the roles swapped (VAX2 drives, VAX1
masters), because `heldout-proof-must-vary-topology` says a rule read off one
master is not a rule.

## The arms: one variable each, from one baseline

Baseline **B** = `RMDEQ "1-0OVMXDQ00"` -- hold CR, no VALBLK, no sub-locks,
release. Every other arm changes exactly one thing from B.

| arm | command | the one variable | capture |
|---|---|---|---|
| **B** | `RMDEQ "1-0OVMXDQ00"` | the baseline: CR | `dq-b-cr.pcap` |
| **R0** | `RMDEQ "0-0OVMXDQ01"` | held mode **NL** | `dq-r0-nl.pcap` |
| **R2** | `RMDEQ "2-0OVMXDQ02"` | held mode **CW** | `dq-r2-cw.pcap` |
| **R3** | `RMDEQ "3-0OVMXDQ03"` | held mode **PR** | `dq-r3-pr.pcap` |
| **R4** | `RMDEQ "4-0OVMXDQ04"` | held mode **PW** | `dq-r4-pw.pcap` |
| **R5** | `RMDEQ "5-0OVMXDQ05"` | held mode **EX** | `dq-r5-ex.pcap` |
| **V** | `RMDEQ "4V0OVMXDQ06"` | `LCK$M_VALBLK` on the `$DEQ`, held PW (the lowest mode a flush is legal at) | `dq-v-pw-valblk.pcap` |
| **S** | `RMDEQ "1-3OVMXDQ07"` | **3 sub-locks** under the root, same mode | `dq-s-cr-3sub.pcap` |
| **L** | B, with a SECOND holder of `OVMXDQ08` at CR on VAX2 that stays | **not the last holder** | `dq-l-not-last.pcap` |
| **V2** | arm V, roles swapped (VAX2 drives, VAX1 masters) | the MASTER, nothing else | `dq-v2-swapped.pcap` |
| **S2** | arm S, roles swapped | the MASTER, nothing else | `dq-s2-swapped.pcap` |

Resource names differ per arm (`OVMXDQ00`..`OVMXDQ08`) so no arm inherits a
resource block, a value block or a master decision from the one before it. Each
arm is its own capture file, started before the command and stopped after SDA.

## Predictions, registered before the captures exist

**P1 -- body[30] IS NOT THE MODE.** Over arms B and R0-R5, `body[30]` takes at
most **two** distinct values and they are drawn from `{0, 1}`. **If body[30]
tracks the held mode 0..5, this prediction is WRONG and the census's
9,858-frame envelope is wrong with it** -- which would itself be the finding,
and would mean the corpus simply never contained a release above CR.

**P2 -- SOMETHING IN THIS MATRIX SELECTS IT.** `body[30]` differs between at
least two arms. The candidates the matrix separates, named now so the reading
cannot be fitted later:
* the held mode (P1 says no, and arms R0-R5 are the test);
* `LCK$M_VALBLK` on the release (arm V against R4 -- SAME mode PW, only the
  flag differs);
* whether the lock has sub-locks (arm S against B -- same mode, same flags);
* whether this is the LAST holder of the resource (arm L against B).
**If body[30] is CONSTANT across all eleven arms, the determinant is outside
this matrix** and the arm's honest output is "not these four", with the
constant recorded as the value OVMX must write (which is already what the
census's bound says) -- a real answer, and the one the item's clause (c)
provides for.

**P3 -- body[32] FOLLOWS body[30], or it does not.** For every arm,
`body[32] == body[30]`. The corpus says so 9,858 times out of 9,858, and this
arm is where that stops being an incidental equality: if any arm breaks it,
body[32] is its own field and the builder must write it from whatever that arm
shows it follows -- **never from "it equalled the other cell in the corpus"**.

**P4 -- A SUB-LOCK's RELEASE IS A DIFFERENT FRAME FROM ITS ROOT'S.** Arm S
produces four releases (three sub-locks and the root). Their `body[30]` /
`body[32]` are read separately, and `body[36:44]` is read with them: rd vms-b67a
measured that a sub-lock REQUEST names its parent there, and whether a sub-lock
RELEASE does is unmeasured.

**P5 -- THE ROLE DOES NOT CHANGE THE ANSWER.** Arms V2 and S2 read the same
cells as V and S. A difference means the cells are a property of the SENDER or
the MASTER, not of the operation, and the whole reading has to be re-stated.

**P6 -- AND NOTHING CRASHES.** Zero
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR` on either
console across all eleven arms. These arms are real-VAX-to-real-VAX, so a crash
here is a lab fault and not OVMX's -- but it is recorded, because an unexplained
peer crash in the lab invalidates every reading taken after it.

## What this arm does NOT do

It does **not** put an OVMX node in the cluster, and it does **not** change
OVMX's builder. The builder change is rd vms-194's clause (c) and follows the
reading; the grounding-table row set is clause (d) and follows the builder; the
fire that sends an OVMX release at a real master is clause (e). Mixing any of
them into this capture would confound the one thing this arm measures.

## Files this directory must carry afterwards

| file | what it is |
|---|---|
| `RMDEQ.MAR` | the driver (committed NOW, before the pod) |
| `PLAN.md` | this file (committed NOW) |
| `ARM.md` | the readings, per arm, with P1-P6 scored |
| `dq-*.pcap` | one capture per arm, payloads unaltered |
| `sda-*.txt` | `SHOW LOCK` / `SHOW RESOURCE` on both nodes, per arm |
| `console-*.log` | both consoles, whole, for the whole run |

and the census tool grows a `--selftest` figure for whatever the arm shows, so
the reading cannot drift away from the captures it came from.
