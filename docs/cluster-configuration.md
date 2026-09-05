# OpenVMX Cluster Configuration

How to configure a booted OpenVMX node so it joins an existing OpenVMS Cluster
(VAXcluster) over the LAN and is counted as a real member.

**See it:** [`docs/demos/cluster-cn3.cast`](demos/cluster-cn3.cast) is a live
asciinema recording of a booted, cap-denied OVMX node reaching `CLUSTER_NODES=3`
against the real lab cluster — `asciinema play docs/demos/cluster-cn3.cast`
(details in [`demos/cluster-cn3.md`](demos/cluster-cn3.md)).

This is the operator-facing guide. The engineering record of *how* the join was
made faithful — the wire protocol, the crash-vector hunt, the executive-resident
SCS/CNXMAN/DLM stack — lives in [`cluster-integration-notes.md`](cluster-integration-notes.md)
(the E1–E85 ledger) and [`design-faithful-cluster-executive.md`](design-faithful-cluster-executive.md).
The config-authoring design is [`design-cluster-config-authoring.md`](design-cluster-config-authoring.md).

---

## What membership means here

An OpenVMX node joins a cluster the way a real VMS node does: it brings up a
cluster port on the Ethernet, multicasts SCA `HELLO`s scoped to a cluster group,
opens an SCS virtual circuit to each existing member, connects the
`VMS$VAXcluster` connection-manager SYSAP, and is admitted through the
membership/state-transition barrier. When it succeeds, the **existing members'
own accounting** counts it:

- `SHOW CLUSTER` on a member lists your node with `STATUS = MEMBER`.
- `F$GETSYI("CLUSTER_NODES")` on every member increases to include you.
- SDA (`ANALYZE/SYSTEM` → `SHOW CLUSTER`) shows your node's Cluster System Block
  with a genuine, sequential CSID and the `member` flag.

The whole join runs **inside the executive** (`vms.ko`), not a userspace daemon.
A node proven this way ran with `CAP_NET_RAW` dropped — the executive drove the
L2 I/O itself. See the proof capture at
[`tests/lab/captures/cn3-achieved-20260905.md`](../tests/lab/captures/cn3-achieved-20260905.md).

> **Crash-safety is release-gating.** OpenVMX must never emit wire traffic that
> can bugcheck a peer VMS node. Every frame the node sends is checked at emit
> time against a measured crash-safe envelope (the E82 runtime guard), and every
> lab capture is re-audited post-hoc (`tools/cluster/cm_wire_safety_audit.py`).
> A frame that would violate the envelope is dropped fail-safe rather than sent.
> See [`cluster-crash-safety.md`](cluster-crash-safety.md).

---

## Prerequisites

- **An existing cluster to join.** OpenVMX at this edition joins an existing
  cluster; it does not boot cluster satellites or serve a duplicate system disk.
- **The same LAN segment.** SCA runs over raw Ethernet (protocol `60-07`). Your
  node and the existing members must share an L2 broadcast domain — the SCA
  `HELLO` is a multicast, not routed.
- **The cluster group number, and its password if the cluster sets one.** This
  is the `CLUSTER_AUTHORIZE` group that scopes which cluster your `HELLO`s reach.
  A node in group *N* only ever talks to members in group *N*.
- **A unique `SCSNODE` and `SCSSYSTEMID`.** Two members may not share either.

---

## The front door: `CLUSTER_CONFIG_LAN.COM`

Configure clustering the way a VMS admin expects — an interactive procedure that
drives SYSGEN to author your node's identity into
`SYS$SYSTEM:OVMXVMSSYS.PAR`, adopted on the next reboot:

```
$ @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM
```

Choose **ADD** to enable clustering on this standalone node and author its
identity, or **CHANGE** to reconfigure an already-clustered node. The procedure
prompts for `SCSNODE`, `SCSSYSTEMID`, `ALLOCLASS`, `VOTES`, and
`EXPECTED_VOTES`, echoes the set back for confirmation, and writes them through
SYSGEN. (`CLUSTER_CONFIG_LAN.COM` is the public VMS name for the LAN/Ethernet
configuration procedure; OpenVMX clusters over raw-Ethernet SCA, so the LAN
variant is the faithful one. The menu offers only what this edition can honestly
perform — see [Not available at this edition](#not-available-at-this-edition).)

### The cluster group and password: `CLUSTER_AUTHORIZE.DAT`

The group number (and optional password) live in
`SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT`, exactly as on VMS. The executive loads it at
boot; the group number becomes the SCA multicast group your `HELLO`s are scoped
to (`vms_cluster_hello_mcast_build`). A node with no `CLUSTER_AUTHORIZE.DAT`
boots with group 0 and reaches no cluster. Set it to the group the target
cluster uses (for example, the lab cluster is group 257).

---

## The parameters, and what each one actually does

Every parameter is honest about its effect. Some drive real executive behavior;
some are recorded and reported but do not yet gate anything at this edition. The
procedure says so at the prompt; it is repeated here so you can plan.

| Parameter | Effect at this edition |
|---|---|
| **`SCSNODE`** (1–6 chars) | **Effectual.** Your node's wire name. The port and connection manager put it on the wire. `CLUSTER_START` is **refused** (`SS$_BADPARAM`) if `VAXCLUSTER` is nonzero and no `SCSNODE` is authored. |
| **`SCSSYSTEMID`** | **Effectual.** Your node's wire system id. Must be unique in the cluster. |
| **`VAXCLUSTER`** | **Effectual, boot-time.** `0` (shipped default) brings up **no** cluster port — no `HELLO` leaves the node. `1` joins if a cluster is present, else reports STANDALONE. `2` forms or joins, printing *"waiting to form or join an OpenVMS Cluster"* on `OPA0:` until it can. |
| **cluster group / password** | **Effectual.** From `CLUSTER_AUTHORIZE.DAT`. Scopes which cluster you reach. |
| **`RECNXINTERVAL`** | **Effectual.** The executive's reconnect loop uses it (seconds a broken circuit may take to recover before the member is removed). |
| **`TIMVCFAIL`** | **Effectual.** The executive's virtual-circuit failure detector uses it. |
| **`VOTES` / `EXPECTED_VOTES` / `QDSKVOTES`** | **Recorded and read, not yet enforced.** The connection manager computes CEVOTES and QUORUM from them and `SHOW CLUSTER` displays the result — but nothing yet **blocks** on lost quorum. Quorum arithmetic is tracking-only at this edition. |
| **`ALLOCLASS`** | **Recorded and reported**, not yet acted on (for `$n$DUAn` device naming). |
| **`LOCKDIRWT`** | **Recorded.** `0` = never a lock-directory node. |
| **`MSCP_LOAD` / `MSCP_SERVE_ALL`** | Govern whether this node serves its disks to the cluster (see the MSCP server; `MSCP_LOAD` gates the served-disk controller). |
| **`CLUSTER_CREDITS`** | **Effectual.** Receive buffers this node commits to each virtual circuit; the executive's credit ledger grants them out of real allocated buffers. Factory default 10. |
| **`NISCS_MAX_PKTSZ`** | Requested SCA packet size, clamped to the interface MTU by the port. |
| **`DISK_QUORUM`** | Quorum-disk device name (empty = none). Recorded; quorum-disk voting is not enforced at this edition. |

### How they take effect

There is **no cluster daemon** reading these files for itself. The chain is the
VMS chain:

1. `CLUSTER_CONFIG_LAN.COM` (or `SYSGEN`) writes the parameters into
   `SYS$SYSTEM:OVMXVMSSYS.PAR`; the group/password into `CLUSTER_AUTHORIZE.DAT`.
2. On the next boot, **`STARTUP.EXE`** loads the whole parameter set into the
   **executive** (`VMS_IOCTL_SYSGEN_LOAD`), loads `CLUSTER_AUTHORIZE.DAT`, then
   starts the cluster (`VMS_IOCTL_CLUSTER_START`).
3. `CLUSTER_START` is refused with `SS$_BADPARAM` if `VAXCLUSTER` is nonzero and
   the identity is incomplete — the executive will not put a nameless node on
   the wire.

> Advanced: the binary parameter store is `SYS$SYSTEM:OVMXVMSSYS.PAR`. You can
> point the executive at an alternate store with `OVMX_SYSGEN_PATH`, but hand-
> editing the binary store is Linux plumbing no VMScluster operator should need
> — use `CLUSTER_CONFIG_LAN.COM`.

---

## A minimal join, end to end

To bring a standalone OpenVMX node into cluster group *G* as node `MYNODE`:

```
$ @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM
    ... choose ADD ...
    What is the node's SCSNODE name        : MYNODE
    What is the node's SCSSYSTEMID          : 1986
    Allocation class ... [0]                : 0
    Votes this node contributes [1]         : 1
    Expected total cluster votes [1]        : 3
    ... enable cluster participation (VAXCLUSTER) ...
```

Ensure `SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT` names group *G* (and the password if
the cluster requires one), then reboot. On the way up you will see the node
bring up its cluster port and, with `VAXCLUSTER = 2`, the *"waiting to form or
join"* banner on `OPA0:` until the existing members admit it.

---

## Verifying membership

Confirm from an **existing member** (its accounting is the source of truth, not
the joining node's):

```
$ SHOW CLUSTER
$ WRITE SYS$OUTPUT F$GETSYI("CLUSTER_NODES")
$ ANALYZE/SYSTEM
SDA> SHOW CLUSTER
```

You want your node listed with `STATUS = MEMBER`, `CLUSTER_NODES` grown to
include it, and an SDA CSB carrying a genuine sequential CSID with the `member`
flag — **sustained**, not a flicker during a transition. A booted node that
appears for a moment and drops was not admitted; a counted, stable member is.

---

## Not available at this edition

Stated plainly, in the spirit of the procedure's own honesty (nothing here is
faked to look present):

- **Booting satellites / duplicate system disks.** `CLUSTER_CONFIG_LAN.COM`
  configures the **local** node only. `ADD` does not provision a remote
  satellite root; `CREATE a duplicate system disk` is not offered.
- **`REMOVE` of a remote member's root.** A node leaves by `CHANGE` with
  `VAXCLUSTER 0`, then reboot.
- **Quorum enforcement.** `VOTES`/`EXPECTED_VOTES`/`QDSKVOTES` are computed and
  displayed but nothing blocks on lost quorum yet — the arithmetic is
  tracking-only. Do not rely on OpenVMX to hang for quorum.
- **`ALLOCLASS`-based device naming** and **quorum-disk voting** are recorded
  but not yet acted on.

Known follow-on (tracked, not a membership blocker): in the proven join the
member committed the transition without an on-wire barrier-release (`op-0c`)
frame; what completes the transition without it is under investigation. See the
ledger.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Node boots STANDALONE, no `HELLO` on the wire | `VAXCLUSTER = 0` (the default), or no `SCSNODE` authored. |
| `HELLO`s leave but no member ever answers | Wrong cluster group in `CLUSTER_AUTHORIZE.DAT`, or the node is not on the members' L2 segment. |
| `CLUSTER_START` refused (`SS$_BADPARAM`) | `VAXCLUSTER` nonzero with an incomplete identity — author `SCSNODE`/`SCSSYSTEMID`. |
| Node appears then drops from `SHOW CLUSTER` | A virtual-circuit or admission failure — check the members' consoles and OPCOM. Capture the wire (`ether proto 0x6007`) and run it through `tools/cluster/cm_wire_safety_audit.py`. |
| A member bugchecked | This must never happen from an OpenVMX frame and is release-gating. Capture the pcap, run the safety audit, and file it — see [`cluster-crash-safety.md`](cluster-crash-safety.md). |
