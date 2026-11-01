# vms-c55, second frame-level specimen: a real OpenVMS VAX V7.3 still bugchecks LOCKMGRERR on main

RAW EVIDENCE + the measurements taken from it. Collected 2026-10-15 by the
cluster lane. This is the SECOND capture of this crash and the first one taken
with the two fixes already merged, so what it settles is that they did not
settle it.

## The fire

openvmx-site run `38779035174`, job `gate (all-at-once)`, `capture=true`. The
build under test is a clone of `3dl-dev/vms` **main at 2026-10-15T17:38:46Z**,
which carries all three of:

| commit | PR | what it did |
|---|---|---|
| `852ed5be5` | #1714 | the diagnostic that names the refused opcode |
| `7c024b5e1` | #1717 | the conformance ratchet, 1 -> 526 covered |
| `86ac250be` | #1715 | this node never tells a directory to forget a resource and then asks who masters it (the STAGED-removal drop) |

Nine case-runs were fired: three runs (`38779035174`, `38779039044`,
`38779043209`) x three cases (`b-first`, `all-at-once`, `page-order-throttled4`
-- the three join orders a LOCKMGRERR has been seen in).

| | bugcheck | refused `cat 0x02 op 0x19` |
|---|---|---|
| r1 `all-at-once` | **LOCKMGRERR** | yes |
| the other eight | 0 | 0 |

The refusal/crash correlation from the earlier arms therefore holds here too,
and now stands at 4 crashes / 4 refusals and 0 / 22 non-crashing case-runs.

**CONFOUND, STATED.** OVMXB announced `V0.7-7` in all nine (the cached Node B,
rd vms-c55's earlier note), so no run is the build under test end to end.
OVMXA -- the node whose frames are in this crash -- announced `V0.7-10` and IS
the build under test. `summary.json` carries the gate's own
`version_why` line.

## What is here

| file | |
|---|---|
| `crash-frames-204.jsonl` | the 384 204-byte VMS$VAXcluster frames of the run (the 134-byte HELLO traffic is dropped) |
| `OVMXA/OVMXB/VAXC.console.log` | all three consoles, VAXC's carrying the register dump |
| `summary.json` | the visitor gate's own verdict for the case |

Read it with `tools/cluster/capture_conformance.py` (which shapes OVMX emitted
and whether any row describes them) and with
`tests/lab/captures/vms-c55-lockmgrerr-20261014/lastframes.py` (what the victim
was handed last).

## The crash, frame for frame

The DLM conversation is four frames, and it is the SAME shape as the 2026-10-14
`b-first` specimen in a DIFFERENT join order:

```
71.081  OVMXA -> VAXC  02/04  directory-entry REMOVAL, RMS$4F.1.0, hash 650412f2
71.090  VAXC  -> OVMXA 02/19  refused by OVMX ("has not proved it runs this
                              implementation") -- an opcode that appears ZERO
                              times VAX-to-VAX in this repo's whole corpus
71.147  OVMXA -> VAXC  02/01  a lookup for the SAME name and hash, body[20:24]=0
71.176  VAXC  -> OVMXA 82/01  body[34] = 0xfe
71.231  VAXC's last frame (a HELLO); then LOCKMGRERR, PC 80A07362
```

`VAXC`'s own clock runs 44.34 s ahead of the capture's (an OPCOM line quoted
inside a frame at capture 17:55:05.674 is stamped 17:55:50.02), so the removal
lands at VAX-clock 17:55:52.3 -- between `received VAXcluster membership request
from system OVMXB` (51.99) and `completing VAXcluster state transition` (53.79).
The register dump is identical to all four known crashes: `PC = 80A07362`,
`R0 = 1`, `R1 = 0x84` (132, the SYSAP body length), `R4 = 80828780`,
`R7 = 0x34`, `PSL = 04080000`, `Current process = NULL`.

## #1715 did not and could not apply here

#1715 drops a **staged** directory removal when a request for that resource is
posted. In this specimen the removal was already on the wire 66 ms before the
lookup was posted, so there was nothing staged left to drop. The pair is not a
race inside one node's queue; it is the normal VMS cycle, and a real VAX does it
too -- in a clean run of the same case `VAXC` asked and removed the same name 66
times, and OVMXA asked `RMS$4F.1.0` 26 times and removed it 12 times. **Remove-
then-ask is not the defect.**

## What IS measurably different about OVMX's request

`body[20:24]` (abs 92). Across the nine case-runs above:

| sender | op-0x01 requests with a nonzero cell | with a ZERO cell |
|---|---|---|
| OVMXA (OVMX) | 0 | **181** |
| VAXC (real V7.3) | 3941 | 26 |

and across every committed capture in `tests/lab/captures`, for real VMS senders
only (DEC OUI `08:00:2b` and `aa:00:04`):

| opcode | PID-form (high byte 0x20, low word < 0x1000) | other (a lock id) | zero |
|---|---|---|---|
| 0x01 ENQ/lookup | **20,365** | 1,116 | 15 (0.07%) |
| 0x03 DEQ | 72 | 9,785 | 0 |
| 0x07 CONVERT | 1 | 3,246 | 0 |

The cell is **per-process, not per-resource**: in
`vms-20c-rebuild-1789489597/rejoin-rebuild.pcap` one value is used with 483
distinct resource names (and others with 90, 72, 38), while 859 names are only
ever asked with one value. A per-resource handle cannot do that. This agrees
with what `src/kernel-core/vms_cluster_codec_dlm.h` already says in prose -- "a
fresh op-01 request carries a PID-form placeholder here" -- and the builder
simply never wrote one.

## And what a real directory ANSWERS a zero cell

The directory answer echoes `body[10:28]` from the request, so one frame carries
both the question's cell and the outcome. Over every committed capture:

| the echoed `body[20:24]` | `body[34]` outcomes |
|---|---|
| nonzero (19,370 answers) | 0xf8 x1004, 0xf9 x5128, 0xfa x13184, 0xfb x39, 0xfc x15 -- **no 0xfe** |
| zero (44 answers) | 0xf9 x32, **0xfe x12** |

(answers from real VMS directories only; OVMX's own answers are excluded, which
is the point -- `tools/cluster/dlm_pid_cell_census.py answers`, and its
`--selftest` pins every figure on this page against the committed corpus.)

0xfe answers a zero cell and nothing else, 12 of 12 -- and a 0xfe is the last
DLM frame the VAX sent before the LOCKMGRERR in BOTH frame-level specimens.

That is what the fix in this change set acts on: the requesting process's id is
read off its PCB at post time and the codec refuses to build a request that can
state neither a master handle nor a requester.

## Still not known, and not guessed

**What cat-0x02 op-0x19 IS.** The whole corpus contains exactly TWO op-0x19
frames and both are VAXC -> OVMXA, one in each of the two crash captures: zero
VAX-to-VAX, so a real VAX sends it only to OVMX, i.e. OVMX reaches a state a
real member never puts a VAX in.

What is measured about the frame, and no more: it arrives on the same
VMS$VAXcluster Con.ID (`050070b1 0800e8db`) as the DLM answers, so it is on the
lock connection and not another SYSAP's; `body[16:20]` is 0x220; `body[20:24]`
is a small count (7 here); and `body[24:40]` reads as the 16-byte space-padded
name `DISK_CL_DRVR`.

Do NOT read a family into those last three. A census of every committed capture
finds `body[16:20] == 0x220` in 10,000+ frames spread over eighteen different
(category, opcode) pairs -- cat-0x01 op-0x06 most of all, but also 02/0d, 82/0d,
01/05, 01/03, 02/03, 02/04, 04/06 and more -- so the value at that offset is not
a marker of anything, and the 16-byte "name" is sometimes a real SYSAP or
process name (`DIRECTORY`, `DIR_LOOKUP`, `DISK_CL_DRVR`) and sometimes OPCOM
text left in the sender's buffer. An earlier draft of this page called the
op-0x19 "the same three fields as" the op-0x06 burst; that was an overstatement
and this corrects it.

Answering what op-0x19 means needs a VAX-to-VAX oracle, not a reading of one
frame.
