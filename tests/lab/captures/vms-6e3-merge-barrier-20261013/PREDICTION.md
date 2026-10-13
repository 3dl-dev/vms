# vms-6e3: a merge-rebuild ADD runs no barrier

Registered 2026-10-13, before the held-out lab arm below was fired.

## The defect (lab e09a, `/data/training/vax/run-fire/e09d/wire.pcap`)

VAX1 (LOCKDIRWT 1) admitted OVMXE (LOCKDIRWT 0). The open carried REBUILD = 1, the merge
type. After the GO, OVMXE sent barrier step 1 (01/0b). VAX1 acked it with body[18] = 0 and
never released step 1. OVMXE stayed inside the transition (`transition_active` set) for the
rest of the run. When OVMXB (1022) asked OVMXE (1030, the highest SCSSYSTEMID) to join,
OVMXE logged "another system is coordinating a state transition; deferring" every 33 s and
OVMXB was never admitted.

## Derivation set

Every ADD open in `tests/lab/captures` and `/data/training/vax/run-fire` as of this commit,
measured by `barrier_census.py` (window: 3 s after the GO):

| rebuild | participant steps | coordinator releases | ADDs |
|---|---|---|---|
| 3 (directory) | yes | yes | 33 |
| 1 (merge) | only from OVMX nodes | none | 7 of 7 |

In the three merges with a real VAX participant besides the coordinator (cn3-achieved 0104,
order1-vax-first 0104, mount-type-over-mscp 0104), that VAX sent no 01/0b. In all 7 merges
the real coordinator acked the OVMX node's step with body[18] = 0 and sent no 01/0c.

## Rule

**R-MERGE.** In an ADD whose open carries body[24] = 1, the transition is complete at the
GO. No participant sends 01/0b and the coordinator sends no 01/0c.

OVMX change: the barrier participant (`barrier_h_go`) and the coordinator (`coord_try_go`)
both complete a merge at the GO. Pinned by `test_cnxman_barrier`
(`test_merge_rebuild_starts_no_barrier`, with a directory-rebuild control) and
`test_cnxman_coord` (`test_f297_zero_weight_joiner_merges`); negative controls
`barrier-merge-runs-steps` and `coord-merge-waits-for-steps`.

## Held-out arm (to fire)

Topology varied from the derivation set: three nodes, OVMX holds the highest SCSSYSTEMID,
and the second joiner's admission is coordinated by OVMX with a real VAX as a participant.

- VAX1 (1025, LOCKDIRWT 1) boots alone and forms.
- OVMXE (1030, LOCKDIRWT 0) joins. VAX1 coordinates a merge.
- OVMXB (1022, LOCKDIRWT 0) joins. It asks OVMXE first (vms-e88 rule).

Predictions:

- P1: in OVMXE's admission, OVMXE sends no 01/0b, and its console logs a completed
  transition within 1 s of the GO.
- P2: OVMXE answers OVMXB's request (no "deferring" line) and coordinates its admission:
  an open with body[24] = 1 to VAX1 and OVMXB.
- P3: in OVMXB's admission, VAX1 sends no 01/0b and OVMXE sends no 01/0c.
- P4: OVMXB is admitted. VAX1's `F$GETSYI("CLUSTER_NODES")` reads 3.
- P5: no bugcheck on VAX1's console; every VAX1 CM request to an OVMX node is answered.
