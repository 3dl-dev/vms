# vms-e18e — in-browser CN=2 with OVMX/VAX, and where CN=3 stops

**2026-09-24, a temporary `k3s-worker` pod (`ovmx-ci/vms-e18e-cn3`, deleted
after use). Nothing was served from vax.3dl.network; the lab's live group-1
reference cluster (`vaxlab-1/2/3`) was never touched.**

Three machines, one page, one in-page L2 hub:

| node | what it is | identity | image |
|---|---|---|---|
| **OVMXA** | OVMX/x86_64 under qemu-wasm | SCSNODE `OVMXA`, SCSSYSTEMID 1987, VOTES 1, EXPECTED_VOTES 2 | `build-boot-artifacts` at `02a8d873`, injected by `build-cluster-demo` |
| **OVMXB** | **OVMX/VAX** under pcjs KA655 (NetBSD/vax substrate) | SCSNODE `OVMXB`, SCSSYSTEMID 1988, VOTES 1, EXPECTED_VOTES 3 | rebuilt this session from `02a8d873`, sha256 `e9e867cf…0f39cb` |
| **VAXC** | a **real** OpenVMS VAX **V7.3** | SCSNODE `VAXC`, SCSSYSTEMID 1989, VOTES 1, EXPECTED_VOTES 1 | the pinned rd vms-2570 volume, sha256 `45355fd2…4fe15` |

All three in cluster group **257**, `VAXCLUSTER=2`.

## 1. Node B really was rebuilt, and the group really is on its frames

The Node B image the demo had shipped was built at `32f743e9` — *before*
`02a8d873` (#1298), so it wrote the literal `0x0001` at abs 22..23 of every SCA
frame and a group-257 VAX discarded all of it. It was rebuilt at `02a8d873` by
`tests/lab-vax/run-boot.sh sysboot-single` with the rd vms-cee injection
(`CLUSTER_SCSNODE=OVMXB CLUSTER_SCSSYSTEMID=1988 CLUSTER_VOTES=1
CLUSTER_EXPECTED_VOTES=3 CLUSTER_VAXCLUSTER=2` +
`CLUSTER_AUTH_FILE=<group 257>`), `FORCE_CROSS_BUILD=1` so the elf32-vax
`vms.kmod` and `STARTUP.EXE` are that tree's.

`frame-census.txt` is the byte-level read-back, taken at the hub itself:

```
  OVMXA  abs22..23 = 01 01  (group 257)  x1703
  OVMXB  abs22..23 = 01 01  (group 257)  x1738
  VAXC   abs22..23 = 01 01  (group 257)  x399
```

Every frame from the OVMX/VAX node carries the same two bytes the real VAX
does, to the same `ab:00:04:01:01:02` (rd vms-147's `LE16(group+0x100)`).
Node B's own console says where that number came from — its executive read it,
it was not plumbed:

```
[  82.5100030] %PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
```

## 2. ⭐ CN=2 in the browser between **OVMX/VAX** and a real OpenVMS VAX V7.3

`cn2-nodeB-v73/`. Boot order `C` then `B`. This is the join rd vms-613 was
opened for, and the first capture of it: every previous OVMX↔real-VMS join in
this directory is OVMX/**x86**.

Node B's own console (`cn2-nodeB-v73/OVMXB.console.log`) — the executive's own
lines, not the harness's:

```
[  82.5100030] %PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)
[  82.7300030] %CNXMAN, waiting to form or join an OpenVMS Cluster
[  86.0200030] %PEA0, channel verified%PEA0, virtual circuit open%CNXMAN, a connection manager was discovered on the interconnect
[  87.3300030] %CNXMAN, the cluster opened the VMS$VAXcluster connection to this node
[  87.8100030] %CNXMAN, the cluster assigned this node a cluster system id
[  92.2900030] %CNXMAN, this node is a member of the cluster
[  92.3400030] %CNXMAN, this node is now a VAXcluster member
[  92.3500030] %CNXMAN, system 00000000000007c4 was added to the cluster
[  92.3600030] %CNXMAN, system 00000000000007c5 was added to the cluster
[  98.4800030] %CNXMAN, completed VAXcluster state transition
```

`0x7c4` = 1988 (OVMXB), `0x7c5` = 1989 (VAXC).

And the oracle — **the real VAX's own CNXMAN and OPCOM**, on its own console
(`cn2-nodeB-v73/VAXC.console.log`), which is VMS reporting on itself:

```
%CNXMAN,  received VAXcluster membership request from system OVMXB
%%%%%%%%%%%  OPCOM  24-SEP-2026 19:08:47.96  %%%%%%%%%%%
19:08:47.91 Node VAXC (csid 00010001) proposed addition of node OVMXB

%CNXMAN,  completing VAXcluster state transition
%%%%%%%%%%%  OPCOM  24-SEP-2026 19:08:51.51  %%%%%%%%%%%
19:08:51.51 Node VAXC (csid 00010001) completed VAXcluster state transition
```

Zero bugchecks on either side.

**Honest scope.** This is the membership announcement and the real VAX's own
proposal/completion, captured live. It is **not** a `SHOW CLUSTER` table read
back off both consoles: on this minimally-tailored V7.3 volume `OPA0:`'s login
could not be driven while the node was in a retry storm (see §4), and that is a
harness limitation, recorded rather than worked around.

## 3. CN=2 in the browser between OVMX/x86 and the same V7.3 node, on the
## three-node page

`cn3-blocked/run2-*`. Boot order `C, A, B`. Node A joined exactly as rd vms-b34
measured, this time with Node B's machine present on the same hub:

```
[   43.220886] %CNXMAN, this node is now a VAXcluster member
```

VAXC, on its own console:

```
%CNXMAN,  received VAXcluster membership request from system OVMXA
%CNXMAN,  proposing addition of system OVMXA
%CNXMAN,  completing VAXcluster state transition
```

## 4. CN=3 — NOT ACHIEVED, and what actually happens

Two runs, opposite orders, the same shape of failure — **whichever node arrives
third does not get in.**

**`C, A, B`** (`cn3-blocked/`). VAXC admits OVMXA at t≈43 s. OVMXB then opens
its circuits and asks, repeatedly, for the rest of the run. VAXC logs the
request every ~17–23 s and **never proposes it**:

```
%CNXMAN,  received VAXcluster membership request from system OVMXB
%%%%%%%%%%%  OPCOM  24-SEP-2026 19:01:06.05  %%%%%%%%%%%
19:01:06.03 Node VAXC (csid 00010001) received VAXcluster membership request from node OVMXB
   ... x30, no "proposed addition of node OVMXB" ever ...
```

OVMXB says so honestly and fabricates nothing:

```
%CNXMAN, membership request to the selected member not answered
%CNXMAN, no cluster member answered this node's membership request: this node
         is NOT a cluster member, and will ask again
```

and OVMXA, the sitting member, logs `%CNXMAN, an unroutable VMS$VAXcluster
frame was received` when OVMXB first appears.

**`C, B, A`** (`timeline/`). VAXC admits OVMXB at t≈92 s (the §2 capture). When
OVMXA then boots, **it** is the one refused, with the mirrored signature:

```
[   41.178808] %PEA0, virtual circuit open
[   41.854323] %CNXMAN, a connection manager was discovered on the interconnect
[   42.208853] %CNXMAN, the cluster assigned this node a cluster system id
[   42.246333] %PEA0, peer announced departure, channel closed
[   43.125923] %CNXMAN, waiting to form or join an OpenVMS Cluster
```

So it is not "OVMX/VAX cannot join" and not "OVMX/x86 cannot join" — each joins
this real V7.3 node on its own. It is the **third** node.

### The measurement that makes this hard to read, and is not being hidden

`timeline/cba-timeline.json` counts every machine's own NIC transmit counter
every 15 s. The real VAX **stops transmitting entirely** in the same window the
third node's NIC comes up, and never resumes:

```
t=  360  tx={OVMXA: null, OVMXB: 1614, VAXC: 1429}   <- A clicked at t=360
t=  420  tx={OVMXA:   96, OVMXB: 2001, VAXC: 1679}   <- A's NIC up
t=  435  tx={OVMXA:  136, OVMXB: 2058, VAXC: 1679}   <- VAXC: +0, from here on
t=  601  tx={OVMXA:  538, OVMXB: 2672, VAXC: 1679}   <- OVMXA also stops
t= 1486  tx={OVMXA:  538, OVMXB: 4169, VAXC: 1679}   <- 15 minutes later
```

A node whose PEDRIVER has been wedged by a peer and a node whose *emulator* has
been starved of CPU by two other emulators look identical in that table. This
pod was measured at **2.0 CPU** while three emulators and a headless Chromium
shared it (`kubectl top pod`), on a `k3s-worker` at 85 % with 3 unrelated
`vaxlab` pods holding ~3 CPU — so starvation is a live, unexcluded explanation,
and rd vms-d25 already carries it as an open question for this rig.

**Nothing here asserts which it is.** A discriminator was run and is described
below; the conclusion this capture records is the *observation*, not a cause.

### The discriminator: same CPU cost, no cluster participation

`observe-discriminator.js`, `discriminator/`. The run is repeated with ONE
variable changed: the third node boots from an initramfs injected with
`--group 1` instead of 257. Same image, same emulator, same boot, same hub
port, and the hub still floods it every frame the other two send (its `rx`
reaches 12,568) — but everything it sends goes to another cluster's multicast
address and carries another cluster's number at abs 22, so VAXC and OVMXB must
ignore all of it. Same boot order, same 180 s staggers, same pod, same
neighbours on `k3s-worker`.

**The real VAX's own NIC transmit counter, third node clicked at t=360 s:**

```
     t    group 257 third node    group 1 third node
   360                    1429                  1467
   420                    1679                  1774
  1486                    1679                  7361
```

With a third **cluster participant**, VAXC stops transmitting within about a
minute of that node's NIC coming up and has not sent a frame 18 minutes later.
With a third node that costs exactly the same to emulate and is merely not in
the cluster, VAXC transmits **7,361** frames and is still going at the end of
the window — as is OVMXB (8,295), which stayed a member throughout.

**So the CN=3 stall is not the cost of a third emulator.** Host resourcing was
the live alternative explanation and this run removes it: what stops the real
VAX is a third node *taking part in the cluster*, not a third node existing.

That makes this an executive-side defect, and one in the most serious class
this project has — a real OpenVMS VAX peer stops driving its own interconnect
while OVMX is talking to it. It is reported, with this evidence, rather than
guessed at: no fix is attempted here, and nothing in this capture claims to
know which frame does it.

## 5. What is NOT claimed

* **CN=3 is not achieved**, in either boot order. No `SHOW CLUSTER` on any
  console named three members at any point in any run.
* No run here reached a driven `SHOW CLUSTER`/SDA read-back on all three
  consoles. On VAXC, `OPA0:`'s LOGINOUT read is broken by the OPCOM broadcast
  storm the unanswered-request loop itself generates (`%LOGIN-F-CMDINPUT, error
  reading command input`, audited on its own console) — a consequence of the
  failure above, not an independent one. The grader
  (`cn3-grade.js`) that does drive the three logins is kept here with the
  capture; it is a throwaway probe built on openvmx-site's committed
  `demo/cluster/e2e/e2e-boot.js` page-driving code, the same precedent as rd
  vms-2570's `probe-run3` and rd vms-b34's `cn2-grade.js`.
* The `frame-census.txt` capture was taken with an extra per-frame hex hook in
  the served page copy. That hook is a main-thread cost and is a plausible
  contributor to the hub throughput in *that* run; the frame BYTES it recorded
  are unaffected, and every run used for §2/§3/§4 was made with the page
  unmodified.

## Files

* `cn2-nodeB-v73/` — the OVMX/VAX ↔ real V7.3 CN=2 join: both consoles + harness log.
* `cn3-blocked/run2-*` — the `C, A, B` run: OVMXA admitted, OVMXB refused ~30 times.
* `timeline/` — the `C, B, A` run: OVMXB admitted, OVMXA refused, plus the
  15-second per-machine transmit timeline.
* `frame-census.txt` — every SCA frame at the hub by source/destination/type,
  with the group number read off abs 22..23 of each.
* `discriminator/` — the group-1 third-node control run: timeline, harness log,
  and that node's own console.
* `cn3-grade.js`, `observe-timeline.js`, `observe-discriminator.js` — the probes.

## Reproducing

```
# in a temporary k3s-worker pod (playwright image), never vax.3dl.network:
node coi-server.js  <demo bundle dir> 8110 &     # openvmx-site demo/cluster/e2e/coi-server.js
node pcjs-server.js <minimal pcjs tree> 8301 &   # machines/dec/vax/{browser,modules}, machines/modules

NODE_B="http://localhost:8301/machines/dec/vax/browser/ovmx-cluster.html?rom=ka655x.bin&diskgz=ovmx-vax-nodeB.img.gz" \
NODE_C="http://localhost:8301/machines/dec/vax/browser/ovmx-cluster.html?rom=ka655x.bin&diskgz=vms73-nodeC-cluster.dsk.gz" \
BOOT_ORDER="C,B,A" STAGGER_MS=180000 node observe-timeline.js
```

The pcjs tree needs **both** `machines/dec/vax/modules/v2/` and
`machines/modules/v2/`: `vaxworker.js` imports `../modules/v2/*` and those
modules in turn import `/machines/modules/v2/*`. A tree with only one of them
loads the page, shows a blank screen and reports `Worker error: undefined` —
which reads exactly like a dead guest.
