# Design — DLM distributed deadlock detection (rung H11, `vms-ec75`)

> Status: design for the last DLM rung. Extends the local wait-for-graph detector
> (`check_deadlock`, `src/kernel-core/vms_lock.c`) to a **cluster-wide** detector:
> a deadlock CYCLE whose edges cross nodes is detected, and exactly one victim's
> waiting `$ENQ` is aborted with `SS$_DEADLOCK` (3594), breaking the cycle; the
> other request proceeds once the victim releases.

## Clean-room provenance (CLAUDE.md Rule 8)

The **structure** — a distributed wait-for graph searched by messages that follow
the wait-for edges from node to node — is standard distributed-deadlock-detection
theory (edge-chasing / Chandy–Misra–Haas) and matches the behaviour OpenVMS
documents for its lock manager's **deadlock search** (a bounded search along the
lock wait queues, one requester chosen as victim and completed with
`SS$_DEADLOCK`; see the OpenVMS System Services and Lock Management documentation,
and `$SSDEF`). The **byte-level wire frame** below is NOT published by VSI for the
DLM SYSAP, so OVMX defines its **own** representation and labels it an OVMX design
choice — never presented as VMS-authentic (identical to every other
`SCS_DLM_OP_*`, which reuse OVMX's own DLM-over-SCS frame). No VSI/HPE binary was
disassembled.

## What exists today (the seam)

- **Local detection is solid.** `check_deadlock()` walks the wait-for graph
  (iterative BFS, `MAX_DEADLOCK_DEPTH = 16`, `exec_trylock` to avoid lock-order
  inversion) keyed on the blocked request's `proc`. A cycle back to `origin_proc`
  ⇒ `SS$_DEADLOCK`. Driven from the queue path in `vms_enq_core_ex` and from the
  post-wait re-scan in the sync waiter.
- **Cross-node is skipped, honestly.** In cross-node mode (`xn` set) the queue
  path deliberately skips `check_deadlock` (`vms_lock.c` ~:1142): every cross-node
  lock shares the daemon delivery `proc` while representing a *different* cluster
  owner (`req_csid`), so the `proc`-keyed local detector would false-positive.
  The comment names `vms-ec75` as the rung that supplies the distributed detector.
- **The wait-for edge is already known at the master.** When a cross-node `$ENQ`
  queues, the master fills the H5/H6 contention outputs: `blocking_csid`,
  `blocking_master_lkid`, `blocking_req_lkid` — i.e. it already knows *who holds
  the conflicting lock*. That is exactly one outgoing wait-for edge, which is all
  edge-chasing needs.

## Algorithm — edge-chasing deadlock search

A cross-node cycle is a sequence of wait-for edges
`R0 →(waits for)→ H0 →(also waits)→ R1 →…→ Rk = R0` that crosses ≥1 node
boundary. Each edge is "a queued request waits for the holder of the conflicting
lock". We chase those edges with a bounded probe.

### Wire op — `SCS_DLM_OP_DLKSRCH` = 6 (OVMX design choice)

Reuses the existing DLM-over-SCS frame; fields (all already present in the frame
or carried in its scalar slots):

| field            | meaning                                                             |
|------------------|--------------------------------------------------------------------|
| `initiator_csid` | CSID of the node whose blocked request started the search          |
| `initiator_lkid` | that blocked request's requester-side handle (cycle-close test)     |
| `blocked_csid`   | CSID currently being chased (the holder we ask "are you blocked?")  |
| `blocked_lkid`   | that holder's lock handle on its own node                          |
| `victim_csid`    | running MIN CSID over requests chased so far (global victim, §3)     |
| `victim_lkid`    | running MIN lkid (tiebreak) — the `(csid,lkid)` lexicographic min    |
| `ttl`            | hop budget, initialised to `MAX_DEADLOCK_DEPTH` (16), decremented   |
| `flag`           | `SEARCH` (forward) vs `VICTIM` (cycle confirmed — abort)            |
| `resnam`         | the resource under contention (for logging / correlation)          |

The cycle-close test uses `(initiator_csid, initiator_lkid)` — the probe has
returned to where it began — but the ABORTED request is `(victim_csid,
victim_lkid)`, the global min, which is identical for every probe traversing the
same cycle. Never conflate the two: closing on the initiator, aborting the min.

One op, two directions via `flag`, so the "op in N places" cost is paid once.

### 1. Initiate (at the master where a cross-node request queues)

In the `xn` queue path (where `check_deadlock` is skipped today): once the request
is queued and `blocking_csid` is a **remote** holder, emit
`DLKSRCH(SEARCH, initiator=(req_csid, req_lkid), blocked=(blocking_csid,
blocking_req_lkid), ttl=16)` to `blocking_csid`'s node. A purely local blocker
(the conflicting lock is held for a local process) is handled by the existing
local `check_deadlock` instead — no probe needed.

### 2. Chase (at a node receiving `DLKSRCH(SEARCH, …)`)

1. `ttl == 0` ⇒ drop (bounded; a dropped probe never fabricates a deadlock).
2. Look up the local holder `blocked_lkid`. Find its process `P`.
3. Does `P` have a **waiting** cross-node request `W` (an entry with
   `waiting == 1`)? If **no** ⇒ dead end, `P` is not blocked, no cycle on this
   path; drop.
4. If `W`'s identity `(its req_csid, its req_lkid)` equals
   `(initiator_csid, initiator_lkid)` — i.e. the chase returned to the request
   that started it — **CYCLE**. Go to step 3 (victim), below.
5. Otherwise forward `DLKSRCH(SEARCH, initiator unchanged,
   blocked=(W's blocking_csid, W's blocking_req_lkid), ttl-1)` to `W`'s blocker's
   node — the next wait-for edge.

Because every edge is a real queued-request→real-holder relationship read from
`res->granted` / `res->waiting`, a detected cycle is a REAL cycle (INV-6): the
detector never guesses "probably deadlocked".

### 3. Victim (GLOBALLY deterministic — min over the cycle, NOT the initiator)

**The double-victim hazard.** "The victim is the initiator" is WRONG: in a 2-node
cycle BOTH ends can initiate a search simultaneously (A probes, B probes); each
probe returns to its own initiator ⇒ both would declare deadlock and both would
abort ⇒ **two** processes killed when exactly **one** should be. That double-abort
is the data-integrity failure this rung exists to prevent. The victim MUST be
chosen by a rule both nodes compute **identically**, independent of who initiated.

**The rule — global minimum over the cycle's real request ids.** Edge-chasing
traverses the ENTIRE cycle before a probe returns to its start, so every probe
(whichever node launched it) visits the SAME set of requests. The frame therefore
carries a running candidate `victim = (victim_csid, victim_lkid)`: at each hop the
chasing node updates it with `min((victim_csid, victim_lkid), (this waiting
request's req_csid, req_lkid))` lexicographically. When the cycle closes, that
accumulated minimum is the victim — the same value for an A-initiated and a
B-initiated probe, so at most one request is ever aborted even if both fire.

The detecting node sends `DLKSRCH(VICTIM, victim=(victim_csid, victim_lkid))` to
the node that MASTERS the victim's queued request (reachable because that request
carries `req_csid = victim_csid`). That master:

1. Finds the queued request `req_csid == victim_csid && req_lkid == victim_lkid`
   on its `waiting` queue.
2. Removes it and completes it with `SS$_DEADLOCK` via the **existing GRANT-reply
   path** (`SCS_DLM_OP_GRANT` with `status = SS$_DEADLOCK`) — that requester's
   `$ENQ` returns `SS$_DEADLOCK`, exactly as the local detector already does.
   Idempotent: a second VICTIM naming an already-aborted request is a no-op, so a
   concurrent A- and B-initiated search that agree on the victim abort it once.

The other request in the cycle is **not** aborted; it stays queued and grants once
the victim's process releases the lock it held (application-driven back-off, the
VMS contract). So the cycle is broken by exactly one `SS$_DEADLOCK`, even under
concurrent bidirectional initiation.

> Harness: prove the single-initiator cycle as the minimal slice. The
> concurrent-both-initiate case (A and B both waiting+searching → still exactly
> one `SS$_DEADLOCK`) is the strongest proof of the global rule; prove it here if
> it does not balloon the slice, else file a follow-on (Rule 5) — but the
> SELECTION LOGIC ships as the global-min rule regardless, never local-initiator.

## Minimal faithful proof (harness)

A genuine 2-node cross-node cycle, no artificial trigger:

- Node A's process holds `R_A` (EX), then `$ENQ R_B` (EX) → queues on `R_B`'s
  master behind B's holder.
- Node B's process holds `R_B` (EX), then `$ENQ R_A` (EX) → queues on `R_A`'s
  master behind A's holder.
- Edges: `A waits-for B` (on `R_B`) and `B waits-for A` (on `R_A`) — a cross-node
  cycle.

**Assert:** exactly **one** waiter's `$ENQ` returns `SS$_DEADLOCK` (the
deterministic victim), the other stays queued (not aborted); the detection is the
executive's own edge-chase over real wait-for state, every marker value read
verbatim off the nodes' SCSD output (INV-6). The victim CSID is deterministic
across runs.

## Reconciliation checklist (the traps this campaign taught)

- **New op `SCS_DLM_OP_DLKSRCH` = 6 → all N places:** `scs_dlm.h` enum,
  `scs_dlm.c` codec validator + `scs_dlm_op_name`, `vms_ioctl.h` `VMS_DLM_OP_*`
  mirror **and** the NetBSD mirror `src/kernel-netbsd/vms_lock_nb.h` (the
  amd64-green-≠-twin-proven trap, #928), `scsd.c` `static_assert`, `vms_lock.c`
  dispatch.
- **New cross-node SEND (the probe + the victim signal) → CHOKED SEND SITE
  TABLE** entry in `scsd.c` (`scs_send_sites`, the #923 census trap), labelled
  new-in-`vms-ec75`.
- **Any new scsd-issued ioctl → census** (`kif_caller_census`): a `vms_kif_*`
  wrapper that issues it, or an `OVMX-UNWIRED` declaration for a scsd-direct call.
- **Any `docs/compat/*.yaml` touched → `render_compat.py`** (the drift gate).
- **INV-6:** every value on every marker is a REAL executive read; a dropped/
  ttl-expired probe reports "no deadlock found", never a fabricated cycle.

## Out of scope (deferred, distinct items if needed)

- N-node cycles > 2 hops are handled by the same edge-chase, but the harness
  proves the 2-node case; a wider harness is a follow-on, not this rung.
- Victim-selection *policy* (VMS can pick by cost/priority) — OVMX picks the
  initiator deterministically; a policy refinement is a follow-on.
