# CN=3 ACHIEVED: booted OVMX admitted as a real VMScluster MEMBER (2026-09-05)

Companion note for `cn3-achieved-20260905.pcap` (1 796 836 bytes, 11 478
frames, `ether proto 0x6007` on vaxlab-2's `br0`). Tank name of the same file:
`join-e85refire-1788619727.pcap`.

sha256 `e1888e1b4be27a1d72e71e686024e14526ee1033ac8bcdd58a19008352ad4dac`.

## Run

- Pod `vaxlab-2` (ns `ovmx-lab`), fresh reset immediately before this run;
  verified clean single-identity CN_2 first: VAX1 self-`F$GETSYI("NODE_SYSTEMID")`
  = `000000000401` (1025), VAX2 = `000000000402` (1026), each correctly
  self-identifying, no cross-identity, no OVMXJ1 residue.
- OVMX joiner `OVMXJ1`, SCSSYSTEMID 1986, Ethernet source `52:54:00:00:00:f4`,
  cluster group 257, `OVMX_DROP_NET_RAW=1` (leg (e) PASS — `CapEff`/`CapBnd`
  both `00000000a80415fb`, `CAP_NET_RAW` clear).
- Build under test: SHA `2720b9e58f9ee845f174056be9dc1f9a5541bf40` (E85 —
  withholds a `cat 0x86` transaction-close frame whose mandatory body[24]
  field OVMX could not ground, rather than send a zero that bugchecked VAX2;
  mints real `cm_txn`/token pairs for barrier steps).

## Crash-check (primary gate) — CLEAN

```
vax1.log: 0 CNXMGRERR/INVEXCEPTN/BUG CHECK/0xb1, boot-banner count 1
vax2.log: 0 CNXMGRERR/INVEXCEPTN/BUG CHECK/0xb1, boot-banner count 1
```
Both VAXes booted exactly once (at pod start) and stayed up the entire
~600s window. Pod: `Running`, 0 restarts.

## Safety gate — clean

```
$ python3 tools/cluster/cm_wire_safety_audit.py --ovmx-mac 52:54:00:00:00:f4 cn3-achieved-20260905.pcap
   (1787 CM-class frames) -- 0 FATAL, 0 WARN --
```

## The wire: CONFIG through barrier, in ~50 ms

| frame | t (s) | direction | cat/op | meaning |
|---|---|---|---|---|
| 204–207 | 18.9265–18.9275 | OVMX→VAX2 | 01/14, 01/01, 01/02 | MODEL, PARAMS, CONFIG |
| 218 | 18.9288 | VAX2→OVMX | 01/03 | COMMIT |
| 223 | 18.9301 | OVMX→VAX2 | 81/03 | COMMIT echo |
| 229–232 | 18.9318–18.9320 | VAX2→OVMX | 01/05 ×4 | LOCKRB rebuild requests |
| 236–242 | 18.9329–18.9330 | OVMX→VAX2 | 81/05 ×4 | LOCKRB echoes |
| 245–832 | 18.9333–18.9785 | VAX2→OVMX | 01/06 (hundreds) | MEMBERSHIP burst |
| interleaved | — | OVMX→VAX2 | 04/00 (coalesced) | credit-return acks, NOT 1:1 per frame — E79's coalescing holds; no flood, no crash |
| 833 | 18.9786 | VAX2→OVMX | 01/09 | transition open (bitmap) |
| 846 | 18.9808 | OVMX→VAX2 | 81/09 | echo |
| 848 | 18.9821 | VAX2→OVMX | 01/0a | barrier GO |
| 853 | 18.9854 | OVMX→VAX2 | 01/0b | barrier step |
| 855 | 18.9855 | VAX2→OVMX | 81/0b | barrier step ack |
| 2311 | 230.7769 | VAX1→OVMX | 06/00 | quiet keepalive, ~211 s later — the connection is stable, not retried |

Honest note: only **one** `op-0b`/ack pair is visible in this capture, and no
`cat 0x01 op=0x0c` (barrier release) frame ever appears. The VAX admitted
OVMXJ1 as MEMBER anyway (see below) — whatever completed the transition did
not require an on-wire op-0c this run. Not asserting a 12-step barrier was
walked; reporting only what the wire shows.

## The ground truth: VAX1 admits OVMXJ1

`F$GETSYI("CLUSTER_NODES")` on VAX1: `CN_3`, sustained across every poll from
t+30s to t+600s (the harness's own gate: `OVMXJ1 STATUS==MEMBER at CN=3 --
watching whether it SUSTAINS to window end` — it did, for the rest of the
600s run).

```
$ SHOW CLUSTER
View of Cluster from system ID 1025  node: VAX1
+--------+----------+---------+
|  NODE  | SOFTWARE |  STATUS |
+--------+----------+---------+
| VAX1   | VMS V7.3 | MEMBER  |
| VAX2   | VMS V7.3 | MEMBER  |
| OVMXJ1 | VMX V0.6 | MEMBER  |
+--------+----------+---------+
```

SDA `ANALYZE/SYSTEM`, OVMXJ1's own Cluster System Block:

```
--- OVMXJ1 Cluster System Block (CSB) 879F9F00 ---
State:  01 open
Flags:  02020002 member,selected,status_rcvd
Cpblty: 00000008 ext_status
SWVers: VMX V0.6            LNM Seqnum: 0000000000000000
Quorum/Votes         0/1    Next seq. number    0003
CSID            00010003    Last ack. seq num   0002
```

`CSID 00010003` is a genuine, sequential CSID (VAX1=`00010001`,
VAX2=`00010002`, OVMXJ1=`00010003`) — not a zero/placeholder, and the `member`
flag matches the exact flag VAX1 and VAX2 carry on each other's CSBs. Both
VAXes remained healthy after the run (no crash, no reboot).

## Chain of fixes this closes

E55/E56 (channel verify + discovery format) → E57 (honest software version) →
E60 (real credits) → E63/E66 (transport counter, incarnation) → E70/E71/E72
(send-refusal naming, join resilience, adopt-open-connection) → E73 (receive
parse) → E75 (credit replenish on piggyback) → E76/E77 (per-connection
envelope, stop bugchecking on stale sequence) → E78 (return receive credit)
→ E79 (coalesce the op-06 ack, gate MEMBER on the real barrier) → E80
(deterministic coordinator) → E81 (stop re-asking a rejected connection) →
E83 (one-VC-per-SYSTEM + channel failover, not per-MAC) → **E85** (withhold
the ungrounded `cat 0x86` close instead of sending body[24]=0) — the last
crash vector, and the one that let this run reach MEMBER clean.
