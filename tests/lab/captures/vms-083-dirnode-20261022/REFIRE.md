# vms-083 RE-FIRE: the fix proven at two real VAXes — the one-sender tree now
# reaches HELD

Run 2026-10-22 12:00Z, same pod `lab083`, same two real OpenVMS VAX V7.3 nodes,
**the fixed executive**: fire branch `lab/vms-083-refire` at `f70b0d9c5` = the
implementation commit (`fea3ffd01`, PR #1891) plus the LAB-ONLY `RMHOLD.EXE`
staging commit. Artifacts `build-boot-artifacts.yml` run **39771822532**,
`cluster_auth_group=1` (`TUAC` v1 group 1 verified in the initramfs before
boot). `ovmx_fire.sh` matched `SOURCE_COMMIT ==
f70b0d9c53281efcccb912b61a061492e378769b` against a clean gated tree **rebuilt
at that commit** and the frame-conformance gate passed for `dlm_enq_req
dlm_rm_ack dlm_rm_ack_nm dlm_rm_14 dlm_deq`.

The five predictions were registered on `rd vms-083` **before** this fire ran
(the fire branch could not carry them: a commit on it would move HEAD off the
commit the artifacts were built from, and `ovmx_fire.sh` would refuse).

Fresh names, re-screened on the real nodes' own SDA because the OVMX node is a
new incarnation (OVMXE is `CSID 00010004` here, read back from VAX2's own
executive with `F$GETSYI("NODENAME",,"00010004")`):

| name | directory node | topology |
|---|---|---|
| `D83C0` | **VAX1** (= the old master; VAX2 has no RSB for it) | the arm-B discriminator |
| `D83C1` | **VAX2** (`DIRENTR VALID, CSID 00010001`) | the arm-A control |

## Scored

**R1 — PASS. The hand-over of a tree whose directory IS the old master reaches
HELD on ONE op-0x13.** `refire-frames.txt`:

```
f3721  OVMXE->VAX1  02/01  D83C0   asked DIRECTLY, no redirect (directory = VAX1)
f4072  VAX1->OVMXE  02/0e  D83C0   -> 82/15 by OVMXE (f4074)
f4075  VAX1->OVMXE  02/0f  D83C0   -> 82/15 (f4077)
f4079  VAX1->OVMXE  02/10  D83C0   -> 82/15 (f4082)
f4083  VAX1->OVMXE  02/13  D83C0   THE ONE end-of-records
f4085  OVMXE->VAX1  02/14  D83C0   ORIGINATED BY OVMX
f4086  VAX1->OVMXE  82/15  D83C0   and the REAL VAX ANSWERS IT 0xfa
```

Before the fix, the identical topology produced **zero** op-0x14 and the tree
never reached HELD (`FIRE.md` arm B, `f7341` and nothing after it).

**R2 — PASS.** OVMXE's own counters immediately after that run:
`trees opened=1 refused=0 records adopted=0 refused=0 ends=1` /
`trees held=1 op-0x14 sent=1 unsent=0 completions unreported=0`.

**R3 — PASS, no regression.** The arm-A control on the same executive:
`f5889` VAX1's 0x13 and `f5890` VAX2's 0x13 (the directory node), then OVMXE's
**two** op-0x14s `f5893`/`f5894`, both answered 82/15 (`f5895`, `f5897`).

Final counters over both runs: `trees opened=2 ends=3 trees held=2 op-0x14
sent=3 unsent=0`. The broken executive over the same two topologies read
`trees held=1 op-0x14 sent=2`. **That one tree is the whole fix.**

**R4 — PASS.** Zero `BUG`/`CNXMGRERR`/`INVLOCKID`/`INVEXCEPTN`/`LOCKMGRERR`/
`HALT`/`FATAL` across all three WHOLE consoles (`refire-console-vax1.log` 654
lines, `refire-console-vax2.log` 587, `refire-console-ovmxe.log` 140);
`F$GETSYI("CLUSTER_NODES")` = 3 on both VAXes at the end; every DCL step
returned. (The one VMS-correct interruption in the window was deliberate: the
previous fire's node was stopped to free the bridge, both VAXes said
`%CNXMAN, quorum lost, blocking activity`, and the new node's join restored
quorum and `CLUSTER_NODES` 3 — a membership event, not a crash.)

**R5 — PASS, and this is the half that cannot be faked.** With the tree adopted,
VAX2 — a real node that was **not** the old master — asked for the same root:

```
f4454  VAX2->VAX1   02/01  D83C0   asks the DIRECTORY
f4455  VAX1->VAX2   82/01  D83C0   0xf8 REDIRECT -- VAX1's directory entry no
                                   longer names VAX1
f4456  VAX2->OVMXE  02/01  D83C0   so it asks OVMX
```

and VAX2's own lock database, on its own SDA:

```
SDA> SHOW RESOURCE/NAME=D83C0
Lock Count:   1   CSID: 00010004          <- OVMXE, per VAX2's own F$GETSYI
Granted queue (Lock ID / Gr mode / Range):
 3C00037C  CR 00000000-FFFFFFFF
```

A real VAX holds a CR lock, on its own granted queue, on a resource its own
executive names OVMX as the master of — on a tree OVMX could not finish
adopting before this change. Its holder process sits in `HIB`
(`SHOW SYSTEM/PROCESS=VHOLD`: `2040021A VHOLD HIB`), which is where `RMHOLD`
goes only after every lock it asked for was granted.
