# vms-083: WHICH MEMBERS OWE AN END-OF-RECORDS (op-0x13)? — the driven,
# one-variable arm

**Registered 2026-10-22, BEFORE the pod exists, before the OVMX node is booted
and before any byte of this capture is taken.** Nothing in this file is written
after the arm runs; the run's results go in `FIRE.md` beside it, scored against
the predictions below exactly as registered.

## The ruling this arm executes

`rd vms-083` (p0, blocks `rd vms-ci.6`), architect's ruling via the conductor,
in two parts:

1. **No owed-op-0x13 rule may be derived from a non-N-of-N census.** 26/34,
   29/34 and 31/34 are not rules — they are a census with unexplained residue,
   and a remaster FSM that waits on a set derived from one is how a tree gets
   stuck un-HELD (`rd vms-5cd`'s double grant). A majority is not a protocol.
2. **Run a pre-registered driven one-variable arm** to find the variable that
   GOVERNS whether a member owes an op-0x13 — hold every other condition fixed
   and move one — **then implement exactly that**, computed from executive
   state, and nothing more.

## What is wrong today, in one paragraph of code

`src/kernel-core/vms_dlm_rm_fsm.c` `rm_owed_snapshot()` snapshots **the whole
CLUB** — `ops->other_members()`, every SELECTED member but this node — into the
tree's owed set, and `rm_owed_complete()` requires `n_done == n_owed`. So if any
member owes no end-of-records, the tree **never reaches HELD**, no op-0x14 is
ever sent, and the adopted locks sit in proxy state: measured as `trees
opened=2 records adopted=0 ends=2, trees held=0, op-0x14 sent=0, proxy_lkbs=3`,
and that is the state in which OVMX **granted EX on a resource its own process
held at CR** (`rd vms-5cd`, 2 of 2 attempts), because a proxy LKB is not on the
granted queue the compatibility check walks.

## The candidate governing variables, and why these two are the live ones

The ruling names five candidates: whether the member HOLDS a lock in the tree;
whether it is the DIRECTORY node for the name; whether it was the OLD MASTER;
member count; and the old-master == directory-node coincidence.

Two are already fixed by the documented and measured exchange
(`src/kernel-core/vms_dlm_adopt.h`, arms RA2/RA4/RA5): the **old master O**
always sends one (`O -> N op 0x13`), and **member count** is held at 3 in every
run compared here. Of the rest, the corpus makes one candidate pair testable
with a single variable, because two runs in hand disagree on exactly it:

* **`vms-c2b` fire A (2026-10-21), which REACHED the hand-over:** O = VAX1,
  N = OVMXE, and **VAX2 sent an op-0x13 while holding NO lock in the tree** —
  its only role was the tree's DIRECTORY node (`SHOW RESOURCE/NAME=RA7T` on
  VAX2 before the hand-over: a directory entry naming VAX1 as master; there
  were no op-0x12 records in the whole fire). Two senders, tree HELD.
* **`vms-a4d7` fire a4d2 (2026-10-22), also HELD:** two senders again — VAX1
  (the old master) and VAX2 (the directory node for the name).
* **The runs that did NOT reach HELD:** one sender, and the `rd vms-ci.6`
  reading of them is that the name hashed the DIRECTORY ONTO THE OLD MASTER, so
  the two roles coincided on one node.

That is a reading of three uncontrolled runs — the thing the ruling forbids
implementing. So this arm **drives it**: one variable, two arms, everything else
byte-identical.

**THE ONE VARIABLE: which node the tree's root NAME assigns as its DIRECTORY
node.** Nothing else moves — same member set, same LOCKDIRWTs, same holder
arrangement, same modes, same trigger, same program, same capture method, same
executive build.

| | arm A (control) | arm B (the variable moved) |
|---|---|---|
| directory node for the tree's name | **VAX2** (≠ the old master) | **VAX1** (== the old master) |
| old master O (first and only holder) | VAX1 | VAX1 |
| new master N (holds the root CR) | OVMXE | OVMXE |
| third member | VAX2 — directory, holds nothing | VAX2 — **neither** directory nor old master nor holder |
| members | 3 | 3 |
| LOCKDIRWT | VAX1 1, VAX2 1, OVMXE 0 | identical |

Arm A is the POSITIVE CONTROL: it must reproduce `vms-c2b`/`vms-a4d7`. Arm B is
the arm that discriminates, because it is the only arrangement in which a
SELECTED member has none of the three roles.

## The bed

Own pod **`lab083`** in `ovmx-lab`, image `ovmx-vaxlab:7`, `requests == limits`,
**deleted after the run**. Two real OpenVMS VAX V7.3 nodes (VAX1 SCSSYSTEMID
1025, VAX2 1026) and one OVMX node (OVMXE, SCSSYSTEMID 1030) on the pod's own
`br0`. Lab consoles are read only through `ovmx-lab-console`; no non-SYS0 SIMH
VAX is ever auto-rebooted.

Read, not assumed, before either arm runs, and recorded in `FIRE.md`:
`F$GETSYI("CLUSTER_NODES")` on both VAXes, `F$GETSYI("LOCKDIRWT")` on both, and
OVMXE's own `LOCKDIRWT` from its parameter set. The GENERAL configuration
(OVMXE LOCKDIRWT 0, both VAXes 1) is the one `rd vms-ci.6` must close in and the
only arrangement in which a real VAX can be both the tree's directory node and
its master — which is what arm B requires.

Fire branch: `lab/vms-083-dirnode`, this commit, which is origin/main plus the
LAB-ONLY commit that stages `RMHOLD.EXE` in the bootable image (`RMHOLD.C` and
its recipe are already on main). `tests/lab/tools/ovmx_fire.sh` REFUSES to start
unless the artifacts' `SOURCE_COMMIT` is exactly the commit the local gated tree
is at and that tree is clean, and unless the frame-conformance gate passes for
every frame type this fire sends. The boot artifacts are built by
`build-boot-artifacts.yml` with **`cluster_auth_group=1`** — an ordinary build
ships group 0 and a group-0 node joins nothing SILENTLY (`rd vms-daa`), which
has already cost one lab run.

**The executive under test is origin/main's**, deliberately: the measurement is
what the REAL VAXes send, which OVMX's own owed-set defect cannot change. Arm
B's prediction P3 uses that defect as its own detector.

## How the one variable is SET, and how it is VERIFIED rather than assumed

The directory node for a resource name is decided by the name's hash over the
directory candidates (the members with non-zero LOCKDIRWT: here VAX1 and VAX2).
OVMX's determination of that hash is not the authority here, and this arm does
not use it to *decide* anything:

1. On VAX1, take candidate roots `D83A0`, `D83A1`, ... one at a time with
   `RMHOLD 100-D83An` (root CR, no sub-locks, `LCK$M_SYSTEM`).
2. For each, read **both** VAXes' own lock databases:
   `SDA> SHOW RESOURCE/NAME=D83An` on VAX1 and on VAX2.
   The node whose RSB is the DIRECTORY ENTRY (`Status: DIRENTR`) for a resource
   mastered elsewhere is that name's directory node. Where master and directory
   coincide, VAX2 has no RSB for the name at all.
3. Keep the first name whose directory is **VAX2** (arm A's name) and the first
   whose directory is **VAX1** (arm B's name). Both names and both SDA reads go
   in `FIRE.md` BEFORE the hand-over is triggered.
4. Second, independent verification on the wire: when OVMXE asks for the root,
   arm A's request must be answered by **VAX2 with a redirect (`0xf8`)** and
   then granted by VAX1, while arm B's goes straight to **VAX1** and is granted
   (`0xfa`) with no redirect. An arm whose wire disagrees with its SDA read is
   INVALID and is re-run, not reinterpreted.

## The steps, identical in both arms but for the name

1. **VAX1 takes the tree.** `RMHOLD 100-<name>` as `HOLD1` — root CR, no
   sub-locks. VAX1 is the first holder, therefore the master.
2. **Read the roles** (step 2/3 above) on both VAXes' SDA. Arm valid only if the
   directory is where the arm says.
3. **OVMXE joins the tree.** `RMHOLD 100-<name>` — root CR, compatible with
   VAX1's CR, so a grant is the honest expectation. OVMXE now holds a lock in a
   tree a real VAX masters (the `rd vms-c2b` precondition).
4. **Trigger the hand-over.** `STOP HOLD1` on VAX1 — the old master's own holder
   ends, which is the RA2/RA5/RA8/`c2b` trigger. Measurement window: **120 s**
   from the STOP (the `c2b` hand-over completed in ~5 s).
5. **Read the result** on all three nodes while OVMXE still holds the root:
   `SHOW RESOURCE/NAME=<name>` and `SHOW LOCK` on both VAXes' SDA, OVMXE's
   `%CNXTRACE-I-DLMOWED` line and its whole console, and the capture.
6. Both arms, then **both arms again with the VAX roles swapped** (VAX2 as old
   master, VAX1 as the third member) — the topology variation the never-crash
   discipline requires before any reading is accepted.

## Pre-registered predictions, each scored PASS / FAIL on its own

**P1 — arm A reproduces the positive control.** Exactly **two** distinct op-0x13
senders for the tree, and they are VAX1 (the old master) and VAX2 (the
directory node). OVMXE originates an op-0x14 to each, both answered 82/15, and
its counters read `trees held=1`, `op-0x14 sent=2`, `unsent=0`.
*FAIL here invalidates the whole arm: the bed does not reproduce the control,
and nothing is implemented.*

**P2 — arm B, the discriminator.** Exactly **ONE** op-0x13 sender for the tree,
VAX1, within the 120 s window. **VAX2 sends none** — it holds no lock in the
tree, it is not the directory node, and it was not the old master.
*If VAX2 sends one anyway, the DIRECTORY hypothesis is FALSIFIED: the governing
variable is not the directory role, no owed-set rule is implemented from this
arm, and a new arm is registered varying "holds a lock in the tree".*

**P3 — the defect reproduces, deterministically, as the detector.** In arm B,
OVMX's tree does **not** reach HELD: `trees held=0`, `op-0x14 sent=0`, and the
`%CNXTRACE-I-DLMOWED` line shows the owed set two wide with one reported
(`n_owed=2`, `n_done=1`). This is `rd vms-5cd` on demand rather than by luck.

**P4 — who is TOLD.** In arm A, VAX1 sends VAX2 an op-0x11 for the tree (the
directory node must move its entry). In arm B, the op-0x0f/op-0x11 traffic to
VAX2 is recorded exactly as captured, whatever it is: if VAX2 is told and still
sends no 0x13, then "told" is not "owes", and the owed set cannot be the told
set either.

**P5 — never crash a peer.** Zero bugcheck, `CNXMGRERR`, `INVLOCKID`,
`INVEXCEPTN` or halt signatures on **both whole VAX consoles** across all four
runs, and `F$GETSYI("CLUSTER_NODES")` reads 3 on both VAXes at the end of each.
Every DCL step returns.

**P6 — the variable really moved.** Both the SDA read (step 2) and the wire
(the `0xf8` redirect in arm A, its absence in arm B) agree with the arm's
intended directory node, in all four runs.

**P7 — the roles-swapped repeats agree.** The swapped runs give the same
sender counts as their unswapped twins. A reading that holds only for one
assignment of real nodes is not accepted (`heldout-proof-must-vary-topology`).

## What each outcome implies for the implementation — fixed in advance

* **P1 ∧ P2 ∧ P7 (the directory role governs).** The owed set becomes, computed
  from executive state and from nothing else:
  the **op-0x0e sender** (the old master, read off the received frame) ∪ **the
  member this node's own directory assignment names for the tree's root name**
  (membership + each member's LOCKDIRWT, the same state the requester path
  already routes a lookup with) ∪ **every member that actually sent op-0x12
  records** (observed, not predicted). No count, no majority, no heuristic. A
  member this node cannot place in any of those three sets is not waited for.
* **¬P2 (VAX2 sends one with no role).** The directory hypothesis is dead. The
  owed set is then a property of SELECTED membership after all, and the un-HELD
  runs had a different cause — which becomes the next arm's question
  (candidate: the op-0x11 recipients). Nothing is implemented from this arm.
* **¬P1.** The bed is wrong. Fix the bed, re-register, re-run.

## Scope — what this arm does NOT do

It does not change the FSM, the codec or any emitted frame; the only OVMX
behaviour it exercises is already shipping (`rd vms-c2b` proved the inbound
hand-over path at two real VAXes). It does not touch `rd vms-5cd`'s grant-side
compatibility walk, which is its own item. And it asserts nothing about members
beyond three: the rule it licenses is about ROLES, and a fourth member is a
topology this arm has not run.
