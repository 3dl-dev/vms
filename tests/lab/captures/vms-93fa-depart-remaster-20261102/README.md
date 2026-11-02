# vms-93fa arm 1 result: the departing VAX did NOT hand OVMX the tree, and OVMX's lock was lost

Pod `lab93fa` (own pod, deleted after), 2026-11-02 03:43-04:12 UTC. Build
`lab/vms-6ad1-ra7` `ef0b7e3fc` (#1900's tree plus RMHOLD staging), artifacts
from run `41352991717`. The gate passed before boot. Scored against
[`PLAN.md`](PLAN.md), committed in `4b4a5da22` before the pod existed.

## What happened

| # | step | observed |
|---|---|---|
| 1 | VAX1 `RMHOLD 123WDEPT` | VAX1 masters `DEPT` (valblk `RMLVB-0123456789`, Seqnum 1) |
| 2 | OVMXE `RMHOLD 100-DEPT` | **P0 held.** VAX1 SDA: `3A000087 CR ... DEPT ... MSTCPY ... Master copy of lock 01000361 on system 00010003` |
| 3 | VAX1 `SHUTDOWN`, `REMOVE_NODE` | `SYSTEM SHUTDOWN COMPLETE`. During it, VAX1 remastered **281 trees, all to VAX2** (`remaster-census-headline.txt`). OVMXE received **no** remaster frame of any kind: every DLM frame to or from OVMXE in the run is an op-0x01/0x04 from before the shutdown (`ovmx-and-dept.pcap.gz`) |
| 3' | | VAX2 removed VAX1 and completed the transition. VAX2: `CLUSTER_NODES=2 CLUSTER_QUORUM=1 CLUSTER_VOTES=1`. OVMXE: `%CNXMAN, quorum lost, blocking activity`, and no later quorum line |
| 4 | VAX2 `RMHOLD 100-DEPT` | granted, **but VAX2 masters `DEPT` itself**: `CSID 00000000`, `Lock Count 1` (its own lock only), value block zero, Seqnum 0 |
| 6 | OVMXE `^Y` | the holder ended; DCL on OVMXE kept running (`SHOW CLUSTER`: OVMXE and VAX2 MEMBER) |

No bugcheck on any console. VAX1 was not rebooted.

## Scoring

| prediction | result |
|---|---|
| P1 OVMX is chosen and adopts | **FAIL.** No node was chosen for `DEPT`. VAX1 sent no 0x0e for it to anyone |
| P2 answers are the real ones | not reached: OVMXE answered no remaster frame |
| P3 the departure completes | **PASS for VAX1** (SHUTDOWN COMPLETE, VAX2 removed it). For OVMXE: it logged a quorum loss and no regain (rd vms-c350) |
| P4 the tree is OVMX's afterwards | **FAIL.** VAX2 masters it |
| P5 OVMX serves the survivor | **FAIL.** VAX2 was granted by itself |
| P6 no bugcheck, no hang | **PASS** for bugchecks. VAX2's DCL never hung. OVMXE reports blocking activity, yet its DCL kept answering |

## What it means

1. **The shutdown path does not remaster a tree to OVMX, even when OVMX holds
   the only lock left in it.** Run E1 (rd vms-50d) saw 273 of 274 trees go to a
   holding VAX. Here 281 went to VAX2, and the one tree whose only other holder
   was OVMX went nowhere. Something in the departing VAX's selection excludes
   OVMXE. Candidates, untested: the identity OVMX advertises, or the DLM state
   OVMX's CSB shows. The activity path does choose OVMX (RA7). The selection
   rule as written in rd vms-50d is incomplete.
2. **The lock OVMX held was dropped from the cluster without OVMX noticing.**
   The tree died with its master, and VAX2 re-created `DEPT` from nothing.
   OVMX still believed it held CR. This is the lock-rebuild gap of rd vms-20c
   (FC-P5.5): no survivor told a new master about OVMX's lock. Only a CR was
   held here. An EX on the OVMX side would have let VAX2 be granted an
   incompatible lock: a double grant.
3. **Quorum after `REMOVE_NODE` is wrong on both sides** (rd vms-c350).

So vms-93fa cannot pass by arrangement alone. Making the departure hand OVMX the
tree needs finding (1). Making OVMX's lock survive needs vms-20c's survivor
sender.

## Files

- `ovmx-and-dept.pcap.gz`: every `DEPT` frame and every DLM frame to or from
  OVMXE (21 frames).
- `remaster-census-headline.txt`: the shutdown's remaster counts.
- Consoles; the VAX logs are gzipped.

`score_remaster` over the *untrimmed* capture reports mismatches on 02/14 pairs.
Those are VAX-to-VAX pairs outside this arm's
predictions. The likely cause, `--pairs` pairing across 281 concurrent
episodes, has not been verified, and they are not scored here.
