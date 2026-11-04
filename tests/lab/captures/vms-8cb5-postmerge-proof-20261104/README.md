# vms-8cb5 post-merge proof on main 2ca718530 (2026-11-04)

The visitor-gate cases that bugchecked a real VAX (`LOCKMGRERR`, vms-8cb5 / vms-c55)
and the demo Node A IMGNOTFND regression (vms-623) re-fired on main **2ca718530**
(after #2015, #2029, #2033, #2028), in three own pods (ns `ovmx-ci`:
`p0-8cb5-gate`, `-gate2`, `-gate3`), each a real VAXC (SIMH VAX, VMS V7.3) plus
OVMX nodes A and B booted from that SHA's payload (Node B build run 41893714312,
site preflight 41898050701). Started 19:58Z, finished 21:40Z.

| case | order | runs | CN=3 (all three members, every node usable) | bugchecks | hangs |
|---|---|---|---|---|---|
| c-first-legacy | C,A,B | 50 (25 gate + 25 gate2) | 50 | 0 | 0 |
| page-order-throttled4 | throttled | 30 (5 gate + 5 gate2 + 20 gate3) | 30 | 0 | 0 |

The loop stops on the first non-CN=3 verdict or any console line matching
`bugcheck|LOCKMGRERR`; it never stopped.

Files: `tally.txt` (one line per run: pod, tag, run, verdict, bugcheck-line count
across every node console), `<pod>/loop.log`, `<pod>/<case>-rN/summary.json`
(per-run grader output: membership, each node's cluster view, VAXC admissions,
restarts, bugchecks).
