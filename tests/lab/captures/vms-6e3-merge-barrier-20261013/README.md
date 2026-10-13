# vms-6e3 held-out arm r6e3a (2026-10-13, pod lab6e3a, lab/int-562-6e3 26a3a09a6)

Fired after PREDICTION.md was committed. Integration = main a711195c1 + #1683 (vms-562ac) +
#1684 (vms-6e3). The frame-conformance gate passed before each OVMX boot (ovmx_fire.sh).

Topology: real VAX1 (1025, LOCKDIRWT 1) formed alone; OVMXE (1030, LOCKDIRWT 0) joined;
OVMXB (1022, LOCKDIRWT 0) joined 5 min later. OVMX holds the highest SCSSYSTEMID.

| prediction | result |
|---|---|
| P1 OVMXE sends no 01/0b in its own (merge) admission; completes at the GO | **held**: open [24] = 1, no 01/0b, console "completed VAXcluster state transition" 16 ms after the GO |
| P2 OVMXE answers OVMXB and coordinates a merge | **held**: no "deferring"; 01/12 relay to VAX1, 01/03 to both, 01/09 with [24] = 1 |
| P3 VAX1 sends no 01/0b; OVMXE sends no 01/0c | **held** (`barrier_census.py`: 2 merges, 0 steps, 0 releases) |
| P4 OVMXB admitted, VAX1 CLUSTER_NODES = 3 | **held**: `CN_3` on VAX1 |
| P5 no bugcheck; every VAX1 CM request to OVMX answered | **held**: 0 bugchecks; each VAX1 06/00 answered 86/00 |

Also seen (rd vms-562ac): OVMXE's 01/03 to VAX1 and OVMXB carried one commit time
(e5877b4fb63ebc00) and VAX1 acked it; OVMXE's 01/14 carried "OVMX x86_64" (count 0x0b) and
VAX1 proceeded. VAX1 reports `F$GETSYI("HW_NAME","OVMXE")` = "X86 ", from the SCS START
hardware type, not the 01/14 record.

Still logged, unexplained: "a transition message arrived in a state that has no edge for
it" about 0.9 s after each OVMX node's own admission (OVMXE 30.08 s, OVMXB 28.07 s). It did
not block anything in this run.

Files: `r6e3a-cm.pcap` (cat 01/81/0b/8b/06/86 frames from the br0 capture),
`r6e3a-{vax1,ovmxe,ovmxb}-console.log`.
