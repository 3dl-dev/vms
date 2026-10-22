# rd vms-a4d7 / rd vms-28bd fire `a4d2` — **THE DEFERRED GRANT REACHED A REAL VMS LOCK MANAGER, AND THE REAL VAX'S PROCESS LEFT ITS WAIT. TWICE, ROLES SWAPPED.**

Scored against `PLAN.md`, which was committed before the OVMX node booted.
**Every one of the eight registered predictions is met.** Three earlier fires of
this same sequence (q85f1 00:31:44Z, q85f2 01:27:59Z, `d5cd1` 04:23Z) ended with
the engine reporting the flip and **no frame going out**.

## The run

| what | value |
|---|---|
| build under test | `lab/vms-a4d7-fire` head `f8f462511` = `work/vms-a4d7-grant-notify` (PR #1877) + the lab-only RMHOLD staging commit |
| artifacts | `build-boot-artifacts.yml` run **39737735927**, `SOURCE_COMMIT` `f8f462511c7d58be6198d03dd4d6fdf10a738a2d`, read back **out of the pod** by `ovmx_fire.sh` before the node booted |
| frame types gated | `dlm_enq_req dlm_enq_ans_queued dlm_grant_notify dlm_deq dlm_rm_ack dlm_rm_ack_nm dlm_rm_lock_ans dlm_rm_14` → `frame_conformance: every byte grounded`, negative control refused the corrupted frame |
| pod / bed | `laba4d7`; the whole bed is in `PRE.md`, recorded before the node booted |
| topology | the GENERAL configuration: OVMXE `LOCKDIRWT 0` (the executive's own `CNXTRACE` says `lockdirwt=0`), VAX1 and VAX2 `LOCKDIRWT 1`, **`CLUSTER_NODES` = 3** |
| capture | `wire.pcap.gz`, one `tcpdump -i br0 ether proto 0x6007` across both arms |

## ONE DEVIATION FROM THE PLAN, AND IT IS THE PLAN THAT WAS WRONG

`PLAN.md` step 2 carried an arrangement check inherited verbatim from fire
`d5cd1`: *"this request must reach VAX1 with NO directory redirect… If a `0xf8`
redirect appears… the arm is re-run on another fresh name."* **For this fire
that check is inverted, and it was not re-run.** Here is the reasoning, in the
open:

rd vms-5cd's own discriminator says a handed-over tree reaches **HELD** when
TWO members send an op-0x13 — which happens when the DIRECTORY node for the
name is a *different* node from the old master, i.e. exactly when OVMX's root
request IS redirected. `d5cd1` wanted the no-redirect arrangement because its
defect lived in the **not-HELD** state. rd vms-28bd needs the opposite: the
tree must reach HELD, or this node's own lock stays a PROXY and its release
takes the proxy path instead of the local-grant path the item is about.

`A4D1` was redirected (`f1512` OVMXE→VAX2 → `f1513` `0xf8` → `f1515`
OVMXE→VAX1 → `f1516` `0xfa`), the tree reached HELD, and the fire proceeded.
The deviation is recorded here rather than smoothed over, and the FIX is to the
plan's inherited sentence, not to this run. Both names tried are on record:
`A4D1` (arm 1) and `A4D2` (arm 2), and both were redirected.

## The hand-over actually happened, and OVMX reached HELD

Arm 1, `A4D1` (frame numbers are `wire.pcap.gz`'s):

```
f2119  VAX1  -> OVMXE  02/0e      f2121  OVMXE -> VAX1  82/15
f2122  VAX1  -> OVMXE  02/0f      f2126  OVMXE -> VAX1  82/15
f2128  VAX1  -> OVMXE  02/10      f2131  OVMXE -> VAX1  82/15
f2133  VAX1  -> OVMXE  02/13   \__ TWO op-0x13 senders: the old master AND
f2134  VAX2  -> OVMXE  02/13   /   the DIRECTORY node for the name
f2137  OVMXE -> VAX1   02/14      f2139  VAX1  -> OVMXE  82/15
f2138  OVMXE -> VAX2   02/14      f2140  VAX2  -> OVMXE  82/15
```

`CNXTRACE`: `trees held=1 op-0x14 sent=2 unsent=0` after arm 1, `trees held=2
op-0x14 sent=4 unsent=0` after both. This is the first time this lane has
recorded the two-op-0x13 case and the HELD state in the same run, and it
CONFIRMS rd vms-5cd's discriminator from the other side.

## The predictions, scored

| | prediction | result |
|---|---|---|
| **P1** | OVMXE **originates** a `cat-0x02 op-0x02` after the local release | **MET.** Arm 1 `f2525` → VAX2; arm 2 `f4371` → VAX1. Both AFTER their own queued answer (`f2287`, `f4214`), neither before. OVMXE originated `02/02 2` over the run and had **never** emitted one before (387 in the whole reference corpus, 0 from OVMX) |
| **P2** | its cells are what the executive holds | **MET, 2 of 2 on every registered cell.** `f2525` carries `body[20:24]=52030001` and `body[24:28]=ad030054` — byte-for-byte the pair of the `0xfb` queued answer `f2287`; `body[30]=body[31]=5` (the EX the request asked for); `body[28:30]=14 00`; `body[4:8]=0`; `body[47]=0`; `body[116:120]=body[124:128]=ffffffff`. Arm 2 the same with `54030001`/`b8020038` |
| **P3** | the arm's own ledger agrees | **MET.** `deferred grants sent=1 owed=0`, `local advances=1 stale=0` after arm 1; `sent=2 owed=0`, `advances=2 stale=0` after both. `DLMOWED: keep_refused=0 table_overflow=0 refused=0 send_failed=0` — every door zero, where `d5cd1` read `send_failed=1` |
| **P4** | the real VAX's process **leaves its wait and holds EX** | **MET, and independently on the VAX's own SDA.** `HOLD2` on VAX2 went **`LEF` → `HIB`**; SDA `SHOW RESOURCE/NAME=A4D1` lists lock **`540003AD` on the GRANTED queue at EX** with an EMPTY waiting queue and `CSID 00010003` (OVMXE) as master — and `540003AD` is byte-for-byte the handle the op-0x02 carried at `body[24:28]`. `STOP HOLD2` **completed**. Arm 2: `HOLD4` on VAX1 `LEF` → `HIB`, SDA lock `380002B8` granted EX, `STOP HOLD4` completed |
| **P5** | never-crash | **MET.** Zero `bugcheck\|CPUSPINWAIT\|fatal\|VAXPORT\|panic\|CNXMGRERR\|INVLOCKID\|LOCKMGRERR\|INVEXCEPTN` across all three WHOLE consoles (1,198 lines) |
| **P6** | the echo does not appear | **MET.** 0 OVMXE-originated `0xfa` grants naming a handle pair this node had queued. The shape the corpus has no instance of has no instance here |
| **P7** | it is a notification | **MET.** `body[4:8]=0` on both, and **0** `cat-0x82 op-0x02` from anybody: nobody answers one, so no transaction is left open on the CSB |
| **P8** | the second arm behaves identically with the roles swapped | **MET.** VAX2 as old master, VAX1 as the queued EX requester, a second fresh name, a different grant destination — same result on every cell and every counter |

## What this fire closes, and what it does not

**CLOSES** rd vms-28bd's open half and rd vms-a4d7: the deferred grant is a
real wire shape, it reaches a real V7.3 lock manager, that lock manager grants
the lock to its own process, and the process that sat unkillable in `RWSCS` on
2026-10-09 and 2026-10-21 now leaves a normal `LEF` wait with `STOP` working.

**DOES NOT CLAIM** anything about `body[34]`, the residue spans, or what makes
the two mode cells differ when they do (41 of 267 in the CONVERT-correlated
population). Each stays recorded-and-not-explained in
`tools/cluster/dlm_late_grant_census.py` (Rule 8).

**ONE GATE GAP FOUND, FILED AS rd vms-daa.** The first boot of this fire
(`a4d1`, set aside in the pod as `/lab/run-fire/a4d1-group0-aborted`) never
joined: `build-boot-artifacts.yml` ships an empty `/etc/ovmx`, so the node ran
on cluster **group 0** and transmitted 113 frames to `ab:00:04:01:00:01` while
the VAXes used `ab:00:04:01:01:01`. `ovmx_fire.sh` checks `SOURCE_COMMIT` and
the conformance gate but not this, and the failure mode is a node that boots
perfectly and silently never joins — which reads exactly like the defect under
test. Worked around with `tests/lab/tools/stage_cluster_group.sh 1`, the group
number read from VMS itself (`SYSMAN> CONFIGURATION SHOW
CLUSTER_AUTHORIZATION` on VAX1: *"Cluster group number: 1 / Multicast address:
AB-00-04-01-01-01"*), never back-derived.
