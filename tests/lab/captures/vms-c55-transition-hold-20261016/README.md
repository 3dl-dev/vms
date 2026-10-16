# vms-c55: OVMX sends lock traffic into a peer's rebuild — 3 of 3 crashes, 0 of 11 clean runs

Measured 2026-10-16 by the cluster lane. This page is the evidence behind the
TRANSITION HOLD in `src/kernel-core/vms_dlm_scs_fsm.c`. Every figure on it is
re-derived from the committed corpus by
`tools/cluster/transition_dlm_census.py --selftest`, which ctest runs
(`cluster_host_transition_dlm_census`), so none of it can quietly go stale.

## What is here

| file | |
|---|---|
| `crash-frames.jsonl` | the THIRD frame-level LOCKMGRERR specimen — openvmx-site run `38811843112`, job `gate (page-order-throttled4)`, ref `work/vms-c55-request-names-the-process` (PR #1730 head `416651090`), `capture=true` |
| `OVMXA/OVMXB/VAXC.console.log` | all three consoles; VAXC's carries the register dump |
| `result.json` | the visitor gate's own verdict for the case |

It is the recurrence that FALSIFIED the zero-PID-cell diagnosis: the request
that preceded this crash carried a NONZERO `body[20:24]` (`0x10000002`) and the
VAX bugchecked anyway, `PC = 80A07362`, byte-identical to the other five.

## The hypothesis this capture killed, stated first

The previous session's lead was "OVMX is a member the VAX admitted that
**silently refuses what it does not recognise**", and the arm was to find a
refused message type that provokes the `cat-0x02 op-0x19`. **It does not
exist.** Measured over the two committed crash captures and the twelve
case-runs of fires `38811815171`, `38811829040`, `38811843112`, `38814006004`
(frames + all three consoles each):

* `%DLM, refusing a lock message` appears **once in the whole corpus** — cat
  `0x02` op `0x19`, in the one crashing case-run.
* `%CNXMAN, an unroutable VMS$VAXcluster frame was received` — the other place
  an inbound frame can be dropped — appears **zero** times in all fourteen runs.
* Of everything a real VAX sends an OVMX node (`01/01,03,04,05,06,08,09,0a,0c,
  0f,12,14`; `02/01,04,0d,19`; `04/*`; `06/00`; `0b/01,06`; `81/0b`; `82/01`;
  `8b/05`), the only inbound frames with no answer behind them are the ones
  that are never answered — the `01/0a` GO and the `01/0c` RELEASE, both
  consumed by the barrier/join FSMs — and the `02/19` itself.

So the refusal set is `{02/19}`, and nothing OVMX refused can have provoked it.

## What DOES separate a crash from a clean run

**OVMX originates a cat-0x02 lock request while a cluster state transition is
running.** `transition_dlm_census.py emitter`:

| | OVMX cat-0x02 originated IN a transition | outside | bugcheck |
|---|---|---|---|
| `vms-c55-lockmgrerr-20261014` | `02/04@step1`, `02/01@step2` | 0 | **YES** |
| `vms-c55-lockmgrerr-20261015` | `02/04@step1`, `02/01@step2` | 0 | **YES** |
| this capture (`38811843112` page-order-throttled4) | `02/04@step1` | 1 | **YES** |
| the other eleven case-runs | **none** | 26 | no |

Twenty-six OVMX-originated lock requests reached a real VAX in clean runs and
none of them killed it. Three reached one *inside a transition* and all three
runs ended in LOCKMGRERR. The window is read from the cat-0x01 EPOCH at
`body[12:16]` and the barrier step at `body[16:20]` — `VMS_OFF_CM_EPOCH` and
`VMS_OFF_CM_STEP`, offsets this repo already grounds. No new field is read.

The crashing sequence, in barrier terms (`transition_dlm_census.py` + the
capture's own frames):

```
VAXC  01/09  epoch 3            the transition opens
VAXC  01/0a  epoch 3            the GO -- the point of no return
OVMXA 01/0b  step 1   -> 01/0c  step 1 released
OVMXA 02/04           <-------- the directory-entry REMOVAL, at STEP 1
VAXC  02/19           <-------- 9 ms later, the opcode that exists nowhere
                                else in this corpus
OVMXA 01/0b  step 2   -> 01/0c  step 2 released
OVMXA 02/01           <-------- the lookup, at STEP 2
VAXC  82/01 / HELLO, then LOCKMGRERR
```

## And a real VMS node does not do it

`transition_dlm_census.py steps`, over all 122 committed pcaps, real DEC-OUI
senders only. Every non-`0x0d` cat-0x02 frame a real node put inside a
transition window, by the barrier step in force:

| step | 10 | 11 | 12 | 6 | 1–5 |
|---|---|---|---|---|---|
| frames | 3 | 16 | 12 | 2 (one `02/08` + its answer) | **0** |

Real members resume lock traffic at the **tail** of a twelve-step barrier.
OVMX sent its removal at step **1**.

`transition_dlm_census.py census` on the one capture that holds a whole
barrier (`vms-20c-rebuild-1789489597/rejoin-rebuild.pcap`, 12 steps, 654
frames between the GO and the last release): inside the window there are
exactly **307 × `02/0d`** rebuild records and **307 × `82/0d`** echoes — and
no lookup, no removal, no release, no convert. Corpus-wide the same split
holds: `02/0d` 2797 inside / 107 outside, `82/0d` 1240 inside / 0 outside.

That is the published behaviour, measured: a state transition quiesces the
lock manager — Phase 1 discards all directory information cluster-wide and
Phase 2 rebuilds it (Davis, *VAXcluster Principles*, pp. 7-40/7-41, p. 6-33).

## Why OVMX did it

There was no stall. `dlm_arm_transition_begin()` only **counted** the
boundary, and `dq_may_transmit()` — the single gate every outbound lock frame
passes — had no transition condition. The quorum hang does not cover this and
says so: `vms_dlm_quorum.h` limit 1, "this node does not stall the POST of
such a request … deferring the post until resume is a queue this rung does not
build."

## What is NOT claimed

**That this is the cause of the LOCKMGRERR.** It is a correlate on the *cause*
side with the same discrimination as the `02/19` (3/3 vs 0/11), and it is
independently an unfaithfulness the corpus settles. What `cat-0x02 op-0x19` IS
remains unknown and is still not guessed: no answer frame for it has been
invented, RULE C has not been relaxed, and the removal has not been suppressed
outside a transition (rd vms-e09's 8,106 real removals stand). The demo-gate
fire scored against `PREDICTION.md` is what says whether the crash stops.
