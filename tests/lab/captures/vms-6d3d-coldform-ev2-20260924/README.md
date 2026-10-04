# vms-6d3d — how a real VMScluster cold-forms with `VOTES=1 / EXPECTED_VOTES=2`

**2026-09-24. Two real OpenVMS VAX **V7.3** systems (SIMH KA655) on one private
bridge inside a disposable `ovmx-lab` pod. The lab's live group-1 reference
cluster (`vaxlab-1/2/3`) was never touched; the pod this ran in was created for
this measurement and deleted after it.**

This is the oracle for the defect in rd vms-6d3d: OVMX's founding predicate
demanded quorum from the founder's **own** votes, so with the textbook two-node
configuration — `VOTES=1`, `EXPECTED_VOTES=2` on both nodes — **no** OVMX node
could ever form a cluster. Real VMS forms exactly that cluster. The question
this capture answers is *how*.

| node | SCSSYSTEMID | VOTES | EXPECTED_VOTES | VAXCLUSTER |
|---|---|---|---|---|
| VAX1 | 1025 | 1 | 2 | 2 |
| VAX2 | 1026 | 1 | 2 | 2 |

Both values were written with `MCR SYSGEN` → `SET VOTES 1` / `SET
EXPECTED_VOTES 2` / `WRITE CURRENT` on each node's own system root, then each
node was cold-booted. Quorum for this configuration is the published
`(CEVOTES + 2) / 2 = (2 + 2) / 2 = 2` — **neither node's own vote reaches it.**

## 1. A node alone with one vote against a quorum of two does NOT form

`VAX1.console.log`. VAX1 was booted with nothing else on the segment and left
there for **18 minutes**. Its console, in full, from SYSINIT to the moment the
second node appeared:

```
%SYSINIT, waiting to form or join a VMScluster system
%VAXcluster-I-LOADSECDB, loading the cluster security database
%MSCPLOAD-I-LOADMSCP, loading the MSCP disk server
```

**Not one `%CNXMAN` line.** No formation, no membership, no cluster system id —
the node sat in "waiting to form or join" and said so honestly. Polled from the
harness the whole time: `grep -c CNXMAN vax1.log` stayed at `0`.

## 2. The instant a second voting system appears, they form — on their COMBINED votes

VAX2 was then powered on (`21:39:33Z`). VAX1's own console and OPCOM, which is
VMS reporting on itself:

```
%CNXMAN,  discovered system VAX2
%CNXMAN,  established connection to system VAX2
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAX1
%CNXMAN,  completing VAXcluster state transition

21:40:00.62 Node VAX1 (sysid 1025) discovered node VAX2 (sysid 1026)
21:40:00.62 Node VAX1 (csid 00000000) established connection to node VAX2
21:40:03.84 Node VAX1 (csid 00000000) proposed formation of a VAXcluster
21:40:03.84 Node VAX1 (csid 00010001) is now a VAXcluster member
21:40:03.84 Node VAX1 (csid 00010001) completed VAXcluster state transition
```

and VAX2, on its own console (`VAX2.console.log`), **at the same instant**:

```
%CNXMAN,  now a VAXcluster member -- system VAX2

21:40:03.85 Node VAX2 (csid 00010002) is now a VAXcluster member
```

Three facts fall out of this, and they are what the fix is built on:

1. **The votes are combined.** One vote is not quorum 2 and VAX1 waited; two
   votes are, and it formed. The quorum figure did not change — the set it is
   weighed against did.
2. **Connectivity comes first.** `established connection to node VAX2` precedes
   `proposed formation` by 3.2 s. A system that is merely *discovered* does not
   contribute its vote; one whose circuit is up does.
3. **The lower SCSSYSTEMID proposed it** (1025, not 1026). n=1, and VAX1 had
   also been up far longer, so this corroborates OVMX's lowest-first election
   ordering without establishing it — the ordering remains an OVMX design value
   standing in for p. 7-32's coordinator lock (`vms_cnxman_coord_fsm.h` §8b).

## 3. The contrast run: `EXPECTED_VOTES=1`, the configuration OVMX already had

`ev1-baseline-VAX1.console.log` / `ev1-baseline-VAX2.console.log` are the SAME
pod's previous boot, before the SYSGEN change, with the golden images' shipped
configuration (VAX1 `VOTES=1 EXPECTED_VOTES=1`, VAX2 `VOTES=0
EXPECTED_VOTES=1`, read back live with `F$GETSYI`). There VAX1's own vote *is*
quorum 1, and it forms **immediately, alone**, two seconds after LOADSECDB —
then admits VAX2 as a *separate* transition:

```
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAX1
%CNXMAN,  completing VAXcluster state transition
%CNXMAN,  received VAXcluster membership request from system VAX2
%CNXMAN,  proposing addition of system VAX2
%CNXMAN,  completing VAXcluster state transition
```

One SYSGEN digit apart, the two runs are the whole measurement: the wait in §1
is caused by `EXPECTED_VOTES`, not by anything else in the configuration.

## 4. Honest scope — what this capture does NOT settle

**The two-node formation in §2 is ONE transition, not two.** VAX2 never printed
`sending VAXcluster membership request to system VAX1` (it *did* in the §3
baseline, line 66 of `ev1-baseline-VAX2.console.log`) and VAX1 never printed
`received VAXcluster membership request` / `proposing addition`. VAX2 was made a
**founding member** in the same transition, and was handed CSID `00010002` by
it.

OVMX, after rd vms-6d3d, forms with the same *predicate* — combined votes over
the systems it can see — but still opens a **single-node** founding transition
and admits the peer on the ordinary op-0x02 admission path immediately
afterwards, which is the shape the §3 baseline shows VMS itself producing in
the other configuration. The multi-participant founding transition is a real
remaining difference, tracked as **rd vms-f29** rather than papered over;
nothing in this tree claims OVMX reproduces it.

**Arm 3, `mixed-ovmx-plus-v73/`, measures exactly that boundary.** One booted
OVMX node and this same real V7.3 system, both `EXPECTED_VOTES=2`: the real VAX
**does** propose a cold formation with the OVMX node supplying the second vote
(`received VAXcluster membership request from system OVMX6D` ->
`proposing formation of a VAXcluster`), so the vote arithmetic agrees across the
two implementations. It does **not** complete: OVMX answers the formation's
records but never adopts a CSID out of a transition it did not coordinate, and
CN=2 is not reached. No bugcheck on either side. Read that directory's README
before quoting this one as a mixed-cluster result.

`coldform-ev2-formation.pcap` is the 30 s of `br0` around the formation
(`21:40:00`–`21:40:30`, 236 × 0x6007 frames) sliced out of the full 450 s
capture. Decoded with `docs/clean-room/tools/cmdiff.py` it shows the formation
running on the ORDINARY transition vocabulary — `PARAMS`/`MODEL`, then
`op03-COMMIT`, `op05-LOCKRB`, `op0a-GO` and the `0b`/`0c` barrier pairs — with
**no** membership-request exchange preceding it. No opcode appears that this
project's own captures have not already seen. (`cmdiff.py`'s src/dst labels are
hardcoded to lab-1's node MACs and render nonsense here; only the category and
opcode columns are being read.)

## 5. Reproducing it

```bash
# a disposable pod over the same PVC, reusing one lab's disks
kubectl -n ovmx-lab apply -f -   # image ovmx-vaxlab:5, NODES="vax1", POD_NAME=<lab>
# on each node, once:
#   MCR SYSGEN / SET VOTES 1 / SET EXPECTED_VOTES 2 / WRITE CURRENT / EXIT
#   RUN SYS$SYSTEM:OPCCRASH        (leaves SIMH alive at its own prompt)
# then boot vax1 ALONE, watch for %CNXMAN for as long as you like, and only
# then bring vax2 up on the same br0.
```

The console FIFO drops long input lines on the DZ mux — keep DCL commands
short (`V=F$GETSYI("VOTES")` then `SHOW SYMBOL V`), or they arrive truncated
as `%RMS-F-RER`.
