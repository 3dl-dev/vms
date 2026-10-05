# rd vms-fcb — LOCKDIRWT on the wire (plan row FC-P3.2)

**Where a system's lock-directory weight travels: cat-0x01 op-0x01 PARAMS
`body[26:28]` (abs 98), LE u16.** Grounded by controlled reconfiguration on a
private three-node OpenVMS VAX V7.3 cluster (the golden 3-node volume, `VAX1`
1025 / `VAX2` 1026 / `VAX3` 1027) on its own bridge inside a disposable pod
(`ovmx-lab/dlmlab`). No `vaxlab-*` pod and no live cluster was touched.

## Method

Each run boots `VAX1` conversationally as founder (`SET VOTES 1`,
`SET EXPECTED_VOTES 1`, `SET LOCKDIRWT w1`, `SHOW LOCKDIRWT`, `CONTINUE`), then
`VAX2` conversationally as joiner (`SET LOCKDIRWT w2`, `SHOW LOCKDIRWT`). The
`SHOW LOCKDIRWT` read-back is on each node's own console log here. Every 0x6007
frame was captured on the bridge; `fcb-L*-params.pcap` keep only the
`VMS$VAXcluster` cat-0x01 op-0x01 records (`trimcm.py`, plus abs 62 == 0, the
CM connection's own message class — two same-category records on another
connection are dropped).

| run | VAX1 SYSBOOT | VAX1 `body[26:28]` | VAX2 SYSBOOT | VAX2 `body[26:28]` |
|---|---|---|---|---|
| L1 | 3 | `03 00` | 0 | `00 00` |
| L2 | 1 | `01 00` | 2 | `02 00` |

Both systems move, in both directions, with nothing else in the record moving
with them (`body[20:22]` and `body[24:26]` read `01 00` on every record of both
runs). `SHOW LOCKDIRWT` reports the V7.3 **default as 0**, range 0..255, not
dynamic.

## What it means for OVMX

The weight vector (Davis p. 6-32) is a SHARED table: every member builds it
from every member's weight. OVMX now advertises its own SYSGEN LOCKDIRWT here
and learns every peer's — a real VAX's included — so in a mixed cluster OVMX's
vector is the VAX's vector. With the V7.3 default (every member 0) p. 6-32's
all-zero rule makes **every member a directory node**, OVMX included: that is
the rule the real VAX was already applying to OVMX nodes (stall-rig captures
show a real VAX addressing directory lookups and rebuild records to an OVMX
member). Serving that role is the next rung; this one makes OVMX hold the
same vector the cluster holds.

Fixtures: `tests/cluster/host/fixtures/cm-params-lockdirwt{0,1,2,3}-oracle.spec`.
