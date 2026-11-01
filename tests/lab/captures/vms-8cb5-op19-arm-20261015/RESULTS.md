# Result: two real VAXes will not produce a cat-0x02 op-0x19. P2(b).

Scored against `PREDICTION.md` in this directory, which was committed and
pushed **before** the arm was fired (`691163252`).

## The fire

`ovmx-lab` pod `vaxlab-2`, the two real OpenVMS VAX V7.3 nodes `vax1` and
`vax2`, already booted and idle. No pod created, none rebooted; driven only
through `ovmx-lab-console`. `tcpdump -i br0 'ether proto 0x6007'` for the whole
window.

Workload: the cycle OVMX performs, performed by real VAXes — a DCL loop of
`OPEN/WRITE` → `WRITE` → `CLOSE` → `DELETE` on the cluster-shared volume
`$2$DUA1` (`VAX1DATA`, `Mnt Cnt 2`), 1500 iterations, **run from each node in
turn** so the removals cross the wire in both directions and neither node's
role is the only one tested.

## Scorecard

| | |
|---|---|
| SCA frames captured | **79,222** |
| senders | `aa:00:04:00:01:04` (VAX1) ×42,230 and `08:00:2b:1e:85:61` (VAX2) ×36,992 — **and nothing else** |
| cat-0x02 op-0x04 **directory removals** | **12,112** |
| cat-0x02 op-0x01 lookups / 82/01 answers | 20,562 / 20,567 |
| **cat-0x02 op-0x19** | **0** |
| bugchecks on either VAX | **0** |

**P1 held** — the idle baseline (`op19-arm-8cb5.pcap`, first window) carried 16
SCA frames, 2 removals and zero op-0x19 before any workload.

**P2(b).** The crash specimens drew a `02/19` within 9–27 ms of a **single**
removal, three times out of three. Here **12,112 real removals, from both
nodes, drew none.** The VAX-to-VAX framing is exhausted: `op-0x19` is elicited
by something only OVMX does, not by the removal-then-ask cycle itself.

**P3 held** — both consoles finished clean, `BUG CHECK` count 0 on each. The
arm harmed no peer.

**The validity condition held**: every `0x6007` source MAC in the capture is one
of the two VAXes. No OVMX node was on the wire.

## What is in this directory, and why it is worth keeping

`vax-to-vax-removal-cycle.pcap` — the first 2,000 cat-0x02/0x82 frames from the
first directory removal onward (171 removals, 666 lookups and their 666
answers, 492 releases). The 15 MB soak is not worth committing; **the cycle is**.
This is the first capture of the exact remove-then-ask cycle OVMX performs,
performed by two real VAXes at scale, so OVMX's cycle can now be diffed against
a real one frame by frame rather than against a handful of incidental removals.

## What this leaves for rd vms-8cb5

Three things are now ruled out without guessing at the opcode:

1. the trigger is **not in the removal's bytes** (every byte diffed against
   5,190 real removals; all differences explained by envelope, resource
   identity, or residue OVMX deliberately zeroes);
2. the DLM rebuild sequence is **not stale** (both nodes stamp seq=2 in both
   crash captures);
3. the cycle itself **does not provoke it** between real nodes — 12,112
   removals, both roles, zero.

So the next arm is **OVMX-to-VAX with one variable**, and the variable is not
the frames OVMX sends — it is what OVMX *is*: a member the VAX has admitted
that refuses every lock message it does not recognise. The honest next question
is whether the VAX's `02/19` goes unanswered only because of RULE C, and what
changes if the refusal is observable to the VAX rather than silent.

**The three don'ts still stand** and this result does not weaken them: no
invented answer frame, no relaxing RULE C, no suppressing the removal. Nothing
may be written into OVMX's handling of `op-0x19` until a real VAX has been seen
to answer one.
