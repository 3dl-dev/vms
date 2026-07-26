# Design: OVMX as a VMScluster Member Node

> Status: DRAFT (vms-pivot.2). Architecture for evolving OVMX from a standalone
> VMS-compatible environment into a node that joins a **real** VMScluster.
> Clean-room invariant (Q6): everything here is derived from observing the wire
> on our SIMH reference cluster + public OpenVMS documentation. No VSI/HPE source
> or binaries are ever read, disassembled, or copied.

## 1. Goal

An OVMX Linux node joins a customer's live VMScluster: it appears in `SHOW
CLUSTER`, participates in the distributed lock manager, serves/accesses MSCP
disks, and can host a workload evacuated from a VSI node — with no Unix leaks
visible to VMS peers or applications. This is Rail A (`vms-ci`); it converges
with Rail B (image activation, `vms-913`) at the rolling evacuation.

## 2. The VMScluster protocol stack (target)

What OVMX must reproduce, bottom-up. Layers confirmed on the wire from our
reference captures are marked ✓.

| Layer | What it is | OVMX must |
|-------|-----------|-----------|
| **Datalink** ✓ | Raw Ethernet, ethertype **0x6007** (DEC LAVC/SCA). HELLO beacons to multicast **AB-00-04-01-xx**; DECnet MACs AA-00-04-00-<node>. | Send/receive raw 802.3 frames of these types on the shared L2 segment |
| **PEDRIVER / NISCA** | The LAN "port": discovers peers via HELLOs, builds **virtual circuits** (VCs) between nodes over one or more LAN paths (channels), handles retransmit/sequencing. | Implement HELLO discovery + VC establishment + the NISCA transport (datagram + sequenced message + block transfer) |
| **SCS (System Communication Services)** | Above the port: named **SYSAPs** on each node form **connections**; SCS carries datagrams, sequenced messages, and block data (for bulk/RDMA-like transfer) between them. | Implement the SCS connection model and the three message classes |
| **SYSAPs** | The system applications that talk over SCS: `VMS$VAXcluster` (connection manager), the **MSCP server/class driver** (disk serving), the **lock manager's** cluster arm, and others. | Implement each SYSAP OVMX needs, starting with the connection manager |
| **Connection Manager** | Cluster **membership**: votes/expected-votes → **quorum**, cluster **state transitions** (node add/remove), the cluster "coordinator". Prevents partitioned clusters (split-brain). | Join as a (initially non-voting) member; correctly follow state transitions |
| **Distributed Lock Manager (DLM)** | Cluster-wide `$ENQ`/`$DEQ`. Each **resource** is *mastered* on one node (found via a **directory** node by hashing the resource name); lock ops route to the master; **remastering** occurs on state transitions. | Extend the local lock manager into a distributed one (see §5). *Highest risk.* |
| **MSCP server / class driver** | Serves local disks to the cluster and mounts remote-served disks; `$n$DUAd` naming with **allocation class**. | Implement MSCP client (mount served disk) then server (serve a disk) |
| **Cluster-wide services** | Cluster-wide logical names, cluster-wide file access (XQP/RMS locking via the DLM), cluster-wide `SHOW CLUSTER`/SYSMAN. | Layer onto the DLM once it exists |

### 2.1 Identity & security
A member is defined by **SCSSYSTEMID** and **SCSNODE** (node name), an
**allocation class**, and — critically — the **cluster group number + cluster
password** (the shared secret in the SCA HELLO authentication). OVMX must present
a valid, non-colliding identity and the matching group/password to be admitted.
These are the exact fields we are decoding from the 134-byte SCA HELLO.

## 3. OVMX architecture — where each layer lives

The central architectural question: **kernel or userspace?** In OpenVMS all of
this lives in the executive (kernel). OVMX has both a userspace stack and kernel
modules (`vms.ko`, `vmsfs.ko`), so we choose per layer.

**Recommendation — a phased split:**

- **Datalink + NISCA + SCS + connection manager → userspace daemon (`SCSD`)**, using an `AF_PACKET` raw socket bound to the cluster interface.
  - *Why userspace first:* it is the fastest path to the RE-driven rungs (membership, MSCP), keeps Docker mode viable, is far easier to iterate against the reference-cluster oracle, and isolates protocol bugs from the host kernel. Ethertype 0x6007 is non-IP, so `AF_PACKET` (raw, `ETH_P_ALL` or the specific protocol) sees and sends exactly these frames.
  - *Cost:* userspace latency and scheduling jitter. Acceptable for HELLO/VC/membership/MSCP; revisited for the DLM hot path.
- **DLM → hybrid.** The *local* lock manager already lives in `vms.ko`. The distributed arm (directory lookup, master routing, remastering) is protocol logic that can live in `SCSD`, but the **grant/release fast path must stay coherent with the kernel lock manager**. Design the kernel lock manager to delegate remote resources to `SCSD` via a well-defined interface (see §5), rather than duplicating lock state.
- **MSCP → userspace initially** (serve/mount via the SCS daemon backed by the existing device/disk layer), with a path to kernel block integration later.

This keeps the risky, iterative protocol work in userspace where we can move fast
against the oracle, while preserving a route to kernel-tight semantics where
correctness demands it (the DLM).

### 3.1 The SCSD daemon (new component)
A new `src/vmsscs/` subsystem: the raw-socket datalink, NISCA VC engine, SCS
connection multiplexer, and the connection-manager SYSAP. Other SYSAPs (MSCP,
DLM-remote) register with it. It exposes a local IPC (UNIX socket / shared
memory) to the kernel modules and to userspace consumers (`SHOW CLUSTER`, mount).

## 4. Kernel vs userspace trade-off (recorded decision)

| | Userspace daemon (chosen for v1) | Kernel module |
|---|---|---|
| Dev velocity vs oracle | High — iterate, capture, diff | Low — rebuild/reboot cycle |
| Docker-mode viability | Yes | No (needs privileged kernel) |
| Latency / atomicity | Adequate for membership/MSCP; risk for DLM | Native VMS-like |
| Blast radius of bugs | Contained to a process | Can panic host |
| Path to DLM coherence | Via defined kernel interface | Direct |

**Decision:** userspace `SCSD` for datalink→SCS→connection-manager→MSCP; kernel
lock manager delegates remote resources to `SCSD` for the DLM. Re-evaluate moving
NISCA into a kernel module only if measured latency blocks correctness.

## 5. Distributed Lock Manager (the crown jewel)

The local lock manager grants/queues locks on named resources within one node.
The DLM extends this cluster-wide:

- **Resource directory:** the resource name hashes to a *directory node* that
  knows which node *masters* the resource. Lock/convert/dequeue requests route to
  the master; the master maintains the granted/conversion/waiting queues and the
  lock value block.
- **Mastering & remastering:** a resource is mastered on first use; on cluster
  **state transitions** (node join/leave) the directory and masters are
  **rebuilt/rebalanced** — the dangerous part.
- **OVMX split:** keep the per-node grant/queue/LVB machinery in the kernel lock
  manager (it already exists); add a `SCSD`-side DLM SYSAP that (a) answers
  directory lookups, (b) forwards remote lock ops over SCS, (c) drives
  remastering during transitions. The kernel manager calls out to `SCSD` for any
  resource not locally mastered.

**Prerequisite (must land before any DLM work):** the public `$ENQ`/`$DEQ` in
`src/libvms/syssvc/sys_lock.c` currently uses `flock()` and bypasses the kernel
lock manager entirely (see §6). Rewire it to `vms_kif_enq` so there is a *single*
authoritative lock manager to make distributed. Tracked as its own item.

**Risk posture:** DLM bugs corrupt shared data or hang the whole cluster — this
is not "retry" territory. `vms-ci.5` must not be claimed done without a stress
test against the reference cluster showing zero integrity violations across
repeated state transitions. Ship membership and MSCP first; approach the DLM only
with the dissector spec complete and a test harness in place.

## 6. Existing-component assessment (keep / rework / drop)

Ground truth from a full codebase survey. **Bottom line: OVMX is strictly
single-node today.** Everything labeled "cluster" in headers (`cluevtdef.h`,
`sysevtdef.h`, `glockdef.h`, `SYI$_CLUSTER_MEMBER/NODES` in `prcdef.h`) is an
**unimplemented API stub** — surface only, no code behind it. The only
*architecturally* cluster-friendly assets are the kernel lock manager's
blocking-AST design and the cross-process common event flags.

| Component | Verdict | Detail |
|-----------|---------|--------|
| **Kernel lock manager** `src/kernel/vms_lock.c` | **KEEP + rework** → DLM seed | 6-mode matrix, 16-byte value blocks, global resource hash, RB-tree lock IDs, BFS deadlock detection, blocking ASTs — all present. Add: node/CSID dimension on resources & lock IDs, directory-node/mastering protocol, hierarchical (parent) locks (currently stubbed). |
| **libvms `$ENQ`** `src/libvms/syssvc/sys_lock.c` | **DROP / replace** ⚠ | The *public* `$ENQ`/`$DEQ` is implemented with POSIX `flock()` on `/tmp/ovmx/locks/*.lck` — it **bypasses the kernel lock manager entirely**, collapses 6 modes to shared/excl, has no real value blocks / blocking ASTs / deadlock detection. Its header comment falsely claims "distributed." **This disconnect must be fixed before any DLM work** — rewire `$ENQ` to `vms_kif_enq` (the wrappers already exist in `vms_kif.c`). |
| **AST + common event flags** `src/kernel/vms_ast.c`, `vms_eflag.c` | **KEEP** | Solid async substrate; the lock manager already delivers blocking ASTs through it (SIGUSR1 → per-mode queues). Common event-flag clusters 2-3 are already cross-process shared state — the conceptual seed for cluster-wide events. |
| **Networking / datalink** | **BUILD NEW** (greenfield) | No AF_PACKET, no raw ethernet, no DECnet/SCS anywhere. `$QIO` has no network path (`sys_qio.c` handles only file VBLK I/O). Only SSH (libssh/TCP) and an AF_UNIX socket for the LNM daemon exist. The entire SCS/NISCA stack is new (`src/vmsscs/`). |
| **Device / disk layer** `src/vmsfs/vmsfs_device.c` | **REWORK** | Static 16-entry name→mountpoint table; no allocation class, no UCB, no MSCP. Add allocation-class namespace (`$n$DGAd`); hang the MSCP server off the `vmsfs.ko` block layer (`src/kernel/vmsfs/vmsfs_blkdev.c`). `DVI$_MSCP_SERVED` is defined but unimplemented. |
| **SYSGEN / node identity** `src/libvms/include/sysgen_params.h`, `ovmx_init.c` | **REWORK** | No SCSNODE/SCSSYSTEMID/VOTES/EXPECTED_VOTES/ALLOCLASS/VAXCLUSTER params exist, and the param struct holds only a `uint32_t` — it **cannot even store a string** like SCSNODE. Node name = Linux hostname (hardcoded `"OVMX"`). Need a node-identity store (extend SYSGEN to typed/string params, or a dedicated cluster-config file) before OVMX can present an identity + cluster group/password. |
| **RMS locking** `src/vmsrms/*` | **REWORK (large)** | RMS does **no file or record locking at all** — not even locally. `FAB$B_SHR`/`RAB$M_RLK` are parsed into structs but never enforced; only a process-local pthread mutex on the IFI counter. Cluster-wide (or even multi-process) file access requires wiring `FAB$B_SHR`→file locks and `RAB$M_RLK`→record locks through the (fixed) lock manager. |
| **Kernel module ABI** `src/kernel/vms_module.c`, `vms_ioctl.h` | **EXTEND (clean)** | Single `unlocked_ioctl` dispatch on `/dev/vms`, magic `'V'`; families 0x01–0x40 used. Add SCS control ops as a new family (e.g. 0x50) + `vms_scs.c` + `vms_kif_*` wrappers. The datalink itself is better as an AF_PACKET consumer in `SCSD` than an ioctl family. |
| **Process/AST substrate** `src/vmsprocess/*` | **KEEP** | Reuse for delivering async cluster state transitions and lock grants. `$SETCLUEVT`/`$SET_SYSTEM_EVENT` need real implementation atop the (new) connection manager. |
| **Cluster stub headers** | **IMPLEMENT** | `cluevtdef.h`, `sysevtdef.h`, `glockdef.h`, `SYI$_CLUSTER_*` — wire real implementations as the SYSAPs come online. |

**Two immediate, isolated prerequisites this surfaced (independent of the wire RE):**
1. **Rewire `$ENQ`/`$DEQ` to the kernel lock manager** — the public lock API bypassing the real manager is a correctness bug *and* a hard blocker for the DLM. Tracked separately; blocks `vms-ci.5`.
2. **Extend SYSGEN + add a node-identity store** (SCSNODE, SCSSYSTEMID, ALLOCLASS, VOTES/EXPECTED_VOTES, cluster group/password) — blocks membership (`vms-ci.3`).

## 7. Mapping to the vms-ci ladder

- `vms-ci.2` dissector/spec → defines the wire formats §2 depends on.
- `vms-ci.3` membership → `SCSD`: datalink + NISCA VC + SCS + connection-manager SYSAP + identity/HELLO auth. **This design gates ci.3.**
- `vms-ci.4` MSCP → MSCP SYSAP over `SCSD` backed by the device/disk layer.
- `vms-ci.5` DLM → kernel-lock-manager ↔ `SCSD` DLM SYSAP split (§5).
- `vms-ci.6` evacuation → converges with Rail B (image activation).

## 8. Open questions for the operator / later decisions

- Target protocol **version** to implement first = whatever the reference cluster (VAX 7.3) negotiates; Alpha/x86 version differences deferred.
- Whether OVMX joins **non-voting** first (safest — cannot break quorum) — recommended.
- Allocation-class and `SCSNODE` assignment policy for OVMX nodes.
- When (if) to promote NISCA into a kernel module.
