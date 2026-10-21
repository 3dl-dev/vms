# vms-c2b: a real VAX master's SDA lists a lock held FOR the OVMX node

**Registered 2026-10-21, BEFORE the OVMX node is booted into the pod and before
any byte of this capture is taken.** This is RA7's (rd vms-6ad1) P0, and RA7
stops if it does not hold.

Fire branch: `lab/vms-c2b-rmhold` at `2de669d99` (RMHOLD.C + its recipe, plus
the LAB-ONLY commit that stages `RMHOLD.EXE` in the bootable image). The boot
artifacts are built from exactly that commit by `build-boot-artifacts.yml` run
`39640670084`, which writes `SOURCE_COMMIT` beside them;
`tests/lab/tools/ovmx_fire.sh` REFUSES to start if the gated tree is not that
commit or is dirty.

## The question

Every committed capture that shows a cross-node lock between OVMX and a real
VAX shows it in ONE direction: **OVMX as the MASTER**, a real VAX as the holder
(`vms-ci6-evac-20261010`: `SHOW RESOURCE/NAME=EVAC$WORKLOAD` on VAX1 reads
`master CSID 00010003 = OVMXE`). Nothing shows a real VAX master's lock
database carrying an LKB held FOR the OVMX node.

That direction is what RA7 needs, because **OVMX does not choose to become a
tree's new master** -- the op-0x0e arrives addressed to it, and every controlled
arm (RA2/RA4/RA5/RA8) shows the new master is a node that HOLDS a lock in the
tree. So this arm fires OVMX's REQUESTER path at a real VAX master and reads
the answer on the VAX's own lock database.

The code under test is shipping, not new: `vms_dlm_shape_fit_for_any_member()`
admits op-0x01 toward an unproven member, the requester FSM
(`vms_dlm_scs_fsm.c`) addresses the directory from the LEARNED or derived hash
and then the master the answer names, and an inbound cat-0x82 from a system not
proven ours is accepted ONLY when the FSM matches it to a request by a handle
THIS executive minted. What is new is `RMHOLD.EXE` -- the program that makes an
OVMX node ASK for a lock in a named tree at a chosen mode, which no committed
OVMX image could do (EVACWL's name and its EX mode are both fixed).

## The bed

Own pod `labc2b` in `ovmx-lab` (`requests == limits` 1500m/2Gi, image
`ovmx-vaxlab:7`), **deleted after the run**. Two real OpenVMS VAX V7.3 nodes and
one OVMX node on the pod's own `br0`.

Read, not assumed, before the fire (2026-10-21 17:0x UTC):
`F$GETSYI("CLUSTER_NODES")` = 2 on both VAXes and `F$GETSYI("LOCKDIRWT")` = 1 on
both. The OVMX node boots with **LOCKDIRWT 0** -- the GENERAL configuration (rd
vms-b5b0: the derived hash routes the lookup), the inverse of the interim one
every prior evacuation run used, and the only arrangement in which a real VAX
can be the tree's directory node AND its master.

| | role |
|---|---|
| VAX1 (`10.2.1.1`, SCSSYSTEMID 1025) | tree's FIRST holder, therefore its MASTER; LOCKDIRWT 1 |
| VAX2 (SCSSYSTEMID 1026) | second real member, LOCKDIRWT 1 |
| OVMXE (SCSSYSTEMID 1030) | the HOLDER under test, LOCKDIRWT 0 |

The tree: `RA7T`, taken by VAX1's `RMHOLD 123WRA7T` (root CR, three sub-locks
S0..S2 at CW, value block `RMLVB-0123456789` written on the down-conversion
from EX). Already running and already read on VAX1's SDA before the OVMX node
exists:

```
SDA> SHOW RESOURCE/NAME=RA7T
Address of RSB:  879B45C0  GGMODE: CR  Status: VALID
Sub-RSB count:          3  CSID:   00000000      <- mastered HERE, on VAX1
Lock Count:             1
Resource: RA7T  Valblk: RMLVB-0123456789  Seqnum: 00000001
Granted queue: 20000684  CR 00000000-FFFFFFFF
```

The fire is `ovmx_fire.sh labc2b <artifacts> c2b <build> dlm_enq_req` -- the
frame-conformance gate must pass for `dlm_enq_req`, the shape this fire puts on
a real VAX's wire, before the node boots.

## The steps

1. **S1 root only.** On OVMXE: `RMHOLD :== $SYS$SYSTEM:RMHOLD.EXE` then
   `RMHOLD 100-RA7T` -- the root `RA7T` at CR, no sub-locks. CR is compatible
   with VAX1's CR, so a grant is the honest expectation and a REFUSAL would be a
   real finding, not a tuning problem.
2. **S2 with sub-locks.** `RMHOLD 123-RA7T` -- root CR plus S0..S2 at CW under
   PARID. Separate from S1 on purpose: a `$ENQ` under a PROXY parent is named at
   the root instead (`vms_enq_core_ex`), a path no arm has fired at a real
   master, so if it fails S1 still stands on its own.
3. Read the truth on BOTH sides while the locks are held: `SHOW RESOURCE/NAME=RA7T`
   and `SHOW LOCK` on VAX1 (the master) and on VAX2 (the bystander), and
   RMHOLD's own `$GETLKIW` report on OVMXE.

## Pre-registered predictions, each scored PASS/FAIL on its own

**P1 -- OVMX asks the right node.** The capture shows OVMXE sending a cat-0x02
op-0x01 for `RA7T` to a real VAX (the directory node for the name), and an
inbound cat-0x82 op-0x01 answer from a real VAX. No op-0x01 for `RA7T` is sent
to a node that is not a member.

**P2 -- the grant is real, on the VAX's own database.** `SDA> SHOW
RESOURCE/NAME=RA7T` on VAX1 still reads `CSID 00000000` (VAX1 masters it) and
its granted queue now carries a SECOND lock whose `SHOW LOCK` names a CSID that
is OVMXE's. **This is the item's done-condition.**

**P3 -- the OVMX side agrees, from its own lock database.** RMHOLD's
`$GETLKIW` report on OVMXE prints the root lock GRANTED at CR with the resource
name `RA7T`, and the lock id it prints is the one the capture's op-0x01 carried
as the requester's handle. A lock id that appears nowhere in the capture, or a
granted mode that disagrees with SDA, is an INV-6 failure and is reported as one.

**P4 -- the value block crossed.** OVMXE's granted root carries VAX1's value
block `RMLVB-0123456789` (RMHOLD requests the root with LCK$M_VALBLK and prints
nothing it did not get; the block is checked in the capture's answer and on
OVMX's console).

**P5 -- the sub-locks (S2).** Either all three S0..S2 are granted at CW and
VAX1's SDA shows three more LKBs for OVMXE, or the $ENQ fails with an honest
status that is printed. A sub-lock reported granted on OVMX that VAX1's SDA does
not list is a FAIL and stops the arm.

**P6 -- nothing breaks.** Zero bugchecks on either VAX console and none on
OVMXE, every DCL step returns, `CLUSTER_NODES` is 3 on both VAXes during the
hold, and no VAX DLM or CM request to OVMXE is left unanswered in the pcap.

**P7 -- the bystander.** VAX2's SDA shows `RA7T` as a resource whose master is
VAX1 (not OVMXE), i.e. OVMX holding a lock did not move the mastery.

## FAIL / STOP conditions, stated before the fire

- **If P2 does not hold, RA7 does not run**, and what blocks it is OVMX's
  requester path at a real VAX master -- not the residue question. Report that,
  and stop.
- **If a real VAX bugchecks**, the fire is a WIN (a crash vector found in a lab)
  and a release-gating defect: capture it, reset the pod, and it gets its own
  item before anything else moves.
- A prediction that fails is **the result**, not a reason to re-cut the
  prediction.

## Lab discipline

- Crash-check BOTH VAX consoles FIRST, before the OVMX node boots.
- Consoles only via `ovmx-lab-console`. Never auto-reboot a non-SYS0 SIMH VAX.
- Delete the pod when the run ends; remove the worktrees and build dirs.
