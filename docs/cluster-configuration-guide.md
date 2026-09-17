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
`SYS$SYSTEM:OVMXVMSSYS.PAR` — the OVMX analogue of VMS's `VAXVMSSYS.PAR`. It is
read **at boot** by `STARTUP.EXE` (PID 1, `src/ovmx_init/ovmx_init.c`):
`read_boot_parameters()` applies `SCSNODE` to the running node's name, and
`load_cluster_sysgen_params()` loads the whole cluster set into the **executive**
through `VMS_IOCTL_SYSGEN_LOAD` (`src/kernel-core/vms_cluster_sysgen.c`). There is
no `scsd` daemon reading the file for itself any more. `OVMX_SYSGEN_PATH` is a
developer override used by host-side tests; the booted system resolves the store
through the Files-11 ODS-2 ACP over `/dev/vms`.

| Parameter | Meaning | Adopted at boot? | Notes |
|---|---|---|---|
| `SCSNODE` | Cluster node name (max 6 chars) | Yes | Half of the identity pair; `STARTUP.EXE` sets the running node name from it (proven end to end — see below). Falls back to `OVMX` only if the store is unreadable. |
| `SCSSYSTEMID` | Cluster system ID | Yes | The other half of the identity pair; loaded into the executive at boot. `SCSNODE`+`SCSSYSTEMID` must be cluster-wide unique. |
| `ALLOCLASS` | Allocation class for shared cluster devices | Recorded | Loaded and reported only; `0` is the documented default. Does not touch any wire frame. |
| `RECNXINTERVAL` | Reconnection interval, seconds | Yes | Sizes the reconnect period after a VC break. Default `20`. |
| `VAXCLUSTER` | Cluster participation (0/1/2) | Yes | The boot-time decision: `0` (the shipped default) brings up **no** cluster port at all; `1`/`2` bring the SCS port up and join/form. It gates the port only — the identity above is loaded regardless of its value. |
| `VOTES` | Votes this node contributes | Recorded | Loaded and persisted, but OVMX always joins **non-voting** (advertises `VOTES=0`); the local value is not advertised. See [votes/quorum](#votes-and-quorum-are-not-enforced). |
| `EXPECTED_VOTES` | Expected total cluster votes | Recorded | Loaded and persisted, but not reconciled — see [votes/quorum](#votes-and-quorum-are-not-enforced). |

### How you author these: `@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM`

The VMS-canon way to configure a node's cluster identity **is shipped**: the
operator procedure `SYS$MANAGER:CLUSTER_CONFIG_LAN.COM` (`CLUSTER_CONFIG.COM`
forwards to it, exactly as on VMS). It is the front door a VMScluster admin
expects — an interactive

```
$ @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM
```

that drives SYSGEN (`USE CURRENT` / `SET SCSNODE`… / `WRITE CURRENT`) to author
`SCSNODE` / `SCSSYSTEMID` / `ALLOCLASS` / `VOTES` / `EXPECTED_VOTES` into
`SYS$SYSTEM:OVMXVMSSYS.PAR`, and is **adopted on the next reboot** by
`STARTUP.EXE` (above). This author → reboot → adopt round-trip is proven end to
end, on a real boot, by
[`tests/qemu/test_cluster_config_lan_e2e.sh`](../tests/qemu/test_cluster_config_lan_e2e.sh):
after the procedure authors a new `SCSNODE` and the node reboots, the boot
console announces `%OVMX-I-SCSNODE, node name … set from SYS$SYSTEM:OVMXVMSSYS.PAR`
and `F$GETSYI("NODENAME")` (the live node name) reads the authored value.

To change this node's identity, run the procedure and pick **CHANGE** (menu
option 2), which reconfigures the local node without altering `VAXCLUSTER`; pick
**ADD** (option 1) to additionally enable cluster participation (`VAXCLUSTER=2`)
on a node that is standalone today. Then reboot. The procedure prints an honest
"not available at this edition" for verbs OVMX cannot perform (REMOVE of a remote
member's root, CREATE of a duplicate system disk) — it never fakes them.

**Still not shipped** (these remain the honest deferrals; the procedure above
does not depend on any of them):

- `SYSMAN PARAMETERS SET`/`SHOW`/`WRITE` for string parameters (numeric-only
  today; string params are filed as `vms-8da`),
- **AUTOGEN** / `MODPARAMS.DAT` feedback.

Conversational **SYSBOOT** (`ovmx.flags=0,1` → the `SYSBOOT>` prompt) is also
available as an alternate pre-boot authoring surface for the same parameters
(see [`docs/design-cluster-config-authoring.md`](design-cluster-config-authoring.md)),
and editing the pre-seeded `.PAR` directly (or pointing `OVMX_SYSGEN_PATH` at a
prepared store) still works for scripted setups. But
`CLUSTER_CONFIG_LAN.COM` is the documented operator path.

## Standing up a two-node cluster

1. **Give each node a unique identity.** On each node run
   `@SYS$MANAGER:CLUSTER_CONFIG_LAN.COM` (see
   [How you author these](#how-you-author-these-sysmanagercluster_config_lancom))
   and author a distinct `SCSNODE` (≤6 chars) and a distinct `SCSSYSTEMID`, then
   reboot. Reusing a `SCSNODE`/`SCSSYSTEMID` a peer has recently seen on another
   system causes the join to be refused outright (the lab documents this as
   `%PEA0, Remote System Conflicts with Known System`).

2. **Match the cluster group.** OVMX joins the reference lab's **group 1** by
   default (`CLUSTER_AUTHORIZE` is a minimal stand-in — see
   [Not yet supported](#cluster_authorize-is-a-lab-only-stand-in)). Both nodes
   must be on the same LAN segment carrying the LAVC/SCA ethertype `0x6007`; the
   transport is genuine raw Ethernet, not a UDP tunnel (`src/vmsscs/scs_hello.c`,
   requires `CAP_NET_RAW`).

3. **Boot both nodes.** As the cluster forms, the executive on each node
   populates its membership block (below). Formation takes on the order of a
   minute.

4. **Confirm membership** with `SHOW CLUSTER`.

## What SHOW CLUSTER reports

`SHOW CLUSTER` reads the **real executive membership block** through `/dev/vms`
(`VMS_IOCTL_CLUSTER_MEMBER_GET` via `vms_kif_cluster_get_members()`), which the
executive populates with `VMS_IOCTL_CLUSTER_MEMBER_SET`/`CLEAR` as members join
and depart (`src/vmsdcl/dcl_cmd_show.c`). Every process reading `/dev/vms` sees
the same member set — there is no per-process fake behind it (INV-6).

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
