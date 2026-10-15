# Pre-registration: can two REAL VAXes be made to exchange a cat-0x02 op-0x19?

Written and committed BEFORE the arm is fired. rd vms-8cb5, the last open
question behind rd vms-c55.

## Why this arm exists

OVMX refusing a `cat-0x02 op-0x19` is present in **5 of 5 LOCKMGRERR crashes and
0 of 30 clean case-runs** — the strongest and only unexplained signal in the
whole investigation. The opcode appears **three times in the entire committed
corpus and all three are VAXC → OVMXA**, one per crash capture. Zero
VAX-to-VAX in 122 captures. So a real VAX sends it only to OVMX.

We may not guess what it means. The only clean-room way to learn it is to watch
a real VAX send one to another real VAX and see what comes back.

## What is already ruled out, so this arm does not repeat it

**The trigger is not in the removal's bytes.** Every byte of OVMX's 17 named
op-0x04 directory removals was diffed against 5,190 real ones. Every offset
that differs is accounted for by the send/ack envelope, by the resource being a
different one (`RMS$`/`F11B$s` vs `F11B$a`: group, mode, name length, name), or
by residue spans OVMX deliberately zeroes (rd vms-42de). Nothing else differs.

**The DLM rebuild sequence is not stale.** `body[14:16]` looked wrong when
pooled across captures (OVMX 1, real 2/3/4) — but in BOTH crash captures OVMXA
and VAXC stamp **seq=2**, agreeing. Dead.

So the trigger is **state or timing**, and the arm must reproduce the
*situation*, not replay bytes.

## The rig

`ovmx-lab` pod `vaxlab-2` — two REAL OpenVMS VAX V7.3 nodes, `vax1` and `vax2`,
already booted, idle since 19:52, with the cluster-shared volume `VAX1DATA` on
`$2$DUA1`. No pod is created and none is rebooted: this is a reserved shared
oracle node used for exactly what it is reserved for, a genuinely-gated
capture that cannot be taken anywhere else. Driven only through
`ovmx-lab-console send`, read only through `ovmx-lab-console read`.

*Constraint noted honestly:* the standing rule "lab pods only in ovmx-lab with
requests == limits" is about pods a lane creates. The `vaxlab` statefulset
predates this lane and has `requests != limits`; scaling it to add a sixth
replica would both inherit that spec and restart nothing but would still be
creating a non-conforming pod, so this arm uses an existing idle node instead
and creates nothing.

## The workload

The cycle OVMX performs, performed by two real VAXes: a node repeatedly takes
and releases a lock on a resource the OTHER node masters, so that each release
sends a directory-entry removal across the wire and each re-acquire sends a
lookup back. From DCL that is a file create/delete loop on the cluster-shared
volume, which makes the XQP take and drop `F11B$` locks and RMS take `RMS$`
locks — the same two families OVMX's removals name.

`tcpdump -i br0 'ether proto 0x6007'` inside the pod for the whole window.

## Predictions, pre-registered

**P1 — the baseline holds.** With both nodes idle the capture contains **zero**
`cat-0x02 op-0x19` frames. If one appears before the workload starts, the
opcode is not workload-related and everything below is void.

**P2 — the discriminating one.** Under a sustained removal-heavy workload
between two real VAXes, either:

- **(a) a `cat-0x02 op-0x19` appears.** Then capture it with its surrounding
  frames and, crucially, **what the recipient answers** — that answer is the
  grounded oracle OVMX's handling must be built from, and nothing may be
  written into OVMX before it exists. Record what preceded it, since that is
  the trigger we have never seen.
- **(b) none appears** after a soak that generates **more directory removals
  than the OVMX runs that drew one** (the crash specimens drew a 02/19 within
  27 ms of a *single* removal; a soak of some thousands is decisive). Then the
  VAX-to-VAX framing is **exhausted**: the opcode is elicited by something only
  OVMX does, and the next arm is OVMX-to-VAX with one variable.

**P3 — no peer is harmed.** Both VAX consoles end the run with no bugcheck and
no `%CNXMAN` error. This is a read-only observation of a normal VMS workload;
if it crashes a real VAX, that is itself a finding and the arm stops.

## What would make this arm worthless

Running it against an OVMX node present on the same wire. The whole point is
two REAL VAXes and nothing else, so the capture must be taken with no OVMX
member in the cluster. The frame census afterwards must confirm that: every
`0x6007` source MAC in the capture is one of the two VAXes.

## The three don'ts, restated from the item

1. Do not invent an answer frame for `op-0x19`.
2. Do not relax RULE C to let it through.
3. Do not suppress the `op-0x04` removal to dodge it — the corpus says the
   removal is correct behaviour (rd vms-e09, 8,106 real removals).
