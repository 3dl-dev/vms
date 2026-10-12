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

## Run 2 (artifacts `evac/int-d52c2` 5f8c04ce7)

Run 2 added the PARAMS quorum and QDSKVOTES commit plus the local-incarnation fix. OVMXE's PARAMS now carried quorum 1 and QDSKVOTES 1. Its self-record still had body[28:36] = 0, and both VAXes refused it again.

The cause was that `pe_incarnation()` answers SS$_NORMAL (1), and the new caller tested `== 0`. This is the E67 family. 0 bugchecks.

## Run 3 (artifacts `evac/int-d52c2` with the SS$_NORMAL fix)

OVMXE admitted both VAXes:
- `system 0000000000000401 was added to the cluster`
- `system 0000000000000402 was added`

The OPCOM traffic between VAX1 and VAX2 shows they are in the same cluster.

**P3 held in part.** CN=3 formed, with OVMXE as founder and coordinator. `F$GETSYI("CLUSTER_NODES")` could not be read on a VAX, because no login was possible (see below). P1, P2, P4 and P5 held. 0 bugchecks.

The lost and re-established VC between VAX1 and VAX2 at 18:39:55 came from the SIMH ^E pauses this run took to sample VAX1's PC. They were operator-induced.

### New finding, filed as rd vms-db1 (p0)
During startup, each VAX sent OVMXE a cat-0x06 close of kind 10. Its record names the F11B$a / CACHE$cm SYSDSK1 resources. OVMXE has no grounded answer for kind 10 and withheld one: `transaction close not answered: this node holds no value for a required field`.

Neither VAX reached `Username:` for more than 20 minutes. Both stayed idle at IPL 3 with live VCs, so this is a process-level hang that OVMX causes. VAX-first orders don't show it, because the VAXes finish startup before OVMXE joins.

Files: `r2-*`, `r3-*` (pcaps and consoles).
