# Plan — faithful executive-resident VMScluster stack: work breakdown

> Companion to `docs/design-faithful-cluster-executive.md` (the DESIGN;
> section references below are into it). This file is the orchestration
> artifact: phases → work items, each cold-executable, with dependencies,
> model caliber and gates. Maps 1:1 onto rd items (item id = the `FC-Px.y`
> tag; `part-of` = `--parent-id`; `blocked-by` = `rd dep add`).
>
> Written 2026-09-02 against origin/main `e94d78da`.

## 0. Conventions every item inherits

**Cold-start preamble (paste into every dispatch).** Read, in order:
`docs/design-faithful-cluster-executive.md` §1, the section named in the
item, §3.9 (coding rules + test ladder), §6 (hazards); then the files the
item lists. Memory files that bind: `cluster-must-be-executive-resident`,
`executive-backed-not-wire-plumbing`, `cluster-promotion-gap` (wire facts),
`netbsd-module-srcs-four-places`, `vms-ko-two-object-lists`,
`asymmetric-arch-red-is-real`. The wire spec is
`docs/cluster-protocol-spec.md` — never re-derive from captures.

**Rules that apply to every item.** Kernel-core cluster TUs include only
`exec_kbackend.h` and kernel-core headers (CI grep gate from FC-P0.1). New
TU → `src/kernel/Makefile` `vms-y` + distro Kbuild + NetBSD `SRCS`. New
ioctl → `vms_ioctl.h` + `vms_lock_nb.h` mirror with `_Static_assert` + both
dispatch switches + `vms_kif_*` wrapper/census entry. Functions ≤ one
screen; FSMs table-driven with injected `ops`; no raw byte offsets outside
the codec; fixed-width types; no substrate include in `_fsm.c`. Every item's
done-condition names its test-ladder rung (design §3.9): **R1** host unit,
**R2** host N-node simulator, **R3** substrate contract test, **R4** QEMU
executive harness (Linux **and** NetBSD-VAX), **R5** real-VAX lab (on a
clone of the lab disk, never the live cluster).

**Caliber legend.** `O5` = Opus-5-tier: protocol/FSM/concurrency/
architecture judgment where the spec is a description, not a recipe.
`S` = Sonnet-tier: wiring, mechanical harvest, fixtures/tests, bindings
against a frozen contract, lab operation from a written recipe. Effort
hints in parentheses.

**Gate legend.** `LAB` = needs a lab capture/observation before or during
the item; `DOC` = needs a published-document transcription; `OP` = needs an
operator ruling; `Q2` = the count-commit oracle.

**Scope change vs the design's phase text.** The coordinator ADD-class
FSM moves from P8 into P3 (FC-P3.12): any all-OVMX cluster (R2 simulator,
R4 harness) needs a coordinator, so P3 cannot be proven executive↔executive
without it. REMOVE/DEPART classes and quorum enforcement stay in P8.

---

## P0 — The executive owns the wire (outcome: a booted OVMX, Linux and NetBSD-VAX, emits and receives cluster HELLOs from inside the executive; no userspace 0x6007 path; `CAP_NET_RAW` dropped from userland)

Design §3.2.1, §3.2.2, §3.3, P0.

| ID | Outcome (end state) | Done-condition (rung) | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P0.1 | **Seam + layer interfaces frozen.** `exec_kbackend.h` declares families §14–§18 (`exec_lan_*`, `exec_lanbuf_t`, `exec_kthread_*`, `exec_timer_*`, `exec_time_now_vms`, `exec_ticks_ms`, `exec_console_printf`) with contract rules 1/2 as doc-comments; the narrow inter-layer headers exist with their `ops` vtables and event enums: `vms_pe.h` (message/datagram services, upper-layer delivery by (SB, Con.ID)), `vms_scs.h` (SYSAP registry, CDT send/receive/credit), `vms_cnxman.h` (transition callbacks to the DLM, CLUB/CSB query), `vms_dlm_scs.h` (role ops), `vms_cluster_snapshot.h` (fixed-width view structs per layer). A `struct vms_cluster` per-node context. CI grep gate script for forbidden includes. | Both kmods compile with stub bindings returning `SS$_NOSUCHDEV`; grep gate runs in CI and fails on an injected `#include <linux/netdevice.h>` in `vms_pe.c`. (R3 stub) | `src/kernel-core/exec_kbackend.h`, `src/kernel-core/vms_pe.h`, `vms_scs.h`, `vms_cnxman.h`, `vms_dlm_scs.h`, `vms_cluster_snapshot.h`, `vms_cluster.h`; `tools/ci/cluster_core_includes_gate.sh` | — | **O5** (high) — this is the contract every parallel item builds against; getting the boundaries wrong serializes the whole plan | — |
| FC-P0.2 | Linux binding of §14–§18 passes the substrate contract test. | R3 on Linux: veth-pair loopback delivers a 0x6007 frame to `rx_cb` in softirq and `exec_lan_xmit` is seen on the peer; multicast add visible in `ip maddr`; timer post-and-wake; kthread start/stop; `exec_time_now_vms` monotone. | `src/kernel/exec_kbackend_linux.h` (+ a small `vms_lan_linux.c` if the inline header grows past a screen), `tests/qemu/test_kmod_cluster_seam.c` | P0.1 | S (medium) | — |
| FC-P0.3 | NetBSD rx-hook spike: recorded facts on the rail's NetBSD tree — which link-layer hook exists (`pfil` on `ifp->if_pfil` in `ether_input` vs `if_input` interposition), the IPL at which `qe`/`xq` deliver input, and that `if_transmit` accepts a pre-built Ethernet frame. | A short note in `docs/research-netbsd-lan-binding.md` with file:line cites into the NetBSD tree used by `tests/netbsd/`; decision recorded for P0.4. | `tests/netbsd/` build tree, NetBSD `sys/net/if_ethersubr.c`, `sys/dev/qbus/if_qe.c`/`if_xq` | — | S (medium) | — |
| FC-P0.4 | NetBSD binding of §14–§18 passes the substrate contract test on NetBSD-VAX. | R3 on the NetBSD-VAX rail (SIMH tap): same assertions as P0.2. | `src/kernel-netbsd/exec_kbackend_netbsd.h`, `vms_lan_netbsd.c`, `tests/netbsd/guest/cluster_seam.c` | P0.1, P0.3 | S (medium) | — |
| FC-P0.5 | The cluster fork context: one kthread per node draining an rx queue and a work queue under `vms_cluster_fork_mutex`; `cf_timer_*` wrappers that post work; clean start/stop; no protocol code runs outside the thread. | R1: the queue/dispatch logic is pure and unit-tested with fake ops (ordering, stop while work pending, timer coalescing); R3: runs on both substrates in the seam test. | `src/kernel-core/vms_cluster_fork.c`/`.h` | P0.1 | **O5** (medium) — concurrency/serialization core; a wrong lock order here poisons every layer | — |
| FC-P0.6 | Codec foundation: `vms_cluster_codec.{c,h}` with typed `get/put_le16/32`, a frame-class registry (ethertype → SCA header → class by length/msgtype), the (SYSAP, category, opcode) allowlist table type, fixture loader reading `docs/clean-room` manifest-hashed specimens, host ctest target `cluster_host` (no kernel headers). | R1: `ctest -R cluster_host` builds on the host in <10 s; a round-trip test on one specimen per class; parser fuzz seed harness stub. | `src/kernel-core/vms_cluster_codec.{c,h}`, `tests/cluster/host/CMakeLists.txt`, `tests/cluster/host/fixtures/` | — | **O5** (medium) — the codec architecture everyone fills in; accessor discipline (§3.9 rule 2) is set here | — |
| FC-P0.7 | HELLO/SOLICIT codec entries (spec §4(a)/(b)/(c), §4(k) padded HELLO) with byte-exactness tests; honest software/version field; logical `aa:00:04:00:<sysid>` at abs 24. | R1: reproduces the reference HELLO specimens byte-for-byte from fields; `test_scs_hello.c` assertions ported. | codec; harvest `src/vmsscs/scs_hello.{c,h}`, `tests/vmsscs/test_scs_hello.c` | P0.6 | S (low) | — |
| FC-P0.8 | Channel FSM (`vms_pe_fsm.c`): HELLO cadence, directed HELLO on first sight, b2/b3/b4 channel verify, packet-size verify, per-remote incarnation tracking, channel timeout. | R1: table-driven transitions each a test; replays a captured channel formation and emits the reference joiner's frames (shape). | `src/kernel-core/vms_pe_fsm.{c,h}` | P0.1, P0.7 | **O5** (medium) — spec §4(a)–(c)/(k) is descriptive; timing/verify semantics need judgment | — |
| FC-P0.9 | `vms_pe.c` glue: `PEA0:` device in `vms_devtab` bound to ETH0:, multicast join, rx queue → fork thread → channel FSM, HELLO timer via `cf_timer_*`; `CLUSTER_DIAG_PORT` ioctl returning the pe snapshot (both dispatches, nb mirror). | R4: booted Linux and NetBSD-VAX nodes show `PEA0:` in SHOW DEVICE and the snapshot reports HELLO tx count advancing. | `src/kernel-core/vms_pe.c`, `vms_devtab.c` (PEA0 entry), `vms_ioctl.h`, `vms_lock_nb.h`, `vms_module.c`, `vms_netbsd.c` | P0.2, P0.4, P0.5, P0.8 | S (medium) | — |
| FC-P0.10 | Cluster SYSGEN parameters live in the executive: `VMS_IOCTL_SYSGEN_LOAD` carries SCSNODE, SCSSYSTEMID, ALLOCLASS, VOTES, EXPECTED_VOTES, VAXCLUSTER, LOCKDIRWT, QDSKVOTES, DISK_QUORUM, RECNXINTERVAL, TIMVCFAIL, CLUSTER_CREDITS, NISCS_MAX_PKTSZ, MSCP_LOAD, MSCP_SERVE_ALL + the CLUSTER_AUTHORIZE record into `struct vms_cluster`; new typed params added to the SYSGEN store where missing. | R4: SYSGEN SHOW on a booted node reads back what STARTUP loaded; `CLUSTER_DIAG` exposes the loaded set; negctl: missing SCSNODE with VAXCLUSTER=2 ⇒ `SS$_BADPARAM` logged on OPA0:. | `vms_ioctl.h` + mirror, `vms_cluster_api.c`, `src/libvms/include/sysgen_params.h`, `src/ovmx_init/ovmx_init.c` (load before cluster start) | P0.1 | S (medium) | — |
| FC-P0.11 | `VMS_IOCTL_CLUSTER_START` (P0 semantic: port up) wired into STARTUP.EXE with VAXCLUSTER gating; `VAXCLUSTER=0` ⇒ no `PEA0:`, no HELLO. | R4: boot with VAXCLUSTER=2 ⇒ HELLOs on the tap; VAXCLUSTER=0 ⇒ none (negctl test both substrates). | `vms_cluster_api.c`, `ovmx_init.c`, `tests/qemu/test_cluster_start_negctl.sh` | P0.9, P0.10 | S (low) | — |
| FC-P0.12 | NetBSD dispatch parity for the DLM: `VMS_IOCTL_DLM_XNODE` (and `DLM_GET_GRANTED`, `DLM_ENUM_WAITS`) dispatched in `vms_netbsd.c` behind `OVMX_KTEST`, same as Linux. | R4: the existing `test_syssvc_dlm_xnode` subtests pass on the NetBSD-VAX rail. | `src/kernel-netbsd/vms_netbsd.c:1243-1285`, `vms_lock_nb.h` | — | S (low) | — |
| FC-P0.13 | Credential experiment (design §5.3): recorded whether VAX1 opens a channel/VC to a HELLO carrying token=0, random, replayed. | R5 on a clone: three runs, SCACP `SHOW CHANNEL` + pcap per run, result table in `docs/research-cluster-credential.md`; decision: ship zero, or escalate. | lab harness (`tests/lab/tools/labjoin_*`), P0.9 build | P0.9 | S (medium, lab) | LAB; OP if only the replayed token is admitted |
| FC-P0.14 | R5 proof, Linux: booted OVMX HELLOs seen on `br0`; VAX SCACP shows the OVMX channel Open; `CLUSTER_DIAG_PORT` shows b4 reached; CapEff of the whole userland lacks `CAP_NET_RAW`. | R5, 3 fresh runs. | lab harness | P0.11, P0.13 | S (medium, lab) | LAB |
| FC-P0.15 | R5 proof, NetBSD-VAX: same as P0.14 from the rail's booted node on the lab tap. | R5. | lab harness + `tests/netbsd/` | P0.11, P0.13 | S (medium, lab) | LAB |
| FC-P0.16 | **Receive-level lock conformance** (design §3.2.3 ruling): seam §1b added — `exec_rxlock_t`, `exec_rxflags_t`, `EXEC_LAN_RX_IPL`, `exec_rxlock_init/destroy/acquire/release`, `exec_cv_wait_rx` — in both bindings (Linux `spin_lock_irqsave` + `prepare_to_wait`/`schedule`; NetBSD spin kmutex at `EXEC_LAN_RX_IPL` + `cv_wait`); `vms_cluster_fork.c`'s input list, work list, stop flag and pool freelist guarded by ONE rxlock + its paired cv; rx/timer callbacks take only the rxlock; the fork thread splices the queue under the rxlock and dispatches under the fork mutex; contract rule 14.1 pasted as the §14–§16 doc-comment; the OPEN HAZARD block in `vms_cluster_fork_bind.c` removed. | R3 on both substrates with the same-CPU hammer: a process-context poster loop pinned to the receiving CPU under a 0x6007 flood for 60 s — no lockup, no panic, no lost wake (every posted item is observed within one scheduling latency); R1 for the queue logic with fake ops. | `exec_kbackend.h` §1b, `exec_kbackend_linux.h`, `exec_kbackend_netbsd.h`, `vms_cluster_fork.c`, `vms_cluster_fork_bind.c`, `tests/qemu/test_kmod_cluster_seam.c`, `tests/netbsd/guest/cluster_seam.c` | P0.2, P0.4, P0.5 | S (medium) — the contract is now exact; wiring against it | — |

## P1 — Virtual circuits in the executive (outcome: the executive forms and sustains a NISCA VC with each VAX and with other OVMX nodes; survives a link bounce)

Design P1, §3.4 (PDT/PB/VC).

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P1.1 | START/STACK/ACK, sequenced-message envelope (msgtype 0x4b/0x5b, `recv_ack@32`, `send_seq@34`), credit-return `0x48` codec entries + specimens. | R1: byte-exact on the formation specimens; incarnation field accessor tested. | codec; harvest `scs_vc.h`, `scs_start.h`, `tests/vmsscs/test_scs_vc.c` | P0.6 | S (low) | — |
| FC-P1.2 | VC FSM: formation with incarnation echo (spec §4(g)/(i)), one shared contiguous `send_seq`, cumulative `recv_ack`, unacked ring keyed by seq, retransmit reusing seq, credit window, TIMVCFAIL, teardown, re-formation after loss. | R1: every transition a test; a loss/reorder unit scenario never freezes `recv_ack`. | `vms_pe_fsm.c` (VC tables), `vms_pe.h` services | P0.8, P1.1 | **O5** (high) — sequencing/retransmit semantics were the source of three campaign stalls | — |
| FC-P1.3 | Port message/datagram services behind `vms_pe.h`: `pe_send_msg(sb, buf)`, `pe_send_dg`, upper-layer delivery callback by (SB, dst Con.ID); block transfer deferred to FC-P6.1. | R1 with a fake upper layer. | `vms_pe.h`, `vms_pe_fsm.c` | P1.2 | S (medium) | — |
| FC-P1.4 | Host cluster simulator core: virtual LAN (loss/reorder/dup/latency, per-link), virtual clock driving `ops.now`/timers, N instances of the pure stack, a scenario DSL, SDA-like snapshot dumps; ctest `cluster_sim`. | R2: 3 simulated `pe` stacks form VCs under 10 % loss; deterministic by seed. | `tests/cluster/sim/` (host-only), consumes `_fsm.c` + codec | P0.6, P1.2 | **O5** (high) — the harness architecture that every later phase leans on | — |
| FC-P1.5 | pcap replay driver for the simulator: feed captured VAX frames to one simulated OVMX instance; assert emitted frames vs the reference joiner (shape + allowlist + seq contiguity). | R2 on the vax3 reference join for the START phase. | `tests/cluster/sim/replay.c`, `docs/clean-room/tools` decoders | P1.4 | S (medium) | — |
| FC-P1.6 | `vms_pe.c` glue for VCs + snapshot fields + `CLUSTER_DIAG_PORT` VC rows. | R4: 2 booted OVMX nodes (Linux, and NetBSD-VAX) form a VC executive↔executive. | `vms_pe.c`, snapshot, `tests/qemu/test_cluster_vc.sh` | P1.2, P0.9 | S (medium) | — |
| FC-P1.7 | R5: VC with VAX1 and VAX2 sustained ≥10 min (SDA `SHOW PORT` counters advance; `CLUSTER_DIAG_PORT` matches); tap bounce re-forms within TIMVCFAIL; both substrates. | R5. | lab harness | P1.6, P0.14, P0.15 | S (medium, lab) | LAB |
| FC-P1.9 | **VC loss-recovery correction** (design §3.2.5 ruling, E10): the receiver discards an out-of-order (ahead) sequenced frame, does not advance `recv_seq`, counts `rx_gaps`, and immediately re-sends the cumulative ack; the sender runs an ack-timeout retransmit ladder from the oldest unacked ring entry onward (same bytes, same `send_seq`, retransmit msgtype `0x7b`/`0x6b` per the codec), cadence/count seeded from spec §4(k)'s measured ladder and labeled OVMX design values; the VC breaks only on ladder exhaustion (`PE_VC_DOWN_RETRANSMIT_EXHAUSTED`, new), `TIMVCFAIL`, or listen timeout; `PE_VC_DOWN_SEQ_GAP` deleted; `vc_down(sysid, reason)` raised to the upper layer through `pe_ops`. | R1: table tests for gap/dup/next/ladder/exhaustion; R2: 3 nodes at 10 % per-link loss with 48 pipelined sends — all delivered in order, 0 VC breaks, retransmits > 0; 100 % one-way loss — exactly one break with reason `RETRANSMIT_EXHAUSTED` after the ladder, then re-formation. | `vms_pe_fsm.{c,h}` (VC tables, `vc_score_seq` consumers, ring/ladder), `vms_cluster_codec_vc` (retransmit msgtype stamp exists: `vms_scs_mark_retransmit`), `tests/cluster/sim/scenarios/vc_loss_*.c` | P1.2, P1.4 | **O5** (medium) — retransmit/ack-timeout semantics in the VC FSM; the ruling is exact but the ladder interacts with credit and the ring | — |

## P2 — SCS in the executive (outcome: live SCS connections with the VAXes; directory service both ways; credits accounted)

Design P2, §3.4 (SB/CDT).

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P2.1 | Codec: connect verbs op 0–10 (spec §4(h)/(m)), MTYPE envelope (inner length, format word, MTYPE, credit, handle pair), types 5/7/8/9, directory lookup request/response with per-name affirmative result. | R1 byte-exact on the specimens; `test_scs_connect*`/`test_scs_dir*` ported. | codec; harvest `scs_connect.{c,h}`, `scs_dir.{c,h}`, `scs_disc.{c,h}`, `scs_env.{c,h}` | P0.6 | S (medium) | — |
| FC-P2.2 | SCS FSM: SB per remote system, CDT connection ladder (OPEN→DISC SENT/RCVD→MATCH→CLOSED), Con.ID allocator (§4(t)), credit ledger per CDT (Send/Receive/Pending, special credit message 8 → 9), MTYPE dispatch to the CDT input routine; the CDL is live for data. **VC-break contract (design §3.2.5):** on `vc_down(sysid, reason)` every CDT on that SB → CLOSED (path-lost), credit ledgers discarded, pending sends fail `SS$_PATHLOST`, each SYSAP's `disconnected()` called; SCS never retries across a break or re-opens on its own; port retransmission is invisible (credit spent once per `scs_send_msg`). | R1: every ladder transition; credit conservation property test; the 8-before-DISCONNECT invariant; a `vc_down` test proving all CDTs close and no message is re-sent by SCS. | `vms_scs_fsm.{c,h}`, `vms_scs.h` | P0.1, P1.3, P1.9, P2.1 | **O5** (high) — the daemon's CDL/credit path was dead code because this is where the design judgment lives | — |
| FC-P2.3 | SYSAP registry (LISTEN/CONNECT/ACCEPT/SEND/RETURN-CREDIT) + `SCS$DIRECTORY` SYSAP: server lookups (HIT for registered names, `NOT PRESENT HERE`), client connect + lookup, transient-connection semantics. | R1 against the dir specimens; the established-join server-half sequence (§4(L)) replays. | `vms_scs_dir.c` (pure), registry in `vms_scs_fsm.c` | P2.2 | S (medium) | — |
| FC-P2.4 | `vms_scs.c` glue + snapshot + `CLUSTER_DIAG_CONN` (CDT rows byte-comparable with SDA `SHOW CONNECTIONS`). | R4: 2 OVMX nodes open directory connections and look each other up (both substrates). | `vms_scs.c`, snapshot, ioctl + mirror | P2.2, P2.3, P1.6 | S (medium) | — |
| FC-P2.5 | Simulator scenarios: connect/accept/disconnect, credit exhaustion, directory lookups across 3 nodes; pcap replay of the connect phase. | R2. | `tests/cluster/sim/scenarios/scs_*.c` | P1.4, P2.3 | S (medium) | — |
| FC-P2.6 | R5: VAX SDA `SHOW CONNECTIONS` lists OVMX CDTs `open` for `SCS$DIRECTORY`; OVMX's own lookup of `MSCP$DISK`/`VMS$VAXcluster` on VAX1 returns HIT; credit conservation over 1 min; both substrates. | R5. | lab harness | P2.4, P1.7 | S (medium, lab) | LAB |

## P3 — CNXMAN: a booted OVMX is a MEMBER (outcome: STARTUP.EXE joins in the executive before the sysdisk mount; VAX SDA shows OVMX's CSB `member,selected,status_rcvd` sustained; OVMX's SHOW CLUSTER/$GETSYI read the CLUB/CSBs; the daemon and its startup path are gone; the CLUSTER_NODES readout is logged)

Design P3, §3.4 (CLUB/CSB), §3.5, §3.7. Spec §4(j)/(o)/(p)/(q)/(r)/(y)/(aa).

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P3.1 | CM codec: the 190-byte `VMS$VAXcluster` body (txn, checksum handling as observed, epoch, role slot, class, bitmap), the cat-01 response recipes (`0x81` echo + three mutations, op 0x12/0x0f rules), cat-06 close with own node-parameter block, the cat-02 op-0d echo recipe, cat-04 acks; the grounded allowlist as codec data. | R1: the op-0d recipe reconstructs 1367/1367 specimens; each cat-01 recipe byte-exact on its specimens; allowlist table has a test that an unlisted pair yields "no response". | codec; harvest `scs_member.{c,h}`, `cm_response_shape` (origin/main `scsd.c:1549`), `tests/vmsscs/test_scs_member*.c` | P0.6 | S (high) — large but mechanical; the recipes are grounded | — |
| FC-P3.2 | LOCKDIRWT on the wire pinned (design §5.1): which PARAMS/CONFIG byte carries it; what VAX1/VAX2/VAX3 advertise. | R5 note in `docs/research-lockdirwt-field.md` with pcap diff and `SHOW CLUSTER ADD LOCKDIRWT` transcript. | lab | — | S (medium, lab) | LAB |
| FC-P3.3 | Join FSM (`vms_cnxman_join_fsm.c`): after START, own `SCS$DIRECTORY` connect at ss=1 → lookups → `MSCP$DISK` client connect (uses P3.4) → VC connect → MODEL/PARAMS/CONFIG burst on our VC; server half accepting members' inbound connects (total connectivity); CSID learned by matching own SCSSYSTEMID in membership records; honest identity + LOCKDIRWT=0 advertised; hand-off to the barrier FSM on XITGO. | R1 per transition; R2 replay of the vax3 reference join emits the reference sequence and reaches the barrier hand-off. | `vms_cnxman_join_fsm.{c,h}`, `vms_cnxman.h` | P0.1, P2.3, P3.1, P3.4 | **O5** (high) — the choreography that took the campaign three model inversions to ground | LAB (P3.2 for the LOCKDIRWT byte; until pinned, advertise the current builder value and log "unpinned") |
| FC-P3.4 | MSCP discovery client (SCC ×2 → GUS NEXT-UNIT walk → OFFLINE terminator) as codec + a small FSM, used by the join. | R1 byte-exact on the af2/vax3 specimens. | `vms_mscp_codec` section of the codec, `vms_mscp_cl_fsm.c` (discovery only) | P0.6 | S (medium) | — |
| FC-P3.5 | Barrier FSM, participant side (`vms_cnxman_barrier_fsm.c`): op-09 open (epoch, bitmap with width instrumentation), op-0a GO, 12 × (op-0b → ack → op-0c), op-0d interleaving via the DLM callback (echo in P3; real rebuild in P5), class ADD/REMOVE/DEPART participation, never answering 0x0a/0x0c, mismatch instrumentation above M=4. | R1 per step; R2 3-node and 8-node transitions with the coordinator FSM. | `vms_cnxman_barrier_fsm.{c,h}` | P0.1, P3.1 | **O5** (medium) — well grounded but the failure mode is "break the cluster" | — |
| FC-P3.6 | CLUB/CSB model + CSB ten-state machine + reconnect FSM (RECNXINTERVAL/TIMVCFAIL once-per-second loop, membership hold, transition proposal on expiry) + last-gasp emission on shutdown. | R1: `test_scs_recnx` ported on the injected clock; CSB state ladder tests. | `vms_cnxman_csb.c`, `vms_cnxman_recnx_fsm.c`, `vms_cluster.h` | P0.1 | **O5** (medium) | — |
| FC-P3.7 | Quorum arithmetic (tracking only): CEVOTES/QUORUM from advertised VOTES + local EXPECTED_VOTES, recomputed on transitions; `$GETSYI` CLUSTER_MEMBER/NODES/VOTES/QUORUM/FSYSID/FTIME/NODE_CSID projection from the CLUB. | R1: `test_scs_quorum` cases ported; projection unit test. | `vms_cnxman_quorum.c`, `vms_cluster_api.c` (`CLUSTER_GET_CLUB`), `sys$getsyi` cutover | P3.6 | S (medium) | — |
| FC-P3.8 | `vms_cnxman.c` glue: instantiates join/barrier/coord/recnx FSMs on the fork context; `$SETCLUEVT` delivery via `vms_ast.c`; `%CNXMAN`/`%VAXcluster` OPA0: lines; snapshot; `CLUSTER_DIAG_CSB`; `CLUSTER_MEMBER_GET` re-pointed at the CSB table (same struct). | R4: 3 booted OVMX nodes (with P3.12) form a cluster; SHOW CLUSTER on each lists the others; both substrates. | `vms_cnxman.c`, ioctls + mirrors, `tests/qemu/test_cluster_membership.sh` | P3.3, P3.5, P3.6, P3.7, P3.12, P2.4 | S (high) | — |
| FC-P3.9 | Boot integration + retirement: `CLUSTER_START` join semantics (returns MEMBER/STANDALONE; VAXCLUSTER 0/1/2; "waiting to form or join" on OPA0:), ordered before the sysdisk mount in STARTUP.EXE; SHOW CLUSTER + `$GETSYI` read the executive; **removed**: `SCS_STARTUP.COM`, `SCSD.EXE` staging, the VMS$VMS.DAT SCS component, `OVMX_*` cluster env gates, `CLUSTER_MEMBER_SET/CLEAR` + kif wrappers, `vms_local_csid`/`dlm_member_csids` module params, the `/var/run/ovmx/cluster_state` file publish, `vms_l2` from the cluster path; `src/vmsscs/` deleted after its tests are ported (P0.7–P3.1). | R4: negctl (VAXCLUSTER=0 ⇒ NOTMEMBER `SS$_NORMAL`; no `/dev/vms` ⇒ `SS$_NOSUCHDEV`); kif census green; compat register rows updated; no `scsd` binary in the image. | `ovmx_init.c`, `distro/rootfs/...`, `dcl_cmd_show.c`, `vms_module.c`, `vms_ioctl.h`, `docs/compat/facilities/cluster-*.yaml` | P3.8 | S (high) — wide but mechanical; teeth via negctl | — |
| FC-P3.10 | Simulator scenarios: 3–8-node join/barrier/departure loops; pcap replay of the full vax3 join asserting OVMX's emitted sequence through `op 0x0c`#12. | R2. | `tests/cluster/sim/scenarios/cnxman_*.c` | P3.3, P3.5, P3.12, P1.5 | S (medium) | — |
| FC-P3.11 | Cluster-clone lab harness for booted nodes: fresh `vaxlab-*` pod from the golden disk, boot OVMX (Linux or NetBSD-VAX) on `br0`, SDA CSB poller, console `F$GETSYI("CLUSTER_NODES")` poller at 1 s, pcap, one `grade.py` producing the pass/fail table. | R5 tooling: one command runs a graded join. | `tests/lab/tools/` (extend `labjoin_*`, `run_db20b.sh` lineage) | P0.14 | S (medium, lab) | LAB |
| FC-P3.12 | Coordinator FSM, ADD class (`vms_cnxman_coord_fsm.c`): op-02 receipt → op-12 relay to members → op-03/05 → op-09 open (epoch, bitmap) → op-0a GO → `12×(M−1)` barrier driver holding step N until all report → op-0d rebuild push hook (DLM callback; echo-only until P5). Selection: drive only when this node received the op-02 or detected the event and no other CM has opened a transition; defer on collision. | R1 per step; R2 an all-OVMX 3-node cluster forms and adds a 4th. | `vms_cnxman_coord_fsm.{c,h}` | P0.1, P3.1, P3.5 | **O5** (high) — coordinator obligations are the `12×(M−1)` law; the selection predicate is INFERRED (§5.5) | — |
| FC-P3.13 | R5, Linux: booted OVMX joins the clone VAX cluster — SDA CSB member sustained ≥3 min, 3 fresh runs, 0 reformations; OVMX SHOW CLUSTER lists VAX1/VAX2 from the executive; CLUSTER_NODES readout logged at T+15/60/180 s on both consoles (the Q2 observation). | R5 graded by P3.11. | lab | P3.9, P3.11, P2.6 | S (medium, lab) | LAB, Q2 (observation, not a blocker) |
| FC-P3.14 | R5, NetBSD-VAX: the same join from the rail's booted node. | R5. | lab | P3.13 | S (medium, lab) | LAB |
| FC-P3.15 | **Seam buffer-granularity conformance** (design §3.2.4 ruling, E1): the barrier FSM emits 132-byte SYSAP bodies through `cnxman_ops.send/respond` after `cnxman_envelope_stamp(csb, body, is_response)` (new pure function on the CSB's `send_msg`/`ack_msg`/`txn` dialogue state; responses echo `txn`/`token`); `cnxman_barrier_link_ops.next_out` deleted; `vms_cm_link` moved to `tests/cluster/host/` as a test-only frame composer (`vms_frame_compose`) until P1.1/P2.1's builders replace it; `cnxman_ops`/`scs_ops`/`scs_sysap_ops.message` doc-comments made exact per §3.2.4 (body = frame+72; SCS owns 56–71; port owns 0–55; `pe_send_msg` takes a service kind that selects msgtype); barrier rung-1 fixtures sliced to `specimen[72:204]`. | R1: all existing barrier byte-exact assertions pass on bodies; a new test proves no CNXMAN TU writes any byte below body offset 0 (grep + a composer round-trip); `cluster_sim` replay unchanged in outcome. | `vms_cnxman.h`, `vms_cnxman_barrier_fsm.{c,h}`, `vms_cnxman_csb.{c,h}` (stamper), `vms_cluster_codec_cm.{c,h}`, `vms_scs.h`, `vms_pe.h`, `tests/cluster/host/` | P3.5 | S (medium) — the contract is exact; mechanical re-plumbing | — |

## P4 — DLM requester role (outcome: an OVMX `$ENQ` on a non-locally-mastered root resource is routed to the right directory node, granted by the master, and visible in the VAX's lock database with OVMX's CSID; DEQ/convert/BLKAST/LVB cross the wire; the standing `F11B$v` mount lock is OVMX's first real registration)

Design P4, §3.6 (D-DLM-1/2/4/5), §5.2. `vms_lock.c` is the engine; read `dlm_resolve_master` (`vms_lock.c:536-564`), `vms_lock_dlm_xnode_dispatch` (`:2529-2646`), `grant_recv` (`:2236-2306`).

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P4.1 | IDSM-DIR: the lock-management chapter of *VAX/VMS Internals and Data Structures* (V5.2; Alpha edition cross-check) transcribed host-only with page cites; a report answering: vector construction (entries per weighted node, member order, rebuild timing), hash input, and whether the hash function is given bit-level; recommendation rung A or B. | A note `docs/research-dlm-directory-algorithm.md` (page cites only; no transcript in git). | host-only transcript area (same regime as Davis) | — | **O5** (medium) — judging what the book actually specifies vs implies | DOC (operator supplies the book) |
| FC-P4.2 | Directory conformance + mis-addressing capture (design §5.2(2)): clone cluster with LOCKDIRWT VAX1=1/VAX2=1 (then 2), VAX3 as requester; ~100 root names (`$ OPEN` files → `RMS$…`, a `MOUNT/CLUSTER` label) → observed directory node per name; one deliberately mis-addressed lookup → reply class + SDA `SHOW RESOURCE` on the wrong node. | R5 dataset `docs/research-dlm-directory-observed.csv` + note; pcap retained per clean-room procedure. | lab | P3.2, P3.11 | S (medium, lab) | LAB |
| FC-P4.3 | `dir_resolve(name) → csid` implemented per the ladder: rung A from the book (replacing `exec_jhash`, `vms_lock.c:527`) with conformance test = zero residuals on P4.2's dataset; or rung B probe-and-cache with per-RSB `dir_csid` and invalidation on every transition class; `exec_jhash` deleted. Rung C escalated with evidence if both fail. | R1 conformance/probe tests; R2 with a simulated weighted set. | `vms_lock.c` (`dlm_directory_csid` → `dir_resolve`), `vms_dlm_scs.h` | P4.1, P4.2 | **O5** (high) — the Rule-8 boundary lives in this function | DOC, LAB, OP (rung C only) |
| FC-P4.4 | Proxy LKB: the requester-side image of a remote-mastered lock is an LKB with `proxy` flag, `master_csid`, `master_lkid`; `vms_dlm_origin` list removed; `$GETLKI`, `$DEQ`, convert, BLKAST delivery, LVB all operate on it; `dlm_resolve_master` cases 2/3 post a request and sleep on the LKB cv; idempotent retransmit keyed `(req_csid, req_lkid)` (salvage from `feat/coord-rebuild-completion`). | R1 on the host backend (P4.9) + existing `test_syssvc_dlm_xnode` subtests still green on R4 both substrates. | `vms_lock.c`, `vms_internal.h` (both), `vms_lock_nb.h` | P0.1, P4.9 | **O5** (high) — lock-engine surgery with cluster-wide correctness consequences | — |
| FC-P4.5 | cat-02 codec: op-01 ENQ/lookup, op-07 convert, op-03/op-04 completion/commit, cat-82 grant parse, BLKAST/LVB fields, op-0d record parse — every field a typed accessor bound to an LKB/RSB field name; the named-vs-empty-resource layouts; fixtures = the db20-b granted frames + the reference join. | R1 byte-exact; a test that no builder accepts a literal lock id. | codec; harvest `scs_dlm.{c,h}`, memory `cluster-promotion-gap` field maps | P0.6 | S (medium) | LAB (P5.1 may re-map op semantics; the table is data) |
| FC-P4.6 | DLM requester FSM (`vms_dlm_scs_fsm.c`): lookup → enqueue → grant → completion/commit per root resource; inbound BLKAST → local AST; convert/DEQ; LVB write-back; retransmit; declines/retries from the master; every outbound field read from the proxy LKB. | R1 per transition; R2 requester vs simulated master/directory (P4.9 backend). | `vms_dlm_scs_fsm.{c,h}` | P4.3, P4.4, P4.5 | **O5** (high) | — |
| FC-P4.7 | The ODS-2 ACP holds a standing `F11B$v<label>` lock for the life of a mount (salvage the `vmsfs_acp.c` delta from `feat/coord-rebuild-completion`) + `DLM_ENUM_STANDING` readback. | R4: after MOUNT the enum lists the lock; DISMOUNT releases it; both substrates. | `src/kernel-core/vmsfs_acp.c`, ioctl + mirror | — | S (medium) | — |
| FC-P4.8 | `vms_dlm_scs.c` glue + `CLUSTER_DIAG_LOCK` (RSB/LKB rows with CSIDs, proxy flag, master handle). | R4: 2 OVMX nodes — a `$ENQ` on node B for a resource mastered on A is granted over the wire (replaces the ioctl-driven H-rung harness), both substrates. | `vms_dlm_scs.c`, ioctl + mirror, `tests/qemu/test_cluster_dlm_wire.sh` | P4.6, P3.8 | S (medium) | — |
| FC-P4.9 | Host backend for `vms_lock.c`: `exec_kbackend_host.h` (pthreads/malloc) sufficient to compile `vms_lock.c` and the FSMs on the host, so the simulator runs the real lock engine. | R1/R2: `vms_lock.c` links into `cluster_sim`; existing lock unit semantics reproduced on one host test. | `tests/cluster/host/exec_kbackend_host.h` | P0.6 | S (medium) | — |
| FC-P4.10 | Simulator DLM scenarios: N-node requester/master/directory with the real engine; contention, BLKAST, LVB, convert, retransmit under loss. | R2. | `tests/cluster/sim/scenarios/dlm_*.c` | P4.6, P4.9 | S (medium) | — |
| FC-P4.11 | R5: VAX1 SDA `SHOW LOCK/ALL` lists a lock with OVMX's CSID on a resource OVMX requested; a VAX holder blocks it and BLKAST fires on OVMX; 10-minute lock loop at 10/s with 0 reformations; both substrates. | R5 graded. | lab | P4.8, P3.13 | S (medium, lab) | LAB |

## P5 — DLM master, directory node, rebuild (outcome: VAX requests for OVMX-mastered resources are granted from real LKBs; OVMX serves directory duty when assigned; transitions drive a real rebuild/remaster; membership holds under sustained real lock load; CLUSTER_NODES=3 if the count is DLM-gated)

Design P5, §3.6 (D-DLM-3/4), §5.4.

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P5.1 | The console-correlated 3rd-VAX join capture (= vms-9c7, design §5.4): full-disk vax3 joins the clone cluster twice (LOCKDIRWT 0 and 1); pcap + 1 s `CLUSTER_NODES` poll on VAX1; per-opcode direction × response-bit table; the last frame class before the count flip. | R5 dataset + `docs/research-count-commit-trigger.md`. | lab (vax3 bed) | P3.11 | S (high, lab) — long, procedural, corruption-risk discipline | LAB, Q2 |
| FC-P5.2 | Opcode → operation semantic table finalized from P5.1 (op-01 lookup vs enqueue; op-03/04/07/12/15; op-0d assignment vs broadcast); codec table updated; design §3.6 addendum. | R1: the codec table test vectors re-labelled; sim replay of the vax3 join reproduces the joiner's DLM sequence. | codec table, `docs/design-faithful-cluster-executive.md` | P5.1, P4.5 | **O5** (medium) — interpretation of the capture | LAB, Q2 |
| FC-P5.3 | Master role on the wire: inbound lookup/ENQ/convert/DEQ for a resource this node masters → `vms_lock_dlm_xnode_dispatch` (direct call) → reply built from the resulting LKB (mode from the granted queue, master handle = lkid, LVB) on the request's transaction; decline for unassigned resources; idempotent on retransmit. | R1; R2 simulated VAX requester; R4 2-node executive. | `vms_dlm_scs_fsm.c` (master tables), `vms_dlm_scs.c` | P4.6, P5.2 | **O5** (high) | Q2 |
| FC-P5.4 | Directory-node role: stored directory table (name → master CSID, holder count); lookup service ("master is X" / "you master it"); entries created from rebuild records and served lookups; never self-assigned. | R1; R2 with a simulated LOCKDIRWT>0 OVMX. | `vms_lock.c` (directory table), `vms_dlm_scs_fsm.c` | P5.2, P4.3 | **O5** (medium) | — |
| FC-P5.5 | Rebuild FSM (`vms_dlm_rebuild_fsm.c`): freeze lock activity on transition start → merge / directory / partial / full per class and joiner LOCKDIRWT → send this node's records for mastered resources to their directory nodes → consume inbound records → thaw; remaster on departure (reuse the DEPART invalidation); invoked by CNXMAN transition callbacks and interleaved with the barrier (spec §4(p) step 5). | R1 per phase; R2 join/leave loops with locks held across transitions and no lost/duplicated grants. | `vms_dlm_rebuild_fsm.{c,h}`, `vms_cnxman.h` callbacks | P5.3, P5.4, P3.5, P3.12 | **O5** (high) — the data-integrity core of the whole stack | Q2 |
| FC-P5.6 | Distributed deadlock search legs in the executive per `docs/design-dlm-distributed-deadlock.md` (probe/victim ops over the DLM SYSAP; global-min victim). | R1; R2 2-node cycle → exactly one `SS$_DEADLOCK`, concurrent initiation still one. | `vms_dlm_scs_fsm.c`, `vms_lock.c` (existing victim path) | P5.3 | S (high) — the design is written; implementation is careful but specified | — |
| FC-P5.7 | Simulator scenarios: master/directory/rebuild with departures mid-rebuild; deadlock cycles; 8 nodes. | R2. | `tests/cluster/sim/scenarios/dlm_master_*.c` | P5.5, P5.6 | S (medium) | — |
| FC-P5.8 | R4: 3 OVMX nodes with cross-mastered locks; kill one; remaster; locks intact; both substrates. | R4. | `tests/qemu/test_cluster_rebuild.sh` | P5.5 | S (medium) | — |
| FC-P5.9 | R5: VAX SDA shows a VAX lock mastered on OVMX; kill VAX2 ⇒ class-03 transition, remaster, VAX1's locks intact; 10-minute mixed load (VAX loop + OVMX RMS loop) with 0 reformations and 0 retry storms; CLUSTER_NODES readout. | R5 graded. | lab | P5.8, P4.11 | S (medium, lab) | LAB, Q2 |

## P6 — MSCP server in the executive (outcome: a VAX MOUNTs an OVMX-served ODS-2 volume and reads/writes a marker file)

Design P6; `docs/design-mscp-direction.md` for lengths and the block-transfer header.

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P6.1 | Port block-transfer service: named buffers, the 28-byte block header (`+4/+6` copied from observation, flagged), READ streaming with the final chunk piggybacked on the end message, WRITE two-frame form. | R1 byte-exact on the vms291 mount capture frames. | `vms_pe_fsm.c` (block transfer), `vms_pe.h`, codec | P1.3 | **O5** (medium) — two header fields are ungrounded; needs judgment not to invent | — |
| FC-P6.2 | MSCP codec: SCC/GUS/ONLINE/READ/WRITE commands and end messages at the measured lengths (28/44/32/36/52), status major/sub split, `P.UNFL` echo rules. | R1: `test_scs_mscp_srv` vectors ported; mutants battery re-pointed. | codec; harvest `scs_mscp_srv.{c,h}`, `scs_mscp.h` | P0.6 | S (medium) | — |
| FC-P6.3 | `vms_mscp_srv.c`: serves executive volumes via `exec_blockdev_*`/the ACP as `$ALLOCLASS$DUAn`; UQB/HQB/HRB; `MSCP$DISK` registered only when a serveable unit exists; MSCP_SERVE_ALL/MSCP_LOAD; write-protect honest on read-only volumes. | R4: OVMX node B mounts A's served volume (needs P7.1 for the client) — or R4-lite: a simulated class driver in `cluster_sim` runs the mount-verify sequence. | `vms_mscp_srv.{c,h}`, SYSAP registration in `vms_scs` | P6.1, P6.2, P2.4, P4.7 | **O5** (medium) | — |
| FC-P6.4 | R5: `%MOUNT-I-MOUNTED` on the VAX; marker round-trip; pcap `ONLINE→GUS→READ→block transfer` from OVMX; ONLINE-END measured from the booted node and its stub row closed. | R5 graded; both substrates. | lab | P6.3, P3.13 | S (medium, lab) | LAB |
| FC-P6.5 | **Port REQUEST DATA responder** (design §3.2.6 E41): `vms_pe`'s block-transfer service answers a header-only block frame naming a locally registered named buffer as source by transmitting that buffer under the same header (`+4`/`+6` echoed verbatim, `+8` counting down, READ's chunking); unknown buffer ⇒ drop + `blk_req_unknown_buffer`; class driver registers the WRITE buffer before the command and withdraws at end-message; server WRITE path issues REQUEST DATA and completes at `+8 == 0`; `writes_undelivered` reaches 0. | R1: responder output byte-exact vs the vms291 WRITE pair; R2: 2 simulated nodes complete a WRITE; R4: OVMX↔OVMX write round-trip; R5: VAX writes a marker onto an OVMX-served volume. | `vms_pe_fsm.c` (block service), `vms_mscp_cl.c`, `vms_mscp_srv.c`, `codec_mscp` | P6.1, P6.3, P7.1 | S (medium) — the choreography is ruled and captured | — |
| FC-P6.6 | **MSCP server I/O off the fork thread** (design §3.2.6 E42 corollary): served-unit `exec_blockdev_*` calls move from the fork work handler (`vms_mscp_srv.c:219/239`) to a served-I/O worker kthread (§15) that posts completions back to the fork queue; the fork thread builds end messages / SEND DATA from completions; CI grep gate: no `exec_blockdev_` symbol in any fork-context path. | R1: server FSM tests with a fake worker; R4: HELLO cadence jitter under a served READ loop stays < 10 ms on both substrates. | `vms_mscp_srv.c`, `vms_cluster_fork.h` (worker post), `tools/ci/cluster_core_includes_gate.sh` | P6.3 | S (medium) | — |
| FC-P6.7 | **Allocation-class carrier capture** (design §3.2.6 E40, lab lane): change VAX1's `ALLOCLASS` on a clone, reboot, diff its SCC end parameter area (the `0x0547` word), `MSCP$DISK` connect data and CM PARAMS against the current capture; confirm with `SHOW DEVICE/FULL` on VAX2; report the carrier in `docs/research-mscp-alloclass-carrier.md`. | R5 note + pcap pair retained per clean-room procedure. | lab | P3.11 | S (medium, lab) | LAB |

## P7 — Disk class driver (outcome: OVMX mounts a VAX-served disk; cluster-wide file access contends both ways)

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P7.1 | `vms_mscp_cl.c`: served units discovered (P3.4 walk) become `$n$DUAn` devices in `vms_devtab` with `DVI$_MSCP_SERVED`; block read/write through named-buffer transfers; controller timeout handling. | R4: 2 OVMX nodes — B mounts A's served volume and reads a file. | `vms_mscp_cl.{c,h}`, `vms_devtab.c`, ACP block-device seam | P6.1, P6.3, P3.4 | **O5** (medium) | — |
| FC-P7.2 | **ACP ↔ served-disk bridge + cluster-wide file access** (design §3.2.6 E42 contract): served units' `vms_devtab` block ops `srvdisk_read_block/_write_block` run in the ACP caller's process context — pool-allocated `vms_srvdisk_irp {lk, cv, done, status}`, named-buffer registration, `vms_mscp_cl_read/_write` posting to the fork queue, `exec_cv_wait_timeout` loop on the IRP's own lock with the server's advertised `P.CTMO` + margin, `SS$_TIMEOUT`/`SS$_PATHLOST` honest failures, cancel work item on timeout; `srvdisk_done` on the fork thread sets status + broadcasts under `irp->lk` (leaf under the fork mutex); the fork thread never waits on an IRP. Then an RMS open on a served volume takes the `F11B$`/`RMS$` locks through P4/P5 and blocks against a VAX holder. | R1: bridge unit test with a fake completion (timeout, path-lost, success ordering); R4: two OVMX nodes — B mounts A's served volume through the ACP and reads/writes a file; R5: `RMS-E-FLK` both directions + SDA shows both CSIDs on the file lock. | `vms_devtab.c` (served-unit block ops), `vms_mscp_cl.{c,h}`, `vmsfs_acp.c` lock paths (already real), test scripts | P7.1, P6.5, P5.9 | **O5** (medium) — the wait discipline interacts with the fork mutex, the rxlock and the ACP's own locks | LAB (R5 leg) |
| FC-P7.3 | `$n$DUAn` spelling from the grounded allocation-class carrier: populate `cddb->alloclass` from the field FC-P6.7 pinned; `<SCSNODE>$DUAn:` remains the rendering for class 0 (VMS behaviour); `alloclass_absent` counts only a server whose carrier is missing. | R1 codec/naming test; R5 `SHOW DEVICE` on OVMX lists `$2$DUA0:` for VAX1's served unit. | `vms_mscp_cl.c`, `vms_devtab.c`, codec field accessor | P6.7, P7.1 | S (low) | LAB (P6.7) |

## P8 — Quorum enforcement, coordinator completeness, departures (outcome: OVMX votes; quorum loss suspends/resumes; OVMX coordinates ADD/REMOVE/DEPART; quorum disk)

| ID | Outcome | Done-condition | Files / interfaces | Blocked-by | Caliber | Gates |
|---|---|---|---|---|---|---|
| FC-P8.1 | Quorum enforcement: on quorum loss the executive suspends its gated seams (process scheduling hooks in `vms_proctab`, AST delivery, ACP I/O issue) with `%CNXMAN, quorum lost, blocking activity`; resumes on regain; documented as the executive-owned subset. | R2 partition scenarios; R4 3-node partition. | `vms_cnxman_quorum.c`, `vms_proctab.c`, `vms_ast.c`, `vmsfs_acp.c` seams | P3.7, P5.5 | **O5** (high) | — |
| FC-P8.2 | REMOVE (class 03) and self-DEPART (class 04) transitions as coordinator and participant; failure detection (VC loss → RECNX expiry → propose); last-gasp datagram on shutdown/bugcheck. | R2; R4 kill/shutdown scenarios both substrates. | `vms_cnxman_coord_fsm.c`, `vms_cnxman_recnx_fsm.c` | P3.12, P3.6, P5.5 | **O5** (medium) | — |
| FC-P8.3 | Coordinator-selection predicate grounded (design §5.5): vary SCSSYSTEMID order/join order/kills on the 3-node clone; record who drives. | R5 note `docs/research-coordinator-selection.md`. | lab | P5.1 | S (medium, lab) | LAB |
| FC-P8.4 | Quorum disk: QDSKVOTES/DISK_QUORUM watcher via the block seam. **Blocked on grounding the quorum-disk on-disk protocol (QUORUM.DAT) from a lab capture; do not invent.** | R5 after a capture of a real quorum-disk cluster. | `vms_cnxman_qdisk.c` | P8.1 | **O5** (medium) | LAB |
| FC-P8.5 | R5: VOTES=1 changes CL_VOTES/CL_QUORUM on the VAX; partition test on the clone; OVMX coordinates vax3's ADD; clean shutdown emits DEPART and the VAXes log the removal without a barrier. | R5 graded. | lab | P8.1, P8.2, P8.3 | S (medium, lab) | LAB |

## P9 — Cluster-wide services (decompose when P5 lands)

Candidates, all `part-of` P9: `$SETCLUEVT` completeness + `$GETSYI` full CLUSTER_* set (S); cluster time / SYSMAN SET TIME semantics (S); `LNM$CWLOGICALS` cluster-wide logical names over the DLM (O5 — cache coherence protocol via LVB); SYSMAN cluster-wide DCL (S).

---

## Dependency DAG — wave view

Waves are sets of items with no unmet `blocked-by` among each other; everything in a wave can be dispatched in parallel. Lab (R5) items serialize on the single lab and are shown as their own lane.

```
WAVE 0  (start immediately; no dependencies)
  O5: P0.1 seam+interfaces      O5: P0.6 codec foundation
  S : P0.3 NetBSD spike         S : P0.10 SYSGEN load*        S : P0.12 NetBSD DLM parity
  S : P4.7 standing F11B$v lock (branch salvage)
  O5: P4.1 IDSM-DIR (DOC gate)
  LAB lane: P3.2 LOCKDIRWT capture → P5.1 vax3 count-commit capture (vms-9c7) → P4.2 directory capture
            (* P0.10 needs only the ioctl header conventions; start with P0.1's header stubbed)

WAVE 1  (after P0.1 and/or P0.6)
  S : P0.2 Linux binding  S : P0.4 NetBSD binding (after P0.3)   O5: P0.5 fork context
  S : codec harvests in parallel: P0.7 HELLO · P1.1 VC · P2.1 SCS · P3.1 CM · P4.5 DLM · P6.2 MSCP · P3.4 MSCP discovery
  S : P4.9 host backend for vms_lock.c
  O5: P3.6 CSB/CLUB/recnx (needs only P0.1)

WAVE 2  (FSMs — all against the frozen interfaces; parallel)
  O5: P0.8 channel FSM → O5: P1.2 VC FSM → S: P1.3 services
  O5: P1.4 simulator core (needs P1.2 for its first scenario; scaffolding can start in wave 1)
  O5: P2.2 SCS FSM (needs P1.3 header only)  S: P2.3 dir SYSAP
  O5: P3.3 join FSM   O5: P3.5 barrier FSM   O5: P3.12 coordinator FSM   S: P3.7 quorum arithmetic
  O5: P4.4 proxy LKB (needs P4.9)   O5: P4.3 dir_resolve (needs P4.1 + P4.2 — may slip to wave 3)
  S : P0.13 credential experiment (LAB lane, needs P0.9 build → effectively wave 3)

WAVE 3  (glue + simulator scenarios + R4 harnesses)
  S : P0.9 pe glue → P0.11 cluster start → P0.14/P0.15 (LAB)
  S : P1.5 pcap replay · P1.6 VC glue · P2.4 SCS glue · P2.5 sim scenarios
  S : P3.8 CNXMAN glue · P3.10 sim scenarios · P3.11 lab harness for booted nodes
  O5: P4.6 DLM requester FSM · S: P4.8 glue · S: P4.10 sim scenarios
  O5: P6.1 block transfer

WAVE 4  (integration + retirement + R5 proofs, lab-serialized)
  S : P3.9 boot integration + retirement (big, mechanical)
  LAB: P1.7 → P2.6 → P3.13 (Linux join, Q2 readout) → P3.14 (NetBSD-VAX join) → P4.11
  O5: P5.2 opcode semantics (after P5.1) → O5: P5.3 master role · O5: P5.4 directory role → O5: P5.5 rebuild FSM
  S : P5.6 deadlock legs · O5: P6.3 MSCP server · O5: P7.1 class driver

WAVE 5
  S : P5.7 / P5.8 · LAB: P5.9 (load + CN=3) → P6.4 → P7.2 · S: P7.2
  O5: P8.1 quorum enforcement · O5: P8.2 REMOVE/DEPART · LAB: P8.3 → P8.5 · O5: P8.4 (LAB-gated)
```

**Critical path** (longest chain, host-side; lab proofs hang off it):
`P0.1 → P0.8 → P1.2 → P1.4 → P2.2 → P3.3/P3.5/P3.12 (parallel) → P3.10 → P3.8 → P3.9 → P3.13`.
Then `P4.1+P4.2 → P4.3 → P4.6 → P4.8 → P4.11 → P5.2 → P5.3/P5.4 → P5.5 → P5.9`.
Items off the critical path that most shorten it if started early: **P1.4**
(simulator — everything from P2 on tests against it), **P4.1** (the book,
wave 0), **P5.1** (the vax3 capture, wave 0 on the lab lane — it de-risks
P4.5/P5.2 and answers Q2 while the host work proceeds).

**Parallelism budget.** Wave 0: 8 items (3 O5). Wave 1: ~11 (2 O5). Wave 2:
~12 (9 O5). Wave 3: ~12 (2 O5). The O5 load peaks in wave 2 (the FSMs); if
O5 capacity is the constraint, order them P1.2 → P2.2 → P3.3 → P3.5/P3.12
→ P3.6 → P4.4, with P0.8 and P1.4 first.

## Gates summary

| Gate | Items blocked | What unblocks |
|---|---|---|
| LAB — LOCKDIRWT field (P3.2) | P3.3's honest advertisement (soft: log "unpinned" meanwhile) | one lab session |
| LAB — vax3 count-commit capture (P5.1, = vms-9c7) | P5.2 → P5.3/P5.4/P5.5; Q2 | days (full-disk vax3 root on a clone) |
| LAB — directory conformance/mis-addressing (P4.2) | P4.3 rung selection | one lab session; needs P3.2 |
| LAB — credential (P0.13) | P0.14/P0.15 shipping form of the HELLO token | one lab session |
| DOC — IDSM-DIR (P4.1) | P4.3 | operator supplies the published book |
| OP — directory rung C | P4.3 only if rungs A and B both fail | operator ruling with P4.1+P4.2 evidence |
| OP (conditional) — credential | P0.14 if only the replayed token is admitted | operator ruling |
| LAB — quorum-disk protocol (P8.4) | P8.4 | a quorum-disk cluster capture |

## Item count

P0 16 · P1 8 · P2 6 · P3 15 · P4 11 · P5 9 · P6 7 · P7 3 · P8 5 · P9 (4 candidates) — **80 items + 4 candidates**. Opus-5-tier: 27; Sonnet-tier: 53 (of which 17 are lab-lane).

Standing seam rulings that bind the glue items (P0.9, P1.3, P2.2, P2.4, P3.8): design §3.2.3 (receive-level lock — `exec_rxlock_t`, one object), §3.2.4 (buffer granularity — body-level at every SYSAP seam; port owns 0–55, SCS 56–71, SYSAP 72+; `pe_send_msg` takes a service kind; `cnxman_envelope_stamp` is the only writer of body[0:8]), and §3.2.5 (VC loss recovery — the port retransmits, go-back-N, break only on ladder exhaustion/TIMVCFAIL/listen timeout; a VC break closes every CDT and reaches the SYSAP; CNXMAN's recnx FSM reconnects).
