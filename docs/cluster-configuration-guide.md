# OVMX Cluster Configuration Guide

This guide is for an operator standing up an OVMX node in a VMScluster at
**V0.6**. It covers exactly what ships: how the cluster identity parameters are
carried, how a two-node cluster forms, what `SHOW CLUSTER` and `$GETSYI` report,
and what cross-node locking does for applications today. Just as important, it is
explicit about what a real VMScluster does that OVMX does **not** do yet — see
[Not yet supported at V0.6](#not-yet-supported-at-v06). Nothing below is
described as working unless a real code path ships it over the real executive at
`/dev/vms` (Rule 9 / INV-6).

> **Scope.** This is the *operator* view. The wire-protocol reverse-engineering
> and the clean-room provenance of every frame live in
> [`docs/cluster-protocol-spec.md`](cluster-protocol-spec.md); the executive
> membership design is [`docs/design-cluster-membership-executive.md`](design-cluster-membership-executive.md).

## Cluster identity parameters

An OVMX node's cluster identity lives in the SYSGEN parameter store,
`SYS$SYSTEM:OVMXVMSSYS.PAR` — the OVMX analogue of VMS's `VAXVMSSYS.PAR`. The
cluster daemon `scsd` reads it at boot through `sysgen_read_string()` /
`sysgen_read_param()` (honoring `OVMX_SYSGEN_PATH`; see `src/vmsscs/scsd.c`).

| Parameter | Meaning | Read by `scsd`? | Notes |
|---|---|---|---|
| `SCSNODE` | Cluster node name (max 6 chars) | Yes (`resolve_node_identity`) | Half of the fatal identity pair. Falls back to `OVMX` only if the store is unreadable. |
| `SCSSYSTEMID` | Cluster system ID | Yes (`resolve_scssystemid`) | The other half of the identity pair. Falls back to `1030`. `SCSNODE`+`SCSSYSTEMID` must be cluster-wide unique. |
| `ALLOCLASS` | Allocation class for shared cluster devices | Yes (`resolve_alloclass`) | Read and reported only; `0` is the documented default. Does not touch any wire frame. |
| `RECNXINTERVAL` | Reconnection interval, seconds | Yes (`scsd_recnxinterval`) | Sizes the reconnect period after a VC break. Default `20`. |
| `VAXCLUSTER` | Cluster participation (0/1/2) | No | Pre-seeded in the `.PAR`, but **not currently consulted** by `scsd`; participation is not gated on it at V0.6. |
| `VOTES` | Votes this node contributes | No | Pre-seeded, but `scsd` does **not** read or advertise the local value — OVMX always joins **non-voting** (advertises `VOTES=0`). See [votes/quorum](#votes-and-quorum-are-not-enforced). |
| `EXPECTED_VOTES` | Expected total cluster votes | No | Pre-seeded, but not reconciled — see [votes/quorum](#votes-and-quorum-are-not-enforced). |

### How you author these at V0.6: edit the pre-seeded `.PAR`

The identity parameters are **pre-seeded** in the shipped
`OVMXVMSSYS.PAR`. To configure a node, you edit that pre-seeded store — that is
the only authoring path at V0.6.

**The VMS-way authoring surface is not shipped at V0.6.** OVMX does **not** yet
provide any of:

- `SYSMAN PARAMETERS SET`/`SHOW`/`WRITE` for string parameters (numeric-only
  today; string params are filed as `vms-8da`),
- a `.PAR` *write* mechanism or conversational **SYSBOOT**,
- **AUTOGEN** / `MODPARAMS.DAT` feedback,
- `CLUSTER_CONFIG(_LAN).COM`.

So you cannot yet author cluster identity "the VMS way" and reboot into it. You
set identity by editing the pre-seeded `.PAR` (or by pointing `OVMX_SYSGEN_PATH`
at a store you have prepared), and `scsd` adopts it on the next boot. This
matches the reconciled milestone status in
[`docs/design-cluster-config-authoring.md`](design-cluster-config-authoring.md).

## Standing up a two-node cluster

1. **Give each node a unique identity.** In each node's pre-seeded
   `OVMXVMSSYS.PAR`, set a distinct `SCSNODE` (≤6 chars) and a distinct
   `SCSSYSTEMID`. Reusing a `SCSNODE`/`SCSSYSTEMID` a peer has recently seen on
   another system causes the join to be refused outright (the lab documents this
   as `%PEA0, Remote System Conflicts with Known System`).

2. **Match the cluster group.** OVMX joins the reference lab's **group 1** by
   default (`CLUSTER_AUTHORIZE` is a minimal stand-in — see
   [Not yet supported](#cluster_authorize-is-a-lab-only-stand-in)). Both nodes
   must be on the same LAN segment carrying the LAVC/SCA ethertype `0x6007`; the
   transport is genuine raw Ethernet, not a UDP tunnel (`src/vmsscs/scs_hello.c`,
   requires `CAP_NET_RAW`).

3. **Boot both nodes.** As the cluster forms, `scsd` on each node populates the
   executive membership block (below). Formation takes on the order of a minute.

4. **Confirm membership** with `SHOW CLUSTER`.

## What SHOW CLUSTER reports

`SHOW CLUSTER` reads the **real executive membership block** through `/dev/vms`
(`VMS_IOCTL_CLUSTER_MEMBER_GET` via `vms_kif_cluster_get_members()`), which
`scsd` populates with `VMS_IOCTL_CLUSTER_MEMBER_SET`/`CLEAR` as members join and
depart (`src/vmsdcl/dcl_cmd_show.c`, `src/vmsscs/scsd.c`). Every process reading
`/dev/vms` sees the same member set — there is no per-process fake behind it
(INV-6).

Three distinct outcomes, never conflated:

- **Members present** → the cluster view: a `View of Cluster from system ID N
  node: X` banner and a `SYSTEMS`/`MEMBERS` table with `NODE`, `SOFTWARE`, and
  `STATUS` columns. A peer whose `SCSNODE` name has not yet been learned is
  shown by its `SCSSYSTEMID`; peer `SOFTWARE` shows the family `VMS` without a
  version OVMX cannot vouch for.
- **Executive reachable, no cluster** → `%SYSTEM-I-NOTMEMBER, this system is not
  a member of a VMScluster` (`SS$_NORMAL`) — the genuine standalone-node answer.
- **Executive unreachable** (no `/dev/vms`) → `%SYSTEM-W-NOSUCHDEV`
  (`SS$_NOSUCHDEV`), a transport failure, *not* a cluster fact. On the real
  runtime `/dev/vms` is always present.

`$GETSYI` agrees with `SHOW CLUSTER`: `SYI$_CLUSTER_MEMBER` and
`SYI$_CLUSTER_NODES` also read the executive membership block through `/dev/vms`
(`src/libvms/syssvc/sys_misc.c`, vms-5919 — the file bridge has been retired from
these readers). If the executive is unreachable, the item is left honestly
unretrieved rather than answered from a file or a fabricated flag.

## Cross-node locking for applications ($ENQ / DLM)

The ENQ-class distributed lock manager is **real and complete** on a real
`/dev/vms` executive — the `vms-7fa` H0–H11 ladder. Between OVMX nodes it does,
today, over the SCS wire:

- cross-node `$ENQ` **grant** on the mastering node, held for the remote
  requester's CSID;
- **block-then-grant**: an incompatible request queues on the real waiting
  queue and grants on a real `$DEQ`;
- **blocking AST (BLKAST)** delivered over the wire, firing a genuine user-mode
  AST on the remote holder;
- **lock value block** replication both ways (write and read crossings);
- dynamic **mastering / remastering** to a survivor on graceful departure,
  rebuilding lock state from the survivors' real origin records;
- **directory-ownership** refusal (a node will not master a resource it is not
  the directory for);
- **distributed deadlock detection** by edge-chasing the real distributed
  wait-for graph, aborting a single globally-deterministic victim with
  `SS$_DEADLOCK`.

All of this is real executive state — no per-process fake ever answers; absence
is always an honest `SS$_UNSUPPORTED` (INV-6). See
[`docs/compat/facilities/cluster-dlm.yaml`](compat/facilities/cluster-dlm.yaml)
and `src/kernel-core/vms_lock.c`.

**Important limitation — how applications reach it.** Cross-node locking today is
**daemon-choreographed and CSID-keyed**. An ordinary application process that
issues `$ENQ` for a resource mastered on a *remote* node still receives an honest
`SS$_UNSUPPORTED` (the "0.4" stub in `vms_lock.c`); a real app-process cross-node
lock **acquisition** path is **post-1.0** (`vms-d1f`). In other words: the DLM
engine and its wire are proven between nodes, but a general application does not
yet transparently acquire a remotely-mastered lock the way it would on VMS.

## Not yet supported at V0.6

A real VMScluster does the following; OVMX at V0.6 does not. These are stated
plainly so no one designs against a capability that is not there.

### Votes and quorum are not enforced

**There is no split-brain protection at V0.6.** Be precise about why:

- OVMX always joins **non-voting**: `scsd` hardcodes an advertised `VOTES=0`
  (`SCS_MEMBER_VOTES_NONVOTING`) so it can never affect a VAX cluster's quorum.
  The local `VOTES`/`EXPECTED_VOTES` in your `.PAR` are **not read** by `scsd`.
- A quorum *model* is present and does run: `scsd` folds each peer's
  wire-advertised `VOTES` into a connection-manager quorum computation
  (`src/vmsscs/scs_quorum.c`, `cm_quorum_note_peer_votes`) and logs
  `SCSD-I-QUORUM ... quorum PRESENT/LOST`. But the gate result is **only
  logged** — it is **never wired to suspend I/O or reconfigure** the cluster.
  Quorum loss does not block anything.
- `EXPECTED_VOTES` is an open reverse-engineering gap on the wire (held at 1 in
  every capture), so the model seeds each peer's `EXPECTED_VOTES` from its
  advertised `VOTES` rather than reconciling a real value.

Net effect for an operator: do not rely on OVMX for quorum arbitration or
split-brain avoidance.

### MSCP-served volumes — absent

A node cannot serve a local disk to the cluster over MSCP, and cannot mount a
volume served by a peer. This is post-0.6 work (`vms-600`).

### Cluster-wide logical names — degrade to system-wide

`$CRELNM` with `LNM$M_CLUSTERWIDE` does not replicate a logical-name table across
members; the scope degrades to system-wide (`docs/compat/facilities/cluster-logicals.yaml`).

### Cluster-wide global sections — absent

`$MGBLSC`/`$CRMPSC` with cluster scope are not implemented.

### CLUSTER_AUTHORIZE is a lab-only stand-in

`CLUSTER_AUTHORIZE` is a **minimal OVMX stand-in** (`src/libvms/include/cluster_authorize.h`):
a tiny typed file holding a group number and a cleartext password, defaulting to
the reference lab's **group 1** only. There is no real `CLUSTER_AUTHORIZE.DAT`
on-disk format, no credential hashing, and no wire authentication. Joining an
**arbitrary** VMScluster (any group/password) is 1.0 work (`vms-732`, `vms-405`).

### DECnet — essentially greenfield

Cluster interconnect and application use of DECnet Phase IV is not implemented
(`vms-30e`).

## Clean-room provenance (Rule 8)

Everything OVMX knows about the VMScluster wire — SCS/NISCA framing, the
membership handshake, the DLM message class — is derived **only** from (a)
observing the wire on our own reference labs and (b) public OpenVMS
documentation (the Cluster Systems manual, `$ENQ`/`$DEQ`/`$LCKDEF`, SDA/SYSGEN/
SYSMAN documented tool output). No VSI/HPE VMS source or binary was ever
disassembled, decompiled, or consulted. The full provenance statement, and the
GROUNDED / inferred / unknown labeling of every field, is
[`docs/cluster-protocol-spec.md` §0](cluster-protocol-spec.md).

Where the public documentation does **not** publish a byte-level layout, OVMX
defines its **own** representation and labels it an **OVMX design choice** — it is
**never** presented as VMS-authentic. In particular:

- The **DLM SCS message opcodes and byte layout** are an OVMX design choice: the
  *semantic* field values are authentic `$LCKDEF` (grant modes, `$ENQ` flags,
  the 16-byte value block) and the routing algorithm is the documented
  directory/master resolution, but the on-wire opcodes (`ENQ=1, GRANT=2, DEQ=3,
  BLKAST=4, REBUILD=5, DLKSRCH=6`) and the body offsets are OVMX-defined,
  because VSI/HPE do not publish the lock manager's SCS byte layout. They will
  be replaced with the authentic layout once a real VAX DLM capture grounds it
  (`docs/compat/facilities/cluster-dlm.yaml`).
- Con.ID assignments and similar internal identifiers are OVMX design choices.

If a future capture contradicts anything OVMX currently emits, the wire wins and
OVMX changes — see the spec's provenance discipline.
