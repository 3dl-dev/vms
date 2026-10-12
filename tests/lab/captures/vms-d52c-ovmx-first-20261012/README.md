# vms-d52c: OVMX founds first, then two real VAXes boot

The predictions in `PREDICTION.md` were registered before any run.

## Run 1 (2026-10-12, pod `labd52c`, artifacts `evac/int-d52c` 62beb846f)

OVMXE (EXPECTED_VOTES 1) founded alone at 44 s. VAX1 and VAX2 (OpenVMS VAX V7.3) booted 300 s after the pod started.

| prediction | result |
|---|---|
| P1: member PARAMS carry 0x5021, the count and the formation, transition and joined times | **held**: `5021`, count 1, [28:36] = [64:72] = the formation time, [36:44] set, epoch 1. Also seen: [20:22] (quorum) and [24:26] (QDSKVOTES) were 0, which led to the follow-up commit |
| P2: no VAX sends a FORM (01/07) | **held**: 0 op-0x07 frames. Both consoles print `%CNXMAN, sending VAXcluster membership request to system OVMXE` |
| P3: both VAXes become members | **failed**: OVMXE proposed each admission, then abandoned it (`a system rejected the proposed state transition`). Both VAXes stayed waiting |
| P4: no bugcheck or hang | **held**: 0 bugchecks |
| P5: no duplicate CSID | **held**: only one cluster exists |

The partition, which was the root cause of the item, is gone. Before this change, the control (`../vms-64b-dlm-seq-20261011/order2-*.pcap`) shows VAX1 sending OVMXE a FORM and founding a second cluster with a duplicate CSID.

### Why the admission failed
OVMXE sent each VAX the op-0x05 membership records. The VAX acked the records about the VAXes with 0x81/0x05 body[18] = 1. It acked the record about **OVMXE itself** with body[18] = 0. That record had body[28:36] = 0.

body[28:36] is the member's incarnation. In the lab corpus, 0 of 176 real records are zero, and 89 equal a node's START abs-80 incarnation. OVMX's local CSB never held its own incarnation.

The fix is the commit "the coordinator's own membership record carries its incarnation". Run 2 is the proof.

Files: `r1-ovmx-first.pcap` (every 0x6007 frame on br0), `r1-ovmxe-console.log`, `r1-vax1-console.log`, `r1-vax2-console.log`.
