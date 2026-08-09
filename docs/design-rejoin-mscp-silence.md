# Diagnosis — the JS_MSCP_CONNECT "silence" is the Rule of Total Connectivity (vms-694)

**Status:** GROUNDED DIAGNOSIS + bounded proposed fix. No wire-visible change shipped
in this branch (the fix needs a live-lab bracket to satisfy guardrail 23; see §6).

**Question owned:** After OVMX is admitted to a running VMScluster, the join/rejoin
path stalls at `JS_MSCP_CONNECT` with the peer answering **neither ACCEPT nor
REJECT** — outright silence — which is distinct from the op-4 REJECT storm already
handled by the retx count/cadence work (#211/#215/#217) and from the cross-peer
Con.ID collision fixed by #221.

---

## 1. Root cause (grounded three ways)

**The silence is the coordinator ABANDONING the state transition because OVMX has
not satisfied the Rule of Total Connectivity — cluster admission is BIDIRECTIONAL,
and OVMX only drives the outbound half.**

### 1a. Doc grounding — Davis, *VAXcluster Principles* (1993), ch. 7 JOIN CLUSTER

- **p. 7-37** (`ch7-part03.md:83`): a joiner "will attempt to join the existing
  cluster only after the number of cluster members it **has connectivity with**
  equals that count."
- **p. 7-39, the "Rule of Total Connectivity"** (`ch7-part03.md:133,137,139`):
  VAX_A (coordinator) "describes VAX_B to each of the current VAXcluster members,
  and they respond by indicating whether or not they have connectivity with
  VAX_B… **VAX_B must report having connectivity with all the current cluster
  members, and they must all report having connectivity with VAX_B. Otherwise,
  VAX_A abandons the state transition at this point.**"
- **What "connectivity" means** (`ch7-part02.md:362`): "An SCS connection exists
  (i.e., the local Connection Manager has connectivity) with the remote." So a
  member "has connectivity with VAX_B" **only if its own SCS connection TO VAX_B
  is established** — which requires VAX_B (OVMX) to have **accepted** that
  member's inbound connect.
- **Why the peer is SILENT, not a REJECT**: "abandons the state transition" is the
  coordinator ceasing to drive the transaction. The SCS connection layer's own
  reject (CONNECT_RSP / REJECT_REQ, Davis Fig. 2-15) is explicit and always
  emitted over an open VC; the *connection-manager* abandonment above emits
  nothing. That is the difference between the op-4 reject storm and this silence.

### 1b. Code grounding — OVMX drives only the outbound half

`src/vmsscs/scsd.c:10811-10820` already states the symptom in-tree:

> "[`OVMX_JOIN_SEQ`] drives OVMX cleanly through 6 of 8 steps … but **STALLS at
> the MSCP$DISK connect on an ESTABLISHED cluster: the established member opens
> its own dir probe to resolve OVMX and does not process the (byte-perfect) MSCP
> connect** — its admission gating differs from the fresh-formation reference."

The join sequencer (`enum join_step`, `scsd.c:702`) is entirely OVMX→member:
`JS_DIR_CONNECT → JS_DIR_LOOKUP_* → JS_MSCP_CONNECT → JS_VC_CONNECT →
JS_ADD_MEMBER`. Nothing in it accepts the **member's inbound** SCS$DIRECTORY or
MSCP$DISK connect to OVMX. The MSCP disk-server accept machinery exists
(`scsd_mscp_srv_*`, wired by vms-34b, `scsd.c:376` `ovmx_mscp_server_enabled()`
default ON, `SDIR listening=3 [MSCP$DISK LISTEN]`) but is **not driven to emit an
accept** on the join path.

### 1c. Live grounding — the members' inbound connects to OVMX are never accepted

Even the best pre-#221 established-join run that reached `add-member`
(`/data/training/vax/k8s-labs/vaxlab-0/logs/scsd-vms694memb1.log`, `OVMX_JOIN_SEQ=1
OVMX_CFG2_PEER=1`) shows, in its own SUMMARY:

```
MSCP-SERVER-ACCEPTS-SENT=0
SDIR listening=3 [... MSCP$DISK ... LISTEN] scans=6 hits=4 connect_scans=2
      no-such-sysap-sent=0 ... enabled=YES
RX-CDL: app-messages=30 delivered=4 no-cdt=22 src-mismatch=4 ...
```

OVMX's outbound half completed (`SCSD-I-MSCPBOUND` ×2, `SCSD-I-CMCONFIG`
add-member, `SCSD-I-CMCONFIG2`), but `connect_scans=2` with
`MSCP-SERVER-ACCEPTS-SENT=0` and `no-such-sysap-sent=0` means **the members DID
open inbound connects to OVMX's MSCP$DISK server, OVMX FOUND the SYSAP in its
listen queue, and yet sent zero accepts**; `no-cdt=22` are the members' follow-on
messages dropped because no server CDT was ever allocated. The members therefore
never had connectivity WITH OVMX, so under the Rule of Total Connectivity VAX_A
abandons — silence.

**Peer-side SDA corroboration** (real VAX `SHOW CONNECTIONS/NODE=`,
`/data/training/vax/cluster/work/{w1C,w4B}.conn`, the peer's own CDT for OVMX):

```
State: 0007 con_sent  Blocked: 0001 con_pend  Local: VMS$DISK_CL_DRVR
       Remote Node::Process: OVMXW1::MSCP$DISK   Remote Con.ID 00000000
State: 0005 disc_sent  Blocked: 0004 disc_pend  Local: SCS$DIR_LOOKUP
       Remote Node::Process: OVMXW1::SCS$DIRECTORY
```

The member (VMS$DISK_CL_DRVR) has an **unanswered** connect to `OVMX::MSCP$DISK`
parked in `con_pend`, remote Con.ID never bound — the exact "no connectivity with
VAX_B" state. Contrast the SUCCESS run `w3A.conn` (`OVMX_PURE_SERVER=1`), where the
member's inbound connects ARE accepted and every CDT reads `0002 open`:

```
State: 0002 open  Local: MSCP$DISK      Remote: OVMXW3::VMS$DISK_CL_DRVR  (nonzero Con.ID)
State: 0002 open  Local: SCS$DIRECTORY  Remote: OVMXW3::SCS$DIR_LOOKUP
State: 0002 open  Local: VMS$VAXcluster Remote: OVMXW3::VMS$VAXcluster
```

`OVMX_PURE_SERVER=1` reaches all-open **because it services the members' inbound
half**; `OVMX_JOIN_SEQ` does not, which is the whole defect. Real-node control
`R1.conn` (a real VAX2 kill→rejoin) never parks in `con_pend`: it goes cleanly to
four `0002 open` CDTs, confirming `con_pend` is OVMX-specific, not a normal
transient.

---

## 2. Current-branch live reproduction (this session, #221 build)

`build-d94/bin/SCSD.EXE` rebuilt from this branch (includes #221). Two
`connwatch.sh` runs on lab-2 (`ovmx-lab`), peer VAX1 SDA sampled throughout:

| run | pod | env | result |
|---|---|---|---|
| `OVMXR0j1` | vaxlab-0 | `OVMX_JOIN_SEQ=1` | peer CDT for OVMXR0 = "not found" entire run; sequencer never fired |
| `OVMXR8j1` | vaxlab-8 (virgin) | `OVMX_JOIN_SEQ=1` | VC opens (STACK+ACK, 2 peers), then peers re-issue START round-0 ×59 (`VCFSM, START received -> none, VC OPEN`); `CONNECT-REQ-SENT=0`, sequencer never starts |

Evidence: `/data/training/vax/k8s-labs/vaxlab-{0,8}/logs/scsd-OVMXR{0,8}j1.log`,
`/data/training/vax/cluster/work/OVMXR{0,8}j1.{status,csb}`.

**Honest caveat / second finding.** On the current HEAD build the JOIN_SEQ path did
not even reach `JS_MSCP_CONNECT` in my hands — it stalls one layer upstream at VC
formation: the VC reaches OPEN on OVMX's side but the peers keep re-STARTing (their
side never completes) and the sequencer's first `SCS$DIRECTORY` connect is never
sent (`START-ACK-SENT=0`, `CONNECT-REQ-SENT=0`). The §4(x)/`vms694memb1` runs on the
Aug-3 build reached `add-member`, so this VC-START non-completion is either a
regression since Aug-3 or a pod/timing artifact — **it is filed as its own follow-up
(`vms-694` child) and blocks a clean current-build capture of the MSCP silence
itself.** The root cause in §1 is grounded on the doc + the code + the pre-#221 live
captures (which reached the MSCP step); the current-build VC-START stall is a
separate, upstream blocker that must be cleared before the §1 fix can be
bracketed live.

> **RESOLVED (2026-08-09, `vms-694`) — it was NEITHER a regression NOR a timing
> artifact: it was a §4(w) SCSSYSTEMID identity conflict on a polluted pod.** The
> VC-START runs above (`OVMXR0`/`OVMXR8`, `OVMXR8j1`) all reused `SCSSYSTEMID`
> **1812**, which `vaxlab-8`'s peer already held (`%PEA0, Remote System Conflicts
> with Known System - REMOTE NODE OVMXR0`). A conflicting member sends START +
> STACK but **withholds its round-2 ACK** and re-issues round-0 START forever, so
> `start_acked` never latches and the sequencer never fires its first CONNECT-REQ —
> exactly the stall recorded here. Proven the same session on the same `main` build:
> a **fresh** identity (`OVMXW0`/1877, `OVMXX0`/1888) on a **clean** pod (`vaxlab-9`)
> reached `CLUSTER_NODES=3`/`XITDONE=1` at t+13 s with the member's round-2 ACK
> present. The VC-formation FSM is byte-unchanged since `f874b04`, so #221's
> fresh-join is intact. Full grounding + the new `SCSD-W-VCNOACK` diagnostic that
> now names this trap: `docs/cluster-protocol-spec.md` §4(w.1). **This does NOT
> touch §1**: the Rule-of-Total-Connectivity MSCP diagnosis stands; it must be
> bracketed on a CLEAN pod with a FRESH identity (never reuse a `SCSSYSTEMID` a pod
> has seen), which is what the §2 runs failed to do.

---

## 3. Why this is NOT one of the already-handled failure modes

- **Not the reject storm** (#211/#215/#217): those handle the peer emitting op-4
  REJECT_REQ. Here the peer emits nothing at the CM layer — it abandons.
- **Not the #221 Con.ID collision**: that dropped the *second* peer's CM frames as
  `SRC_MISMATCH`. This is present with a single peer and is about the *inbound
  connect* never being accepted, not a Con.ID slot collision.
- **Not "wait longer"**: the members' `con_pend` never resolves because OVMX never
  sends the accept; no amount of retx headroom changes it.

---

## 4. Proposed fix (bounded, next increment)

Make OVMX satisfy the Rule of Total Connectivity by **accepting the members'
inbound SCS connections during the join**, not only in `OVMX_PURE_SERVER` mode:

1. On the JOIN_SEQ receive path, when the SDIR connect-scan matches a listed SYSAP
   (`MSCP$DISK`, and the member's `SCS$DIRECTORY` probe), **emit the op-1 echo /
   op-4 accept and allocate the server CDT** — the exact step `w3A`
   (`OVMX_PURE_SERVER=1`) performs and `vms694memb1` (`connect_scans=2`,
   `MSCP-SERVER-ACCEPTS-SENT=0`) skips. Candidate site: the SDIR connect-scan →
   accept branch in `scsd.c` that increments `sdir_connect_scans` but never
   reaches `mscp_srv_accepts` under JOIN_SEQ.
2. Ensure the member's follow-on MSCP commands land on that CDT (drives
   `RX-CDL no-cdt` from 22 → 0) via the already-wired `scsd_mscp_srv_msg_input`.

The accept machinery is fully built (vms-34b/#169, vms-291/#131); the gap is that
JOIN_SEQ does not *drive* it. Fix is therefore wiring, not new protocol — and
Rule 8 clean: OVMX puts nothing new on the wire, it *answers* a connect the member
already sends.

### Fail-pre / pass-post regression (to land WITH the fix)
`tests/vmsscs/test_scsd_wire.c::test_joinseq_accepts_members_inbound_mscp_connect`:
seed a `peer_state` mid-JOIN_SEQ, deliver a member-originated MSCP$DISK
`CONNECT_REQ` (op-0) addressed to OVMX's `MSCP$DISK` listen Con.ID, and assert OVMX
(a) allocates a server CDT at `PS_MSCP_SERVER_CONID(ps)` and (b) emits an op-4
accept. FAILS pre-fix (accepts=0), PASSES post-fix. Not committed here because it
must fail-pre against a fix that is not in this branch — shipping a red test would
break CI.

---

## 5. Live bracket to validate the fix (next session)

1. Clear the VC-START upstream stall (§2 caveat) so JOIN_SEQ reaches JS_MSCP_CONNECT.
2. `connwatch.sh <virgin-pod> <tag> <store> 150 <ident> OVMX_JOIN_SEQ=1` — confirm
   the peer's CDT for `<ident>` now shows `MSCP$DISK ← <ident>::VMS$DISK_CL_DRVR`
   reaching `0002 open` (was `con_pend`), OVMX log shows `MSCP-SERVER-ACCEPTS-SENT>0`
   and `RX-CDL no-cdt=0`, and `CLUSTER_NODES` advances to 3.
3. Positive control: `OVMX_PURE_SERVER=1` (already reaches all-open, `w3A`).
   Real-node control: `lab2rejoin.sh` (`R1.conn`, all `0002 open`).

---

## 6. Provenance / clean-room

All doc cites are to the host-only Davis transcript (`~/cluster/transcript/`,
copyrighted — page cites only, never committed). All wire/peer-state claims are
from OUR lab captures and real-VAX SDA output. No VSI/HPE source or binary was
examined. The bidirectional-connectivity admission rule is GROUNDED (Davis p.7-39
+ live peer CDT), added to `docs/cluster-protocol-spec.md` §4(y).

---

## 7. The §1 MSCP-accept diagnosis is FALSIFIED by its own required bracket (`vms-694`, 2026-08-09)

**Status: the §4 fix was NOT implemented — because the code-gap it targets does
not exist at HEAD, and the accept it proposes to drive is not the admission
discriminator.** This section is the durable record so the next session does not
re-implement a phantom fix. Bracketed exactly as §5 required: a CLEAN pod, a
FRESH never-admitted identity, `OVMX_JOIN_SEQ=1`, kill-switch control (guardrail
23), SCSD.EXE built from this branch (== `main` at `542fb57`, no wire change).

### 7a. The receive-path accept is ALREADY driven, independent of `join_step`

Measured directly through `scsd_handle_frame()`
(`tests/vmsscs/test_scsd_wire.c::test_joinseq_accepts_members_inbound_mscp_connect`):
a peer parked at `JS_MSCP_CONNECT` that receives the member's inbound op=0
MSCP$DISK connect DOES emit the op=1 echo + op=4 accept and DOES allocate the
server CDT at `PS_MSCP_SERVER_CONID(ps)`. The `(b1.5)` block (`scsd.c`, "SERVER-
FIRST established-join") is gated on `ovmx_mscp_server_enabled()`, **not** on the
sequencer state, so §1b's "JOIN_SEQ drives only the outbound half" is not true of
the code at HEAD. #221 (per-peer Con.ID) + the pre-existing `(b1.5)`/`(b2)`
default-path accepts already answer both the member's MSCP$DISK and SCS$DIRECTORY
inbound connects during admission.

### 7b. Live bracket — the kill-switch does NOT gate admission

| arm | pod / ident | env | MSCP-ACCEPTS-SENT | outcome |
|---|---|---|---|---|
| positive | vaxlab-9 / OVMXZ2 (1812) | `OVMX_JOIN_SEQ=1` | **30** | `XITDONE=1`, step 8/8 add-member, member CONFIRMED our server (`SCSD-I-MSCPSRVOK`); peer CDT `MSCP$DISK ← OVMXZ2::VMS$DISK_CL_DRVR` = `0002 open` |
| kill-switch | vaxlab-7 / OVMXK0 (1812) | `OVMX_JOIN_SEQ=1 OVMX_MSCP_SERVER=0` | **0** | `XITDONE=1`, step 8/8 add-member — STILL admitted |

**Membership was real in BOTH arms**, proven on the peer's own console at SCSD
exit: `Node OVMXZ2 (csid 00010005) has been removed from the VAXcluster` /
`Node OVMXK0 (csid 00010003) ... removed`. A node only gets a CSID and a
"removed" line if it had been configured IN. So OVMX joins as a full VAXcluster
member with the MSCP$DISK server ON *or* OFF — the members' connectivity to OVMX
is satisfied by the VMS$VAXcluster + SCS$DIRECTORY connections, not by the
MSCP$DISK connect, and the §1 "Rule of Total Connectivity ⇒ MSCP accept" chain
does not hold for admission on this lab config.

### 7c. What the §2 pre-#221 "silence" actually was

Already resolved in the §2 RESOLVED note: an `SCSSYSTEMID` identity conflict on a
polluted pod (spec §4(w.1)). On the clean/fresh-id bracket §5 itself mandated, the
JS_MSCP_CONNECT stall does not reproduce. The real advance past JS_MSCP_CONNECT
was landed by #221 + the §4(w) identity-conflict fix, not by an MSCP-accept
change.

### 7d. Residual, filed on `vms-694` (not this branch's scope)

Not admission, but not yet the full `w3A` all-open steady state either: in both
arms the peer's `VMS$VAXcluster ← OVMX::VMS$VAXcluster` CDT was sampled at
`0007 con_sent` (not `0002 open`) at teardown, and the ON arm's member repeatedly
re-opened MSCP$DISK connects (the vms-298 residual per-connection-handle churn:
each "SECOND MSCP$DISK connect" is offered the SAME per-peer server handle). These
are the open questions worth the next increment — the MSCP-accept receive-path
wiring is not.

**Evidence paths** (host, tank volume):
`/data/training/vax/k8s-labs/vaxlab-9/logs/scsd-z2joinON.log`,
`/data/training/vax/k8s-labs/vaxlab-7/logs/scsd-k0killsw.log`,
`/data/training/vax/cluster/work/{z2joinON,k0killsw}.{csb,status}`.

---

## 8. The rejoin residual, re-bracketed clean and LOCATED (`vms-694`, 2026-08-09)

After §7 falsified the MSCP-accept diagnosis, the genuine open bug narrowed to
**a just-departed identity's rejoin not completing (`XITDONE=0`)**. That is now
freshly bracketed at HEAD and located. Full record + evidence:
`docs/cluster-protocol-spec.md` **§4(O.9)** (and the prior `vms-449` grounding at
§4(O.2)/§4(O.3)). Summary:

- **Clean bracket** on one virgin pod (`vaxlab-10`, `SCSD.EXE` from `main`
  `a283f6a`): first join `OVMXA4`/1812 → `XITDONE=1`; **rejoin same id →
  `XITDONE=0`** (both fast, ~45 s, and slow, ~7 min); **fresh id `OVMXB0`/1813 on
  the same pod → `XITDONE=1`**. The stall is id-reuse, not a broken pod.
- **The §4(w)/#230 identity-conflict hypothesis is ELIMINATED as the cause:** the
  VC-START completes on every rejoin (0 `%PEA0`, 0 `VCNOACK`, VC `OPEN`,
  `start_acked=1`, member advertises incarnation N=2/3 and OVMX echoes it into
  `0x41 [22:24]`). The refusal is **downstream of the VC**, at CM readmission.
- **Two surface modes of the one refusal:** fast rejoin — the peer holds the
  prior `VMS$VAXcluster 0002 open` CDT and **rejects** OVMX's new joiner connect
  (`000B rej_sent`, non-zero reason); slow rejoin — the peer holds OVMXA4's CSB
  in `long_break`, **accepts** the connect (`JOINBOUND`), OVMX sends the op 0x02
  REJOIN form (`cm_apply_rejoin_form`), and the member then **abandons** the CM
  transaction (`DISC SENT`, no barrier).
- **Not inherent aging.** A real VAX crash-rejoins under the same id
  (`vax3-class03-crash-REJOIN-SUCCESS`). The gap is OVMX's op 0x02 REJOIN form is
  *exercised but incomplete*. **Operational response now:** mint a fresh identity
  per boot (arm d). **Fix (next increment):** complete the op 0x02 REJOIN-form CM
  transaction against that capture so a reused identity re-admits.
