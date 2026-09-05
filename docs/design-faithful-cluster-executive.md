# Design — the faithful, executive-resident VMScluster stack

> Status: DESIGN (architecture + phased plan), 2026-09-02. Executes the operator
> ruling of 2026-09-02: the cluster stack (port driver, SCS, connection manager,
> DLM distributed operation and rebuild, MSCP serving, quorum) is built
> **inside the executive** (`vms.ko` / the NetBSD kmod), at full VMS depth.
> The userspace `src/vmsscs/` daemon is a non-portable strawman: harvest its
> wire knowledge as spec, discard its orchestration.
>
> Grounding: origin/main at `e94d78da` (+28 on the local checkout); every
> file:line below was read on that tree. Clean-room (Rule 8): VMS behaviour is
> taken from public documentation (Davis, *VAXcluster Principles*; the OpenVMS
> Cluster Systems / IDSM lock-management chapters; AA-L619A-TK for MSCP) and
> from our own lab captures (`docs/cluster-protocol-spec.md`). No VSI/HPE
> source or binary is consulted anywhere in this design.
>
> How to read: §2 is the audit (what is real vs strawman, cited). §3 is the
> target architecture with the hard calls — §3.2.1/§3.2.2 the substrate seam
> that makes Linux and NetBSD-VAX one source, §3.6 the DLM roles and the
> directory-resolution ladder, §3.9 the structure/testability rules. §4 is
> the phased plan, structured to decompose into rd outcome items. §5 lists
> what needs a lab oracle-capture before its phase can be fully specified.
> §6 is the crash-hazard register.

---

## 1. The frame: roles, not frames

Real OpenVMS runs SCS (the System Communication Services), the LAN port
driver (PEDRIVER / NISCA), the Connection Manager (CNXMAN: membership,
transitions, the join barrier, quorum) and the lock manager's distributed arm
(directory, mastering, rebuild) in kernel mode, serialized at fork IPL. A
cluster node is not a program that emits the right bytes; it is an executive
whose lock database, membership database and connection database are the
things the bytes describe. Every wire field is a projection of executive state.

Three invariants shape every decision here:

- **INV-6 (real data or honest omission).** Every value on the wire is read
  from a real executive object (LKB, RSB, CSB, CDT, VC). No field is copied
  frame→frame, templated, or placeholdered. The wire crashes of this campaign
  (INVLOCKID from a placeholder lock id; LOCKMGRERR from a corrupted echoed
  resource name; INCONSTATE from reflecting a peer's own Con.IDs) are all the
  same class: a frame with no state behind it.
- **Rule 8 (clean-room).** Frame *shapes* are reproduced from captures and
  public docs; unpublished *algorithms* (the DLM directory hash, the
  CLUSTER_AUTHORIZE credential hash, the transaction checksum) are never
  recomputed. Where VMS behaviour depends on such an algorithm, OVMX either
  consumes the cluster's answer from the wire or declines the role honestly
  (§3.6, §5).
- **Rule 9 (one runtime, executive serves, fail-honest).** No userspace
  fallback. If the executive cannot do it, the service fails with an `SS$_`
  status; it is never simulated per-process.

Two corollaries the operator has already ruled:

- A "green" result on the userspace daemon proves nothing and ports nothing
  (memory `cluster-must-be-executive-resident`). The tell is that cluster
  fixes shipped without a kernel rebuild.
- "Faithful" means the machine behind the wire exists. A registration frame
  without a real cross-node `$ENQ` behind it, a completion that echoes a
  granted handle it never recorded, a grant for a resource the executive does
  not master — all imitation (memory `executive-backed-not-wire-plumbing`).

---

## 2. Current-state audit — real vs strawman, per subsystem

Verdict key: **REAL** = executive-resident and genuine; **REAL/PASSIVE** =
genuine engine but only driven from userspace; **STRAWMAN** = implemented in
the userspace daemon (glibc process, `src/vmsscs/`); **ABSENT**.

| # | Subsystem (VMS) | What VMS does | OVMX today (cited) | Verdict |
|---|---|---|---|---|
| 1 | Cluster interconnect / LAN port (PEDRIVER, NISCA/PPD) | Executive port driver `PEA0:` owns the LAN device for ethertype 0x6007; HELLO/SOLICIT discovery, channel verify, virtual circuits with sequencing/ack/retransmit, datagram + sequenced-message + block-transfer services. | Frame builders + VC FSM in `src/vmsscs/scs_hello.c`, `scs_vc.c`, `scs_datalink.c`. Wire I/O: on a booted node the daemon reaches the NIC through `VMS_IOCTL_L2_OPEN/SEND/RECV/CLOSE` (`src/kernel/vms_l2.h:134-137`, `src/kernel-core/vms_l2.c:132-302`) — a kernel `AF_PACKET` socket exposed as a **synchronous ioctl pipe**; the daemon polls it. `vms_l2.c` is Linux-only and outside the NetBSD `SRCS` (`vms_l2.c:13-27`). No unsolicited receive path exists in the executive; `exec_kbackend.h` has no kthread/timer/rx-callback family (families §1–§13, `src/kernel-core/exec_kbackend.h`). | STRAWMAN (the port); the L2 socket is a pipe, not a port driver |
| 2 | SCS (SB/PB/PDT, CDT/CDL, connections, credits, SYSAP directory) | Executive; SYSAPs LISTEN/CONNECT/ACCEPT; credit-based flow control; `SCS$DIRECTORY` lookup service; MTYPE dispatch to the CDT. | `scs_config.c` (SB/PB/PDT), `scs_cdt.c`, `scs_conn.c`, `scs_credit.c`, `scs_dir.c`/`scs_sdir.c`, `scs_poll.c`, `scs_env.c`. Per `docs/design-scs-followup.md:26-38` the CDL delivery path and live credit accounting are **dead code**; data goes around them. All userspace. | STRAWMAN |
| 3 | Connection Manager (CNXMAN): CSB/CLUB, identity, join (CCSTART/MODEL/PARAMS/CONFIG), coordinator relay, the 12-step transition barrier, transition classes ADD/REMOVE/DEPART, RECNXINTERVAL, last-gasp | Executive; SHOW CLUSTER and `$GETSYI` read the CSBs. | The state machine lives in `scsd.c` (`cm_response_shape` allowlist at origin/main `scsd.c:1549`; barrier `cm_send_barrier_step` `:6356`; `scs_member.c` builders; `scs_recnx.c` CSB states). The executive holds only a **populated-by-ioctl** name/state table `vms_cluster_members[96]` (`vms_lock.c:158-160`, `VMS_IOCTL_CLUSTER_MEMBER_SET/CLEAR/GET` `vms_ioctl.h:741-775`) that SHOW CLUSTER reads (`docs/design-cluster-membership-executive.md`). The executive never learns its own CSID: `vms_local_csid` is an insmod param defaulting to 1 (`vms_module.c:60-63`), loaded with an empty param string (`src/ovmx_init/ovmx_boot_linux.c:118-141`). | STRAWMAN (CM); the executive table is a mirror, not a CM |
| 4 | Quorum / votes | Executive: VOTES, EXPECTED_VOTES, QDSKVOTES, CEVOTES/QUORUM arithmetic; on quorum loss the node **suspends** process activity until quorum returns; quorum disk. | `scs_quorum.c` — the documented arithmetic (`:79-118`), log-only, never gates anything (`docs/cluster-configuration-guide.md:142-160`). OVMX always advertises VOTES=0. No executive state. | STRAWMAN (math), ABSENT (enforcement, quorum disk) |
| 5 | DLM — local lock manager | Executive `$ENQ/$DEQ/$GETLKI`, modes, conversion, LVB, blocking ASTs, deadlock search. | `src/kernel-core/vms_lock.c` — genuine: 6-mode matrix, RB-tree lock ids, granted/waiting queues, LVB, BLKAST via `vms_ast.c`, local deadlock BFS. RMS and the ODS-2 ACP take real locks through it. | **REAL** |
| 6 | DLM — distributed arm (requester / directory node / master node; remaster; rebuild on transition) | Executive: three-case lookup (local master / local directory / remote directory), request routed to the master, directory entries weighted by LOCKDIRWT, rebuild (directory / merge / partial / full) coupled to the CNXMAN transition, remaster on departure. | Master-side receive path is real and complete: `vms_lock_dlm_xnode_dispatch` (`vms_lock.c:2529-2646`) handles ENQ (block-then-queue), GRANT receive into requester origin records (`:2236-2306`), DEQ with deferred grant (`:2104-2208`), BLKAST (`:2328-2382`), REBUILD/remaster (`:2442-2481`), DLKSRCH victim (`:1941-2005`), LVB crossing. **Requester send side is stubbed**: `dlm_resolve_master` returns `SS$_UNSUPPORTED` for any remote directory/master (`:548-564`). Directory = `exec_jhash(name) % n` over a static insmod vector (`:527-533`, `dlm_member_csids` `vms_module.c:82-86`) — an OVMX hash, explicitly not VMS's; using it against the real cluster broke the cluster (memory `cluster-promotion-gap` pm(2)). Engine is **passive**: no thread, no timer, never sends; only ever answers an ioctl. The cluster CM's cat-02 traffic is deliberately silenced in userspace (`scsd.c:1578-1622`). NetBSD never dispatches `VMS_IOCTL_DLM_XNODE` (`vms_netbsd.c:1243-1285`). | REAL/PASSIVE (engine); STRAWMAN (wire coupling); ABSENT (requester send, directory role, rebuild FSM) |
| 7 | MSCP server (`MSCP$DISK`) | Executive MSCP server serves local units to the cluster; served units named `$ALLOCLASS$DUAn`. | `scs_mscp_srv.c` — a userspace responder over a **file descriptor** (`scs_mscp_srv.c:93-146`), `pread/pwrite` on an image. Real MSCP semantics (AA-L619A-TK), but not the executive's volume. ONLINE-END is book-only. | STRAWMAN (placement); protocol knowledge REAL |
| 8 | MSCP client (disk class driver `DUDRIVER` role, `VMS$DISK_CL_DRVR`) | Executive class driver; served disks are real devices, mountable, cluster-wide file access via F11B$ locks. | `scs_mscp.c` — SCC/GUS walk only (join-time discovery). No device, no mount, no I/O. | ABSENT (device); STRAWMAN (discovery) |
| 9 | Node identity / cluster config (SCSNODE, SCSSYSTEMID, ALLOCLASS, VOTES, EXPECTED_VOTES, VAXCLUSTER, LOCKDIRWT, QDSKVOTES, RECNXINTERVAL, TIMVCFAIL, CLUSTER_CREDITS, NISCS_*), CLUSTER_AUTHORIZE group/password | SYSBOOT loads params into the executive; SYSINIT forms/joins before STARTUP.COM. | Typed SYSGEN store exists (`src/libvms/include/sysgen_params.h`; SCSNODE/SCSSYSTEMID/ALLOCLASS/VOTES/EXPECTED_VOTES/VAXCLUSTER) and `cluster_authorize.h` (group + plaintext password, no hash). Consumed only by the daemon (`scsd.c:229-352`). The executive receives none of it. Cluster start = `SCS_STARTUP.COM` running `SCSD.EXE` detached (`distro/.../SYS$STARTUP/SCS_STARTUP.COM`). The HELLO credential is a **replayed capture constant** `SCS_HELLO_LAB_NONCE_BYTES` (`scsd.c:15993`). | STRAWMAN (consumption); the credential is a fabrication-class crutch (§5.3) |
| 10 | Cluster-wide services ($SETCLUEVT/$GETSYI CLUSTER_*, cluster-wide logical names, SYSMAN, cluster time) | Executive + CLUSTER_SERVER. | `$GETSYI` CLUSTER_MEMBER/NODES read the daemon's published file; SHOW CLUSTER reads the executive mirror table. Nothing else. | ABSENT |
| 11 | Wire-protocol spec (frame shapes, choreography, the allowlist of grounded (SYSAP,cat,op) pairs) | — | `docs/cluster-protocol-spec.md` (7.9K lines, byte-verified), `docs/design-cluster-join-choreography.md`, `docs/design-mscp-direction.md`, builders in `scs_member.c`/`scs_dlm.c`/`scs_hello.c`/`scs_connect.c`/`scs_dir.c` with 87 host unit tests. | **REAL knowledge** — harvest as spec + codec |

Two facts the audit surfaces that the plan depends on:

- The executive today has **no active execution context**: no substrate
  kthread, timer or unsolicited-receive hook (`exec_kbackend.h` §1–§13). A
  port driver that must send HELLOs every ~2 s, retransmit, time out VCs and
  run a barrier cannot be built without adding those primitives (§3.3).
- The unmerged branch `feat/coord-rebuild-completion` (28 commits) contains
  two pieces that ARE executive and portable: the ODS-2 ACP taking a standing
  per-volume `F11B$v<label>` lock for the life of a mount (`vmsfs_acp.c`
  delta) and `vms_lock_dlm_xnode_enq_idempotent` (retransmit idempotency keyed
  by `(req_csid, req_lkid)`). Both are salvage; everything else on that branch
  is daemon patching and is discarded.

---

## 3. Target architecture

### 3.1 Layering

```
 personality (DCL SHOW CLUSTER, $GETSYI, $SETCLUEVT, MOUNT, RMS)      user mode
 ───────────────────────────── executive boundary ─────────────────────────────
 vms_cluster_api.c   readback + control surface (CSB view, CLUB, PEA0 diag)
 vms_cnxman.c        Connection Manager: CSB/CLUB, join, coordinator, barrier,
                     transitions, quorum, RECNXINTERVAL, cluster events
 vms_dlm_scs.c       DLM SYSAP: VMS wire  <->  vms_lock.c (roles, rebuild FSM)
 vms_mscp_srv.c      MSCP server SYSAP  (serves executive volumes)
 vms_mscp_cl.c       disk class driver  (served units as devices in vms_devtab)
 vms_scs.c           SCS: SB/PB/PDT, CDT/CDL, connect FSM, credits, SCS$DIRECTORY
 vms_pe.c            LAN port (PEDRIVER role): HELLO/SOLICIT, channels, VCs,
                     sequencing/ack/retransmit, datagram/message/block transfer
 vms_cluster_codec.c pure frame build/parse (no state, host-unit-testable)
 ───────────────────────────── substrate seam ─────────────────────────────────
 exec_lan_*  exec_kthread_*  exec_timer_*  exec_time_*   (new, exec_kbackend.h)
 Linux: packet_type(ETH_P_SCA) + dev_queue_xmit   NetBSD: if_input shim + if_transmit
```

Everything above the seam is `src/kernel-core/` and compiles unchanged into
both kmods (the existing single-source discipline: `vms_lock.c` already
builds for both). `vms_lock.c` remains the lock engine; it grows the three
distributed roles and the rebuild FSM but not the wire.

### 3.2 Module / TU map

New TUs in `src/kernel-core/` (each also a new entry in `src/kernel/Makefile`
`vms-y`, the distro Kbuild, and the NetBSD `SRCS` — the "TU in N places"
trap, memory `vms-ko-two-object-lists`, `netbsd-module-srcs-four-places`):

| TU | Owns | VMS objects it models (public names) |
|---|---|---|
| `vms_cluster_codec.c` (+ `.h`) | Pure build/parse of every grounded frame class: HELLO/SOLICIT header, START/STACK/ACK, credit return, connect verbs op 0–10, the 190-byte CM body, cat-02 DLM bodies, MSCP command/end/block-transfer headers. **No state, no allocation, no substrate calls** — compiles on the host for unit tests against the capture manifest. Carries the grounded (SYSAP, category, opcode) **allowlist** as data. | — |
| `vms_pe.c` | The port: `PEA0:` device in `vms_devtab` bound to ETH0:'s netif; multicast group join; HELLO cadence; channel table with the b2/b3/b4 verify handshake and packet-size verify; virtual circuit per remote system (START/STACK/ACK, incarnation echo, `send_seq`/`recv_ack`, retransmit-with-same-seq, TIMVCFAIL); the three port services (datagram, sequenced message, block transfer with named buffers). Delivers received messages upward by (VC, Con.ID pair). | PDT, PB (path block), VC, "channel" |
| `vms_scs.c` | SB (one per remote system), CDT/CDL keyed by local Con.ID (monotonic allocator, high word reseeded per boot — spec §4(t)), connection FSM (CONNECT-REQ/ECHO/RESP/CONFIRM, alt ACCEPT/CONFIRM, DISCONNECT-REQ/RSP, the 8/9 credit pair), credit accounting (Send/Receive/Pending per CDT, the special credit message), SYSAP registry with LISTEN/CONNECT/ACCEPT/SEND/RETURN-CREDIT; the `SCS$DIRECTORY` SYSAP (server and client lookup); MTYPE dispatch to the CDT's message-input routine. | SB, CDT, CDL, SYSAP, SCS$DIRECTORY |
| `vms_cnxman.c` | CLUB (cluster block: local CSID, epoch, member bitmap, votes/quorum, transition state, coordinator); CSB per remote CM (ten connectivity states NEW…LOCAL, flags member/selected/status_rcvd); the `VMS$VAXcluster` SYSAP; join drive (own directory connect at ss=1 → lookups → MSCP$DISK client connect → VC connect → MODEL/PARAMS/CONFIG burst, spec §4(L)/(m)/(o)); server half (accept members' inbound connects — Rule of Total Connectivity §4(y)); op-06 membership burst receipt; the 12-step barrier as participant; op-0d rebuild-record echo (recipe §4(p), 1367/1367); relay + coordinator role (op-12, op-09/0a/0b/0c driving `12×(M−1)`) when OVMX is coordinator; transition classes ADD/REMOVE/DEPART; RECNXINTERVAL/TIMVCFAIL reconnect loop; last-gasp; cluster events to `$SETCLUEVT` waiters via `vms_ast.c`; OPCOM-style `%CNXMAN` console lines. | CLUB, CSB, CNXMAN |
| `vms_dlm_scs.c` | The lock manager's SYSAP arm: marshals `vms_lock.c` role operations to/from cat-02 frames on the `VMS$VAXcluster` connection; owns the rebuild FSM's wire side; delivers inbound requests to `vms_lock_dlm_xnode_dispatch` **as a direct call** (no ioctl). | — |
| `vms_lock.c` (extended) | Three roles: requester (proxy LKB for a remote-mastered resource, replacing the ad-hoc `vms_dlm_origin` list), master (already: LKBs with `req_csid/req_lkid`), directory node (a stored directory table: resource → master CSID, populated from the wire); `dlm_resolve_master` completes its cases 2/3 by calling `vms_dlm_scs` and sleeping on the LKB's cv (the `enq_wait_sync` pattern); rebuild FSM (freeze → per-type rebuild → thaw) driven by CNXMAN transition callbacks; remaster on departure (exists: `DLM_MEMBER_DEPART` path). | RSB, LKB, directory |
| `vms_mscp_srv.c` | MSCP server SYSAP: serves executive-owned volumes (through the ACP's block layer, `exec_blockdev_*`) as `$ALLOCLASS$DUAn`; SCC/GUS/ONLINE/READ/WRITE + block transfer; UQB/HQB/HRB analogues; `MSCP_SERVE_ALL`/`MSCP_LOAD`. | DSRV, UQB, HQB, HRB |
| `vms_mscp_cl.c` | Disk class driver: discovers served units (SCC ×2, GUS NEXT-UNIT walk), creates `$n$DUAn` devices in `vms_devtab` backed by an MSCP connection; block read/write via named buffers; the ACP mounts them like any block device. | DUDRIVER role, CDDB |
| `vms_cluster_api.c` | The personality surface (§3.5): SYSGEN param load, cluster start, CSB/CLUB readback, diagnostics. | — |
| `vms_cluster_fork.c` | The single fork context (§3.3): input queue, work queue, the kthread loop, timer wrappers (`cf_timer_*`) — the only TU besides the bindings that touches §15/§16. | the fork-IPL serialization |

Each protocol layer above is split `vms_<layer>_fsm.c` (pure state
machine, injected ops) + `vms_<layer>.c` (executive glue) +
`vms_<layer>_snapshot.h` (read-only view) per §3.9, so the table names the
layer, not one file.

#### 3.2.1 The substrate contract — both bindings designed together

NetBSD-VAX is a co-equal, release-gated target (platform direction: VAX
mainstream co-release). The cluster stack is therefore specified as a
**contract** in `exec_kbackend.h` with both bindings written in the same
phase; a phase is not done until the NetBSD-VAX rail passes its proof. The
existing single-source discipline (`vms_lock.c` builds into both kmods)
extends to every new TU: `src/kernel/Makefile` `vms-y`, the distro Kbuild,
and the NetBSD `SRCS`/enum quartet are updated together (memory
`netbsd-module-srcs-four-places`, `vms-ko-two-object-lists`).

| Contract (kernel-core sees only this) | Linux binding (`exec_kbackend_linux.h`) | NetBSD binding (`exec_kbackend_netbsd.h`) | Notes / to verify on the rail spike |
|---|---|---|---|
| `exec_lan_open(ifname, ethertype, rx_cb, ctx)` — register for unsolicited frames of one ethertype on the netif `vms_devtab` resolved for ETH0: | `dev_add_pack(&pt)` with `.type = htons(ETH_P_SCA)` (0x6007, `<linux/if_ether.h>`), `.dev = ndev`; `rx_cb` runs in softirq | Two documented paths, chosen by spike: (a) link-layer `pfil(9)`: `pfil_add_hook(fn, ctx, PFIL_IN, ifp->if_pfil)` — `ether_input()` runs `pfil_run_hooks(ifp->if_pfil, …, PFIL_IN)` before ethertype dispatch on NetBSD ≥ 8; (b) interpose `ifp->if_input` (a public `struct ifnet` member; the interception `agr(4)`/`bridge(4)` use): the shim peeks `ether_type == 0x6007`, consumes those, chains the rest to the saved `ether_input`. `rx_cb` runs at softint (`IPL_SOFTNET`) via `if_percpuq`, or in the driver interrupt on the VAX `qe`/`xq` drivers — see contract rule 1 | Rule 1: `rx_cb` may only copy into a pool buffer, enqueue under a spin lock initialized at the rx IPL (`mutex_init(…, IPL_NET)` / spinlock), and wake the fork thread. Never allocate, never sleep. Spike checklist: which of (a)/(b) exists on the rail's NetBSD version; `qe`/`xq` input IPL |
| `exec_lan_close()` | `dev_remove_pack` | `pfil_remove_hook` / restore `if_input` | — |
| `exec_lan_xmit(frame, len)` — a complete Ethernet frame, source MAC already set | `alloc_skb` + `skb_put` + `skb->dev` + `dev_queue_xmit` | `m_gethdr`/`m_copyback` + `(*ifp->if_transmit)(ifp, m)` — bypasses `ether_output`'s ARP/sockaddr path (what `bridge(4)` does); set `m->m_pkthdr.rcvif = NULL`, `M_BCAST/M_MCAST` flags from the dst MAC | Both callable from the fork thread (process context). Frame buffers are pool-allocated, never on the stack (VAX kernel stack is small) |
| `exec_lan_mc_add/del(mac)` — join the cluster HELLO multicast `AB-00-04-01-<group>` | `dev_mc_add(ndev, mac)` | `sockaddr_dl` + `if_mcast_op(ifp, SIOCADDMULTI, &sdl)` (= `ether_addmulti` through `if_ioctl`) | — |
| `exec_lan_hwaddr(out[6])`, `exec_lan_mtu()`, `exec_lan_link_up()` | `ndev->dev_addr`, `ndev->mtu`, `netif_carrier_ok` | `CLLADDR(ifp->if_sadl)`, `ifp->if_mtu`, `ifp->if_link_state` | `NISCS_MAX_PKTSZ` clamps to MTU |
| `exec_kthread_create(fn, arg, name)` / `_stop` / `_should_stop` | `kthread_run` / `kthread_stop` / `kthread_should_stop` | `kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, fn, arg, &l, name)` / a stop flag + `cv_signal` + `kthread_join` / flag test | One thread per node: the cluster fork context |
| `exec_timer_init(t, cb, ctx)` / `_arm(t, ms)` / `_cancel(t)` — `cb` runs in a no-sleep context | `timer_setup` / `mod_timer` / `del_timer_sync` | `callout_init(c, CALLOUT_MPSAFE)` / `callout_schedule` / `callout_halt` (softint) | Rule 2: `cb` only posts a work item to the fork queue and wakes the thread; all logic runs in the thread |
| `exec_workq_post(item)` / `exec_workq_wait()` — the fork queue | list + spinlock + `wait_event_interruptible` | list + `mutex(IPL_NET)` + `cv_wait` | Already-present `exec_lock_t`/`exec_cv` families are reused; this is a thin composition |
| `exec_time_now_vms()` — VMS absolute time (100 ns ticks since 17-NOV-1858) | `ktime_get_real_ns()` + epoch offset | `getnanotime()` + epoch offset | Incarnation quadword, boot time, CLUSTER_FTIME |
| `exec_console_printf(fmt, …)` — `%CNXMAN`/`%VAXcluster` lines on OPA0: | `printk(KERN_ERR …)` routed to the console (KERN_INFO is suppressed at the QEMU loglevel — memory `forking-daemon-over-bgn-ladder`) | `printf(9)` | OPCOM-class events; also delivered to `$SETCLUEVT` waiters |

Dispatch parity is part of the contract: every cluster ioctl added to
`src/kernel/vms_ioctl.h` is mirrored in `src/kernel-netbsd/vms_lock_nb.h`
(struct, `_IOWR` encoding, `_Static_assert` on size) and dispatched in
**both** `vms_module.c` and `vms_netbsd.c`. The existing omission —
`vms_netbsd.c:1243-1285` dispatches `DLM_MEMBER_DEPART` but never
`VMS_IOCTL_DLM_XNODE` — is fixed in P0 (it becomes a direct call from
`vms_dlm_scs.c` anyway, but the `OVMX_KTEST` harness entry must exist on both).
A CI leg runs the NetBSD-VAX cross-build of every new TU (memory
`asymmetric-arch-red-is-real`: x86_64-only green hides VAX reds).

What the NetBSD-VAX kernel genuinely cannot do, and the alternative:

- No `tracepoint`-style hooks and a **small kernel stack** on VAX: no frame
  buffers or CSB/CDT snapshots on the stack — pool-allocated, passed by
  pointer (a coding rule, §3.9).
- ILP32: every wire/lock structure uses fixed-width `uint32_t/uint16_t`
  (already the `vms_lock.c` style — memory `ilp32-width-proof`); no `long`.
- `qe`/`xq` (DEQNA/DELQA under SIMH) deliver frames at interrupt IPL on some
  paths: contract rule 1 keeps `rx_cb` IPL-safe on both substrates by
  construction.
- No `dev_add_pack` equivalent: the `pfil`/`if_input` interposition above is
  the NetBSD-native mechanism (used in-tree by `bridge`/`agr`), not a Linux
  assumption transplanted.

#### 3.2.2 The seam as a deliverable — write the stack once

The factoring that keeps Linux and NetBSD from diverging is in scope and is
the first artifact P0 produces: the complete substrate-primitive inventory
the cluster stack is allowed to touch, and a rule that kernel-core cluster
code includes **only** `exec_kbackend.h` and kernel-core headers.

**Inventory.** `exec_kbackend.h` today has 13 families (§1 locking,
§2 cv-shaped wait/wake, §3 copyin/out, §4 memory (`exec_zalloc`,
`exec_zalloc_atomic`, `exec_free`, `exec_free_deferred`), §5 host-task
binding, §6 RCU-lite, §7 sleepable mutex, §8 block device, §9 barriers,
§10 arena, §11 primary netdev, §12 host TCP socket, §13 AF_PACKET socket).
The cluster stack reuses §1, §2, §4, §6, §7, §8 (MSCP server), §9, §11 and
adds five families — the complete list, nothing else:

| Family | Primitives | Used by |
|---|---|---|
| §14 LAN port | `exec_lan_open/close`, `exec_lan_xmit`, `exec_lan_mc_add/del`, `exec_lan_hwaddr/mtu/link_up`; opaque `exec_lanbuf_t` {`data`, `len`} owned by the core after `rx_cb` copies into it | `vms_pe.c` only |
| §15 fork context | `exec_kthread_create/stop/should_stop` | `vms_cluster_fork.c` only |
| §16 timers | `exec_timer_t`, `exec_timer_init/arm/cancel` (callback = no-sleep, posts work) | `vms_cluster_fork.c` only (all layers arm timers through the fork module's `cf_timer_*` wrappers, so timer idioms never spread) |
| §17 time | `exec_time_now_vms()`, `exec_ticks_ms()` (monotonic) | codec (incarnation/boot time), FSMs (deadlines, injected in tests) |
| §18 console | `exec_console_printf` | `vms_cnxman.c` (OPCOM-class lines) |

The existing `exec_list`/`exec_hash`/`exec_rbtree` headers stay the only
container idioms; no `list_head`, no `TAILQ`.

**Where substrate specifics would leak, and the seam that stops them.**

| Leak | Linux idiom | NetBSD idiom | Seam rule |
|---|---|---|---|
| Frame buffers | `sk_buff` | `mbuf` chain | The core never sees either. `rx_cb` receives `(const uint8_t *frame, size_t len)` and copies into an `exec_lanbuf_t` from the core's own pool; `exec_lan_xmit` takes `(const uint8_t *, size_t)` and the binding builds the skb/mbuf and frees it. Copy cost is irrelevant at cluster rates (≤ low thousands/s) and buys a substrate-free core |
| Receive context | softirq | softint / driver IPL | Contract rule 1 (copy, enqueue under an rx-IPL lock, wake); the fork thread is the only context that runs protocol code, on both substrates |
| Timers | `timer_list` in softirq | `callout` in softint | Contract rule 2 (post-and-wake only); the core's FSMs never run in a timer callback |
| IPL / lock classes | spinlock + irqsave | spin `mutex(9)` at IPL | `exec_rxlock_t` (§3.2.3) for the ONE rx-touching object, the fork queue; `exec_mutex_t` for the fork mutex; `exec_lock_t` never at receive level; the core names no IPL (`EXEC_LAN_RX_IPL` is a binding constant) |
| Netif identity | `struct net_device *` | `struct ifnet *` | The binding resolves ETH0: → its own handle inside `exec_lan_open`; the core holds only the device name string from `vms_devtab` |
| Multicast join | `dev_mc_add` | `if_mcast_op` | `exec_lan_mc_add(mac[6])` |
| Thread lifecycle | `kthread_should_stop` | stop flag + `kthread_join` | `exec_kthread_should_stop()`; the fork loop checks it once per iteration |
| Time | `ktime` | `bintime`/`timespec` | Two integer-returning calls (§17) |
| Word width / alignment | LP64 | ILP32 VAX, strict alignment | Wire and lock structures are `uint8_t[]` + accessor functions in the codec (`get_le16/32`, `put_le16/32` — never a cast through a struct pointer); `_Static_assert` on every ABI struct in both bindings |
| Ioctl encoding | `_IOWR` on x86 | `_IOWR` on VAX | Identical numbers asserted in both headers (existing discipline) |

**The split, with a size sense** (estimates from the daemon's line counts
for the same functions — `scsd.c` alone is 16K lines of orchestration that
becomes state tables):

| Layer | kernel-core (shared) | Linux binding | NetBSD binding |
|---|---|---|---|
| Codec (all frame classes, allowlist tables) | ~2 500 | 0 | 0 |
| `vms_pe.c` (channels, VCs, services) | ~1 500 | 0 | 0 |
| `vms_scs.c` | ~1 500 | 0 | 0 |
| `vms_cnxman.c` (+ `_join_fsm.c`, `_barrier_fsm.c`, `_quorum.c`) | ~2 500 | 0 | 0 |
| `vms_dlm_scs.c` + `vms_lock.c` delta (roles, rebuild FSM) | ~2 700 | 0 | 0 |
| `vms_mscp_srv.c` / `vms_mscp_cl.c` | ~2 000 | 0 | 0 |
| `vms_cluster_fork.c` (queue, thread loop, timer wrappers) | ~300 | 0 | 0 |
| `vms_cluster_api.c` (ioctl bodies) | ~400 | dispatch cases ~60 | dispatch cases ~60 |
| Substrate families §14–§18 | contract header ~120 | ~250 (`packet_type`, skb build, `kthread`, `timer_list`, time, console) | ~300 (`pfil`/`if_input` shim, mbuf build, `kthread`, `callout`, time, console) |
| **Total** | **~13 500 written once** | **~310** | **~360** |

Roughly 40:1 shared to per-substrate. The bindings are deliberately dull:
each function is a one-screen adapter with no protocol knowledge, so a
reviewer can verify parity by reading both side by side.

**Adding a third substrate** (the test that the seam is real): implement
§14–§18 (~300 lines), add the ioctl dispatch cases, add the TUs to that
kernel's build list, run the host test suite (which exercises zero substrate
code) and then the substrate contract test (§3.9: `exec_lan` loopback,
timer/kthread post-and-wake). No cluster-logic file changes. P0's proof
includes a CI grep gate: no `#include <linux/…>`, `<sys/mbuf.h>`,
`<net/if.h>` etc. in `src/kernel-core/vms_pe.c`, `vms_scs.c`,
`vms_cnxman*.c`, `vms_dlm_scs.c`, `vms_mscp_*.c`, `vms_cluster_*.c`.

#### 3.2.3 Receive-level synchronization — RULING (2026-09-02, from FC-P0.5)

**The gap.** The seam's only lock, `exec_lock_t`, is a plain `spin_lock`
on Linux (`exec_kbackend_linux.h:87`) and an `IPL_NONE` kmutex on NetBSD
(`exec_kbackend_netbsd.h:102`). The cluster fork queue is written from the
substrate's receive context (Linux softirq; NetBSD softint or the `qe`/`xq`
device IPL) and read from the fork thread. A process-context holder of a
plain spinlock preempted by a same-CPU softirq taking the same lock is a
hard lockup; an `IPL_NONE` kmutex taken above `IPL_NONE` is a NetBSD panic.
So the fork queue cannot be guarded by `exec_lock_t`, and §3.3's "rx-IPL
`exec_lock_t`" was under-specified.

**Ruling: option (2) — the seam gains ONE receive-level lock class, and
the core uses it for exactly ONE object, the fork queue.** Option (1)
(the LAN binding owns the queue lock) is rejected: it moves the queue's
semantics — ordering, drop policy, high-water, the wake — into
per-substrate code where they cannot be reviewed once and the snapshot
readers cannot see them; and it still needs an IPL-safe lock, just an
unnamed one. This is also the VMS shape: a device ISR runs at device IPL,
does minimal work, and hands a fork block to the fork queue with an
interlocked queue instruction; the fork routine runs at fork IPL
(`IPL$_SCS`, 8, for SCS-class port drivers) and does all protocol work
there. The cross-IPL handoff is a single interlocked queue — one shared
object, one synchronization class — which is what this ruling reproduces.

**Seam additions (§1b, "receive-level lock"), exact symbols:**

```
typedef <substrate> exec_rxlock_t;      /* Linux: spinlock_t; NetBSD: kmutex_t SPIN at EXEC_LAN_RX_IPL */
typedef <substrate> exec_rxflags_t;     /* Linux: unsigned long (irqsave); NetBSD: int (unused; IPL kept in the mutex) */
#define EXEC_LAN_RX_IPL  <binding>      /* NetBSD: the highest IPL at which exec_lan rx callbacks may run
                                            (IPL_NET unless the P0.3 spike shows qe/xq input above it);
                                            Linux: no-op constant */
void exec_rxlock_init(exec_rxlock_t *);      /* Linux spin_lock_init; NetBSD mutex_init(l, MUTEX_DEFAULT, EXEC_LAN_RX_IPL) */
void exec_rxlock_destroy(exec_rxlock_t *);
void exec_rxlock_acquire(exec_rxlock_t *, exec_rxflags_t *);  /* ANY context. Linux spin_lock_irqsave; NetBSD mutex_spin_enter */
void exec_rxlock_release(exec_rxlock_t *, exec_rxflags_t *);  /* Linux spin_unlock_irqrestore; NetBSD mutex_spin_exit */
int  exec_cv_wait_rx(exec_cv_t *, exec_rxlock_t *, exec_rxflags_t *);
      /* thread context only; the §2 cv contract with the rxlock as interlock: enqueue on cv while
         holding the rxlock, atomically release + sleep, re-acquire before return; returns 0 (no signal
         semantics needed — the fork thread is a kthread; stop is a predicate).
         Linux: prepare_to_wait(TASK_UNINTERRUPTIBLE) → spin_unlock_irqrestore → schedule() → finish_wait
                → spin_lock_irqsave.  NetBSD: cv_wait(cv, l) with the spin mutex as interlock. */
/* exec_cv_signal / exec_cv_broadcast: unchanged; legal from receive level when the caller holds the
   rxlock the cv is paired with. */
```

**Contract rule 14.1 (the doc-comment to paste over §14/§15/§16):**

> RECEIVE-LEVEL SYNCHRONIZATION. Exactly one executive object is shared
> between the substrate's receive context and thread context: the cluster
> fork queue (`vms_cluster_fork.c`: input-frame list, posted-work list,
> stop flag, and the pool freelist that rx pops buffers from). It is
> guarded by ONE `exec_rxlock_t` and ONE `exec_cv_t` paired with it.
> (a) Receive level — the §14 rx callback and §16 timer callbacks — may
> take ONLY the rxlock: pop a pool buffer / copy / enqueue / `exec_cv_signal`
> / release. O(1), no allocation, no sleep, no `exec_lock_t`, no
> `exec_mutex_t`, no call into protocol code. Empty pool ⇒ drop + counter.
> (b) Thread context takes the rxlock only to splice the whole queue into a
> private list, to post work, or to sleep via `exec_cv_wait_rx`; it never
> runs protocol code under it. All protocol state (PE/SCS/CNXMAN/DLM) is
> guarded by the fork mutex (`exec_mutex_t`), held by the fork thread
> across a dispatch batch. Lock order: fork mutex → rxlock; rxlock → any
> other lock is forbidden. (c) `exec_lock_t`/`exec_mutex_t` are never
> taken at receive level. (d) The fork thread's sleep predicate
> ("input empty ∧ work empty ∧ ¬stop") is tested under the rxlock and
> every mutation of it happens under the rxlock — the §2 lost-wakeup-free
> contract with the rxlock as the shared interlock.

**Who takes what:**

| Context | May take | Never |
|---|---|---|
| rx callback (Linux softirq / NetBSD softint or `EXEC_LAN_RX_IPL`) | rxlock (acquire/release around O(1) enqueue + signal) | `exec_lock_t`, `exec_mutex_t`, allocation, protocol code |
| timer callback (§16) | rxlock (post a work item + signal) | same |
| fork thread | fork mutex across a batch; rxlock briefly to splice/post/wait | protocol code under the rxlock |
| process-context posters (a `$ENQ` needing the wire, MOUNT, ioctl control) | rxlock briefly (post + signal), then sleep on their own object's cv/`exec_lock_t` | the fork mutex while holding the rxlock |
| snapshot readers (diagnostic ioctls) | fork mutex for protocol state; queue-depth counters are plain integers read under the rxlock | — |

**Conformance follow-up** (one plan item, FC-P0.16): P0.2 and P0.4 add
§1b; P0.5's queue/wake path moves from `exec_lock_t` to `exec_rxlock_t` +
`exec_cv_wait_rx` with the pool freelist under the same lock; the R3
contract test gains a same-CPU hammer (a process-context poster loop
pinned to the CPU that receives, under a 0x6007 flood) that must not
lock up or panic on either substrate.

#### 3.2.4 Seam buffer granularity — RULING (2026-09-02, E1 from FC-P3.5)

**The question.** `cnxman_ops.send/respond` (frozen in FC-P0.1) takes a
`body`; FC-P3.1's codec and FC-P3.5's barrier FSM emit complete 204-byte
frames through a stand-in (`vms_cm_link`) and a `next_out` hook that lets
the FSM fill the envelope. Frame-level or body-level?

**Ruling: body-level. Each layer owns exactly its own header; a SYSAP
never sees the port or SCS headers.** This is the VMS layering (Davis
ch. 2/ch. 4, public): the PPD/port driver (PEDRIVER) owns the datalink and
virtual-circuit header — sequencing, acks, retransmission; SCS owns the
SCS header — message type, credit, connection ids; a SYSAP (CNXMAN, the
lock manager's arm, the MSCP server) hands SCS an application message and
receives one. A SYSAP that fills `send_seq` is the same category error as
a daemon that fills a lock id. It is also the fast-test shape: the barrier
FSM's rung-1 truth is "this 132-byte body on this connection", and the VC
and SCS FSMs are each tested on their own header without a CM body.

**Ownership of every byte of the 204-byte `VMS$VAXcluster` class**
(frame-absolute offsets; spec §2, §4(d), §4(g), §4(h)(1a), §4(j)):

| Bytes | Field | Owner | Written by |
|---|---|---|---|
| 0–13 | Ethernet dst/src MAC | port | `vms_pe` (dst from the PB's channel; src = HW MAC) |
| 14–31 | SCA header: src logical `aa:00:04:00:<sysid>` @24, msgtype @30 | port | `vms_pe`; msgtype from the **service kind** SCS requests (`0x5b` connect-phase control, `0x4b` data, datagram, block) — the §4(m) phase rule is SCS's knowledge, passed down as a parameter, never a byte the SYSAP sets |
| 32–35 | `recv_ack`, `send_seq` | port (VC) | `vms_pe` at transmit time; retransmit resends the same buffer with the same seq |
| 36–55 | incarnation mirror @36, observed mirrors @40/44/48 | port (VC) | `vms_pe` (FC-P1.1's builder; until it lands these are explicit zeros — never a template) |
| 56–71 | SCS header: inner length, format `0x0004`, MTYPE, credit, dst/src Con.ID | SCS | `vms_scs` from the CDT (MTYPE 10 for application messages; 0–9 for its own control verbs; credit from the ledger) |
| 72–75 | SYSAP dialogue counters `send_msg`/`ack_msg` (body[0:4]) | CNXMAN (per-CSB dialogue state) | one stamper, `cnxman_envelope_stamp()` |
| 76–79 | `txn`, `token` (body[4:8]) | CNXMAN (originations: own txn; responses: echoed; token never computed) | the same stamper |
| 80–203 | category, opcode, payload (body[8:132]) | the SYSAP logic: CNXMAN FSMs for cat-01/04/06, the DLM arm for cat-02 (the echo recipe included) | the FSM/role that emits it |

**Seam contract, layer by layer (the consequent for P0.9 / P1.3 / P2.2 /
P3.8; the frozen signatures stand — their doc-comments are made exact):**

- **CNXMAN ↔ SCS** (`cnxman_ops.send(dst_csid, body, len)`,
  `respond(body, len)`): `body` is the 132-byte SYSAP body, body[0:8]
  already stamped by `cnxman_envelope_stamp(csb, body, is_response)` — a
  pure function on the CSB's dialogue counters, called by every CNXMAN
  emitter (join, barrier, coordinator, the DLM arm's cat-02 messages ride
  the same stamper; the DLM arm never writes body[0:8]). Glue (`vms_cnxman.c`):
  `send` → the CSB's `VMS$VAXcluster` CDT → `scs_send_msg`; `respond` →
  `scs_send_msg` on the CDT the request being dispatched arrived on (the
  glue records the current-request CDT across a dispatch). `respond` stays
  distinct from `send` because the correlation the peer requires is
  (connection, txn) — and only the glue knows the connection.
- **SCS ↔ port** (`scs_ops.send(dst_sysid, dst_conid, body, len)` /
  `scs_send_msg(local_conid, body, len)`): SCS copies the body into a pool
  transmit buffer at offset 72, fills 56–71 from the CDT, debits a credit,
  and calls `pe_send_msg(pe, sysid, buf, svc)` with `svc ∈ {PE_SVC_CONTROL,
  PE_SVC_MESSAGE, PE_SVC_DATAGRAM, PE_SVC_BLOCK}`. On receive SCS parses
  56–71, credits the ledger, dispatches by dst Con.ID and MTYPE, and calls
  `scs_sysap_ops.message(ctx, local_conid, frame + 72, inner_len − 16)`.
- **port** (`pe_send_msg`, FC-P1.3): fills 0–55, assigns `send_seq`, keeps
  the buffer in the VC's unacked ring for retransmit (same bytes, same
  seq), transmits via `exec_lan_xmit`. On receive it does VC accounting on
  32–35 and delivers the whole frame plus a `vms_frame_info` (`scs_off = 56`,
  `body_off = 72`) to SCS — no copy, no strip.
- **Transmit buffer** = the pool `exec_lanbuf_t` (§3.2.2): one allocation
  per message, headers prepended in place by the two lower layers, the same
  buffer retransmitted. The 132-byte copy from the FSM's scratch into the
  pool buffer is deliberate: it keeps the FSMs pure (scratch is theirs) and
  costs nothing at cluster rates.

**Codec consequence.** The codec is organized by owner: `codec_hello`/
`codec_vc` build and parse 0–55 (port); `codec_scs` 56–71; `codec_cm`,
`codec_dlm`, `codec_mscp` build and parse **bodies** (frame + 72 onward,
`body[]`-relative accessors, which is how the spec and the field maps in
memory `cluster-promotion-gap` are already written). A full-frame composer
(`vms_frame_compose(link, scs, body)`) exists for **tests and the
simulator only** — it is how rung-1 byte-exactness against a 204-byte
specimen and the pcap replay are done — and is never called from a layer.

**FC-P3.5 conformance (one item, FC-P3.15):** the three frame-emitting
call sites emit 132-byte bodies via `ops->send/respond` after
`cnxman_envelope_stamp`; `cnxman_barrier_link_ops.next_out` is deleted
(its `env` half becomes the stamper; its `link` half was never the FSM's);
`vms_cm_link` is demoted to a test-only helper under `tests/cluster/host/`
until FC-P1.1/FC-P2.1's builders replace it, then deleted. The barrier's
rung-1 fixtures become `specimen[72:204]` slices — the existing byte-exact
assertions hold unchanged on the body.

#### 3.2.5 Virtual-circuit loss recovery — RULING (2026-09-02, E10 from FC-P1.4)

**The question.** FC-P1.2's VC receiver breaks the circuit on any sequence
gap (`h_vc_rx_seqmsg` step 3, citing Davis p. 2-31 and spec §4(h)(4a)).
The simulator shows that under 10 % per-link loss with pipelined sends this
tears the VC down on every lost frame; recovery works, but SCS above would
see every connection broken on a single packet loss. Faithful, or a bug?

**Ruling: a fidelity bug. Break-on-first-gap conflates the *guarantee*
with its *detection*.** What p. 2-31 governs is the contract and its
consequence: SCS guarantees delivery and per-VC ordering; *if the port
cannot satisfy the guarantee*, the VC is explicitly broken and every
connection on it with it. The mechanism by which a port satisfies the
guarantee under loss is retransmission — and the wire shows the real port
doing exactly that before it ever breaks anything: sequenced messages
carry an explicit **retransmit msgtype** (`0x7b` is the retransmit of
`0x5b`, spec §4(h)), retransmits **reuse the original `send_seq`** (§4(L),
"advancing it per retransmit is wrong"), 506 duplicate/retransmit
sequenced frames were counted across the corpus (§4(h)(4a)), and the
unacked padded-HELLO **retransmit ladder** (§4(k)) shows a real VAX
retrying an unacknowledged frame ~25 times over tens of seconds before
giving up. A receiver that breaks on the first out-of-order frame makes
the sender's ring, the retransmit marker and the ladder pointless. The
"0 gaps in 321,599 VAX-sourced messages" census was taken on a lossless
SIMH bridge; it does not discriminate between "VAX never tolerates a gap"
and "the LAN never lost a frame," and the ladder evidence decides it.

The port's *silence* detectors are separate and stay: `TIMVCFAIL` (the VC
failure timer) and the listen timeout (§4(M)) detect a peer that has gone
quiet; ch. 2 publishes the rule, not the timer, and the port driver owns
it.

**The faithful receiver/sender model (NISCA/PPD sequenced-message service):**

| Event | Receiver (`vms_pe_fsm.c` VC tables) | Sender |
|---|---|---|
| in-order frame (`seq == recv_seq+1`) | advance `recv_seq`, credit, ack, deliver | — |
| duplicate (`seq <= recv_seq`) | ack, never deliver twice (unchanged) | — |
| **gap** (`seq > recv_seq+1`) | **discard the frame, do not advance, count `rx_gaps`, and immediately re-send the cumulative ack of `recv_seq`** (the duplicate ack tells the sender where the hole is) — **no break** | — |
| ack timeout on the oldest unacked entry | — | **retransmit from the oldest unacked entry onward, in order, same bytes, same `send_seq`, retransmit msgtype** (go-back-N: the receiver discards everything after the hole, so the sender resends the tail) |
| ack received | release the ring through the acked seq (unchanged) | ladder resets for the released entries |
| retransmit ladder **exhausted** for an entry | — | **break the VC**, reason `PE_VC_DOWN_RETRANSMIT_EXHAUSTED` ("delivery guarantee cannot be satisfied", p. 2-31) |
| `TIMVCFAIL` / listen timeout with no traffic | break the VC, reason `PE_VC_DOWN_TIMVCFAIL` (unchanged) | — |

Receive window = 1 (go-back-N, no reorder buffer): the only scheme that
is *always* correct without grounding a window size the book does not
publish; a selective-repeat window would be an optimization with no
oracle. The ladder cadence and count are OVMX design choices labeled per
the spec's §5 discipline, seeded from the §4(k) measured ladder (the only
retransmit timing a real VAX has shown us); `PE_VC_DOWN_SEQ_GAP` is
deleted (a gap is a counter, never a reason).

**Consequent contract for FC-P2.2 (SCS) — a VC break is a real event and
SCS does *not* hide it.** Davis p. 2-31: when the VC breaks, every
connection it supported is broken. So:

- Port-level retransmission is invisible to SCS: SCS sees an ordered,
  gap-free stream, and a sequenced message spends its credit exactly once
  at `scs_send_msg` regardless of how many times the port retransmits it.
- On `PE_VC_DOWN_*` the port raises `vc_down(sysid, reason)` to SCS; SCS
  moves **every CDT on that SB to CLOSED** with reason path-lost, discards
  their credit ledgers (a re-formed VC starts new connections with fresh
  credits), fails pending sends with an `SS$_` status (OVMX: `SS$_PATHLOST`),
  and calls each SYSAP's `disconnected(local_conid, reason)`. SCS never
  retries a message across a VC break and never re-opens a connection on
  its own — that is a SYSAP decision.
- CNXMAN is the SYSAP that reconnects: its `recnx_fsm` (FC-P3.6) holds
  membership and retries once a second for `max(RECNXINTERVAL, remote
  TIMVCFAIL)` (spec §4(aa)), re-forming the VC and re-opening the
  `VMS$VAXcluster` connection; the CDT ladder in P2.2 therefore needs no
  "suspended" state — closed, then re-opened by the SYSAP.

This makes the receiver ruling load-bearing: because a VC break has real
CM consequences (RECNX loop, possibly a transition), the port must absorb
transient loss itself, which is exactly what the retransmit ladder is for.

**Receive anchor — RATIFIED (2026-09-03, from FC-P1.9).** A port that
formed the circuit starts at `recv_seq = 0` and the first in-order
sequenced frame must be `seq = 1`; the FC-P1.2 `recv_anchored` flag
("adopt the first sequence number seen") is deleted. Grounding: spec
§4(i).A — the post-START SCS VC resets to `send_seq = 1` on **both** sides
(0 residuals), and §4(h)(4a)'s "anchor" is a property of a **capture
scanner** attaching mid-flight, not of a port. Under go-back-N the flag
was a silent data-loss defect: lose frame 1, anchor on frame 2, ack a
message never received, the sender releases it from the ring. There is no
faithful case for a non-1 first sequence: NISCA has no resume-mid-session
mode — every formation *and every re-formation* (RECNX, §4(aa), reuses the
formation path) is a new START/STACK/ACK that resets both sides, and the
FSM resets on `RX_START` in every state. The one place a large sequence
number appears — the established member's round-0 `0x41` START itself
(§4(i)) — is the START frame's own field, excluded from sequenced scoring.
A peer that violates §4(i).A now gaps and is broken by the retransmit
ladder with a reason, never silently corrupted. No formation-vs-
reformation distinction is needed.

**FC-P1.2 correction (one item, FC-P1.9):** receiver discard + re-ack on
gap; sender ack-timeout retransmit from the oldest unacked entry with the
retransmit msgtype and a bounded ladder; break only on ladder exhaustion,
`TIMVCFAIL`, or listen timeout; R2 acceptance at 10 % loss with pipelined
sends = **all** messages delivered in order, **0** VC breaks, retransmits
> 0; and at 100 % one-way loss = exactly one break with reason
`RETRANSMIT_EXHAUSTED` after the ladder, followed by SCS `disconnected`
on every CDT of that SB.

#### 3.2.6 Served-disk path — RULINGS (2026-09-03, E40/E41/E42 from FC-P6.3/P7.1)

**E41 — WRITE block transfer is a server-initiated REQUEST DATA that the
client's PORT answers automatically. Ruled from the book; no capture
needed (the choreography is already in `vms291-mount-A.pcap`).**
Davis ch. 2 (pp. 2-32..2-41, host-only, page cites) defines the SCS block
data service as two operations on *named buffers*: **SEND DATA** (move a
local buffer to a remote named buffer) and **REQUEST DATA** (fetch a
remote named buffer into a local one). Both are initiated by the side that
knows both names — in MSCP that is always the **server**, which read the
host's buffer name off the command's buffer descriptor (`{offset, buffer
name, conid}`, AA-L619A-TK Table A-6) and minted its own. READ = server
SEND DATA (data frames toward the host, last chunk piggybacked on the end
message); WRITE = server **REQUEST DATA**: a header-only 28-byte block
frame naming the host's buffer as *source* and the server's as
*destination*, which the host's **port** answers — with no SYSAP
involvement — by transmitting the named buffer's contents under the same
header. That is exactly the measured shape in `docs/design-mscp-direction.md`
("WRITE is a two-frame request/response whose two 28-byte headers are
byte-identical — only the presence of data distinguishes them"). The
FC-P6.1 model ("data-frame-first") described SEND DATA only; the port
needs the second half:

- `vms_pe` block-transfer service gains an **automatic REQUEST DATA
  responder**: on a header-only block frame whose *source* name is in
  this port's named-buffer table, transmit the data from that buffer in
  the same chunking the READ path uses, echoing the header (`+4`/`+6`
  copied verbatim — still ungrounded, still copied not composed), with
  `+8` counting down; a request naming an unregistered buffer is dropped
  and counted (`blk_req_unknown_buffer`).
- The class driver registers the WRITE data buffer under the name it
  places in the command's buffer descriptor *before* sending the command,
  and withdraws it at end-message; the server's WRITE path issues
  REQUEST DATA after ONLINE/parameter checks and completes the command
  when `+8` reaches zero.
- Codec: `codec_mscp` distinguishes the two block forms by data presence
  only; the responder's output is asserted byte-exact against the vms291
  WRITE pair.

**E40 — the serving node's ALLOCATION CLASS is not carried by MSCP; it is
a controller-identity attribute VMS transports beside the connection.
Lab capture required; the node-qualified fallback is itself faithful.**
Allocation class is not an MSCP concept (AA-L619A-TK has no such field),
and in VMS's model it is a property of the *controller* — an HSC's
allocation class, a port allocation class — which the MSCP server
impersonates for its served units. So the class driver learns it from the
controller identity it receives when it connects to `MSCP$DISK`, not per
unit. Candidate carriers, in the order to check: (1) the SCC end message's
controller-dependent parameter area — the word AA-L619A-TK Table A-7 calls
reserved reads a constant `0x0547` in every specimen and is unexplained;
(2) the 16-byte SCA connect data of the `MSCP$DISK` connection (spec
§4(N)); (3) the CM node-parameter block. The decisive capture (lab lane):
on a clone, change VAX1's `ALLOCLASS` (SYSGEN, reboot) and diff VAX1's
SCC end, its `MSCP$DISK` connect data and its CM PARAMS against the
current capture; the field that tracks the change is the carrier; confirm
with `SHOW DEVICE/FULL $n$DUA…` on VAX2 (the class driver's CDDB reports
the class it learned). Until then FC-P7.1's rendering stands **and is
correct VMS behaviour**: a served unit whose server has allocation class
0 is named `<SCSNODE>$DUAn:` on VMS. OVMX renders exactly that, counts
`alloclass_absent`, and never asserts a `$n$` it did not receive — INV-6.
One-line unblock once grounded: populate `cddb->alloclass` from the
carrier and switch the spelling.

**E42 — the ACP consumes an async served-disk transfer by parking an
IRP and sleeping in process context; the fork thread never blocks on
storage. Ruled from the I/O model.** On VMS a `$QIO` to a served disk
builds an IRP, the class driver's start-I/O sends the MSCP command, the
IRP waits, the end message arrives at fork level, the class driver
completes the IRP, and I/O post-processing wakes the requester (`$QIOW`
= wait on that completion). The XQP/ACP issues its disk I/O the same way
and waits on it. OVMX reproduces the shape:

- `exec_blockdev_read/write_block` stay synchronous and process-context
  (their contract, `exec_kbackend.h` §8: "MAY SLEEP; call only from
  process context with no exec_lock held"). A served unit's block ops in
  `vms_devtab` are `srvdisk_read_block/_write_block`, which run in the
  ACP caller's process context and implement the wait:
  1. allocate a pool `struct vms_srvdisk_irp { exec_lock_t lk; exec_cv_t cv;
     uint8_t done; uint32_t status; ... }` (pool, never the stack — VAX);
  2. register the data buffer as a named buffer (for WRITE, E41);
  3. `vms_mscp_cl_read/_write(..., srvdisk_done, irp)` — which posts work
     to the fork queue through the rxlock (legal from process context);
  4. sleep: `exec_lock(&irp->lk); while (!irp->done) exec_cv_wait_timeout(&irp->cv,
     &irp->lk, ctmo_ms, &to); exec_unlock(...)` — the §2 cv contract on
     the IRP's own lock; `ctmo_ms` = the controller timeout the server
     advertised in its SCC end (`P.CTMO`, 20 s in the lab) plus margin;
  5. on timeout post a cancel work item and return an honest failure
     (`SS$_TIMEOUT`); on `vc_down` the class driver fails every
     outstanding IRP of that server (`SS$_PATHLOST`); mount verification
     (the VMS `%SYSTEM-I-MOUNTVER` retry) is a P7 follow-on, not a silent
     retry.
- `srvdisk_done` runs on the fork thread (under the fork mutex):
  `exec_lock(&irp->lk); irp->status = st; irp->done = 1;
  exec_cv_broadcast(&irp->cv); exec_unlock(&irp->lk)`. Lock order: fork
  mutex → `irp->lk` (a leaf). The fork thread never waits on an IRP.
- **Corollary for the MSCP SERVER (FC-P6.3) — a contract violation to
  fix:** `vms_mscp_srv.c:219/239` call `exec_blockdev_read/write_block`
  from the fork work handler, i.e. synchronous disk I/O on the cluster
  fork thread. VMS's MSCP server issues local I/O asynchronously (an IRP
  to the local driver) and completes the MSCP command on the local I/O's
  completion; it never stalls the port. OVMX: served-unit local I/O is
  dispatched to a **served-I/O worker** (a second `exec_kthread`, §15,
  "MSCP server worker") that performs the synchronous `exec_blockdev_*`
  and posts a completion work item back to the fork queue; the fork
  thread builds the end message / SEND DATA from that completion. Rule:
  **the cluster fork thread never calls `exec_blockdev_*`.** (A HELLO
  cadence stalled behind a 20 ms disk read is a TIMVCFAIL risk under load
  and a `12×(M−1)` barrier latency on every member.)

#### 3.2.7 Receive classification of SCS application messages — RULING (2026-09-03, E48 from FC-P6.5)

**The gap.** The frozen classify table grants a Con.ID only to
length-keyed classes; MSCP END messages at SCA content 86 (SCC), 90
(READ), 102 (ONLINE), 110 (GUS) get none (110/102 are excluded by the
connection-control rule's `ctrl_type != 10` guard; 86/90 match nothing),
so a booted class driver receives only the 94-content WRITE end.

**Ruling: yes — the §4(h)(1b) envelope extends to the END classes, and
that is already measured, not inferred. The faithful fix is a
length-generic, MTYPE-keyed application-message class; no capture is
needed.** Grounding:

- Layering (Davis p. 4-13/4-15, public; `docs/design-mscp-direction.md`
  §1.2): SCS sets MTYPE = "application message" (10) for SYSAP-to-SYSAP
  traffic on an open connection and **dispatches on MTYPE to the CDT named
  by the destination Con.ID** — never on length. Length-keyed classes were
  a capture-census convenience; an SCS receiver keyed on length is the
  wrong shape.
- Envelope on the END classes is measured, not extrapolated: the SCC END
  is "the 86-content MTYPE-10 class … 954 frames pair exactly with 954 SCC
  commands" and the server builder "reproduces a real captured VAX server
  answer byte for byte" (`design-mscp-direction.md` Phase D part 1) —
  that identification *read MTYPE at [46:48] on 86-content frames*, and a
  byte-exact reproduction *wrote the handle pair at [50:58]*. The block-
  transfer decode states the command/**end** class **passes** the SCS
  envelope conformance test (`content[44:46] == 0x0004`) that block frames
  fail. The vms291 mount capture holds every END length (28/44/32/36/52
  measured ⇒ content 86/102/90/94/110 = 58-byte envelope + body) on the
  MSCP connection, and the (1b) envelope claim already names 110 and 94.
- Rule 8: nothing new is asserted about abs 64/68 — the Con.ID pair at
  content [50:58] (abs 64/68) is the (1b) field the corpus measured on
  these very frames; the only thing changing is that the classify table
  stops requiring a per-length rule to *find* it.

**The class (P2.1b-style widen, item FC-P2.7):** `VMS_FCLS_SCS_APPLMSG`
— matches any sequenced-message frame with `content[44:46] == 0x0004`,
`content[46:48] == 10`, and `content[42:44] == content_len − 44`; grants
`VMS_FCAP_CONID` at [50:58]; body at content 58 (abs 72), body length =
inner length − 14. It subsumes the 94-content `SCS_APPLMSG94` class
(kept as an alias for the frozen-table no-regression net) and is checked
**after** the connection-control classes so the `ctrl_type` rule keeps
precedence for MTYPE 0–9. Block-transfer frames fail the format-word test
and stay in their own class. Codec test: the vms291 END specimens at
86/90/102/110 all classify `SCS_APPLMSG` with the Con.ID pair equal to
the provoking command's pair reversed (the "swapped by direction on
established connections" fact, `design-mscp-direction.md` §1.1), plus the
existing frozen-table vectors unchanged.

`vms_l2.c` (the ioctl pipe) is **not** on the cluster path. It stays as a
generic privileged LAN facility (the eventual `$QIO` LAN-driver surface for
user-mode protocols, gated on `PHY_IO`) if another lane needs it; otherwise it
is retired with the daemon.

### 3.3 Execution model — one fork context, like VMS

VMS serializes PEDRIVER/SCS/CNXMAN at fork IPL: one logical thread of control
mutates the SB/PB/CDT/CSB/RSB databases. OVMX reproduces that shape instead
of inventing fine-grained locking across five new databases:

- **Receive path.** The `exec_lan` rx callback runs in the substrate's rx
  context (Linux softirq; NetBSD softint or the `qe`/`xq` interrupt): pop a
  pool `exec_lanbuf_t`, copy the frame, push it onto the fork queue and
  signal — all under the one `exec_rxlock_t` (§3.2.3). Nothing else
  happens in rx context, on either substrate.
- **The cluster fork thread** (`exec_kthread`, one per node, "CNXMAN fork"):
  drains input frames, expired timers, and work requests posted by process
  context (a `$ENQ` needing the wire, a MOUNT needing an MSCP command) —
  strictly one at a time under `vms_cluster_fork_mutex`. All PE/SCS/CNXMAN
  state is touched only from this thread. This is the executive **actor**.
- **Timers** (`exec_timer`): HELLO cadence, channel/VC timeouts, retransmit,
  RECNXINTERVAL, barrier-step watchdog (instrument only — spec §4(p) says do
  not time out a slow step), MSCP polls. A timer callback only posts a work
  item to the fork queue.
- **Process context** (`$ENQ` from RMS/ACP, MOUNT, `$GETSYI`): posts a
  request and sleeps on the LKB/IRP cv (`exec_cv_wait`), exactly as
  `enq_wait_sync` does today. `vms_lock.c` keeps its own `res->lock` /
  `vms_lock_id_lock` discipline; the fork thread takes those like any caller.
  `$GETSYI`/SHOW CLUSTER readback copies a snapshot under the fork mutex.
- **Lock order:** `vms_cluster_fork_mutex` → `res->lock` → `vms_lock_id_lock`;
  the fork-queue `exec_rxlock_t` is a leaf (nothing is taken under it).
  The fork thread never sleeps holding an `exec_lock_t` (existing contract).

Rejected: a per-subsystem kthread (PE, SCS, CM) with message passing — more
concurrency than VMS has, and the barrier/rebuild coupling is exactly the
place where cross-database ordering matters. One fork context is the faithful
and the safer shape; per-node throughput at cluster message rates (tens per
second at steady state, low thousands during a rebuild) does not need more.

### 3.4 Data model (public-doc shapes, OVMX layouts)

- **PDT/PB/channel/VC** (`vms_pe.c`): per remote system a PB with the set of
  verified channels (local netif × remote MAC), the incarnation counter
  advertised in directed HELLOs, the VC with `send_seq`, `recv_seq`, `recv_ack`,
  an unacked-message ring keyed by seq (retransmit reuses the seq), credit
  state, TIMVCFAIL deadline. VC message delivery is by `(remote SB, dst Con.ID)`.
- **SB/CDT** (`vms_scs.c`): SB per remote system (SCSSYSTEMID, SCSNODE,
  software/hardware type from START/STACK, the VC). CDT per connection with
  local/remote Con.ID, SYSAP, state (the ch.2 ladder OPEN→DISC SENT/RCVD→
  MATCH→CLOSED), Send/Receive/Pending credit counters, message-input callback.
  The SYSAP registry maps name → {listen callback, connect-accept policy,
  credits}. `SCS$DIRECTORY` is itself a SYSAP with a lookup table over the
  registry.
- **CLUB/CSB** (`vms_cnxman.c`): CLUB {local CSID (learned, see below), epoch,
  membership bitmap (width undetermined on the wire — store ≥32 slots and
  reconcile), CEVOTES/QUORUM, `cluster_nodes` (the count), transition in
  progress {class, coordinator, barrier step, outstanding op-0d}, coordinator
  CSID (INFERRED predicate — §5.5)}. CSB per remote CM {CSID, SCSSYSTEMID,
  SCSNODE, votes, LOCKDIRWT, software version, connectivity state (ten), flags,
  the `VMS$VAXcluster` CDT, incarnation}. The existing `vms_cluster_members[]`
  mirror and its SET/CLEAR ioctls are **retired**; `CLUSTER_MEMBER_GET`
  becomes a projection of the CSB table (same struct, so SHOW CLUSTER is
  untouched).
- **Local CSID.** The cluster assigns it during the ADD transition; the
  authoritative map is SDA on VAX1 (VAX1 `0x00010001`, VAX2 `0x00010002`, OVMX
  `0x00010003`, memory `cluster-promotion-gap`). The membership record that
  carries `{sysid, csid}` per member (cat-01 op-05, `csid = sysid_off + 16`)
  was parsed-by-identity once and worked (7b2e3cde `CSIDLEARN`); the commit
  body's discrete assignment field is not isolated (spec §5). CNXMAN learns
  the local CSID by matching its own SCSSYSTEMID in the membership records;
  until learned the node is `NEW` and issues no DLM traffic. `vms_local_csid`
  and `dlm_member_csids` module params are retired — the DLM reads the CLUB.
- **RSB/LKB/directory** (`vms_lock.c`): RSB gains `dir_csid` (stored, not
  computed — §3.6), `master_csid` (exists), a rebuild generation; LKB gains a
  `proxy` flag (the requester-side image of a lock mastered elsewhere,
  carrying `master_lkid`/`master_csid`, replacing the `vms_dlm_origin` side
  list so `$GETLKI`, `$DEQ`, convert and BLKAST all see one object). The
  directory table is a hash of `{resource name → master CSID, holder count}`
  for resources this node is directory for.

### 3.5 Interfaces

**Boot (SYSINIT semantics).** VMS forms/joins the cluster in SYSINIT before
STARTUP.COM and before the system disk is mounted cluster-wide. STARTUP.EXE
(PID 1) reproduces the sequence:

1. `VMS_IOCTL_SYSGEN_LOAD` — push the typed SYSGEN params the cluster needs
   (SCSNODE, SCSSYSTEMID, ALLOCLASS, VOTES, EXPECTED_VOTES, VAXCLUSTER,
   LOCKDIRWT, QDSKVOTES, DISK_QUORUM, RECNXINTERVAL, TIMVCFAIL,
   CLUSTER_CREDITS, NISCS_MAX_PKTSZ, MSCP_LOAD, MSCP_SERVE_ALL) plus the
   CLUSTER_AUTHORIZE record into the executive. Missing SCSNODE/SCSSYSTEMID
   with VAXCLUSTER≥1 is fatal, as it is on VMS.
2. `VMS_IOCTL_CLUSTER_START` — the executive brings up `PEA0:` on ETH0:,
   starts the fork thread, and (VAXCLUSTER=2, or 1 with a cluster present)
   drives the join; the call returns when the CLUB reports MEMBER or, for
   VAXCLUSTER=0/1-with-no-cluster, STANDALONE. VAXCLUSTER=2 with no quorum
   waits, printing `waiting to form or join an OpenVMS Cluster` on OPA0:
   (VMS behaviour), so the harness can observe it.
3. Only then the system disk mount and STARTUP.COM. `SCS_STARTUP.COM`, the
   `SCSD.EXE` image, `VMS$VMS.DAT`'s SCS component and the `OVMX_SCS_DATALINK_*`
   CMake option are removed.

**Personality readback** (`vms_cluster_api.c`, all read-only projections):
`CLUSTER_MEMBER_GET` (kept), `VMS_IOCTL_CLUSTER_GET_CLUB` → `$GETSYI`
CLUSTER_MEMBER/CLUSTER_NODES/CLUSTER_VOTES/CLUSTER_QUORUM/CLUSTER_FSYSID/
CLUSTER_FTIME/NODE_CSID/SCSNODE, `$SETCLUEVT` via the AST path, and a
diagnostic family mirroring SDA: `CLUSTER_DIAG_PORT` (channels, VCs, seq/ack
counters), `CLUSTER_DIAG_CONN` (CDTs), `CLUSTER_DIAG_CSB`, `CLUSTER_DIAG_LOCK`
(RSB/LKB with CSIDs). The diagnostics exist so tests assert **executive
state** (INV-6), never a frame count.

**Retired**: `VMS_IOCTL_CLUSTER_MEMBER_SET/CLEAR`, `VMS_IOCTL_DLM_XNODE` as a
userspace entry (it becomes an internal function; the ioctl may survive
behind `OVMX_KTEST` for the two-node executive harness), `DLM_MEMBER_DEPART`
(CNXMAN calls the function), module params `vms_local_csid`/`dlm_member_csids`.
Every retirement also removes its `vms_kif_*` wrapper and updates both
`vms_ioctl.h` and `vms_lock_nb.h` in step.

### 3.6 The DLM: three roles, and the Rule-8 directory question

The public model (IDSM lock-management chapter; Davis ch. 7; `docs/research-
alpha-dlm-wire.md:135-165`): for a `$ENQ` on resource R, (1) if R is already
mastered locally → local; (2) else if this node is R's *directory node* →
consult the directory; if no master, this node becomes master; (3) else send a
lookup to R's directory node; it answers with the master (or "you master it");
then send the lock request to the master. Directory nodes are chosen by a hash
over the members weighted by LOCKDIRWT; nodes with LOCKDIRWT=0 are never
directory nodes. On a transition the lock database is rebuilt; the OpenVMS
Cluster Systems manual names four rebuild types (merge when a LOCKDIRWT=0 node
joins; directory when a LOCKDIRWT>0 node joins; partial/full on departures).

What OVMX may and may not do (Rule 8): it may implement every role, every
message and every rebuild type; it may **not** compute the directory hash.
The 90b3bbbd regression (self-computed directory → cluster reformation) is the
proof that a wrong guess is a cluster-breaker, not a local error.

Decisions:

- **D-DLM-1 — join with LOCKDIRWT=0 and advertise it honestly.** A
  LOCKDIRWT=0 node is never a directory node, never receives directory
  duty, and triggers only a merge rebuild — the smallest faithful footprint,
  and a legitimate VMS configuration (satellites). This is the configuration
  the lab's CLUSTER_CONFIG'd VAX2/VAX3 most likely run (§5.1 verifies).
- **D-DLM-2 — directory resolution with N weighted nodes is solved, not
  declined.** The requester must route each *root* resource's lookup to its
  directory node (sub-resources never hash: the whole lock tree is mastered
  where its root is — public, OpenVMS Cluster Systems "lock manager"
  section — so only root names matter). The members' LOCKDIRWT values are on
  the wire (SHOW CLUSTER on a VAX displays every member's LOCKDIRWT; the
  PARAMS field is pinned in §5.1), so OVMX always knows the weighted set
  `W`. When `|W| = 1` there is nothing to resolve. When `|W| ≥ 2` OVMX
  climbs a three-rung ladder; each rung is correct and non-fabricating, and
  the ladder ends in a working mechanism, never in a refusal:

  **Rung A — implement the published algorithm.** The distributed lock
  manager's directory selection is described in *VAX/VMS Internals and Data
  Structures* (Kenah, Goldenberg, Bate; Digital Press, V5.2 edition) and the
  Alpha edition (Goldenberg, Dumas, Saravanan, 1997) — published books,
  admissible under Rule 8 on exactly the regime this project already uses
  for Davis and AA-L619A-TK (host-only transcript, page cites only, never
  committed). The lock-management chapter documents the structure: a
  directory vector rebuilt at every state transition in which each node with
  LOCKDIRWT>0 occupies LOCKDIRWT entries, indexed by a hash of the root
  resource name. The P4 gate item **IDSM-DIR** transcribes that chapter and
  answers one question: does the book give the hash function and the vector
  ordering at the bit level? If it does, `dlm_directory_csid()` is
  reimplemented from the book (replacing `exec_jhash`, `vms_lock.c:527`) and
  the vector is built from the CLUB's members in the documented order.
  Implementing a published description is clean-room by definition; the
  §5.2 capture is then a **conformance check** (predicted vs observed
  directory node over ~100 root names, zero residuals), not a derivation.
  This is the expected outcome and the one to attempt first.

  **Rung B — discovery from the cluster's own answers.** If the book gives
  the structure but not the function, OVMX never computes; it asks. A real
  lock manager must cope with lookups that reach a node which is not (or no
  longer) the directory for that name — the vector changes during a rebuild
  while requests are in flight — so the protocol has a defined answer to a
  mis-addressed lookup. §5.2's second observation pins what it is. If the
  answer is a **refusal/retry-class reply** (the node declines to act as
  directory), OVMX resolves by probe: on first use of a root resource send
  the lookup to `W[0]`; on refusal, `W[1]`, …; cache `dir_csid` in the RSB;
  invalidate every RSB's `dir_csid` at every transition (the existing
  `DLM_MEMBER_DEPART` invalidation generalized to all transition classes)
  so a stale cache is never used across a vector change. Bound: at most
  `|W|−1` extra round trips per root resource per transition epoch; every
  routing decision is a value the cluster returned (Rule 8, INV-6 clean —
  the same principle as `dir_csid=0`, which the lab proved VAX1 accepts).
  Sub-resources inherit the root's answer.

  **Rung C — only if the protocol itself does not guard mis-addressing.**
  If a wrong node **serves** a mis-addressed lookup (creates a directory
  entry), probing could produce two masters for one resource — a
  data-integrity hazard — so Rung B is forbidden and the remaining honest
  paths are an operator ruling: (i) permit black-box behavioural
  derivation of the hash for interoperability (input: root names + weighted
  set; output: observed directory node) as a documented exception to Rule 8,
  or (ii) accept a configuration statement ("OVMX clusters run one
  LOCKDIRWT>0 node") that OVMX enforces at `CLUSTER_START` by **joining
  anyway** (membership does not need the directory), keeping local and
  `|W|=1` locking fully functional, and reporting cross-node locking on
  foreign root resources as `SS$_UNSUPPORTED` with one `%CNXMAN` line —
  a reported limitation, not a panic. Rung C exists so the ladder is
  complete; the design expects to stop at A.

  The three rungs plug into one function (`dlm_resolve_master`, case 3)
  behind one interface (`dir_resolve(name) → csid`), so the choice among
  them is a P4 configuration of that function, not a redesign.

  **IDSM-DIR RESOLVED (2026-09-03, `docs/research-dlm-directory-algorithm.md`).**
  The book (Davis ch. 6/7) publishes the **vector and the index rule
  fully** — entries per member = LOCKDIRWT, one per member when all are 0,
  contiguous, own entries read 0, `hash16 mod n` indexes it, resized in
  Phase 1 and filled in Phase 2 before the synchronized rebuild — so that
  is **rung A, built now**. The hash **function** is not given bit-level;
  but p. 6-50 documents that every directory lookup **carries the sender's
  16-bit hash value on the wire** and that the directory node uses the
  received value. So the value is taken from the wire, never computed —
  **rung A′ ("hash-from-the-wire")**: `rsb->hash16` is learned from every
  cat-02 frame carrying it (lookups received, master requests received,
  rebuild registrations; field = body[10:12], confirmed offline in FC-P4.2),
  and a lookup is sent **only** with a wire-learned hash (a placeholder
  hash makes the directory node create a bogus "you master it" entry —
  the campaign's grant storm, explained). Self-check: every received
  lookup must index one of OVMX's own entries, else counted and logged
  (falsifies the CSV-order hypothesis, the one residual). Rung B (probe)
  is expected **unsafe** (p. 6-31 outcome 3 creates an entry with no
  directory check described) — FC-P4.2 decides; rung C(i) goes to the
  operator with the p. 6-50 framing (VMS broadcasts the input/output pairs
  by design). Until then only a root name OVMX is the *first* in the
  cluster to touch (its own private volumes/files) is `SS$_UNSUPPORTED`;
  every shared name, membership, directory duty and mastering are
  unaffected, and the rebuild chain (P4.6, P5.3–5.5) proceeds.
- **D-DLM-3 — directory-node role is built anyway** (for LOCKDIRWT>0 later,
  and because the rebuild pushes records at whichever node the cluster
  chooses): a stored directory table, populated from rebuild records and
  from lookups it serves; it never self-assigns.
- **D-DLM-4 — every cat-02 message is bound to a real object.** Inbound
  ENQ/convert/DEQ at the master → `vms_lock_dlm_xnode_dispatch` (exists) →
  the reply is built from the resulting LKB (grant mode from the granted
  queue, master handle = the LKB's lkid, LVB from the RSB). Outbound requests
  originate from a real proxy LKB in `waiting`; a grant received completes
  it; a completion/commit is sent only from a proxy LKB that holds the
  master's handle it received. A request for a resource this node neither
  masters nor is directory for is **declined** (the honest answer the current
  daemon gives), never granted.
- **D-DLM-5 — idempotency and correlation live in the executive.** Salvage
  `vms_lock_dlm_xnode_enq_idempotent` (branch) keyed by `(req_csid,
  req_lkid)`; every reply rides the CM transaction (`txn`, send/ack
  counters) of the request it answers — the "grounding" the VAX requires.

The cat-02 opcode → operation mapping (op-01 ENQ, op-07 convert, op-03/op-04
completion/commit, op-0d rebuild record, op-12/op-15 unknown) is GROUNDED for
shape and INFERRED for semantics (spec §4(f), §5). The 17K-message "grant
storm" of the daemon experiments (`LNM$CWLOGICALS`/`F11B$aSYSDSK1` re-requested
35/s after a "grant") is consistent with those op-01s being **directory
lookups** that OVMX answered as if they were master grants. The executive
implements the *operations* (lookup / enqueue / convert / dequeue / grant /
blocking-AST / value-block / directory-entry / rebuild) and binds opcodes to
them through the codec table, so an opcode re-assignment after §5.4's capture
is a table edit, not a redesign.

### 3.7 Quorum, votes, coordinator

- VOTES=0 first (P3); the CLUB tracks CEVOTES/QUORUM from every member's
  advertised VOTES and the local EXPECTED_VOTES from day one so `$GETSYI`
  reports truth.
- Enforcement (P8): with VOTES>0 a quorum loss must **suspend** the node
  (VMS: process scheduling blocked, `%CNXMAN, quorum lost, blocking activity`)
  and resume on regain. In OVMX the executive gates the scheduler seam it
  owns (the proctab / AST delivery / ACP I/O issue points), not the Linux
  scheduler — an honest subset documented as such. Quorum disk (QDSKVOTES,
  DISK_QUORUM) via the executive block seam on a local disk, P8.
- Coordinator role (P8): the relay (`op 0x12`), the transition open/GO/barrier
  driver (`12×(M−1)`), REMOVE (`0x03`) and self-DEPART (`0x04`) classes.
  Selection predicate is INFERRED (§5.5); OVMX never *claims* the role
  unprompted — it drives a transition only when the documented condition
  holds (it detected the failure and no other CM has started one, Davis
  p. 7-30) and defers on collision.

### 3.8 Hard calls recorded (with rejected alternatives)

| # | Decision | Rejected | Why |
|---|---|---|---|
| 1 | Executive-resident, single-source in `kernel-core`, substrate seam extended with LAN/kthread/timer primitives | Keep the daemon and "harden" it; or a kernel module that is Linux-only | Ruled by the operator; Linux-only would repeat the `vms_l2.c` NetBSD hole |
| 2 | One cluster fork context (kthread + fork mutex) | Per-layer threads; run everything in rx softirq | VMS shape; ordering across PE/SCS/CM/DLM is where the crashes were |
| 3 | Pure codec TU, host-unit-tested against the capture manifest; grounded-pair allowlist as data | Build/parse inline in the state machines | The allowlist saved two VAXes; keep it auditable and fuzzable outside the kernel |
| 4 | Join at boot from STARTUP.EXE via `CLUSTER_START`, SYSINIT ordering | Keep `SCS_STARTUP.COM` as a DCL trigger | A cluster join precedes the system disk mount on VMS; a DCL-phase start is a different OS |
| 5 | Executive learns its CSID from the cluster; no module params | Keep `vms_local_csid`/`dlm_member_csids` | They made the executive a phantom cluster-of-one (memory pm(2)) |
| 6 | LOCKDIRWT=0 first; directory resolution by the A/B/C ladder (published algorithm → cluster-answer discovery → operator ruling); `exec_jhash` deleted | Keep `exec_jhash`; guess; refuse when `|W|≥2` | Rule 8 + INV-6; 90b3bbbd proved a guess breaks the cluster; a refusal is a cop-out — the ladder ends in a working mechanism |
| 7 | Proxy LKB replaces `vms_dlm_origin` | Keep the side list | One object per lock is what `$GETLKI`/`$DEQ`/convert/BLKAST need; the side list already duplicates LKB fields |
| 8 | MSCP server serves executive volumes through the ACP block seam | Keep the fd-backed server | The served unit must be the same volume the ACP mounts, or a VAX MOUNT and OVMX see different data |
| 9 | Diagnostics ioctls mirror SDA | Prove phases by pcap counts | INV-6: assert executive state; pcap corroborates |
| 10 | Non-voting membership before quorum enforcement | Votes first | A voting node with a wrong quorum implementation can partition a real customer cluster |
| 11 | The substrate seam (§3.2.2) is a deliverable: five new families, opaque buffers, both bindings in the same phase, a grep gate on core includes | Linux-first with NetBSD "later"; per-substrate cluster TUs | VAX is co-release; `vms_l2.c` shows what "later" produces (Linux-only, absent from the NetBSD SRCS) |
| 12 | Pure FSMs with injected ops, table-driven, host-simulated N-node cluster before any kernel build (§3.9) | Logic in glue; prove by booting | Fast loop, auditable, fuzzable; the campaign's crashes were untestable-without-a-VAX code paths |

### 3.9 Structure and verifiability — designed in

The operator's bar: fast build/test loop, no monster functions, code that
is auditable at a glance and provable without a full boot. These are rules,
enforced by review and by gates where a gate is cheap.

**Layout per layer.** Each layer is three files with a narrow interface:

| File | Contains | May include | Runs where |
|---|---|---|---|
| `vms_<layer>_fsm.c` / `.h` | The state machine(s): a context struct, an event enum, a table `handlers[state][event]`, one small handler per transition, and the actions it emits through an injected `struct <layer>_ops { send, arm_timer, cancel_timer, now, log, alloc, free }` | kernel-core headers, the codec, `exec_kbackend.h` **only for `exec_list/hash/rbtree` and integer types** | host tests, the host cluster simulator, the kmod |
| `vms_<layer>.c` | Executive glue: instantiates the FSM with production ops (the fork module's send/timer/log), owns the layer's objects, exposes the snapshot | `exec_kbackend.h`, the fork module | kmod only |
| `vms_<layer>_snapshot.h` | A read-only, fixed-width view struct of the layer's state (what SDA would show) | integer types | diagnostics ioctls **and** host tests assert on the same struct |

Layers on this pattern: `pe` (channel FSM, VC FSM), `scs` (connection FSM,
credit ledger), `cnxman` (`join_fsm`, `barrier_fsm`, `transition_fsm`,
`quorum` — pure arithmetic, `recnx_fsm`), `dlm_scs` (`rebuild_fsm`, the
role dispatch), `mscp_srv`/`mscp_cl` (command FSMs). `vms_lock.c` keeps its
current shape (it already has no substrate specifics beyond the seam).

**Coding rules.**
1. A function does one thing and fits on a screen (≤ ~60 lines; a table
   entry, not a `switch` ladder). `scsd.c`'s 16K lines and its 2 000-line
   dispatch functions are the anti-pattern.
2. No raw byte offsets outside `vms_cluster_codec.c`; the codec exposes
   typed getters/setters per field (`cm_body_txn()`, `dlm_body_mode()`…).
   Two crashes came from `body[N]` arithmetic in orchestration code.
3. No globals except one per-node `struct vms_cluster` passed explicitly;
   no substrate include in any `_fsm.c`/codec TU (CI grep gate, §3.2.2).
4. Every FSM handler is a test case; every grounded frame class has a
   fixture from the clean-room manifest; every response recipe (§4(p)/(r)
   of the spec) is a table with a byte-exactness test over its specimens.
5. Fixed-width types everywhere on wire/ABI structs; `_Static_assert` sizes
   in both bindings.
6. Deadlines and identities are injected (`ops.now`, params), never read
   from the substrate inside an FSM — so a test drives time.

**Test ladder per subsystem** (each rung must be green before the next is
attempted; the first three run on a developer host in seconds):

| Rung | What runs | Proves | Subsystems |
|---|---|---|---|
| 1 Host unit | `_fsm.c` + codec under `cc`, fixtures from the capture manifest; parser fuzz on corpus seeds | Frame shapes, response recipes, single-FSM transitions, quorum arithmetic, credit conservation, rebuild FSM sequencing, directory ladder logic | all |
| 2 Host cluster simulator | N instances of the **pure** stack (pe+scs+cnxman+dlm_scs FSMs) wired to a virtual LAN with configurable loss/reorder/duplication/latency and a virtual clock | Join/barrier/transition end-to-end across 2–8 simulated nodes, departures, rebuilds, coordinator behaviour, `12×(M−1)`, deadlock search — in milliseconds, deterministic, with the simulator's own SDA-like snapshots | pe, scs, cnxman, dlm_scs, quorum |
| 3 Substrate contract test | A tiny kmod test per binding: `exec_lan` loopback (Linux veth pair; NetBSD tap), timer post-and-wake, kthread start/stop, `exec_time` monotonicity | The binding honours the contract on each substrate | §14–§18 bindings |
| 4 Executive harness | 2–3 booted OVMX nodes under QEMU on a shared L2 (the existing DLM harness fabric), Linux **and** NetBSD-VAX | The real executive runs the stack: executive↔executive VCs, membership, cross-node `$ENQ` via the wire, diagnostics ioctls match | all |
| 5 Real-VAX lab (clone) | Booted OVMX vs VAX1/VAX2 (+vax3): SDA, console, pcap | Interop with real VMS — the only rung that proves faithfulness | all, per phase |

The simulator (rung 2) is the biggest single accelerator: the whole
CN=3 campaign was one re-fire per ~30 minutes against a live lab; the same
scenarios run in a loop on the host once the FSMs are pure. It also lets an
implementer reproduce a lab pcap: feed the captured VAX frames to one
simulated OVMX instance and assert its emitted frames against the
reference joiner's.

**Build loop.** Rungs 1–2 are a `ctest -R cluster_host` target with no
kernel headers; rung 3–4 build the kmods (existing `Dockerfile.bootable`
path); rung 5 is the `run_db20b.sh`-class harness re-pointed at the booted
node. The compat register rows (`docs/compat/facilities/cluster-*.yaml`)
move from `stub/partial` to `implemented/verified` only on rung 5 evidence.

---

## 4. Phased implementation plan

Each phase is one or more rd outcomes: a verifiable end state, provable with
the oracles named, completable in a session by an implementer picking it up
cold. "Reuse" names real code lifted; "New" names net-new executive code;
"Harvest" names daemon code lifted **as codec/spec only** (rewritten in
`kernel-core`, its unit tests ported). Dependencies are strict.

Oracles, in order of authority: real-VAX **SDA** (`SHOW CLUSTER/CSB`,
`SHOW CONNECTIONS`, `SHOW LOCK/ALL`, `SHOW PORT`), the VAX **console** OPCOM
lines and `F$GETSYI("CLUSTER_NODES")`, OVMX's executive **diagnostic ioctls**,
and the **pcap** on `br0`. Every lab run is on a **clone** of the lab disk
(`d0.dsk` golden), never the live working cluster (§6). The lab is the
2-node VAX cluster (`kubectl -n ovmx-lab`), plus the vax3 3-node bed once
vms-9c7 lands.

### P0 — Substrate + port skeleton: the executive owns the wire

Outcome: a booted OVMX (Linux substrate) emits the cluster HELLO from
**inside `vms.ko`** and receives directed HELLOs; no userspace process
touches ethertype 0x6007; `CAP_NET_RAW` is dropped from the whole userland
subtree (the existing `labjoin` negctl teeth) and the join harness still sees
HELLOs.

- New: the seam families §14–§18 in `exec_kbackend.h` with **both**
  bindings (Linux `packet_type`/`dev_queue_xmit`/`dev_mc_add`/`kthread`/
  `timer_list`; NetBSD `pfil` or `if_input` shim/`if_transmit`/
  `if_mcast_op`/`kthread`/`callout` — the spike in §5.6 picks the rx
  mechanism, inside this phase); the rung-3 substrate contract test on
  each; the CI grep gate on core includes; `vms_netbsd.c` dispatch parity
  (including the missing `DLM_XNODE` case). `vms_cluster_fork.c` (queue,
  thread, timer wrappers). `vms_pe.c` skeleton + `vms_pe_fsm.c` channel
  FSM: `PEA0:` device, HELLO timer, multicast join, rx queue.
  `vms_cluster_codec.c` HELLO/SOLICIT (harvest `scs_hello.c` + its tests)
  with the rung-1 host test target. `VMS_IOCTL_SYSGEN_LOAD` (cluster params
  + CLUSTER_AUTHORIZE) and `VMS_IOCTL_CLUSTER_START` (returns after port up
  for this phase). Honest identity in the HELLO software field (existing
  ruling, memory `honest-os-identity-broadcast`).
- Proof: pcap shows OVMX HELLOs with the logical `aa:00:04:00:<sysid>` at
  abs 24 and the real HW MAC as source; VAX `SCACP SHOW CHANNEL` (or SDA
  `SHOW PORT`) lists OVMX's channel Open; `CLUSTER_DIAG_PORT` reports the
  channel with the b4 CONFIRM reached; negctl: `VAXCLUSTER=0` ⇒ no HELLO, no
  `PEA0:`. **The same proof on the NetBSD-VAX rail** (a booted NetBSD-VAX
  OVMX on the lab's `br0` via its tap) — P0 is not done Linux-only.
- Open: §5.3 (credential nonce) — P0 ships the replayed lab nonce **only**
  behind `OVMX_KTEST` with the executive refusing to start the port in a
  shipping image until §5.3 is resolved, or ships zero and measures. Decide
  by the §5.3 experiment during P0.

### P1 — Virtual circuits in the executive

Outcome: the executive forms and sustains a NISCA VC with each VAX
(START/STACK/ACK with the incarnation echo, sequenced messages, cumulative
acks, retransmit-with-same-seq, credit return, TIMVCFAIL), survives a link
bounce via re-formation, and never freezes a peer's `recv_ack`.

- Harvest: `scs_vc.c` FSM + `scs_start.h` seq state (rewrite in `vms_pe.c`),
  codec for START/STACK/ACK/credit (`scs_vc.h`), unit tests
  `test_scs_vc.c`. Reuse: none from the executive.
- Proof: rung 2 — the host simulator forms VCs between 3 pure stacks under
  10% loss and reordering with no `recv_ack` freeze; rung 4 — the
  2-OVMX-node QEMU fabric forms VCs executive↔executive on Linux and on
  NetBSD-VAX; rung 5 — VAX SDA `SHOW PORT` / CSB sequence counters advance
  against OVMX for ≥10 min, `CLUSTER_DIAG_PORT` shows matching
  `send_seq`/`recv_ack`, a tap bounce of the OVMX NIC re-forms the VC within
  TIMVCFAIL.

### P2 — SCS: connections, credits, the directory service

Outcome: the executive holds live SCS connections with the VAXes — it
accepts their `SCS$DIRECTORY` connects and answers lookups (HIT for
`VMS$VAXcluster`, `MSCP$DISK`; `NOT PRESENT HERE` for `MSCP$TAPE`), opens its
own directory connection and looks SYSAPs up, runs the full connect verb set
including confirm, disconnect and the 8/9 credit pair, and accounts credit
per CDT.

- Harvest: `scs_config.c` (SB/PB/PDT), `scs_cdt.c`, `scs_conn.c`,
  `scs_credit.c`, `scs_dir.c`/`scs_sdir.c`, `scs_disc.c`, `scs_env.c`,
  `scs_connect.c` builders + tests. The CDL delivery path is built **live**
  this time (data goes through the CDT's message-input routine).
- Proof: VAX SDA `SHOW CONNECTIONS` lists OVMX CDTs `open` for
  `SCS$DIRECTORY` and `VMS$VAXcluster`; `CLUSTER_DIAG_CONN` matches Con.IDs
  byte-for-byte with SDA; a directory lookup from OVMX to VAX1 returns HIT
  (executive-side assertion, corroborated by pcap); credit conservation
  holds on a 1-minute window.

### P3 — CNXMAN: a booted OVMX is a MEMBER, non-voting, LOCKDIRWT=0 (the milestone)

Outcome: STARTUP.EXE joins the cluster in the executive before mounting the
system disk; VAX1 SDA shows OVMX's CSB `member,selected,status_rcvd`
sustained; OVMX's own `SHOW CLUSTER` lists VAX1/VAX2 from its CSB table;
`$GETSYI CLUSTER_MEMBER` is TRUE from the CLUB; the daemon, its startup
files and the mirror ioctls are gone. **And the count experiment runs**:
`F$GETSYI("CLUSTER_NODES")` on both VAX consoles is read at T+15/60/180 s.

- New: `vms_cnxman.c` (CLUB/CSB, join drive per spec §4(L)/(m)/(o), server
  half, op-06 receipt, barrier participant, op-0d echo recipe, CSID learn
  by identity, RECNXINTERVAL loop, `$SETCLUEVT`), `vms_cluster_api.c`
  readback, SYSINIT ordering in `ovmx_init`. Harvest: `scs_member.c` codec
  and field maps, `cm_response_shape`'s allowlist as codec data,
  `scs_recnx.c` state ladder + injected-clock test, `scs_quorum.c`
  arithmetic. Retire `vms_cluster_members[]` SET/CLEAR, module params,
  `SCS_STARTUP.COM`, `SCSD.EXE`, `vms_l2` from the cluster path.
- Proof: rung 2 — the simulator runs a 3-node join with the reference
  pcap's VAX frames replayed at one simulated OVMX and asserts the emitted
  frames against the reference joiner (shape + allowlist), plus an 8-node
  join/departure loop; rung 5 — (a) SDA CSB member sustained ≥3 min on a
  clone cluster, 3 fresh runs, 0 reformations; (b) OVMX `SHOW CLUSTER` from
  the executive; (c) negctl `VAXCLUSTER=0` ⇒ no join, `SS$_NORMAL`
  NOTMEMBER; (d) the count readout logged both ways; (e) the same join from
  a booted NetBSD-VAX node. If CLUSTER_NODES reaches 3 here, Q2 (§5.4) is
  answered "CNXMAN completion suffices"; if not, P3 is still complete (its
  outcome is the CSB + executive readback) and Q2 moves to P5.
- Depends: P2. Lab: §5.1 must have pinned the LOCKDIRWT field so OVMX can
  advertise 0 honestly (else advertise whatever the PARAMS builder carries
  today and log it as unpinned — do not guess a field).

### P4 — DLM requester role: OVMX's own locks go to the cluster

Outcome: a `$ENQ` issued by an OVMX process on a resource not mastered
locally is sent to the directory node learned from the wire, granted by the
master, and appears in the VAX's own lock database with OVMX's CSID as the
requester; `$DEQ`, convert, BLKAST delivery to the OVMX holder and LVB
read/write cross the wire. The standing `F11B$v<label>` mount lock (branch
salvage) is the first real standing lock OVMX registers.

- Reuse: `vms_lock.c` grant/queue/LVB/BLKAST core; `grant_recv` logic
  folded into the proxy LKB; `enq_wait_sync`. New: proxy LKB, completed
  `dlm_resolve_master` cases 2/3 via `vms_dlm_scs.c`, outbound cat-02
  builders bound to LKB fields (op-01 body[24:28] = the proxy LKB's lkid,
  flags from the LKB, mode from `requested_mode`), completion/commit from
  the received master handle, idempotent retransmit. Harvest: `scs_dlm.c`
  codec + spec §4(f) field map; the db20-b grant-accepted frame shape as a
  codec test vector.
- Proof: VAX1 SDA `SHOW LOCK/ALL` (or `SHOW RESOURCE`) lists the resource
  with a lock whose CSID is OVMX's; `CLUSTER_DIAG_LOCK` shows the proxy LKB
  granted at the requested mode with the master's handle; a VAX process
  holding EX on the same name (a MOUNT-visible lock like `F11B$v` on a
  volume both mount, or a file lock via `RMS-E-FLK` on a served file once P7
  exists) blocks OVMX's request and BLKAST fires; no reformation over 10 min
  of a lock loop at 10/s.
- Depends: P3; gate item **IDSM-DIR** (D-DLM-2 rung A: transcribe the
  published lock-management chapter, implement `dir_resolve` from it or
  select rung B from the §5.2 observation). `exec_jhash` is deleted in this
  phase whatever the rung.

### P5 — DLM master + directory-node roles + rebuild coupled to transitions

Outcome: VAX lock requests for resources OVMX masters are granted from real
LKBs and completed; OVMX serves directory lookups for resources it is
directory for (when assigned by the cluster, i.e. LOCKDIRWT>0 experiments);
on a member departure OVMX remasters/rebuilds per the transition class; on
OVMX's own join/departure the rebuild handshake is driven from real state;
Julie's bar: **membership holds under sustained real lock load** (a VAX
process loop + an OVMX RMS loop on a shared resource for 10 min, 0
reformations, 0 retry storms).

- Reuse: `vms_lock_dlm_xnode_dispatch` (all ops, called directly),
  `DLM_MEMBER_DEPART` invalidation, REBUILD/remaster, DLKSRCH victim. New:
  stored directory table; rebuild FSM (freeze → merge/directory/partial/full
  → thaw) hooked to CNXMAN transition callbacks; op-0d records consumed into
  the directory table (not merely echoed) when OVMX is the assigned
  directory; inbound lookup vs enqueue disambiguation per §5.4's capture;
  decline path for unassigned resources; SEARCH legs of distributed deadlock
  in the executive (the design in `docs/design-dlm-distributed-deadlock.md`
  moves from daemon orchestration to `vms_dlm_scs.c`).
- Proof: VAX SDA shows a VAX-held lock whose master CSID is OVMX; kill VAX2
  ⇒ OVMX's diagnostics show the class-03 transition, remaster, and VAX1's
  locks intact; the 10-minute load run; CLUSTER_NODES=3 on both consoles
  if Q2 is DLM-gated.
- Depends: P4; §5.4 capture for opcode semantics.

### P6 — MSCP server in the executive: a VAX MOUNTs an OVMX volume

Outcome: OVMX serves its ODS-2 volume(s) as `$<ALLOCLASS>$DUAn` through the
executive's block seam; VAX1 `MOUNT`s it, `TYPE`s a marker file, writes one
(when the volume is served read/write), and the F11B$ locks for that volume
flow through P5's master role.

- Harvest: `scs_mscp_srv.c` protocol (SCC/GUS/ONLINE/READ/WRITE ends,
  block-transfer header, the measured lengths) + `test_scs_mscp_srv.c`
  vectors. New: `vms_mscp_srv.c` over `exec_blockdev_*`/the ACP; `MSCP$DISK`
  SYSAP registration only when a serveable unit exists (never a listener
  that black-holes).
- Proof: `%MOUNT-I-MOUNTED` on the VAX console; marker round-trip; pcap
  `ONLINE→GUS→READ→block transfer` from OVMX; ONLINE-END measured from the
  booted node (closes the stub in `docs/design-mscp-direction.md`).
- Depends: P3 (P5 for the volume locks to be honest under a shared mount).

### P7 — Disk class driver: OVMX mounts a VAX-served disk

Outcome: VAX-served units appear as devices in OVMX (`SHOW DEVICE` lists
`$2$DUA0:` with `DVI$_MSCP_SERVED`), the ACP mounts one, `TYPE` works, and
cluster-wide file access is real: an RMS open on OVMX takes the F11B$/RMS$
locks that a VAX process contends on, and vice versa.

- New: `vms_mscp_cl.c`, device entries in `vms_devtab`, ACP block I/O over
  named-buffer transfers. Harvest: `scs_mscp.c` discovery walk.
- Proof: `RMS-E-FLK` observed on the VAX when OVMX holds the file, and on
  OVMX when the VAX does; SDA shows the F11B$ locks with both CSIDs.
- Depends: P4, P5.

### P8 — Quorum, votes, coordinator, departure classes

Outcome: OVMX with VOTES=1 changes CL_VOTES/CL_QUORUM on the VAX's SHOW
CLUSTER; losing quorum suspends OVMX's executive-gated activity and regaining
it resumes; OVMX coordinates an ADD (vax3 joining while OVMX is the highest
node) and a REMOVE (a VAX killed) with the `12×(M−1)` barrier; a clean
OVMX shutdown emits last-gasp + class-04 DEPART; quorum disk optional.

- Depends: P5; the vax3 bed (vms-9c7) for a coordinated ADD.
- Proof: consoles + SDA; the partition test on the clone cluster.

### P9 — Cluster-wide services

`$SETCLUEVT` (P3 provides the events), `$GETSYI` full CLUSTER_* set, cluster
time (SYSMAN SET TIME semantics), cluster-wide logical names
(`LNM$CWLOGICALS` via the DLM), SYSMAN cluster-wide DCL. Decompose when P5
lands.

### Substrate parity — part of every phase, not a track

There is no separate NetBSD track: each phase's proof includes the
NetBSD-VAX rail (P0 channel, P1 VC, P2 connections, P3 join, P4/P5 locks,
P6/P7 MSCP) because the stack is one source and the bindings land in P0.
What each later phase adds per substrate is only ioctl dispatch cases
(`vms_module.c` + `vms_netbsd.c`) and `vms_lock_nb.h` mirrors, checked by
the existing `_Static_assert` discipline and the arch-asymmetry CI legs
(memory `asymmetric-arch-red-is-real`).

### What is reused vs new (summary)

| Reuse (executive, portable) | Net-new executive code | Harvest as codec/spec (rewrite) | Discard |
|---|---|---|---|
| `vms_lock.c` engine: modes, queues, LVB, BLKAST, deadlock, `dlm_xnode_dispatch` (all ops), depart invalidation, remaster, DLKSRCH victim; `vms_ast.c`, `vms_devtab.c` ETH0:, `exec_blockdev_*`, the ACP; branch salvage: standing `F11B$v` mount lock, idempotent ENQ | `vms_pe.c`, `vms_scs.c`, `vms_cnxman.c`, `vms_dlm_scs.c`, `vms_mscp_srv.c`, `vms_mscp_cl.c`, `vms_cluster_api.c`, substrate LAN/kthread/timer families, proxy LKB, directory table, rebuild FSM, requester send path, SYSINIT ordering | `scs_hello/vc/start/connect/dir/disc/env/member/dlm/mscp*.{c,h}` builders + field maps + 87 unit tests; `cm_response_shape` allowlist; `scs_recnx` ladder; `scs_quorum` arithmetic; `docs/cluster-protocol-spec.md` | `scsd.c` orchestration, `scs_datalink.c`, `scs_poll.c`, `scs_cdt/conn/credit/config.c` as code (their tests inform the rewrite), `SCS_STARTUP.COM`, `SCSD.EXE`, `OVMX_*` env gates, `SCS_HELLO_LAB_NONCE_BYTES`, `CLUSTER_MEMBER_SET/CLEAR`, `vms_local_csid`/`dlm_member_csids`, `feat/coord-rebuild-completion` daemon patches |

---

## 5. Open questions that need a lab oracle-capture (with the recipe)

Ordered by which phase they gate.

### 5.1 LOCKDIRWT on the wire (gates P3's honest advertisement, P4's directory node)

Question: which PARAMS/CONFIG field carries LOCKDIRWT, and what do VAX1/VAX2/
VAX3 advertise? Recipe: on VAX1 `SHOW CLUSTER/CONTINUOUS`, `ADD LOCKDIRWT`
(SYSTEMS class) — read every member's value; then `SYSGEN SET LOCKDIRWT 1`
on a clone's VAX2 root, reboot VAX2 into the clone cluster, pcap its
MODEL/PARAMS/CONFIG frames, diff against the LOCKDIRWT=0 capture; the byte
that moved is the field. Also record whether the op-0d record volume to the
joiner changes with its LOCKDIRWT (it decides whether op-0d is directory
assignment or something else — §5.4). Cost: one lab session.

### 5.2 Directory resolution with `|W| ≥ 2` (gates P4; feeds D-DLM-2's ladder)

Two things to obtain, in order.

**(1) IDSM-DIR — the published algorithm.** Transcribe the lock-management
chapter of *VAX/VMS Internals and Data Structures* (V5.2 edition; the Alpha
edition as a cross-check) to the host-only transcript area, page-cited like
the Davis transcript. Record: the directory-vector construction (how many
entries per weighted node, in what member order, when rebuilt), the hash
input (root name only? length? parent?), and whether the hash **function**
is given at the bit level. If it is, implement it — that settles the
question at rung A.

**(2) One lab capture, two observations.** Clone cluster; set `LOCKDIRWT`
VAX1=1, VAX2=1 (second run VAX2=2 for weighting); VAX3 stays LOCKDIRWT=0 as
the requester. From VAX3 generate ~100 **root** resources with known names
— e.g. `$ OPEN` 100 files on the shared disk (each RMS file lock `RMS$…` is
a root resource) and `$ MOUNT/CLUSTER` a volume with a chosen label
(`F11B$v<label>`). pcap on `br0`: for each root name, the node VAX3 sends
the first lookup to = the observed directory node. This yields
(a) **conformance data for rung A** (predicted vs observed, zero residuals
expected), or, if no function is published, the data that a rung-C(i)
ruling would need; and (b) **the mis-addressing behaviour for rung B**:
from VAX3 (or from OVMX P4 in `OVMX_KTEST` mode) send one lookup for a
name whose observed directory is VAX2 to VAX1 instead, and record VAX1's
reply class and whether VAX1's `SDA> SHOW RESOURCE` afterwards holds a
directory entry for it. Refusal + no entry ⇒ rung B is safe; entry created
⇒ rung B is unsafe and rung C goes to the operator with this evidence.
Cost: one lab session; it shares the vax3 bed with §5.4.

### 5.3 The cluster credential (gates P0 shipping)

The HELLO carries a 4-byte token derived from CLUSTER_AUTHORIZE group/password
by an unpublished function; OVMX replays `SCS_HELLO_LAB_NONCE_BYTES` captured
from the lab. Recipe: on a clone, boot OVMX P0 with (i) token = 0, (ii) a
random token, (iii) the replayed token; observe whether VAX1 opens the
channel/VC in each case (SCACP + pcap). If (i) or (ii) is admitted, the token
is not an authenticator on this cluster and OVMX ships zero (honest). If only
(iii) is admitted, escalate exactly as §5.2(b) — same principle, same
decision — and until ruled, the executive refuses to start the port in a
shipping image without an operator-provided token.

### 5.4 cat-02 opcode semantics and the op-0d rebuild (gates P4/P5 mapping; the count-commit Q2)

Questions: is cat-02 op-01 to a directory node a lookup, an enqueue, or both
(the 35/s re-request storm suggests OVMX answered lookups with grants)? What
do op-03/op-04/op-07/op-12/op-15 mean? Is op-0d directory assignment (to a
LOCKDIRWT>0 joiner) or a broadcast every joiner receives? Which frame
precedes `CLUSTER_NODES` 2→3? Recipe: vms-9c7 — a full-disk vax3 joining the
clone cluster, pcap on `br0` + `F$GETSYI("CLUSTER_NODES")` polled on VAX1's
console at 1 s with the same pod clock, twice: once with vax3 LOCKDIRWT=0,
once =1. Correlate the count flip to the last frame class before it; tabulate
per-opcode direction × response-bit. This single capture answers Q2, §5.1's
second half, and the opcode mapping. It is also the 3-node load bed for P5.

### 5.5 Coordinator selection (gates P8)

INFERRED "highest node number" (spec §4(p)). Recipe: on the 3-node clone,
vary SCSSYSTEMID ordering and join order, kill members, observe who drives
the barrier. Until grounded OVMX never self-elects (§3.7).

### 5.6 NetBSD LAN binding — resolved by design; a P0 spike picks the rx hook

The binding is specified (§3.2.1): rx by link-layer `pfil(9)` if the rail's
NetBSD version runs `pfil_run_hooks(ifp->if_pfil, …, PFIL_IN)` in
`ether_input`, else by interposing `ifp->if_input`; tx by `if_transmit`
with a complete header; multicast by `if_mcast_op`. The P0 spike (half a
day, on the rail's NetBSD tree) records three facts: which rx hook exists,
the IPL at which `qe`/`xq` deliver input (sets the rx-queue lock's IPL),
and that `if_transmit` accepts a pre-built frame on those drivers. Neither
outcome changes the core; both are one-screen adapters.

### 5.7 Barrier step count above M=4, bitmap width, `op 0x0f`

Grounded only to four members (spec §4(p)). The executive instruments a
mismatch (logs, does not abort) and the vax3 bed extends M. `op 0x0f`
(REMOVE class extra step) is answered by echo per the grounded recipe; its
meaning is not needed to participate.

---

## 6. Hazard register — how this stack can crash a real cluster, and the guards

| Hazard (observed) | Guard in this design |
|---|---|
| Answering an ungrounded (SYSAP,cat,op) with a body echo reflected a peer's Con.IDs → `INCONSTATE`/`INVEXCEPTN` on two VAXes | The allowlist is codec data; unknown pairs are logged and not answered; category is resolved through the CDT's SYSAP first |
| A placeholder lock id in a completion → `INVLOCKID` bugcheck, CNXMGRERR cascade | Completions are built only from a proxy LKB holding a master handle it received (D-DLM-4); no constant lock ids exist in the codec |
| Applying cat-01 mutations to an op-0d body corrupted the resource name → `LOCKMGRERR` | Per-category response recipes in the codec with unit tests on the 1367-specimen corpus |
| Self-computed directory → cluster reformation | No hash in the executive (D-DLM-2/3) |
| Unsolicited pre-barrier op-01 made the coordinator withhold XITGO | The requester role is inactive until the CLUB is MEMBER and the transition has completed; no DLM traffic during a transition except the rebuild FSM's |
| A malformed 58-byte frame (inherited length words) dropped as a runt, freezing `recv_seq` | Codec derives every length from what it emits; frame-length asserts in the unit tests |
| A shared-`send_seq` hole from an unprocessable connect | One VC send path (`vms_pe.c`), one contiguous seq, retransmit reuses seq; the join sequencer is the only thing that pipelines connects |
| Kernel bug panics the host | Pure codec + state machines are host-unit-tested and fuzzed on the capture corpus before they meet the substrate; the substrate binding is minimal; the fork thread wraps state-machine entry in bounds checks; a `PEA0:` STOP path exists for the harness |
| Testing against the live lab | Every P-phase run is on a clone (`d0.dsk` golden copy, fresh `vaxlab-*` pod) |

---

## Appendix A — where the wire spec lives (do not re-derive from captures)

`docs/cluster-protocol-spec.md`: §2 framing; §4(a)–(c) HELLO/SOLICIT; §4(g)
START/STACK/ACK + Con.ID binding; §4(i) incarnation echo; §4(h)/(m)
connection verbs 0–10, credit pair 8/9; §4(t) Con.ID allocation; §4(d)/(j)
the 190-byte CM body and transaction correlation; §4(L)/(o) join drive;
§4(p) barrier + op-0d recipe + the allowlist rule; §4(q) MEMBER follows the
transition; §4(r) role slot / transition class; §4(y) total connectivity;
§4(aa) RECNXINTERVAL; §4(f) DLM field map; §4(e)/(n) + `design-mscp-direction.md`
MSCP; §5 the unknown/inferred register. `design-cluster-join-choreography.md`
for the joiner-drives model. Memory `cluster-promotion-gap` for the DLM
member-body offsets measured in the CN=3 campaign (body=frame+72; cat@8,
op@9, req_lkid@[4:8]; named: flags@[20:24], requester lkid@[24:28], master
handle le16@[28:30], mode@[30]; resname len@[47], name@[48]).

## Appendix B — daemon harvest list

Lift as codec + tests: `scs_hello.{c,h}`, `scs_vc.h` constants + FSM tables,
`scs_start.h`, `scs_connect.{c,h}`, `scs_dir.{c,h}` templates and the
per-name affirmative result, `scs_disc.{c,h}`, `scs_env.{c,h}`,
`scs_member.{c,h}` (all builders and `SCS_MEMBER_*` maps),
`scs_dlm.{c,h}`, `scs_mscp*.{c,h}` (message classes, lengths, block-transfer
header), `cm_response_shape` (as the allowlist table), `scs_recnx` ladder
and its injected-clock test, `scs_quorum` arithmetic and test; the
`tests/vmsscs/*_mutants.py`/`*_figures.py` byte-exactness batteries re-pointed
at the executive codec; `docs/clean-room` manifest tooling unchanged.
