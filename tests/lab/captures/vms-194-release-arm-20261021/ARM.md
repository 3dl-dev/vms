# vms-194 (a): the driven RELEASE arm, scored. body[30] is NOT the lock mode.

Pod `vaxlab-4` in `ovmx-lab`, 2026-10-21 22:54-22:59 UTC. Two real OpenVMS VAX
V7.3 nodes, **no OVMX node** (`CLUSTER_NODES` read 2 on both consoles before
and after, `LOCKDIRWT` 1 on VAX1, `LOCKDIRWT` 1 and SCSSYSTEMID 1026 on VAX2).
VAX1 is `aa-00-04-00-01-04` (`0104` below), VAX2 `aa-00-04-00-a3-96` (`a396`).

**Substrate provenance, recorded rather than left out.** That pod runs
`ovmx-vaxlab:5`, not the `:7` its manifest names -- as every shared `vaxlab`
replica did at the time, which is filed as rd vms-386. `:5`'s headline absence
is the SIMH `set clock nocatchup` fix (rd vms-549c), whose failure mode is a
host-scheduling stall presenting as a VAX hang. **This arm is not
timing-sensitive**: it reads the CONTENT of nine frames, one variable at a
time, and a stall shows up as a hang rather than as different bytes. Nothing
hung (both consoles run clean and the cluster read 2 nodes afterwards), and a
fire whose predictions turn on a WAIT STATE -- rd vms-85f's -- was deliberately
NOT moved onto this pod for the same reason.

Scored against [`PLAN.md`](PLAN.md) and its **AMENDMENT 1**, both committed
before the capture existed (`6b42369e2` and `fe27b48b4`). The predictions were
not re-cut.

Driver: the **committed** `DLMENQ.EXE`
([`../vms-c03-dlm-opcodes-20260911/dlm-drivers/DLMENQ.MAR`](../vms-c03-dlm-opcodes-20260911/dlm-drivers/DLMENQ.MAR)),
already assembled on the lab volume. `DLMENQ <mode>2<name>` acquires and
HOLDS; `DLMENQ <mode>0<name>` acquires and `$DEQ`s. The holder runs on the node
that must MASTER the tree, so every release crosses the wire.

## The arms, as they ran

Each arm has its own resource name, so no arm inherits a resource block, a
value block or a master decision from the one before it. The holder is an
NL lock on the master node; the driver acquires at the arm's mode and releases.

| arm | holder (master) | driver | the one variable |
|---|---|---|---|
| B | VAX2 `02DQB` | VAX1 `10DQB` | baseline, CR |
| R0 | VAX2 `02DQR0` | VAX1 `00DQR0` | held mode NL |
| R2 | VAX2 `02DQR2` | VAX1 `20DQR2` | held mode CW |
| R3 | VAX2 `02DQR3` | VAX1 `30DQR3` | held mode PR |
| R4 | VAX2 `02DQR4` | VAX1 `40DQR4` | held mode PW |
| R5 | VAX2 `02DQR5` | VAX1 `50DQR5` | held mode EX |
| L | VAX2 `02DQL` **and** `12DQL` | VAX1 `10DQL` | a co-holder at a NON-NULL mode |
| B2 | VAX1 `02DQB2` | VAX2 `10DQB2` | the roles swapped |
| R5B | VAX1 `02DQR5B` | VAX2 `50DQR5B` | the roles swapped, at EX |

## How a release is attributed to an arm

A release carries **no resource name** -- the codec refuses to read one,
because the bytes a real `$DEQ` leaves at `body[46:]` belong to whatever last
used the buffer. So each release is attributed through the **handle pair its
own grant carried**: the granted `cat-0x82 op-0x01` names (master lkid,
requester lkid) at `body[20:28]`, and the release names the same two longwords.
That is the requester's own pair, not a reading of a name.

The attribution is **committed, not a throwaway script**:
`tools/cluster/dlm_deq_census.py --arm <pcap>`, with every figure below pinned
in its `--selftest` (ctest `cluster_host_dlm_deq_census`).

```
=== rd vms-194 driven RELEASE arm: dq-modes.pcap
arm      sender   held mode  b[30]  b[32]
DQR0     0104     0          0      0
DQB      0104     1          0      0
DQL      0104     1          0      0
DQB2     a396     1          0      0
DQR2     0104     2          0      0
DQR3     0104     3          0      0
DQR4     0104     4          0      0
DQR5     0104     5          0      0
DQR5B    a396     5          0      0

attributed releases in the whole capture: 89; body[32] == body[30] on 89
```

## Scoring

**P1 -- body[30] IS NOT THE MODE. PASS, and more strongly than predicted.**
P1 predicted "at most two distinct values, drawn from {0,1}". Over the six
held modes the cell is **CONSTANT at 0** -- including arm R5, a real VAX
releasing an **EX** lock, where OVMX's builder wrote **5**. A lock mode runs
0..5 and this cell does not.

**P2 -- SOMETHING IN THIS MATRIX SELECTS IT. FAIL, honestly, and that is the
answer the item's clause (c) provides for.** `body[30]` is 0 in all nine arms:
the held mode does not select it (R0-R5), a non-NULL co-holder does not (L
against B), and the role does not (B2/R5B). **The determinant is outside the
part of the matrix this pass drove.** The two remaining candidates are the ones
the amendment deferred: `LCK$M_VALBLK` on the release (arm V) and sub-locks
under the root (arm S), which `DLMENQ` cannot express.

*One measurement about those two, stated as a measurement and not as a rule.*
Within this capture every release whose request named a PARENT carried 1 (73 of
73) and every root release carried 0 (16 of 16) -- and in this capture the
sub-locks are exactly the locks with a value block, so the two are confounded.
**Corpus-wide neither holds**: over 39,582 releases attributed the same way
across 157 captures, "sub-lock" agrees with the 1 on 89.8% and "has a value
block" on 96.9%. A 96.9% rule is not a rule, this corpus-wide attribution is
itself noisier than the arm's (handles are reused over a long capture), and
Rule 8 forbids inferring the cell's meaning from values. So it is recorded here
as what would be TESTED next, not as what the cell is.

**P3 -- body[32] FOLLOWS body[30]. PASS.** Equal in all nine arms, and in
**89 of 89** attributed releases in the capture -- which includes the 73 that
carry 1, so the equality is not an artefact of both being 0. The corpus says
the same thing 41,049 times out of 41,049.

**P4 -- a sub-lock's release. NOT SCORED: it belongs to the deferred arm S.**

**P5 -- THE ROLE DOES NOT CHANGE THE ANSWER. PASS.** Arms B2 and R5B are driven
from VAX2 against a VAX1 master -- the sender, the master and the directory node
all change places -- and read the same 0, 0. The cell is a property of the
operation, not of the node.

**P6 -- NOTHING CRASHES. PASS, counted.** Zero
`bug check|bugcheck|CPUSPINWAIT|fatal|VAXPORT|panic|CNXMGRERR` lines on both
consoles over the whole run, and `F$GETSYI("CLUSTER_NODES")` reads 2 on both
nodes afterwards. These arms are real-VAX-to-real-VAX; no OVMX frame was on
this wire.

**And D1's acceptance evidence, which the plan says this capture also carries:**
`body[4:8]` is **0 on all 89** releases in this capture -- no real release opens
a transaction, confirmed on fresh frames taken after the D1 fix was written.

## What this arm changed in the executive

`vms_dlm_deq_build()` wrote the LKB's lock mode at `body[30]`; it now writes the
measured `VMS_DLM_DEQ_REL_CELL` (0) at `body[30]` **and** at its mirror
`body[32]`, and the arm (`dq_build_deq` in `vms_dlm_scs_fsm.c`) no longer plumbs
`p->lkmode` into a frame at all. `struct vms_dlm_deq`'s field is renamed
`rel_cell` and documented PARSE-ONLY: it reports what an inbound release
carried, and the builder deliberately does not consult it, because echoing it
would put a peer's byte on this node's wire.

## Files

| file | what it is |
|---|---|
| `PLAN.md` | the predictions + AMENDMENT 1, both committed before the capture |
| `RMDEQ.MAR` | the driver the DEFERRED arms V and S still need |
| `dq-modes.pcap` | the capture, payloads unaltered |
| `console-vax1.log`, `console-vax2.log` | both consoles, whole |
| `ARM.md` | this file |
