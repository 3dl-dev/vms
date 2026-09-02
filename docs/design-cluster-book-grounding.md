# Design grounding — the faithful cluster executive against *VAXcluster Principles*

> Status: GROUNDING PASS, 2026-09-02. Companion to
> `docs/design-faithful-cluster-executive.md` (DESIGN) and
> `docs/plan-faithful-cluster-executive.md` (PLAN). This document anchors the
> same design in the published book and extracts the algorithms the FSM
> implementers need. It does not propose a new design; where the book sharpens
> or contradicts a premise, §5 says so with a page cite so the design/plan can
> be corrected.
>
> Source: Roy G. Davis, *VAXcluster Principles* (Digital Press, 1993), host-only
> transcript `~/cluster/transcript/` (ch. 2, 4–8). Cited as `p. N-M`. Clean-room
> (Rule 8): everything below is public description from a published book;
> there is no VMS source in the transcript and none is quoted. No unpublished
> value is computed or guessed anywhere in this document.
>
> **Not in the transcript** (so still capture-grounded, not book-grounded):
> chapter 3 (port drivers, LAN/NISCA discovery, HELLO, channels, VC
> sequencing — P0/P1 material); any wire encoding (opcodes, offsets); the
> resource-name hash function; the member order inside the Lock Directory
> Weight Vector; the size of the Cluster System Vector; the on-disk layout of
> `QUORUM.DAT`.

---

## 1. Directory resolution (resolves FC-P4.1 rung A vs B)

### 1.1 What the book specifies — the Lock Directory Weight Vector (LDWV)

| Element | Book statement | Cite |
|---|---|---|
| Hash input | "the resource name … the bytes that make up the resource name character string" | p. 6-31, p. 6-49 |
| Hash output | a **16-bit** value; the same value on every system for a given name | p. 6-32, p. 6-49 |
| Directory index | `hash16 mod n` where `n` = number of LDWV entries; index in `0..n−1` | p. 6-31, p. 6-32 |
| LDWV entry content | the CSID of the system that entry belongs to; **entries for the local system contain 0** ("logically equivalent, not physically identical") | p. 6-32, Fig. 6-18 p. 6-33 |
| Entries per system | = that system's LOCKDIRWT; **if LOCKDIRWT is 0 on every member, the vector is forced to one entry per system** | p. 6-32 |
| Layout | all entries for a given system are **contiguous**; the entry at a given offset corresponds to the same system on every member's copy | p. 6-32 |
| Size rule | `n = Σ LOCKDIRWT` over the selected systems, or `n = number of selected systems` if all are 0 | p. 7-35 (FORM), p. 7-40 (JOIN grows by joiner's LOCKDIRWT) |
| When built | initialized at cluster formation; a joiner allocates its vector while processing the Phase 1 message; every member **fills it in during Phase 2**, before the lock-database rebuild | p. 6-33, p. 7-35, p. 7-41, p. 7-37 / 7-42 / 7-47 |
| On departure | remaining systems "collapse their copies … by removing the entries corresponding to the departing system"; a join may "logically expand this vector in place" using room left by departures, else reallocate-and-copy | p. 6-33, p. 7-40 |
| Consequence | any change of the vector can move a root name to a different directory node, so on those transitions "all existing directory node information is discarded throughout the cluster" and masters re-register — the **directory rebuild** | p. 6-33, p. 6-53 |
| Who rebuilds it | the Connection Manager ("responsible for rebuilding the Lock Directory Weight Vector"), and each CSB stores its system's LOCKDIRWT for that purpose | p. 7-23 |

The lookup itself (p. 6-31): a root resource not known locally → hash → `idx = hash16 mod n` → `LDWV[idx]`; if the entry is 0 (local) the local node is the directory node and, having no entry, "simply assumes mastery"; else the **lock request is sent to the directory node**, with three outcomes: (a) directory node is also master → it resolves the request; (b) it knows the master → it returns the master's CSID and the requester re-sends to the master; (c) it has no entry → it tells the requester "become the master" and records that. Once resolved, the master CSID is stored in the root RSB and propagated through the tree; one lookup per tree per node while it holds any lock in the tree (p. 6-32). Sub-resources never hash (confirms design D-DLM-2's "only root names matter").

### 1.2 The directory node's side (what FC-P5.4 must reproduce)

p. 6-50: the request "includes a root resource name, and it also includes the **hash value derived from that name by the system that sent the request**. This is done as an optimization". The directory node right-shifts that hash by `16 − log2(RESHASHTBL rounded up)` (p. 6-49 fn) to index its own Resource Hash Table, scans that chain **by name**, and:

- no matching RSB → **creates one at the end of the chain**, stores the requester's CSID as master, flags it a *directory entry* RSB;
- matching directory-entry RSB → copies its master CSID into the reply;
- matching entry with master CSID **0** → the directory node is itself master and resolves the request (p. 6-51).

A directory-entry RSB lives until "the master of the tree releases that mastery as a result of the last lock in the tree being dequeued" (p. 6-50). Three ways a directory node becomes master (p. 6-51): it acquires the first lock in the cluster on the root (entry created with master CSID 0); the master departed; dynamic remastery moved the tree to it.

### 1.3 What the wire already shows

The op-01 body the campaign builds carries a **le16 at `body[10:12]` named `dir_hash`** and sends it as an "honest 0" (`scs_member.c` on `feat/coord-rebuild-completion`, put_le16(body+10, dir_hash); the memory notes for db20-b record "dir_hash=0 … NOT rejected"). A 16-bit field in the lock-request body is exactly what p. 6-49/6-50 describe. Its identification as the requester's hash value is a **book-supported inference**, to be pinned by FC-P4.5 with the ac4 specimens (predict: the field varies with the name and is identical across requesters and across time for the same name; it is the only 16-bit name-dependent field). Spec §4(f).1 leaves `body[10:20]`, `[26:30]`, `[32:46]` unassigned; p. 6-38 says a request to a master also carries the sender's **LOCKDIRWT** and its **activity level** for the tree (§2.2), so two more of those bytes are spoken for.

### 1.4 Verdict

**Rung A for the structure, NOT rung A for the function; rung B as written is forbidden by the book.**

1. **Implementable now from the book (rung A):** LDWV construction (size rule, entries-per-node, contiguity, local-entry-is-0, all-zero fallback), the resolution rule `csid = LDWV[hash16 mod n]` with `0 ⇒ self`, when the vector is rebuilt and by whom, the three-outcome lookup protocol, the directory-entry RSB semantics and lifetime, and the directory-node's serving algorithm (index by the wire-supplied hash, match by name, create-or-answer). FC-P5.4 (directory-node role) and the rebuild-receive side need **no hash function at all**: the requester supplies the hash and the match is by name.
2. **Not in the book:** the hash function ("a series of mathematical and logical operations", p. 6-31 / p. 6-49) and the **member order** within the LDWV (contiguity and cross-node equivalence are stated; which system gets offset 0 is not). These are the two residuals for `dir_resolve(name) → csid`.
3. **Rung B (probe-and-cache) is unsafe** — the design's own rung-C trigger fires: a directory node "finds no matching RSB, creates one … stores in this RSB the CSID of the system that sent the request to master the tree" (p. 6-50) with no check that it is the correct directory node for the name. A mis-addressed lookup creates a second directory entry and therefore a second master (§5 D3). Worse, the hash is *in the request* and used to pick the chain: sending a wrong hash to the **right** node also creates a duplicate directory entry (the real one sits in another chain). So the design's "when |W| = 1 there is nothing to resolve" (D-DLM-2) is false: even with a single directory node OVMX cannot originate a lookup for a name whose hash16 it does not know without corrupting that node's directory (§5 D2).
4. **Where that leaves FC-P4.3.** `dir_resolve` decomposes into `hash16(name)` and `LDWV[hash16 mod n]`. The vector half is rung A. For `hash16`, the honest sources are (in order): (i) the IDSM lock-management chapter (the remaining rung-A candidate; Davis does not give it — FC-P4.1's DOC gate stays open on exactly that one question); (ii) **observed values**: every inbound lookup and every rebuild registration OVMX receives carries `(name, hash16)` from the cluster — a cache keyed by name, populated only from frames the cluster sent, is INV-6/Rule-8 clean (it consumes the cluster's answer, computes nothing) and lets OVMX originate requests for any name it has been told the hash of; (iii) names OVMX has never seen on the wire (its own `F11B$v<label>`, the node-specific system locks a joiner creates before joining, p. 6-35) cannot be originated honestly without (i) — report `SS$_UNSUPPORTED` with one `%CNXMAN` line, the design's rung-C(ii) shape, narrowed to *unseen root names* instead of *all foreign names*. The member-order residual is settled the same way: it is observable (which node the cluster addresses for a name with a known hash), and the §5.2 conformance capture should record it. Rung C(i) (behavioural derivation of the function) remains an operator ruling; note the dataset it would need is now much richer than the design assumed — `(name, hash16)` pairs, not `(name, node)`.

Concretely for the implementer of `dir_resolve`:

```
dir_resolve(name):
    if not is_root(name): return root's stored master   (p. 6-32)
    h = hash16_cache.lookup(name)      # from inbound lookups/registrations only
    if h is None: return SS$_UNSUPPORTED   # honest: cannot originate an unseen root
    n = ldwv.len                       # Σ LOCKDIRWT, or member count if all 0   (p. 7-35)
    e = ldwv[h % n]                    # 0 ⇒ local node is directory            (p. 6-31/32)
    return (e == 0) ? LOCAL_DIRECTORY : e
```

Every rebuild registration and lookup OVMX serves as a directory node uses the hash **in the request** to index (or, since OVMX's own table is private, any local index) and matches by name — no cache miss is possible on the serving side.

---

## 2. Dynamic remastery and rebuild (FC-P5.5, FC-P5.4, FC-P5.3)

### 2.1 Mastery rules (as of V5.5, the version the lab's VAXes are at or past)

| Rule | Cite |
|---|---|
| Initial master = the first system to acquire a lock on the root | p. 6-30, p. 6-37 |
| Greater LOCKDIRWT always has priority for mastering a shared tree | p. 6-37, p. 6-38 |
| Among equal LOCKDIRWT, the system with the most `$ENQ/$ENQW/$DEQ` activity averaged over time, subject to thresholds | p. 6-37, p. 6-38 |
| (Historic V5.2–5.4-3) activity count 0/1/2 = LOCKDIRWT 0 / >0 non-directory / >0 directory; directory node "most eager"; mastery moves only to a strictly greater count | Table 6-5 p. 6-35, p. 6-36 |

### 2.2 The activity/interest machinery (formulas)

- Each system keeps, per tree it uses, a count of its `$ENQ/$ENQW/$DEQ` operations; once a second it recomputes  
  `NEW_AVERAGE = PREVIOUS_AVERAGE × 7/8 + COUNT × 1/8` and resets COUNT to 0 (an 8-second weighted average) (p. 6-38).
- **Every lock request sent to the master carries the sender's LOCKDIRWT and its current activity level for that tree** (p. 6-38) — wire fields FC-P4.5 must locate and FC-P4.6 must fill from real counters.
- Master-side, on each request (pp. 6-38–6-39):
  - sender LOCKDIRWT < master's → serve, nothing else;
  - sender LOCKDIRWT > master's → set **check-for-better-master** on the tree; still serve;
  - equal → set **remaster-pending** and record the sender's CSID as candidate iff sender activity ≥ **system threshold 10** *and* sender activity ≥ master activity + **activity threshold 10**; still serve.
- **Every 8 seconds** the master scans its trees; at most **5** (the *remastering quota*) are offloaded per scan (p. 6-39):
  - check-for-better-master → scan GRANTED/CONVERTING/WAITING of the root for the greatest LOCKDIRWT; move there (random among ties);
  - remaster-pending → `cost = total locks on all resources in the tree`, `benefit = 16 × (candidate activity − master activity)`; move if `benefit > cost`, **or** the same system set the flag for **3 consecutive** 8-second intervals.
- **Every 60 seconds** the master scans root RSBs for trees used by only one system and moves mastery to that system (within the quota) (p. 6-40).
- Graceful `SHUTDOWN.COM` (not CLUSTER_SHUTDOWN): the departing system offloads mastery **before** the transition — to the sole other user, else the highest-LOCKDIRWT user, random among ties (pp. 6-36–6-37).
- Value blocks: not invalidated by dynamic remastery as of V5.5 (p. 6-15 fn); invalidated when the master fails and all surviving locks are NL/CR (p. 6-15); not invalidated on graceful OPCCRASH removal (p. 6-15).

### 2.3 Rebuild types and their selection

| Transition | Rebuild | Cite |
|---|---|---|
| FORM CLUSTER | full (chosen by the coordinator) | p. 7-35 |
| JOIN, joiner LOCKDIRWT > 0 | **directory** (the more extensive type; merge is subsumed — p. 7-40 fn: merge < directory < full) | p. 7-40 |
| JOIN, LOCKDIRWT = 0 on **all** selected systems | **directory** (every system holds one LDWV entry, so the joiner takes its share) | p. 7-40 |
| JOIN, joiner 0 and ≥1 other nonzero | **merge** | p. 7-40 |
| JOIN, a more extensive rebuild already pending | transition **abandoned** | p. 7-40 |
| RECONFIGURE (departure) | full if pending; else **directory** if a dir/merge rebuild is pending, or the departing system's LOCKDIRWT > 0, or all remaining have LOCKDIRWT = 0; else **partial** | p. 7-44 |
| Any transition | abort dynamic remastering in progress first | p. 6-36 |

Ch. 6's V5.2 phrasing ("nonzero joins → directory **and** merge", p. 6-36) is the same rule seen from the lock manager: a directory rebuild includes re-registering everything.

What each type does:

- **merge** (p. 6-35): the joiner's node-specific locks on system-level resources (created before it joined and therefore mastered by it) are registered with their directory nodes by the joiner executing the directory-lookup algorithm.
- **partial** (p. 6-34, 6-35): delete every reference to locks held on the departed system; remaster only trees the departed system mastered — first by remaining nonzero-LOCKDIRWT systems, then by zero ones; try to grant requests blocked by the departed holder's locks.
- **directory** (p. 6-33, 6-53): every system discards its directory-entry RSBs, builds a new LDWV, and "go[es] through the motions of doing directory lookup operations for the trees they master, and thus register themselves with what are now the directory nodes for those trees" — an entirely new set of directory-entry RSBs. No redundancy of directory information is kept, by design (p. 6-53).
- **full** (p. 6-33): discard all mastery and directory knowledge; scramble to remaster (pre-5.0 default; V5.2+ only for coexistence / FORM).

### 2.4 Sequencing across a transition (what the rebuild FSM must reproduce)

- Phase 1 (coordinator proposal) carries the rebuild **type**; each receiver records it, adjusts/allocates its LDWV, and rejects the proposal if it knows of a more extensive pending rebuild (p. 7-35, 7-41, 7-45). Phase 2 (commit, "point of no return") tasks run per node at their own pace: nodemap → CSBs, proposed → effective quorum cells, votes/member totals, MEMBER flags, CSV entry, **LDWV filled in** (p. 7-37, 7-42, 7-46/47).
- Then "each system waits until all the other systems are ready to do the rebuild. Then they all go through the rebuild procedure in absolute unison, carefully synchronized by VAX_A" (p. 7-37, 7-42, 7-47). The transition ends when the coordinator releases its coordinator lock. Since V5.4 a node rebuilds up to **three trees concurrently** (p. 6-37).
- Fault-tolerance model (pp. 6-52–6-53): the master holds all RSBs of the tree plus *local-copy* LKBs (its own locks) and *master-copy* LKBs (everyone else's); every other user holds only RSBs it uses and *process-copy* LKBs, each paired with a master copy. Non-master departs → the master deletes that system's locks, frees empty RSBs, re-runs CONVERTING/WAITING grants. Master departs → survivors "provide the new master with descriptions of the locks they hold" after the new master is chosen; the new master re-runs grants.
- Deadlock-search infrastructure re-selected at each transition: one system is chosen "effectively at random" as the **timestamp server** (p. 6-24); timestamps = server time + 50 ms; lifetimes 50 → 100 → 200 → 400 → … → **1600 ms max** on abort/restart; Table 6-4 (p. 6-24) gives the newer/equal/older rules; each node keeps a second, local timestamp = local time + 1600 ms (p. 6-25). Propagation messages carry: the Lock ID to continue from, the originating **EPID**, and the best-candidate victim (EPID, deadlock priority, Lock ID) (p. 6-23); a separate victim-selection message goes to the victim's system (p. 6-23). Conversion deadlocks are local to the CONVERTING queue and never use the timestamp machinery (p. 6-26/6-27).

### 2.5 Structures the master / directory / rebuild FSMs must carry

| Structure | Fields the book names | Cite |
|---|---|---|
| RSB | name; most restrictive granted mode; GRANTED/CONVERTING/WAITING heads; **master CSID (0 = this node)**; parent RSB (0 = root); child-RSB count; per-tree queue links with head in the root RSB; root-RSB pointer; queue of root RSBs; *directory-entry* flag; forward/back chain links (chain head is forward-only) | Fig. 6-23 p. 6-48, p. 6-49, p. 6-50, p. 6-51 |
| LKB | PID; requested mode; granted mode; completion-AST/blocking-AST addresses; EFN; parent LKB (0 for root lock); sublock count (`SS$_SUBLOCK` on `$DEQ` if nonzero); kind: local copy / master copy / process copy | p. 6-40, p. 6-30, p. 6-52 |
| Lock ID (V5.5+) | bits 0–15 index into the contiguous Lock ID Table; bits 16–23 XPAGE index or 0; bits 24–30 sequence; bit 31 = 0. Unused entries chain a free list; table grows a page (128 entries) when < 256 free; XPAGE = 256 entries, never deallocated; LOCKIDTBL + 256 initial, LOCKIDTBL_MAX an estimate only | Fig. 6-21 p. 6-46, pp. 6-41–6-45 |
| Resource Hash Table | 2^n entries, n from RESHASHTBL; index = hash16 >> (16−n); up to 65K entries (V5.5) | p. 6-49 |
| Per-process | lock-ownership queue in the PCB (granted at head, ungranted at tail — used by the deadlock search and by process rundown); deadlock priority (u32, initial 0) | p. 6-52, p. 6-20 |
| Timeout queue | one per node; examined once a second; `DEADLOCK_WAIT` seconds triggers a search; conversion search first if the lock is CONVERTING, then multiple-resource search | p. 6-18, p. 6-27 |
| Caches (V5.5) | free-RSB cache (trim 3 when > 10); free-LKB cache (> 100: trim min(3 %, 10); 10–100: trim 1) | p. 6-53 |

OVMX's `vms_lock.c` may keep its own containers (RB-tree lock ids, etc.); what must be **faithful on the wire and in behaviour** are the master-CSID-0 convention, the directory-entry flag and lifetime, the three LKB kinds, the sublock rule, and the queue disciplines already in `vms_lock.c`.

---

## 3. SCS and the Connection Manager (FC-P1–P3)

### 3.1 Layering — confirmed

The four layers SYSAP / SCS / PPD / PI (p. 2-52, Fig. 2-26) and the split of SCS responsibilities between the port driver (VC dialogue, routing, credits, buffer allocation, send/map routines) and `SYS$SCS` (CDT/RDT/RSPID allocation, `SCS$DIRECTORY` and `SCS$DIR_LOOKUP`) (p. 2-55/2-56) map onto the design's `vms_pe.c` / `vms_scs.c` split. `SYS$CLUSTER` is one SYSAP (p. 7-3) and its single SCS connection to each remote `SYS$CLUSTER` is multiplexed by **ACKMSG** with a *facility code* per client — Connection Manager, Lock Manager, CLUSTER_SERVER (p. 7-4, p. 8-5) — which is what the campaign's category byte (cat-01 CM, cat-02 DLM) is a projection of. The distributed part of the lock manager lives in `SYS$CLUSTER` (p. 2-57): the design's `vms_dlm_scs.c` as the DLM's SYSAP arm on the `VMS$VAXcluster` connection is the right shape.

### 3.2 Virtual circuits (FC-P1.2) — refinements

- START/STACK/ACK dialogue with **both** timing cases (Fig. 2-7 / 2-8, pp. 2-12–2-15); the acceptable-response table (p. 2-14): in START SENT, START→send STACK (state START RECEIVED), STACK→OPEN + send ACK; in START RECEIVED, ACK or STACK→OPEN (send ACK on STACK), START→resend STACK; timer + OS-dependent retry limit; **implied ACK**: any circuit-requiring packet received in START RECEIVED opens the circuit (p. 2-16).
- SB contents (p. 2-16): CPU type/hardware revision, OS name/version, 48-bit SCS System ID (SCSSYSTEMID), SCS node name (SCSNODE), 64-bit software incarnation (VMS: boot time). A node keeps an SB for itself and PBs for loopback circuits (p. 2-16/2-17).
- Formative PB/SB queued to the PDT until OPEN; on OPEN, an existing SB is reused and refreshed if it had no PBs (a rebooted node); **masquerade tests**: same System ID ⇒ node names must match and vice versa; if a PB already exists the incarnation numbers must match, else formation is abandoned (p. 2-20/2-21 fn). The design's SB/PB model should add these checks (they explain the "DEAD CSB for the old incarnation" state, p. 7-24).
- Message guarantees: loss or mis-order of a sequenced message **breaks the VC** and every connection on it (p. 2-31, p. 2-36) — the design's "never freeze a peer's recv_ack" hazard is the book's rule, not a lab quirk.
- Last-gasp: on receipt the port driver "immediately closes the virtual circuit … then notifies all SYSAPs" (p. 7-29); PPD-level datagram classes include a "node stop datagram" (p. 4-14).

### 3.3 Connections, CDT/CDL, credits (FC-P2.2/2.3)

- Connection states, both sides (pp. 2-22–2-27, Figs. 2-14–2-16): CLOSED → CONNECT SENT → CONNECT ACK → OPEN (source); LISTEN → CONNECT RECEIVED → ACCEPT SENT → OPEN (target); REJECT SENT; OPEN → DISC SENT → DISC ACK → CLOSED (initiator) and OPEN → DISC RECEIVED → DISC MATCH → CLOSED (peer); simultaneous disconnect collapses to DISC SENT → DISC MATCH → CLOSED (p. 2-27). Both SYSAPs must invoke DISCONNECT (p. 2-27). The design's ladder (§3.4) names only OPEN→DISC SENT/RCVD→MATCH→CLOSED — add the connect-side and DISC ACK states.
- CONNECT_REQ / ACCEPT_REQ carry up to **16 bytes of SYSAP data**; the Connection Managers use it to identify their VMS version and reject/break connections with disapproved versions (p. 2-25). A 16-bit reason code rides REJECT_REQ / DISCONNECT_REQ (p. 2-26). SCS control messages need no connection but do need a VC (p. 2-31).
- CDT fields (p. 2-28): local/remote SYSAP names, state, SB and PB pointers, connect data, VC-loss error handler, message/datagram input routines; **CONID low 16 bits = CDL index**; CDL size = SCSCONNCNT + 200 (p. 2-29/2-30). Messages carry destination CONID (copied by the SYSAP) and source CONID (port driver); datagrams get both from the port driver (p. 2-35). RSPID/RDT (p. 2-34): low 16 bits index the RDT; 0 means "no reply expected".
- Credits (pp. 2-43–2-45): Send Credit (remote's belief), Receive Credit (local mirror), Pending Receive Credit; piggyback in every message header's credit field (p. 4-13/4-14: `CREDIT | SCS MTYPE` word, then DEST/SRC CONID); **special credit message** when local Receive Credit < `SCSFLOWCUSH + remote Minimum Send Credits` (p. 2-44). Credit Wait queues CDRPs on the CDT (p. 2-45). The campaign's "8/9 credit pair" is the special-credit mechanism's projection; credit conservation (design R1 property test) is the book's invariant.
- Datagram buffer accounting per connection with DFREEQ/MFREEQ per port (p. 2-42/2-43).
- `SCS$DIRECTORY` / `SCS$DIR_LOOKUP` (pp. 2-48–2-50): LISTEN creates an SDIR + a *listening CDT*; one connect request at a time per SYSAP, otherwise "busy, try later"; the accepted connection gets a **separate** CDT (its CONID, not the listening one); the poller connects, asks per name, disconnects — a transient connection — every `PRCPOLINTERVAL`, polling all nodes, and stops polling a name on a node once connected to it. This validates the design's "transient-connection semantics" and its per-name affirmative/`NOT PRESENT` result.
- MTYPE taxonomy (p. 4-13/4-14): SCS MTYPE ∈ {application message, application datagram, SCS control}; PPD MTYPE ∈ {SCS message, SCS datagram, START/STACK/ACK, error-log datagram, node-stop datagram}; PPD supplies length from the PPD MTYPE field.

### 3.4 Connection Manager: identity, membership, transitions (FC-P3.x, P8)

- Two levels: **CNXMAN** (connectivity, SCS connections between `SYS$CLUSTER`s, reconnects, the membership list) and **CONMAN** (transitions, quorum, partitioning) plus QUORUM (quorum disk) (p. 7-3). The design's `vms_cnxman.c` covers both; naming the split inside it (`_csb.c`/`_recnx` vs `_join/_barrier/_coord/_quorum`) already matches.
- Rule of Total Connectivity (p. 7-1/7-2); a node is in at most one cluster (p. 7-2); current members have priority over joiners regardless of votes (p. 7-12, 7-29); a joiner is admitted only if every member has a CM connection to it (p. 7-11).
- **CSB** (p. 7-23/7-24): VOTES, EXPECTED_VOTES, QDSKVOTES, **LOCKDIRWT**, flags (member of some cluster / of the local cluster / removed; quorum-disk-name agreement; CLUSTER_SHUTDOWN notified; is-local), connectivity state ∈ {NEW, CONNECT, ACCEPT, OPEN, DISCONNECT, WAIT, RECONNECT, REACCEPT, DEAD, LOCAL}, pointer to the SB (and back), a **nodemap** of connectivity, SELECTED/MEMBER/REMOVED/SHUTDOWN flags, temporary/assigned CSID. Each CSB of a member also carries "a count of the total number of systems in that cluster", learned in the connectivity dialogue (p. 7-37).
- **CSV** (p. 7-25): CSID = `(sequence << 16) | index`; index = CSV slot (slot 0 never used); sequence starts at 1 and increments on each reuse of the slot; slots handed out round-robin; a departed system's slot keeps its sequence in the low 16 bits with the high 16 cleared; a rejoining system gets a **new CSB and a new CSID**. The lab's VAX1 `0x00010001`, VAX2 `0x00010002`, OVMX `0x00010003` are first-use slots 1–3 — exactly this rule. Nodemap bit = CSID low 16 bits (p. 7-34 fn).
- **CLUB** (p. 7-26): total votes, member count, CEVOTES, quorum, quorum-disk votes, foundation time, last-transition time, coordinator identity, transition phase, *proposed* vs *effective* quorum cells, nodemap, figure of merit, CLUSTER/LOST_CNX/ADJ_QUORUM/SHUTDOWN flags, head of the CSB queue, local CSB, CLUDCB pointer.
- **Coordinator**: "effectively random … very often the first VMS system to detect an event" (p. 7-2); must obtain permission — the **coordinator lock** — from every selected system; a system already granted to another refuses; collisions back off a random short interval (p. 7-32). For JOIN the **joiner selects** whom to ask: highest VAXcluster protocol level, then highest ECO level, then the CSB nearest the end of the CLUB's CSB queue (p. 7-37/7-38) — and it asks only once the number of members it has connectivity with equals the member count those CSBs advertise (p. 7-37). CLUSTER_SHUTDOWN uses no coordinator (p. 7-32). FORM requires the coordinator to have VOTES > 0 (p. 7-28, 7-33); zero-vote systems join after formation (p. 7-33).
- **Phase 1 / Phase 2** commit (pp. 7-33–7-49): Phase 1 message = nodemap, figure of merit, proposed quorum/CEVOTES/qdisk votes/qdisk-eligible flag, foundation timestamp, founder's SCSSYSTEMID, rebuild type; receivers validate (connectivity to every nodemap member, quorum-disk access, membership consistency, pending-rebuild extent, and for RECONFIGURE "can I see a better cluster" — a higher figure of merit rejects the proposal, p. 7-45) and ack or request abandonment; coordinator abandons on any rejection or connectivity loss; Phase 2 = commit, cannot be abandoned; Phase 2 tasks per §2.4; removed members get MEMBER cleared / REMOVED set, their CSV slot released, connectivity explicitly broken with a **disconnect reason that forces the removed node to crash** (p. 7-46). JOIN specifics: coordinator assigns the joiner's CSID (p. 7-39); describes each member `{SCSSYSTEMID, incarnation, CSID}` to the joiner and the joiner to each member, all must confirm connectivity (p. 7-39); the coordinator sends the joiner the **index + sequence of every unused CSV entry** so CSID assignment stays cluster-consistent (p. 7-39). Admission tests when the cluster lacks quorum (p. 7-38).
- **Reconnect**: after a non-last-gasp loss the CM retries once a second for max(local RECNXINTERVAL, remote-supplied value); LAN remote value = 16 s before V5.5, = remote **TIMVCFAIL** from V5.5; CI/SHAC = 3 × max(1, ⅔ TIMVCFAIL) (p. 7-30). Only if no other CM has started a transition does the local CM start one (p. 7-30).
- What the CM's own `%CNXMAN` events and SHOW CLUSTER read: MEMBERS class from CSBs (status + last change, VOTES/EXPECTED_VOTES/RECNXINTERVAL/QDSKVOTES/LOCKDIRWT, quorum-disk agreement, CM connection states); CLUSTER class from the CLUB (votes, members, quorum, quorum-disk name/votes, formation time, last transition) (p. 7-24, 7-26, pp. 8-32–8-34). These are the `CLUSTER_DIAG_CSB` / `CLUSTER_GET_CLUB` field lists.

---

## 4. Quorum and configuration (FC-P3.7, FC-P8)

| Item | Book | Cite |
|---|---|---|
| Members selected for the computation | all visible potential members minus those excluded by the connectivity rule or departing | p. 7-6 |
| CEVOTES | `NEW_CEVOTES = max{ EXPECTED_VOTES of selected members ; Σ VOTES of selected members ; OLD_CEVOTES }`, OLD = 0 until a cluster forms, then the last completed transition's value — quorum never decreases by itself | p. 7-6, p. 7-9 |
| QUORUM | `(NEW_CEVOTES + 2) / 2` (integer) | p. 7-6, p. 7-48 |
| Disruptive joiner | admission refused if it would raise quorum above available votes | p. 7-9, p. 7-29 |
| Figure of merit | `256 × V + N` per totally-connected subcluster; highest wins; ties random (usually the subcluster containing the first detector); quorum-disk votes count in V but the disk is not in N | p. 7-12, 7-13, 7-23 |
| Quorum-loss blocking | since V5.4: the **QUORUM capability bit** cleared in every CPU's capability mask so no process (whose PCB requires QUORUM) is scheduled; pre-5.4: hold IPL 4 | p. 7-10 |
| `SET CLUSTER/EXPECTED_VOTES` | clamps to `[V, 2V−1]`; REMOVE_NODE = implicit set to CEVOTES − removed votes; `SET CLUSTER/QUORUM Q` ⇒ `2Q−1` | p. 7-31, p. 7-47 |
| Quorum disk | DISK_QUORUM names it (adopted from another member if unset; cannot change without reboot; disagreement ⇒ no quorum disk); QDSKVOTES effective = min over potential watchers; needs ≥1 watcher; direct path required; not a shadow set; `QUORUM.DAT` in `[000000]`, accessed without the DLM | pp. 7-14–7-15, 7-21–7-22 |
| Watcher | local access mode when the file's start LBN is known; reads every QDSKINTERVAL; becomes a Watcher (disk *trustworthy*) after **4 consecutive** error-free read/write cycles; remote-mode nodes ask a random Watcher every QDSKINTERVAL; static vs dynamic quorum; formation may block 3–4 × QDSKINTERVAL | pp. 7-15–7-18 |
| CLUDCB states | NOT READY, READY, ACTIVE, CLUSTER, VOTE, REMOTE INACTIVE, REMOTE ACTIVE | pp. 7-26–7-27 |
| `QUORUM.DAT` content (semantic) | founder's SCSSYSTEMID; foundation timestamp; updater's CSID; updater's incarnation time; dynamic-quorum flag; updater's visible votes + quorum; activity counter (incremented per cycle by members; a would-be former must see it unchanged for 2 further reads) — layout **not** given | pp. 7-18–7-21 |
| VAXCLUSTER 0/1/2 | 0 no SYS$CLUSTER; 1 conditional (CI/DSSI/`NISCS_LOAD_PEA0`); 2 always | p. 7-4 |

The design's plan to gate the executive-owned seams (§3.7, FC-P8.1) is the honest analogue of the capability-bit model; name it as such and gate at the process-selection point (the book's mechanism is scheduler-level, not I/O-level).

---

## 5. Discrepancies and sharpenings (correct the design/plan here)

| # | Design/plan premise | Book | Cite | Action |
|---|---|---|---|---|
| D1 | D-DLM-1: "a LOCKDIRWT=0 node is never a directory node, never receives directory duty, triggers only a merge rebuild" | If **every** member has LOCKDIRWT = 0 the vector has one entry per system, so a 0 node **is** a directory node for ~1/N of root names, and its join is a **directory** rebuild | p. 6-32, p. 7-35, p. 7-40 | D-DLM-1 holds only if ≥1 VAX advertises LOCKDIRWT > 0 (FC-P3.2 decides). This is the likely reason the lab pushed 222 op-0d registrations at a LOCKDIRWT=0 OVMX: it *was* assigned directory duty. FC-P5.4 moves from "later" to a P3/P4 prerequisite whenever the lab VAXes run 0 |
| D2 | D-DLM-2: "when |W| = 1 there is nothing to resolve" | A lookup carries the requester's 16-bit hash and the directory node indexes its hash table with it; a wrong value plants a duplicate directory entry in the wrong chain | p. 6-49, p. 6-50 | Originating a lookup requires the true `hash16` even with one directory node. `body[10:12]` (`dir_hash`, sent 0) must never be sent for a name whose hash OVMX has not observed. Revise D-DLM-2 per §1.4 |
| D3 | Rung B (probe W[0], W[1]…) is safe if the protocol refuses mis-addressed lookups | A directory node creates an entry for any name it does not know, naming the requester master | p. 6-50, p. 6-31 | Rung B is **forbidden**; the ladder is A (IDSM) → observed-hash cache → honest `SS$_UNSUPPORTED` for unseen roots → C(i) operator ruling. §5.2(2)(b)'s capture is no longer needed to decide this |
| D4 | Operator direction / memory: "the op-0d rebuild from VAX1 tells OVMX which resources it is assigned → OVMX **masters** those" | A rebuild registration is the **master** registering its tree with the new **directory node**; the receiver stores a directory-entry RSB `{name → master = sender}`; it does not take mastery. A directory node masters a tree only in three cases | p. 6-33, p. 6-50, p. 6-51, p. 6-53 | Consuming op-0d = populate the directory table with the sender as master (the op-0d's `body[4]` holder handle is the master's lock, consistent). OVMX mastering those trees would create two masters. Correct FC-P5.3/P5.5 wording and the memory's "master THOSE" line; keep "OVMX genuinely serves directory duty and grants what it genuinely masters" |
| D5 | cat-02 op-01 inbound at OVMX ⇒ ENQ ⇒ grant from `vms_lock.c` | An op-01 to a **directory node** is a lookup-or-lock-request: answered by a grant only when the directory node is master (entry with master CSID 0); otherwise by the master's CSID (redirect), or by "you master it" + new entry | p. 6-31, p. 6-50, p. 6-51 | The 35/s re-request storm is the predicted symptom of answering redirects with grants. FC-P5.2 must split op-01 handling three ways by directory-table state; FC-P5.3's "decline for unassigned resources" becomes "create a directory entry naming the requester master" when OVMX is the directory node for the name (rung-A vector tells it) |
| D6 | §3.6 rebuild-type list (merge / directory / partial / full "on departures") | Exact selection rules per transition, including the all-zero and pending-rebuild cases; full only on FORM or when pending | p. 7-40, p. 7-44, p. 7-35 | Replace the list in §3.6 and FC-P5.5 with §2.3's table |
| D7 | §5.5 coordinator selection INFERRED "highest node number" | JOIN: the **joiner picks** by protocol level → ECO → CSB nearest the CLUB queue tail; then the pick must win the coordinator lock; departures: usually the first detector, random back-off on collision | p. 7-37/38, p. 7-32, p. 7-2 | FC-P3.12 selection predicate = "I was asked and I hold the coordinator lock"; FC-P3.3 must choose the join target by advertised protocol/ECO level (fields to pin in the MODEL/PARAMS burst — FC-P3.2 scope) and wait for `connected members == advertised count` before asking. FC-P8.3 tests protocol/ECO/queue order, not SCSSYSTEMID order |
| D8 | §3.4 "membership bitmap width undetermined; store ≥32 slots" | Nodemap bit = CSID low 16 bits = CSV slot; width = CSV size (round-robin slots, so > member count over time) | p. 7-25, p. 7-34 fn | Size the CLUB/CSB nodemaps by CSV slot count; instrument the max slot seen; do not equate width with member count |
| D9 | §3.4 CSID learned by matching own SCSSYSTEMID in membership records | Confirmed: the coordinator describes every member as `{SCSSYSTEMID, incarnation, CSID}`; also sends the joiner the unused-CSV `{index, sequence}` list | p. 7-39, p. 7-33 | Add the CSV (with per-slot sequence) to the CLUB model and a codec entry for the unused-slot list (FC-P3.1); a rejoin gets a **new** CSID (p. 7-25) — never reuse |
| D10 | Memory: "granted op-01 encodes OVMX's node-index in `req_lkid` low 16 bits" | Lock ID low 16 bits = Lock ID Table index; bits 16–23 XPAGE; 24–30 sequence | Fig. 6-21 p. 6-46, p. 6-45 | Re-derive: `0x00010003` reads as table entry 3, sequence 1 (mirrors the CSID rule, p. 7-25). FC-P4.5's "no builder accepts a literal lock id" already covers it; do not encode a node index into a lock id |
| D11 | §3.4 CDT ladder OPEN→DISC SENT/RCVD→MATCH→CLOSED | Full state set incl. CONNECT SENT/ACK/RECEIVED, ACCEPT SENT, REJECT SENT, DISC ACK; simultaneous-disconnect path | pp. 2-23–2-27 | Extend FC-P2.2's table |
| D12 | Honest OS identity in the HELLO (memory ruling) | The CMs exchange version identity in the 16-byte CONNECT/ACCEPT data and **reject or break** connections with disapproved versions | p. 2-25 | The honest-identity risk sits at the `VMS$VAXcluster` connect, not only the HELLO; FC-P3.3 must handle REJECT with reason and log it as the identity gate |
| D13 | Q2: is the CLUSTER_NODES count DLM-gated? | Member count and MEMBER flags are Phase 2 tasks done **before** the synchronized rebuild; the transition is "over" only after the rebuild and the coordinator releasing its lock | p. 7-42, p. 7-37 | Sharpen Q2: a CSB showing `member` with the count still 2 is anomalous relative to the book — FC-P5.1 should read SHOW CLUSTER's CLUSTER-class "total number of members" (p. 8-34) alongside `F$GETSYI("CLUSTER_NODES")` and pin which cell each reads |
| D14 | §3.4 RECNX loop "RECNXINTERVAL/TIMVCFAIL once per second" | Timeout = max(local RECNXINTERVAL, remote TIMVCFAIL) on LAN ≥ V5.5; the remote value is **advertised** by the peer | p. 7-30 | FC-P3.6 stores the peer's advertised value per CSB (a PARAMS field to pin, FC-P3.2) |
| D15 | FC-P4.5 codec fields | Requests to a master carry the sender's LOCKDIRWT and 8-s activity average; lookups carry hash16 | p. 6-38, p. 6-50 | Pin all three in the unassigned bytes of §4(f).1; FC-P4.6 fills them from real per-tree counters (§2.2) |
| D16 | FC-P5.5 rebuild FSM | Three-tree concurrency; abort dynamic remastery on transition start; graceful-shutdown pre-offload; value-block invalidation rule on master failure; new master learns the tree from survivors' process copies | p. 6-37, p. 6-36, p. 6-15, p. 6-53 | Add to FC-P5.5's phase list; the "survivors describe their locks to the new master" step is a message class FC-P5.2 must identify |
| D17 | FC-P3.7 VOTES=0 tracking-only, yet FC-P3.12/R2 expects an all-OVMX cluster to **form** | FORM needs a coordinator with VOTES > 0; zero-vote nodes only join | p. 7-28, p. 7-33 | The R2/R4 all-OVMX scenarios need ≥1 node with VOTES > 0 and the FORM flow (Phase 1 marked "form", full rebuild); either pull minimal FORM into P3.12 or seed simulator clusters from one voting node |
| D18 | FC-P8.2 REMOVE semantics | Removed node receives a disconnect reason that forces it to crash; last gasp closes the VC immediately | p. 7-46, p. 7-29 | OVMX on the receiving end must treat that reason as a fatal cluster exit (bugcheck-equivalent), not a reconnect |
| D19 | FC-P5.6 deadlock legs | Timestamp server per transition, 50 ms → 1600 ms lifetimes, Table 6-4 rules, EPID + best-candidate propagation, separate victim message, conversion search first | pp. 6-23–6-27 | Check `docs/design-dlm-distributed-deadlock.md` against these; the timestamp-server election is a CNXMAN transition output the design does not list |
| D20 | FC-P5.4 directory table "holder count" | A directory-entry RSB is released only when the master releases mastery (last lock dequeued) — a master→directory message | p. 6-50 | The directory table needs a "mastery released" op (FC-P5.2 to identify); OVMX as master must send it when its last lock in a tree goes |
| D21 | FC-P8.4 "do not invent QUORUM.DAT" | The semantic contents and the arbitration rules are published; only the layout is not | pp. 7-18–7-21 | Keep the LAB gate for the layout; the FSM (states, 4-cycle trust, activity counter, foreign-cluster decision) can be built and R1/R2-tested now |

---

## 6. What the transcript settles for each plan item (index)

| Item | Book-grounded now | Still capture/DOC-gated |
|---|---|---|
| FC-P1.2 VC FSM | START/STACK/ACK state table, retry rules, implied ACK, masquerade tests, VC-break-on-loss rule | sequence/ack field encodings, TIMVCFAIL cadence (ch. 3 absent) |
| FC-P2.2/2.3 SCS | full connection state set, CDT/CDL/CONID, credit algorithm + special credit rule, directory service + poller semantics, MTYPE taxonomy | opcode/format bytes |
| FC-P3.3 join FSM | join-target selection, wait-for-count precondition, Phase 1/2 content and validations, CSID/CSV assignment, admission tests | field offsets in MODEL/PARAMS/CONFIG (LOCKDIRWT, protocol/ECO, TIMVCFAIL, member count) |
| FC-P3.5/3.12 barrier/coordinator | coordinator lock, Phase 1 abandon rules, Phase 2 tasks, synchronized rebuild, lock release | mapping of op-09/0a/0b/0c to "wait until all ready → rebuild in unison" |
| FC-P3.6 CSB/recnx | ten states, flags, nodemap, reconnect timeout formula, DEAD-on-new-incarnation | — |
| FC-P3.7 / P8.1 quorum | all formulas, figure of merit, capability-bit blocking, EXPECTED_VOTES clamps | — |
| FC-P4.1/4.3 dir_resolve | vector + index rule + serving algorithm (rung A); hash **not** given | hash16 (IDSM or observed), member order |
| FC-P4.5 codec | three more request fields to pin (hash16, LOCKDIRWT, activity) | offsets |
| FC-P5.3/5.4 master/directory | grant-vs-redirect-vs-you-master rule, directory-entry lifetime, master-CSID-0 convention | op semantics table (FC-P5.2) |
| FC-P5.5 rebuild | type selection, per-type actions, sequencing, fault-tolerance model, 3-tree concurrency | message classes for registration, mastery release, survivor lock descriptions |
| FC-P5.6 deadlock | full timestamp/EPID/victim protocol | encodings |
| FC-P8.4 quorum disk | states, trust rule, arbitration, file semantics | on-disk layout |
