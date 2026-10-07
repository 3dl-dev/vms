# rd vms-f29 — OVMX as a founding member of a cluster a real VAX forms

**The setup.** One real OpenVMS VAX V7.3 node (VAXC, SCSSYSTEMID 1989, booted
conversationally with `SET EXPECTED_VOTES 2`) and one booted OVMX node (OVMXA, 1993,
`VOTES 1`, `EXPECTED_VOTES 2`, group 257) share a private bridge in a disposable pod
(`ovmx-lab/dlmlab`, rig `/lab/run-dlm3`, driver `scenF.sh`). Neither vote reaches quorum on
its own. The VAX has the lower SCSSYSTEMID, so it is the one that forms the cluster.

**The oracle** is two real V7.3 nodes doing the same thing
(`../vms-6d3d-coldform-ev2-20260924/coldform-ev2-formation.pcap`). Every founding member is
made in ONE class-0x01 transition, in this order:
1. op-0x03 COMMIT (class 1);
2. one op-0x05 membership record per founding member, tagged **20 01**;
3. op-0x07 FORMATION open (role 0x40, class 1, nodemap at body[55]);
4. op-0x0a GO, then the 12-step barrier.

The founding member never sends a membership request.

**What OVMX got wrong** (main, `F29-base-main-*`): the VAX proposes the formation, but the
OVMX join is in its back-off. Nobody claimed to belong to a cluster, so OVMX asked no one,
and no cell in that state answers the commit. The formation stalls and OVMX stays outside
it. On its own path, OVMX also refused the records' 20 01 tag, which only ADD's 20 02 had
been grounded on, and did not know op 0x07.

**After the fix** (`F29-2-*` and three repeats, F29R-1..3): the VAX logs `proposing
formation`, `now a VAXcluster member`, `completing VAXcluster state transition`. OVMX logs
`a cluster member proposed forming a cluster with this node as a founding member` and `the
cluster assigned this node a cluster system id`. Then SHOW CLUSTER on OVMXA shows:

```
| OVMXA  | 00010002 | VMX V0.7        | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
```

All 4 runs reached CN=2 with 0 bugchecks. The baseline run on main did not reach CN=2.

`*-cm.pcap` keeps only the cat-0x01/0x04/0x06 dialogue (`trimcm.py`, see
`../vms-fcb-lockdirwt-20261004/`).
