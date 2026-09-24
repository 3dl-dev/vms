# vms-b34 — CN=2 with a real OpenVMS VAX V7.3 node in cluster **group 257**

**2026-09-24, `ovmx-lab/vaxlab-4`, on an ISOLATED bridge (`br1` + `tap5`/`tap6`)
created for this run inside the pod. The lab's live group-1 reference cluster
(`vax1`/`vax2` on `br0`) was never touched and stayed up throughout.**

Two runs, one variable: the executive's source.

| Run | OVMX Node A built from | Result |
|---|---|---|
| 1 | `origin/main` @ `8455eebd` (the shipped behaviour) | **NO JOIN.** `%PEA0, channel verified`, then nothing, for the whole window |
| 2 | the same tree + this change | **MEMBER in 19.6 s**, sustained, confirmed on the real VAX's own console |

## The topology

* **Node C** — `VAXC`, SCSSYSTEMID **1989**, cluster group **257**, `VOTES=1`,
  `EXPECTED_VOTES=1`: a real **OpenVMS VAX V7.3** volume (the browser demo's
  Node C disk, `tools/lab-vax/build_nodeC_vms73_cluster.sh`, built through the
  real VMS install dialogue + `@SYS$MANAGER:CLUSTER_CONFIG_LAN`) booted under
  SIMH on `tap5`. It founds its own one-member VMScluster:
  `%CNXMAN, proposing formation of a VAXcluster` → `now a VAXcluster member --
  system VAXC` → `completing VAXcluster state transition`.
* **Node A** — `OVMXA`, SCSSYSTEMID **1987**, group **257**, `VOTES=1`,
  **`EXPECTED_VOTES=2`**: a booted OVMX x86_64 node under QEMU/TCG on `tap6`.
  `EXPECTED_VOTES=2` is deliberate — this node's own vote cannot satisfy
  quorum, so it **cannot found a cluster of its own** and reaching MEMBER can
  only be a real admission by VAXC.
* **`CAP_NET_RAW` DROPPED** from the whole QEMU subtree in both runs
  (`*.caps`: `CapEff/CapBnd = 00000000a80415fb`), so a userspace `AF_PACKET`
  open would `EPERM` — the L2 I/O is the executive's kernel socket or it does
  not happen.

This is the same topology the browser demo has, and the same one rd vms-2570's
20-minute run6 failed in.

## Run 1 — the defect, on the wire (`br1-run1-defect.pcap`)

`frame-census.txt`, first block. Every frame, both directions, at
`ab:00:04:01:01:02` — group 257's multicast address, correct since rd vms-147:

```
1712  VAXC  -> MCAST    120  abs22=0101  a0   multicast HELLO
 240  OVMXA -> MCAST    120  abs22=0101  a0   multicast HELLO
 403  VAXC <-> OVMXA    120  abs22=0101  b3/b4  channel verify
  96  VAXC  -> OVMXA    106  abs22=0101  41   VC START
 803  OVMXA -> VAXC     106  abs22=0001  41   VC STACK   <-- another cluster's number
   0                          --          48   VC ACK
```

`ovmxa-run1-defect.console.log`:

```
[   15.886414] %PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
[   15.891470] %CNXMAN, waiting to form or join an OpenVMS Cluster
[   18.927861] %PEA0, channel verified
```

...and then **nothing** for the rest of the window — no `virtual circuit
open`, no further `%CNXMAN`. `SHOW CLUSTER/CIRCUITS`, sampled every 20 s:

```
  Remote system ID  State  Send   Recv   Unacked  Retransmits
  00000000000007C5      2       1      0        0            0
```

State **2 = STACK SENT**, unmoving. `SHOW CLUSTER` listed only `OVMXA`, with a
blank CSID, for the entire run; the port's own counters read
`frames tx 377 (errors 0), rx 187 (dropped: nobuf 0, badclass 0)` and climbing
— healthy transport, no drops, no errors.

`vaxc-run1-defect.console.log` shows VAXC never mentioning OVMXA at all.

**The mechanism.** abs 22..23 of every SCA frame is `LE16(cluster group)`.
OVMX wrote the literal `0x0001` there, from three constants labelled "the
observed constant connect flag" — correct for exactly one cluster, the lab's,
which is group **1**. VAXC therefore discarded every one of OVMX's 803 STACKs
as belonging to another cluster and re-sent its START; the circuit never
opened; with no `vc_up` there was nothing for `cnxman_discover_peers()` to
promote to a CSB; with no CSB the join FSM had no target and CNXMAN correctly
said nothing more. **The silence was honest.** Full account:
`docs/cluster-integration-notes.md` **E88**, oracles in
`docs/cluster-protocol-spec.md` §3.

**Not a regression.** The lab joins of PR #1288 were real; they worked because
that cluster is group 1, the one number for which the constant is right. This
configuration had never worked.

## Run 2 — the same node, the fix (`br1-run2-fixed.pcap`)

`frame-census.txt`, second block. **Every frame in both directions now carries
`abs22=0101`**, and the conversation completes:

```
697  OVMXA -> VAXC     46  abs22=0101  48   VC ACK        (0 of these in run 1)
131  VAXC  -> OVMXA    46  abs22=0101  48   VC ACK
500  VAXC  -> OVMXA   190  abs22=0101  4b   sequenced (CM config/params)
312  OVMXA -> VAXC    190  abs22=0101  4b
...  the full 0x4b/0x5b membership dialogue, both ways
```

`ovmxa-run2-fixed.console.log`:

```
[   15.455832] %PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
[   15.460971] %CNXMAN, waiting to form or join an OpenVMS Cluster
[   18.755750] %PEA0, channel verified
[   18.761021] %PEA0, virtual circuit open
[   19.574466] %CNXMAN, the cluster assigned this node a cluster system id
[   19.630174] %CNXMAN, this node is a member of the cluster
[   19.640504] %CNXMAN, this node is now a VAXcluster member
[   19.641957] %CNXMAN, system 00000000000007c3 was added to the cluster
[   19.644129] %CNXMAN, system 00000000000007c5 was added to the cluster
```

and `SHOW CLUSTER` on Node A, sustained for the whole 600 s poll window:

```
View of Cluster from system ID 1987 node: OVMXA

| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXA  | 00010002 | VMX V0.7        | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
```

`SHOW CLUSTER/CIRCUITS` reads **State 3 (OPEN)** with the sequence numbers
advancing (`Send 381 Recv 568 Unacked 0 Retransmits 0`).

### The oracle — the real VAX's own words (`vaxc-run2-fixed.console.log`)

Not an OVMX self-report. VAXC's own CNXMAN and OPCOM, at its console:

```
%CNXMAN,  received VAXcluster membership request from system OVMXA
%CNXMAN,  proposing addition of system OVMXA
16:20:53.24 Node VAXC (csid 00010001) received VAXcluster membership request from node OVMXA
16:20:53.24 Node VAXC (csid 00010001) proposed addition of node OVMXA
%CNXMAN,  completing VAXcluster state transition
16:20:53.32 Node VAXC (csid 00010001) completed VAXcluster state transition
```

and, when the harness tore the OVMX node down at the end of the window — an
unclean departure by construction — VAXC's own PEDRIVER and connection manager
show what they had been holding, and then do the correct VMS thing:

```
%CNXMAN,  lost connection to system OVMXA
%PEA0, Port has Closed Virtual Circuit - REMOTE NODE OVMXA
%CNXMAN,  quorum lost, blocking activity
```

(quorum-blocked afterwards is normal for a one-vote node whose cluster had
grown to expect two.)

**Never crashes a peer:** zero bugchecks on either side across both runs and
both teardowns (`grep -c 'BUGCHECK|FATAL'` → 0 on both VAXC logs; `Kernel
panic`/`Oops:` → 0 on both OVMX logs). VAXC survived run 1, run 2, a reboot
between them, and the run-2 teardown.

## In-browser: CN=2, the same fix, the same real V7.3 volume (2026-09-24)

`browser-cn2-result.json`, `browser-cn2-run.log`, `browser-nodeA.console.log`,
`browser-cn2-final.png`. Run in a temporary `k3s-worker` pod
(`ovmx-lab/vms-b34-cn2`, deleted after use), never served from
vax.3dl.network. Node A = the demo bundle built by
`tools/cluster-web-demo/build-cluster-demo` from **these same fixed
artifacts**; Node C = **this same pinned V7.3 volume**, SHA-256
`45355fd2…4fe15` — byte-identical to the one rd vms-2570 run6 used — served
from a minimal local pcjs tree.

**`pass: true` at t=108 s.** Both legs, neither of them an OVMX self-report
alone:

Node A's own console, in the browser (`browser-nodeA.console.log`):

```
[   39.071299] %PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
[   39.102474] %CNXMAN, waiting to form or join an OpenVMS Cluster
[   41.627264] %PEA0, channel verified
[   41.670649] %PEA0, virtual circuit open
[   42.405352] %CNXMAN, the cluster assigned this node a cluster system id
[   43.692235] %CNXMAN, this node is a member of the cluster
[   43.739395] %CNXMAN, this node is now a VAXcluster member
[   43.745130] %CNXMAN, system 00000000000007c3 was added to the cluster
[   43.752510] %CNXMAN, system 00000000000007c5 was added to the cluster
[   45.081034] %CNXMAN, completed VAXcluster state transition
```

Node A's `SHOW CLUSTER` (leg 1):

```
View of Cluster from system ID 1987 node: OVMXA    24-SEP-2026 17:13:19

| NODE   | CSID     | SOFTWARE        | STATUS           |
|--------+----------+-----------------+------------------|
| OVMXA  | 00010002 | VMX V0.7        | MEMBER           |
| 1989   | 00010001 |                 | MEMBER           |
```

Node C — the real VMS node — on its own console (leg 2):

```
%CNXMAN,  proposing formation of a VAXcluster
%CNXMAN,  now a VAXcluster member -- system VAXC
%CNXMAN,  completing VAXcluster state transition
%CNXMAN,  received VAXcluster membership request from system OVMXA
%CNXMAN,  proposing addition of system OVMXA
%CNXMAN,  completing VAXcluster state transition
```

and Node A's port, in the browser (screenshot):
`channels 1, circuits 1 / cluster group 257 (CLUSTER_AUTHORIZE) /
frames tx 857 (errors 0), rx 628 (dropped: nobuf 0, badclass 0)`.

Compare rd vms-2570 run6 on the shipped executive: 20 minutes, 7,305
error-free frames, `channels 1, circuits 1`, and `SHOW CLUSTER` naming only
itself the whole time.

### A harness confound worth recording

The first two in-browser attempts here read `rx 0` at the executive while the
page's own NIC counter showed ~1,000 frames delivered — Node A never formed a
channel at all. That was **not** the executive: the bundle had been generated
against a local `openvmx-site` checkout at `87aa991`, two commits **before**
`00c241a` ("fix in-browser cluster RX — WebSocket readyState constants on
FakeWebSocket instances", rd vms-0cd2). Rebuilt against `f1fe677` — the commit
rd vms-2570 run6 also used — with nothing else changed, the same Node A image
joined in 108 s. Both non-converging runs are kept (`cn2-run1`/`cn2-run2` in
the pod, summarised here) so the distinction is on the record: the browser
lane's RX fix is a prerequisite for this proof, not part of it.

## Honest scope

* **Not** a licensed VMScluster. VAXC logs `%LICENSE-E-NOAUTH, DEC VAXCLUSTER
  use is not authorized on this node` at startup and `%LOGIN-I-NOVAXCLUSTER,
  DEC VMSCLUSTER license is not active` at login, and admitted OVMXA anyway.
  The missing PAK was a live hypothesis for this failure and is **refuted** by
  run 2.
* **Not** an SDA `SHOW CLUSTER` CSB dump from VAXC. This minimally-tailored
  V7.3 volume carries no TERMTABLE entry for its console
  (`%SET-W-NOTSET/-SET-I-UNKTERM`), and a `SET TERMINAL/PAGE=0/NOBROADCAST`
  sent during the live window wedged OPA0:'s input for the rest of the run —
  so SDA was not reached. The VMS-side evidence above is CNXMAN's and
  PEDRIVER's own console output instead, which is VMS reporting on itself, not
  OVMX reporting on VMS.
* CN=3 (adding Node B) is not attempted here.
* The in-browser grader (`cn2-grade.js`) is a throwaway probe built on
  openvmx-site's committed `e2e-boot.js` page-driving code — the same
  "throwaway probe, not the repo" precedent as vms-2570's `probe-run3`. Its
  full transcript is `browser-cn2-run.log`.

## Reproducing

```
# in the pod, on an isolated bridge (never br0):
ip link add br1 type bridge; ip link set br1 up
ip tuntap add dev tap5 mode tap; ip link set tap5 master br1; ip link set tap5 up
ip tuntap add dev tap6 mode tap; ip link set tap6 master br1; ip link set tap6 up

# Node C: the vms-2570 V7.3 demo disk, SIMH `at xq tap:tap5`
python3 /usr/local/bin/nodedrv.py <dir> <log> --boot "B DUA0"

# Node A: the booted OVMX node, artifacts built with CLUSTER_AUTH_GROUP=257
ART_DIR=... OUT_LOG=... SCSNODE=OVMXA SCSSYSID=1987 OVMX_TAP=tap6 \
  OVMX_MAC=52:54:00:00:00:0a OVMX_VOTES=1 OVMX_EXPECTED_VOTES=2 \
  bash tests/lab/tools/labjoin_pod_boot.sh
```

(The run above used a local copy of `labjoin_pod_boot.sh` with two extra
console probes — `SHOW CLUSTER/CIRCUITS` and `SHOW CLUSTER/LOCAL_PORTS` — added
to the poll loop; nothing else differs.)
