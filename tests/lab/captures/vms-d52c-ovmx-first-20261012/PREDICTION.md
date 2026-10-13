# vms-d52c lab prediction (registered before any run)

Registered 2026-10-12, before the first fire of a build that contains this change.

## Setup
- Pod `labd52c` (image `ovmx-vaxlab:7`, `ASYM_MSCP=1`, `NODES="vax1 vax2"`). The VAXes start 300 s after the pod does.
- OVMXE is booted at once through `tests/lab/tools/ovmx_fire.sh`, so the frame-conformance gate runs first. It has VOTES 1, EXPECTED_VOTES 1 and group 1.
- Artifacts: branch `evac/int-d52c`, which is this change + vms-64b (#1648) + vms-239.
- Topology: 3 nodes. OVMXE is the founder and coordinates the admissions; the two real VAX V7.3 nodes are joiners. Run 2 of the same capture used the opposite roles (a VAX founds, OVMXE joins): `tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap`.

## Predictions (each is checked on the run's pcap and consoles)
- **P1.** Every PARAMS (cat 01 op 01) that OVMXE sends to a VAX after OVMXE founds carries all of the following:
  - body[18:20] = its member count
  - body[12:14] = 0x5021
  - body[28:36] = body[64:72] = the formation time, nonzero
  - body[36:44] nonzero
  - body[44:48] = the epoch of its last transition
- **P2.** No VAX sends OVMXE a formation proposal (cat 01 op 07).
- **P3.** Neither VAX console shows it forming a cluster. Both VAXes become members of the cluster OVMXE founded, and OVMXE sends the admission opens (cat 01 op 09). On a VAX, `F$GETSYI("CLUSTER_NODES")` is 3.
- **P4.** Neither VAX bugchecks or hangs.
- **P5.** No two members hold the same CSID.

Falsifier: any VAX 01/07 to OVMXE, or a second founding, falsifies the root cause in the commit message (a PARAMS from a member that lacks the member state makes a booting VAX treat that member as clusterless).

If P1 and P2 hold but P3 does not (for example, OVMXE cannot admit a VAX), the partition is still gone, and the admission gap is filed as its own item.
