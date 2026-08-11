# Rejoin CM state map: OVMX's readmission against the book's JOIN protocol

**Status:** design record, `vms-f61` (2026-08-11). Written under the operator's standing
ruling to STOP wire byte-diffing the rejoin and instead map OVMX's connection-manager
behaviour against the documented SCA / connection-manager state machine, then implement
OVMX's own. Twenty wire-level increments (§4(O.11)–§4(O.20)) each proved
necessary-but-not-sufficient; the blocker is "probably something FUNDAMENTALLY missing" —
a CM protocol STATE or phase OVMX never performs.

Clean-room (CLAUDE.md Rule 8): every reference to *VAXcluster Principles* (Davis, 1993) is
by **page number only**. No book text is reproduced here, in code, in commits, or in the
spec. OVMX implements its own representation of the documented protocol.

---

## 1. The frontier this map starts from (GROUNDED, §4(O.20))

After `vms-a63`/#299 the wire delivery is clean:

- op 0x02 (the joiner's add-member request) rides the **member-initiated**
  `VMS$VAXcluster` connection (§4(O.11/12)), as SYSAP `sms=3` contiguous after
  model(1)+params(2) (§4(O.13)), on a contiguous per-VC `send_seq` (§4(O.14)), delivered
  LEAN (§4(O.16)), with the coordinator's per-VC `recv_seq` UNFROZEN and advancing past
  the config (§4(O.19)/§4(O.20)), in REJOIN form (`body[36:40]` = membership generation,
  §4(O.10)).
- **And the coordinator STILL returns `cm_responses=0`**: no `cat 0x04 op 0x04`
  reciprocation, no op 0x03 commit, no op 0x05 lock-rebuild, no op 0x06 barrier; it never
  opens a reciprocal connection (`connect_sent=0`, `remote_conid=0`); `CLUSTER_NODES`
  stays 2; `XITDONE=0`.
- **The load-bearing asymmetry:** on a FIRST join the coordinator's console logs
  `received VAXcluster membership request` + `proposed addition of node X`; on a REJOIN it
  logs ONLY `timed-out lost connection`. Same pod, same identity, same votes. The
  coordinator's connection manager **never registers a membership request** on a rejoin,
  even though op 0x02 is now provably delivered to it in-order.

§4(O.20) also established, by elimination across the whole series, that the
first-join-vs-rejoin discriminator is **NOT**: the op 0x02 body (§4(O.10)), connection
selection (§4(O.11/12)), SYSAP contiguity (§4(O.13)), per-connection `send_seq`
(§4(O.14)), coordinator-VC bloat (§4(O.15/16)), send credit (§4(O.17)–refuted §4(O.19)),
or quorum votes (same identity/votes admits on first-join). What is left is the
**member-side handling of a node that was a member before** — which is exactly what the
book's connection-manager chapter specifies, and what this map examines.

---

## 2. What the book says the member does — and does NOT do — for a returning node

### 2.1 There is NO separate "rejoin" protocol at the CM layer

The CSB connectivity-state machine (Davis pp. 7-23/7-24) defines the **NEW** state as "the
CSB has just been allocated" and states explicitly that a NEW CSB can represent *either* a
newly discovered connection manager *or* a connection manager in a system that left and is
returning — the two are the **same** state. The CSID discussion (pp. 7-24/7-25) is
categorical: when a system leaves and later rejoins, its **old CSB is deallocated and a new
CSB is created for it "just as if it were joining the cluster for the first time,"** and it
is assigned a **new CSID**. The old incarnation's CSB, if still present, is moved to the
**DEAD** state (p. 7-24: "a new incarnation of a VAX system has been seen; the CSB whose
connection state is DEAD represents the old incarnation").

**Consequence for OVMX.** The book grants a rejoin no distinct wire transaction. From the
member's connection manager, a returning node runs the **JOIN CLUSTER** state transition
(pp. 7-37 to 7-42) identically to a first-time joiner. OVMX's rejoin-specific apparatus —
`cm_rejoin_target_mode()`, `cm_apply_rejoin_form()`, the REJOIN-form op 0x02 with its
generation ordinal, the op-6 "credit-first" (already shown to be a mislabeled DISCONNECT_REQ
in §4(O.19)), the standalone-ack hold — models a protocol fork the book says does not exist
at this layer. Some of it is banked wire corrections (incarnation stamping, §4(O.20), is
real); some of it (op-6 credit-first, §4(O.19)) is inert. **None of it makes the member run
the JOIN transition, because the thing that gates the JOIN transition is not on the
joiner→coordinator op 0x02 at all.**

### 2.2 The JOIN completion gate is the Rule of Total Connectivity — a THREE-PARTY check

The book gates admission on connectivity with **every** current member, verified by the
coordinator against **all** members, not on a two-party joiner↔coordinator exchange:

- **Joiner precondition (p. 7-37).** A system attempts to join only after "the number of
  cluster members it has connectivity with equals" the member count carried in the CSBs it
  built. Only then does it send the join request — to the **one** member with the highest
  VAXcluster software protocol level (tie → highest ECO level → CSB nearest the end of the
  CLUB's CSB queue).
- **Admission definition (p. 7-29).** "A VAX system is permitted to join an existing
  cluster if its Connection Manager has an SCS connection with the Connection Manager in
  **each of the current members**, and if its admission would not disrupt quorum."
- **Coordinator verification — Rule of Total Connectivity (p. 7-39).** After the join
  request the coordinator acquires the coordinator lock from **each** selected member
  (p. 7-38/7-39), assigns the joiner a CSID, then runs a connectivity dialogue: it
  **describes each current member (SCS System ID, incarnation time, CSID) to the joiner,
  the joiner acknowledges connectivity with each; and it describes the joiner to each
  current member, and they must ALL report connectivity with the joiner.** "Otherwise,
  VAX_A abandons the state transition at this point" — BEFORE Phase 1 (the "proposed
  addition" message, p. 7-40).

The FORM-cluster flow (pp. 7-33/7-34) shows the same connectivity dialogue is a distinct
message round: VAX_A "sends messages describing each selected system to all the other
selected systems," each replies whether it has connectivity, and VAX_A records the reply in
a per-system nodemap and deselects any system that a peer cannot see.

**This maps the frontier exactly.** op 0x02 is the join request; the coordinator receives
it at SCS but ABANDONS before Phase 1 because the three-party connectivity gate is not
satisfied — which is precisely why the console logs neither `received VAXcluster membership
request` (a request it will abandon is not registered as an admitted one) nor `proposed
addition` (Phase 1 is never reached). The book even names the failure OVMX's own code
already fears (`scsd.c` `cm_response_shape` comment): "a joiner that fails to answer
something the coordinator gates on can strand a cluster-wide transition, which times out and
drops healthy members."

### 2.3 The grounded op-map that fits this reading

From the SUCCESS oracle choreography (§4(O.10/11)) and the vms-e81 grounding
(`scsd.c` ~line 6683):

| op | role in the JOIN transition | book page |
|---|---|---|
| op 0x14 / op 0x01 | joiner's model + params to EACH member; each member reciprocates its own within ~1 ms | 7-37 (connectivity established via the CSB dialogue) |
| op 0x02 | joiner's add-member / **join request** to the coordinator, ~110 ms after the LAST member reciprocates; its `ams` acks that reciprocal op 0x01 | 7-37 |
| op 0x04 | coordinator's response beginning the admission (connectivity-describe / Phase 1 "proposed addition") | 7-39 / 7-40 |
| op 0x03 | Phase 2 **commit** ("point of no return") | 7-41/7-42 |
| op 0x05 | lock-management database rebuild | 7-42 |
| op 0x06 | rebuild **barrier** (the "absolute unison" synchronization) | 7-42 |

The vms-e81 comment grounds the pre-op-0x02 rule directly from the reference wire: **the
joiner sends model+params to *each member it has a VC with* and emits op 0x02 only after the
LAST member reciprocates.** op 0x02 is data-driven on **all** members having answered — the
joiner-side face of the Rule of Total Connectivity.

---

## 3. The candidate missing state (to be confirmed / refined by the wire)

**Hypothesis (book-grounded, §2.2):** on a rejoin OVMX drives the readmission as a
TWO-party exchange with the coordinator and does not establish/prove `VMS$VAXcluster`
connection-manager connectivity with the **non-coordinator** member(s). The coordinator's
Rule-of-Total-Connectivity verification (p. 7-39) — "describe the joiner to each current
member, they must all report connectivity" — therefore fails for the non-coordinator, and
the coordinator abandons before Phase 1. On a FIRST join the fresh, uncomplicated topology
happens to satisfy this; on a REJOIN the non-coordinator's residual/DEAD CSB and the
never-completed connectivity dialogue with it leave the gate unsatisfied.

This is the exact shape of "a CM phase OVMX never performs": not another byte in op 0x02,
but **the joiner's participation in the coordinator's clusterwide connectivity verification
with ALL members**, which OVMX only ever exercises against the coordinator.

## 4. Wire confirmation (GROUNDED this session, `vms-f61`)

### 4.1 The SUCCESS oracle readmission IS 3-party and MEMBER-DRIVEN

Flow analysis of `vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` (VAX3 rejoiner
`08:00:2b:11:22:33`; VAX1 `aa:00:04:00:01:04`; VAX2 `08:00:2b:78:56:b9`) shows **two
complete, independent, symmetric bring-up sequences run sequentially**, one per member:

- **VAX1↔VAX3** (f1176–1218): **VAX1 opens** the `VMS$VAXcluster` connect (f1200) → VAX3
  answers as TARGET (f1201) → VAX1 op00 (f1202) → **VAX3 op02** (f1204) → **VAX1 op03**
  (f1206) → op06/op07 (f1213–1217).
- **VAX2↔VAX3** (f1224–1263): **VAX2 opens** (f1246) → VAX3 answers (f1247) → VAX2 op00
  (f1248) → **VAX3 op02** (f1250) → **VAX2 op03** (f1252) → op06/op07 (f1259–1262).
- **Then the two established members reconcile the new membership with each other** via
  `op0c` (f1308 VAX1→VAX2 70 B, f1312 VAX2→VAX1 526 B) amid all-pairs `op0a`.

VAX3 originates **no** outbound `VMS$VAXcluster` connect anywhere; both members open toward
it and VAX3 answers each as TARGET. VAX3 sends op02 to **VAX1 first**, then VAX2, and **both
members independently run the full op02/op03 exchange.** Admission requires VAX3 to be
independently admitted by **both** members and then reconciled between them (`op0c`) — a
three-party protocol, exactly the Rule of Total Connectivity (p. 7-39) and the FORM/JOIN
connectivity dialogue (pp. 7-34/7-37/7-39).

**Corrected opcode reading (supersedes the §4(O.10) vocabulary for this window):** there is
**no distinct `op04` value at abs[60]**. The member's "reciprocation" the daemon waits for
is **`op03` — the member's COMMIT on the connection *it* opened** (Phase 2, "point of no
return," pp. 7-41/7-42). op06/op07 close the per-member handshake; `op0c` is the
member↔member membership reconciliation.

### 4.2 The OVMX rejoin — reaches both members, admitted by neither (GROUNDED, daemon summary)

Same identity/pod first-join-vs-rejoin bracket (`vaxlab-1`, identity `OVMA60`,
`scsd-a63V1.log` first-join vs `scsd-a63V2rejoin.log` rejoin), read from the daemon's own
per-peer exit summary:

| per-peer counter | first-join (SUCCESS, `XITDONE=1`) | rejoin (FAIL, `XITDONE=0`) |
|---|---|---|
| `cm_responses` (M1026 / VAX1) | **76 / 151** | **0 / 0** |
| `cm_config` | YES | no |
| `vaxcluster_member` | CONNECTED(reached-OPEN) | no |
| VC / channel / dir | OPEN / UP / YES | OPEN / UP / YES |

Both members reciprocate the CM dialogue on the first join and **neither** does on the
rejoin, with the **same identity on the same pod**. OVMX *does* reach both members at
VC/channel/directory level and *does* drive op-6 + op 0x02 REJOIN toward both — so the
"OVMX only talks to the coordinator" hypothesis (§3) is **REFUTED**. The failure is that the
members will not engage the CM JOIN dialogue for a **returning** identity.

### 4.3 Candidate causes REFUTED this session (kept, so they are not re-chased)

- **Quorum self-suspend — NOT the cause.** OVMX logs `present_votes 1→0 → quorum LOST
  (suspend + wait)` on the rejoin — but the **identical** benign 1→0 flip appears on the
  SUCCESSFUL first-join (`scsd-a63V1.log` 02:32:34.028→.102), which then reaches `XITDONE`
  170 ms later. The quorum flip is a transient, not the blocker.
- **`remote_conid=0x00000000` / `connect_sent=0` — NOT anomalies.** Both appear on the
  successful first-join's per-peer summary too. OVMX answers member-driven connects
  (`CONNECT_REQ=39 received, connect_sent=0`) on both paths; that is the oracle's topology
  (members open), not a defect.
- **Stale `incarnation_time` — NOT the live cause.** `ovmx_incarnation_time()` reads
  `CLOCK_REALTIME` at nanosecond resolution per process start, so every rejoin presents a
  **fresh** [66:74] incarnation. The vms-2f3 live-incarnation fix is present and working;
  it is necessary-but-not-sufficient. (Its own code comment, `scs_start.c:155-164`, records
  the abandon that a *frozen* incarnation caused — the member reusing OVMX's old System
  Block and its VC sequence state — which no longer applies since the value is now live.)
- **"OVMX ignores the non-coordinator" — REFUTED** (§4.2): OVMX reaches and drives both.

## 5. Where the frontier stands, and the responsible next step

The rejoin blocker is the same one §4(O.20) reached, now grounded from two more angles: with
the wire delivery clean (recv_seq unfrozen, op 0x02 delivered in-order, REJOIN form,
incarnation fresh, both members reached), **the established members still run zero of the
member-driven per-member op02/op03 JOIN handshakes for a returning identity** — they
reconcile *among themselves* (`op0c`) but do not admit OVMX. On a first join, with an
identity the cluster has never held, both members run the full handshake and OVMX joins.

Per the book this is the member deciding, for a node whose CSB it has held, between
**re-admitting it as a fresh JOIN** (p. 7-24 DEAD → p. 7-25 "a new CSB is created for it just
as if it were joining the cluster for the first time") and treating the return as some other
transition. **What OVMX must present so that every member runs the fresh per-member JOIN
handshake — not just the coordinator, and not a member↔member reconciliation that excludes
OVMX — is the open frontier.** The oracle's shape names the target precisely: OVMX must
elicit, from **each** member independently, that member opening its `VMS$VAXcluster`
connection to OVMX and driving op00→(OVMX op02)→**op03 commit** on it, culminating in the
members' `op0c` reconciliation *including* OVMX.

**Discipline (why no speculative wire change ships here).** Every remaining lever is a wire
behaviour toward a live VAX, and OVMX's own code records that an ungrounded control frame in
this exact region has crashed real VAX nodes and frozen `recv_seq` (`scsd.c`
`cm_response_shape`; the `vms-760` runt precedent; §4(O.19)/§4(O.20)). The next increment must
be an **isolation experiment**, not a guess: instrument, per member, whether that member
opens a fresh `VMS$VAXcluster` connect to OVMX after OVMX's fresh-incarnation START and
whether OVMX's answer reaches the state on which the member gates its op02/op03 — i.e. pin
whether the member is running a JOIN transition for OVMX at all, or a member↔member
transition that leaves OVMX out. That experiment, grounded against the oracle's two
per-member handshakes, is what turns "the members do not engage" into a named, fixable state.

### 5.1 Live lab proof of the relocated frontier (`vms-f61`, `vaxlab-0`, 2026-08-11)

A first-join→rejoin bracket on a fresh CN_2 pod, **one identity `OVXF61`/1961 throughout**,
the static READMITMAP daemon (`md5=4a3977f0…`, staged+verified in-pod), `OVMX_JOIN_SEQ=1`,
hard `timeout` on every op:

| arm | scenario | `cm_responses` (VAX1 / node-2) | READMITMAP verdict | incarnation presented | `XITDONE` |
|---|---|---|---|---|---|
| A1 | first join (no sidecar) | **186 / 40** | both **ADMITTED**; SUMMARY `admitted=2 no_engage=0` | `0x00bc0cccd00e23cc` (live) | **1** (CN_3) |
| B2 | rejoin, same id | **0 / 0** | both **NO-ENGAGE**; SUMMARY **RETURNING-IDENTITY NON-ADMISSION** `admitted=0 no_engage=2` | `0x00bc0ccd1991e23a` (live) | **0** (stays CN_2) |

The bracket confirms the frontier exactly: with the **same identity on the same pod** and a
**distinct live incarnation** presented on each arm (so a fresh incarnation reached both
members), the first join admits cleanly (both members ran the per-member JOIN handshake) and
the rejoin elicits **zero** CM responses from **both** members (`member_vc=DISC SENT`). The
`READMITMAP-SUMMARY` names the state directly — `RETURNING-IDENTITY NON-ADMISSION: every
reached member ran ZERO per-member JOIN handshakes`. This is the §4(O.10)/§4(O.20) member
non-reciprocation, now (a) grounded to the per-member JOIN-handshake count, (b) proven not to
be an incarnation-freshness problem (both arms carry live, distinct incarnations), and (c)
surfaced as a single verdict line for the next isolation. `XITDONE` did NOT flip — as expected;
this increment MAPS the missing state and instruments it, it does not fix it.

Evidence (host, tank volume): `/data/training/vax/k8s-labs/vaxlab-0/logs/scsd-f61-A1firstjoin.log`
(XITDONE=1, READMITMAP admitted=2) and `scsd-f61-B2rejoin.log` (XITDONE=0, READMITMAP
no_engage=2), with `d94-f61-A1firstjoin.pcap` / `d94-f61-B2rejoin.pcap` (identity `OVXF61` on
the wire both arms).

### 5.2 First increment shipped by `vms-f61`

A **safe, log-only, kill-switched** readmission diagnostic (`SCSD-I-READMITMAP`,
`OVMX_NO_READMITMAP=1` to silence) that emits, at exit, per member: whether OVMX presented a
fresh incarnation, whether that member opened a `VMS$VAXcluster` connect to OVMX, the
per-member `cm_responses` / `op03`-commit count, and the readmission verdict. It ships no
wire change (guard 8: the working first-join path is byte-unchanged), with a fail-pre/pass-post
unit test, and a live lab bracket re-grounding the relocated frontier (below). It exists so
the next isolation is one log line, not another pcap dig.

---

## 6. The suppressing state ISOLATED — the member's residual per-identity CSB (`vms-4dd`, spec §4(O.22))

§5 named the frontier ("a member NO-ENGAGEs a RETURNING identity that a FRESH one is ADMITTED")
and left the *cause* as the open question. `vms-4dd` isolates it with a single-factor lab
bracket and grounds it to the book, shipping no wire change.

### 6.1 The controlled bracket (GROUNDED, `vaxlab-1`, 2026-08-11)

A **freshly-booted** lab-2 pod (deleted + recreated so VAX1/VAX2 hold a CLEAN member state —
`SHOW CLUSTER` = VAX1/VAX2 only, no residual OVMX entries), the `vms-4dd` READMITMAP daemon
(`md5=e824f3c1…`, staged+verified in-pod), **`OVMX_JOIN_SEQ=1`**, a hard `timeout` on every op,
three consecutive arms — **all in first-join op02 form (no sidecar)** so the op02 wire shape is
held CONSTANT across arms — with two brand-new identities minted for the run
(`OVX40`/1973, `OVX50`/1974, never used on any pod):

| arm | identity | member holds a residual CSB for it? | `cm_responses` (VAX1/VAX2) | READMITMAP | `XITDONE` |
|---|---|---|---|---|---|
| A `4dAf` | `OVX40` (FRESH) | **no** | **196 / 39** | admitted=2 | **1** (CN_3) |
| B′ `4dBr` | `OVX40` (RETURN) | **yes** (created by arm A) | **0 / 0** | non-admission | **0** |
| C `4dCf` | `OVX50` (FRESH) | **no** | **196 / 40** | admitted=2 | **1** (CN_3) |

**The flip.** Same pod, same daemon, same op02 form, `OVMX_JOIN_SEQ=1`, run back-to-back. The
ONLY factor that differs between B′ (fails) and C (succeeds) is **whether the member already
holds a CSB for the presented SCSSYSTEMID**. Flipping just that factor flips the member from
NO-ENGAGE to ADMITTED. The member-side suppressing state is therefore the **residual
per-identity CSB the member retains for a node it saw before** — not the op02 body, not the
incarnation freshness, not quorum, not "only-coordinator" (all held constant or already
refuted).

### 6.2 The suppressing state is DIRECTLY OBSERVABLE in real VMS `SHOW CLUSTER`

The bracket makes the residual CSB visible on the oracle's own console:

- **Before arm A:** `SHOW CLUSTER` on VAX1 lists VAX1/VAX2 only.
- **After arm A** (`OVX40` joined to CN_3, then the daemon exited): VAX1 logs
  `%PEA0, Port has Closed Virtual Circuit - REMOTE NODE OVX40` and `SHOW CLUSTER` now lists
  **`OVX40 | VMX V0.1 | BRK_NON`** — a retained, broken, non-member CSB. The member does NOT
  free it on VC loss.
- **After arm B′** (the RETURN attempt): the same entry advances to **`OVX40 | … | BRK_NEW`**
  and the VC closes again — the member re-`NEW`s the *residual* CSB instead of building a fresh
  one and running JOIN.
- On a **hammered** pod (`vaxlab-0`), prior identities `OVXF61` and `OVX460` were observed stuck
  in `BRK_NEW` for **hours**, and on that pod even a FRESH identity (`OVX40` arm, `4ddA`) drew
  `NO-ENGAGE` (0/0) — the accumulation of un-reclaimed residual CSBs degrades admission for
  everyone. This is why the clean bracket required a freshly-booted pod, and it shows the
  residual CSB is effectively **permanent** for OVMX identities: the member does not age it out.

`BRK_NON` (broken, non-member) and `BRK_NEW` (broken, new) are the real VMS `SHOW CLUSTER`
connection-state strings for that node's CSB — the oracle naming its own retained state.

### 6.3 Book grounding (Davis, page cites only — clean-room Rule 8)

- **p. 7-24 (DEAD) + p. 7-25 (CSV).** A member keeps a CSB for every node it "has had"
  connectivity with; on departure the CSB is **retained** (only the CSV entry is released), and
  **on rejoin the old CSB is deallocated and a fresh one built "just as if it were joining for
  the first time," with a new CSID.** The trigger to move the old CSB to **DEAD** is the member
  observing a **new incarnation** of that SCSSYSTEMID. A FRESH identity has no residual CSB to
  collide with, so a NEW-state CSB is built and JOIN proceeds; a RETURNING identity collides
  with the retained CSB and is admitted only once the member has moved it DEAD → deallocated →
  rebuilt. `BRK_NON`/`BRK_NEW` are exactly this retained-but-not-reclaimed CSB.
- **pp. 7-33 / 7-39.** A member matches an arrival to a CSB by **both SCSSYSTEMID and software
  incarnation time**; a return presents the same SCSSYSTEMID with a *changed* incarnation time,
  and that mismatch against the stale CSB is what constitutes "a new incarnation has been seen."
- **pp. 7-40 / 7-41.** A join is **abandoned** if a more extensive rebuild (e.g. the prior
  membership's removal) is still pending, or if a member still carries the departing node as a
  member — a concrete "the leave must finish before the return is admitted."

### 6.4 The OVMX-side gap, and the fix design (relocated frontier)

OVMX already presents a **fresh incarnation on every run** (`incarnation_presented` is distinct
each arm — `0x…6c62`, `0x…5b38`, `0x…7d9b`), so §4.3's "stale incarnation" is not the gap. Yet
the member never reclaims `OVX40`'s residual CSB: it holds it `BRK_NON` after departure and
re-`NEW`s it to `BRK_NEW` on return rather than running the DEAD → deallocate → fresh-JOIN path
(pp. 7-24/7-25). So the OVMX gap is one of **two** candidates, and separating them is the next
single-factor isolation (both are wire-region changes and neither ships speculatively — the
`vms-760` runt precedent):

- **(F1) Unclean leave.** OVMX's departure is a bare VC break (`Port has Closed Virtual
  Circuit`), not a CM leave, so the member never runs the removal reconfiguration (evict →
  `REMOVED` → break connectivity, p. 7-46) and the CSB is retained. *Fix:* on daemon exit, drive
  the CM leave so the member reclaims the CSB; *test:* a clean leave drops `OVX40` from
  `SHOW CLUSTER` (no `BRK_NON`), and a subsequent return then admits.
- **(F2) Incarnation not read on the return path.** The member may not be reading OVMX's fresh
  incarnation in the config/START field it matches against the stale CSB (pp. 7-33/7-39), so it
  never sees "new incarnation" and never moves the CSB to DEAD. *Fix:* present the incarnation in
  that exact field on the return; *test:* the member moves the residual CSB DEAD → deallocates →
  runs JOIN (`cm_responses>0`, `XITDONE` 0→1).

**Relocated frontier (for the next increment):** instrument, on a RETURN against a member that
holds a `BRK_NON`/`BRK_NEW` CSB for the identity, (a) whether a clean CM leave at prior-departure
makes the member reclaim the CSB (F1), and (b) whether the member's config/START dialogue on the
return carries OVMX's incarnation to the CSB match (F2). One of those two flips `BRK_NEW` →
reclaimed → JOIN; the bracket that names which is the fix.

### 6.5 What `vms-4dd` ships (no wire change)

1. This isolation record (§6) and the spec §4(O.22) note.
2. A **verdict refinement** to the READMITMAP diagnostic: `ADMITTED` now requires the membership
   latch **AND** `cm_responses > 0`. Arm B′ exposed a per-member false positive — VAX1's CDT
   reached OPEN off a residual/partial reconnect (`vaxcluster_open_reached` latched) while it ran
   ZERO CM handshakes (`cm_responses=0`, `XITDONE=0`), and the latch-only classifier reported
   that member `ADMITTED`, masking the RETURNING-IDENTITY NON-ADMISSION. A member that sent 0 CM
   responses did not run the per-member JOIN handshake this run; it now reads `NO-ENGAGE`
   (INV-6: never report success from residual state). Log-only, kill-switch `OVMX_NO_READMITMAP`
   unchanged, byte-unchanged on the wire (guard 8), with a fail-pre/pass-post unit case in
   `tests/vmsscs/test_scsd_wire.c`.

Evidence (host, tank volume): `/data/training/vax/k8s-labs/vaxlab-1/logs/scsd-4dAf.log`
(`XITDONE=1`, admitted=2), `scsd-4dBr.log` (`XITDONE=0`, returning-identity non-admission),
`scsd-4dCf.log` (`XITDONE=1`, admitted=2), with the matching `d94-4dAf/4dBr/4dCf.pcap`
(`OVX40`/`OVX50` on the wire) and the console `SHOW CLUSTER` transitions in `vax1.log`.


## 7. F1 vs F2 bracketed — both refuted; the frontier relocates to the CM JOIN transition (`vms-944`, spec §4(O.23))

§6.4 left two candidate causes for why the member does not readmit a returning OVMX identity —
**(F1)** unclean leave (the member never runs removal reconfiguration, so the CSB is retained) and
**(F2)** the member does not read OVMX's fresh incarnation in the field it matches against the stale
CSB. `vms-944` brackets them one factor at a time on a freshly-booted `vaxlab-1`, observed through
**SDA** (VAX1 parked in `ANALYZE/SYSTEM`, `SHOW CLUSTER` per arm) rather than the coarser console
`BRK_NON`/`BRK_NEW` strings §6 relied on. Both hypotheses are refuted, and the SDA view overturns
§6.4's reading of the console strings.

### 7.1 The bracket (GROUNDED, `vaxlab-1`, clean CN_2, 2026-08-11)

One identity `OVX940`/1973, first-join op02 form held constant, `OVMX_JOIN_SEQ=1`, hard `timeout`
per op, `csbwatch.sh` (VAX1 in SDA sampling the CSB for `OVX940`):

| arm | what | member's CSB for `OVX940` (SDA) | `XITDONE` |
|---|---|---|---|
| B1 `944B1f` | FRESH join, live incarnation | CSB built at **`879EBA40`**, Incarnation **11-AUG-2026 05:18:23**, Flags `02060002 member,selected,status_rcvd`; console logs `received VAXcluster membership request from node OVX940` | **1** |
| (departure) | daemon exits — bare VC break, no CM leave | CSB `879EBA40` → State `09 wait`, Flags `06040005 long_break,**removed**,status_rcvd,send_status` | — |
| B2 `944B2r` | RETURN, `OVMX_INCARNATION_TIME=52975296000000000` = **1-OCT-2026** | old CSB `879EBA40` **DEALLOCATED**; **NEW CSB `879EBE00`**, State `01 open`, Incarnation **11-AUG-2026 → 1-OCT-2026**, Flags **`00000000` (non-member)**; NO membership request through T+42s+ | **0** |

### 7.2 F1 is refuted — the member DOES run removal reconfiguration

The B2 **T-PRE** sample (before B2's daemon starts, i.e. purely the state B1's ordinary departure
left) shows CSB `879EBA40` already carrying **`removed`**. Only the removal reconfiguration clears
the MEMBER flag and sets REMOVED (§3 / ch7-part03 p. 7-30). OVMX sent no CM leave — a bare VC break —
and the member ran removal anyway, exactly as the book says it must after a non-last-gasp VC loss:
reconnect for RECNXINTERVAL, then reconfigure regardless (p. 7-29/7-30). And p. 7-25 makes CSB
retention the NORMAL post-departure state for **any** leave (a clean `SHUTDOWN` retains the CSB too;
only the CSV entry is released). So "unclean leave → CSB retained" never separated the failing arm
from the passing one. **F1 dead.**

### 7.3 F2 is refuted — the member DOES read our incarnation and reclaims the CSB

Pinning the return's incarnation to **1-OCT-2026** — ~7 weeks after the 11-AUG join, impossible to
confuse — makes any member read of `[66:74]` unmissable. The SDA oracle showed it plainly: the CSB's
stored **Incarnation advanced 11-AUG-2026 → 1-OCT-2026**, and the CSB was **deallocated and rebuilt at
a new address** (`879EBA40` → `879EBE00`). That is Davis's rejoin reclaim verbatim: p. 7-24 (**DEAD**:
"a new incarnation of a VAX system has been seen") → p. 7-25 ("the old CSB is deallocated, and a new
CSB is created for it just as if it were joining for the first time"). **The member reads OVMX's
incarnation and reclaims. F2 dead — the read is directly observable.**

This overturns §6.4's reading: the console `BRK_NON → BRK_NEW` §6 interpreted as "the member re-`NEW`s
the *residual* CSB rather than building a fresh one" is, under SDA, a **freshly-allocated NEW-state
CSB at a new address** — the reclaim, not a re-NEW. `NEW` is exactly the state a just-allocated CSB
takes (p. 7-23), and it represents "a system that left and is returning" as well as a newly discovered
one — the same state either way. §6's console-only view could not see the address change; SDA can.

### 7.4 The relocated frontier — reclaim succeeds, the CM JOIN transition does not run

With the CSB reclaimed (new address), OVMX's fresh incarnation read into it, and the SCS connection
re-`open`, the rebuilt CSB **stayed non-member (`Flags 00000000`) for the whole arm** and **no
`received VAXcluster membership request` was logged** — the **CM JOIN state transition never ran**
(`XITDONE=0`). The gate is neither the CSB nor the incarnation; it is the JOIN transition itself, the
three-party Rule of Total Connectivity (pp. 7-37/7-39) that §2.2 already named as the completion gate.
This converges the two readings: §2.2 (the JOIN completion gate) and §6 (the residual CSB) resolve to
one frontier — **the member rebuilds a fresh CSB for the returning node exactly as the book says, then
does not run JOIN for it.** The next isolation instruments the three-party connectivity dialogue on the
return: which of the describe/acknowledge steps (coordinator↔joiner, coordinator↔other-member) the
returning identity fails, that a fresh identity passes.

### 7.5 The one open sub-question, and what `vms-944` ships

**Open sub-question.** B2 pinned a *future* incarnation to make the read visible. Whether a REALISTIC
(near-now, slightly-later) incarnation ALSO triggers the reclaim — settling whether §6.4's "never
reclaims" was purely a console-string artifact or whether the reclaim needs a coarse incarnation
delta — is the one remaining single-factor arm: a realistic-incarnation return under the same SDA
observation. It does not move the frontier (§6's `4dBr`, realistic incarnation, already gave
`XITDONE=0`); it only settles the mechanism label on the console strings.

**What `vms-944` ships (no wire change).** (1) This record (§7) and the spec §4(O.23) note. (2) A
verdict refinement splitting **`RECLAIMED-NOJOIN`** (SYSAP re-opened but `cm_responses==0`) out of
`NO-ENGAGE`, so the reclaim-succeeded / JOIN-did-not-run case is named honestly instead of masked as a
"residual/stale reached-OPEN" (which §6's verdict comment assumed and B2 refuted). Still a non-admission
(never `ADMITTED`; INV-6). Log-only, byte-unchanged on the wire (guard 8), kill-switch
`OVMX_NO_READMITMAP` unchanged, with a fail-pre/pass-post case in `tests/vmsscs/test_scsd_wire.c`.

Evidence (host, tank volume): `/data/training/vax/cluster/work/944B1f.csb` / `944B2r.csb` (the SDA CSB
timelines above), `944B1f.status` (`XITDONE=1`) / `944B2r.status` (`XITDONE=0`), and
`/lab/k8s-labs/vaxlab-1/logs/scsd-944B1f.log`/`scsd-944B2r.log` + `d94-944B1f/944B2r.pcap`. Wire
`[66:74]` incarnation distinct per run (arm A `0x00bc0cd311bc6c62`, arm B′ `0x00bc0cd380815b38`).
*VAXcluster Principles* pp. 7-23/7-24/7-25/7-29/7-30/7-37/7-39.


## 8. Sub-question settled, and the CM JOIN gate bisected on the wire — OVMX DRIVES op 0x02, the coordinator ABANDONS it (`vms-0425`, spec §4(O.24))

§7 relocated the frontier to "reclaim succeeds, CM JOIN does not run" and left ONE open
sub-question: §7's reclaim was demonstrated with a **blatantly future** incarnation pin
(1-OCT-2026, chosen for SDA day-resolution visibility). Does a **realistic near-now**
incarnation — what OVMX presents in normal operation — ALSO trigger the reclaim, or is the
reclaim an artifact of a coarse incarnation delta (in which case the true frontier would be
"make the member see a new incarnation", not the JOIN transition)? `vms-0425` settles it with a
same-boot single-factor bracket, then bisects the JOIN gate itself on the wire.

### 8.1 Sub-question SETTLED — a realistic incarnation reclaims identically to the future pin (GROUNDED, `vaxlab-1`, clean CN_2, 2026-08-11)

Two first-join→return pairs on ONE freshly-booted pod, two brand-new identities, first-join op02
form held constant, `OVMX_JOIN_SEQ=1`, hard `timeout` per op, VAX1 parked in SDA sampling each
identity's CSB (`csbwatch.sh`). The two pairs differ in **exactly one factor**: the incarnation
the RETURN presents.

| pair | arm | identity | return incarnation | member's CSB for the identity (SDA) | `XITDONE` |
|---|---|---|---|---|---|
| R (realistic) | `0425Rf` | `OVXR40`/1975 | — (first join) | CSB built `8796AE80`, advances to `member,selected` (02060002) | **1** |
| | `0425Rr` | `OVXR40` | **default live (near-now)** | old CSB `8796AE80` DEALLOCATED → **NEW CSB `879DF540`**, `1-JAN-2001` ref-time (a fresh CSB), Flags `02040000 status_rcvd` (**non-member**) | **0** |
| F (future) | `0425Ff` | `OVXF40`/1976 | — (first join) | CSB built `879DEF00`, advances to `member,selected` | **1** |
| | `0425Fr` | `OVXF40` | **`1-OCT-2026`** (`OVMX_INCARNATION_TIME=52975296000000000`) | old CSB `879DEF00` DEALLOCATED → **NEW CSB `879DF2C0`**, Incarnation visibly `11-AUG-2026 → 1-OCT-2026`, Flags `02040000 status_rcvd` (**non-member**) | **0** |

**The flip that is NOT there.** The realistic return (`0425Rr`) and the future-pinned return
(`0425Fr`) produce **byte-identical CSB outcomes**: the member DEALLOCATES the residual CSB and
REBUILDS a fresh one at a new address (a genuine reclaim, Davis pp. 7-24/7-25), which reaches
`02040000 status_rcvd` — the **exact** intermediate state a SUCCESSFUL first join passes through —
and then **stalls there, never advancing to `member,selected`**. The only observable difference is
the SDA-displayed Incarnation *date* (`11-AUG` vs `1-OCT`); the reclaim, the stall, and `XITDONE=0`
are the same. **A realistic incarnation reclaims exactly as the future pin does — the reclaim needs
no coarse delta.** §6.4's "never reclaims" was a `SHOW CLUSTER`-string artifact (SDA sees the
address change the console strings could not). The sub-question is closed: **the frontier does NOT
relocate; it stays at the CM JOIN transition.**

### 8.2 The CM JOIN gate bisected — OVMX DRIVES op 0x02; the coordinator does not START the ADD

Both the SUCCESS first-join (`0425Rf`) and the return (`0425Rr`) were captured (pcap, in-pod
`br0`). Decoding the CM (config/membership) frames on the `VMS$VAXcluster` connection — category
`body[8]`, opcode `body[9]`, per `scs_member_parse` — with the SAME identity on the SAME pod
(OVMX `b2df5fa41233`, VAX1 `aa0004000104`, VAX2 `08002b45161a`, VAX2 = the coordinator OVMX
selects) gives the discriminating diff:

| step | first join `0425Rf` (`XITDONE=1`) | return `0425Rr` (`XITDONE=0`) |
|---|---|---|
| OVMX → coordinator op 0x02 (join request) | **sent** (t≈29.57s) | **sent** (t≈31.47s) — *OVMX drives op 0x02 on the return too* |
| coordinator → other member op 0x12 RELAY | **sent** (relays the new member) | **absent** |
| coordinator → OVMX op 0x03 COMMIT | **sent** → OVMX 0x81-responds (`cm_responses++`) | **absent** (`cm_responses=0`) |
| what the coordinator does instead | op 0x05 lock-rebuild, op 0x06 membership burst → OVMX joins | a member↔member **cat-0x06 MEMBERSHIP** reconcile with the OTHER member that EXCLUDES OVMX, then a transition **ABORT** (op 0x04) |
| OVMX's own `VMS$VAXcluster` VCs at exit | OPEN | **CONNSTUCK in DISC SENT** (`still-open-at-exit=2`) |

**This bisects the either/or the frontier posed.** It is **not** that OVMX omits op 0x02 — OVMX's
own log confirms it drives the deferred op 0x02 in REJOIN form to the coordinator
(`SCSD-I-CMCONFIG2 … sent DEFERRED op 0x02 … to node 2 … expect its 0x04 ack then its op 0x03
COMMIT`) and correctly addresses it. And the member is **not** merely "waiting silently" — the
coordinator RECEIVES op 0x02 and actively runs a *different* transition (member↔member reconcile +
abort), starting **no** ADD for OVMX. Per Davis p. 7-38 the coordinator, on receiving a join
request, runs admission/quorum tests and **"ignores the request … and no state transition occurs"**
when they are not satisfied; the FIRST join satisfies them, the RETURN (against a member that has
just reclaimed OVMX's CSB) does not. The gate is the **coordinator's decision to ignore an
op02-driven ADD for a returning identity** — one step further in than §7's "CM JOIN does not run".

Note also an OVMX-side connection-hygiene defect surfaced by the same capture (not the gate, but
real): on the return OVMX's `VMS$VAXcluster` VCs end **CONNSTUCK** in `DISC SENT` and OVMX re-drives
its add-member burst with `remote_conid=0x00000000` (`SCSD-I-CMREADMIT … remote=0x00000000`). The
correctly-addressed op 0x02 (with the member's live conid) still drew no commit, so this is not the
gate — but it is a distinct bug worth its own item.

### 8.3 The relocated frontier, and what `vms-0425` ships (no wire change)

**Relocated frontier.** The next isolation is **SDA on the COORDINATOR** (the member OVMX sends
op 0x02 to — VAX2 here), across the departure and the return, to see WHY it declines to start an
ADD for a reclaimed identity that it admits when fresh: does its CLUB nodemap / quorum-vote
accounting still carry residual state for the departed OVMX (so its p. 7-38 admission tests fail),
even though the per-identity CSB was reclaimed? That is the named, testable next step — and it is a
COORDINATOR-side observation, a different oracle vantage than §4–§7's member-CSB SDA.

**What `vms-0425` ships (no wire change, guard 8 — the working first-join path is byte-unchanged).**
(1) This record (§8) and the spec §4(O.24) note. (2) The sub-question bracket above (realistic vs
future incarnation, same boot, single factor) closing §7's open sub-question. (3) A READMITMAP
**verdict refinement**: `JOIN-ABANDONED` (`joiner_cfg2_sent` && `cm_responses==0`) split out of
`RECLAIMED-NOJOIN`/`NO-ENGAGE`, so the member OVMX actually DROVE the join request to (the
coordinator) is named distinctly from a member it only reached — surfacing "OVMX did its part, the
coordinator abandoned" as one log line for the next isolation. It is checked BEFORE the open latch
because OVMX's own VC ends CONNSTUCK on the return (so `vaxcluster_open_reached` under-reports, but
the op02-was-driven fact does not — the live `0425Rr` READMITMAP read `NO-ENGAGE` under the old
latch-first classifier, masking that OVMX had driven op 0x02). Log-only, kill-switch
`OVMX_NO_READMITMAP` unchanged, with a fail-pre/pass-post case in `tests/vmsscs/test_scsd_wire.c`.
Still a non-admission (never `ADMITTED`; INV-6).

Evidence (host, tank volume): CSB timelines `/data/training/vax/cluster/work/0425{Rf,Rr,Ff,Fr}.csb`
with `.status` (`0425Rf`/`0425Ff` `XITDONE=1`; `0425Rr`/`0425Fr` `XITDONE=0`); daemon logs
`/lab/k8s-labs/vaxlab-1/logs/scsd-0425{Rf,Rr,Ff,Fr}.log` and pcaps `d94-0425{Rf,Rr,Ff,Fr}.pcap`
(identities `OVXR40`/1975, `OVXF40`/1976 on the wire); CM-frame decoder `docs/clean-room/tools/cmdiff.py`; code
`src/vmsscs/scsd.c` (`readmit_verdict_of`, `READMIT_JOIN_ABANDONED`); unit
`tests/vmsscs/test_scsd_wire.c`. Live pass-post with the refined daemon: `scsd-0425Rr2.log` reads
coordinator `verdict=JOIN-ABANDONED`, non-coordinator `NO-ENGAGE`, SUMMARY `join_abandoned=1
no_engage=1` (the old classifier read both `NO-ENGAGE`). *VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39.

## 9. SDA on the COORDINATOR — CNXMAN never PROPOSES the return addition; the readmission connection never settles OPEN (`vms-9a7`, spec §4(O.25))

§8 (`vms-0425`) bisected the CM JOIN gate on the wire and relocated the frontier to **SDA on the
coordinator** — the VAX OVMX drives op 0x02 to (VAX2 in this lab) — across departure+return, to see
why it declines an op02-driven ADD for a reclaimed identity it admits when fresh. `vms-9a7` does
exactly that, and the answer is one layer BELOW where §8 was looking.

### 9.1 Method — coordinator-side SDA, single factor, same boot

`tools/coordfast.sh`: one virgin `vaxlab-2` (CN_2), one minted identity (`OVX4`/1980),
`ANALYZE/SYSTEM` parked on the **coordinator** VAX2 for the whole run, sampling `SHOW CLUSTER`,
`SHOW CLUSTER/NODE=OVX4`, `SHOW CONNECTIONS/NODE=OVX4` on a cadence. The daemon runs FRESH (no
sidecar → admitted), is SIGKILLed (crash), and returns after a ~22 s gap with the prior-admission
sidecar present (op 0x02 takes the REJOIN form). The ONLY factor that differs between the two op02
drives the coordinator sees is fresh-vs-return. Daemon = current source (`build-9a7`); `build-d94`
predates seven rejoin fixes (a63/46f/9af/f61/4dd/944/0425) and is invalid for a return arm — this
was itself a trap: a `build-d94` return never drives op 0x02 at all.

A prior long-gap arm (~46 s, `9a7v-r`) and a too-short 8 s arm both fell into non-op02 branches
(members removed → OVMX rejoin-target waits forever; or reconnect-wait for the old incarnation) — the
return outcome is **path-dependent on the departure→return gap vs the member's reconnect window**.
The ~22 s gap reproduces §4(O.24)'s op02-driven case, which is the one this map targets.

### 9.2 What the coordinator's CSB shows

| moment | coordinator CSB for the identity | note |
|---|---|---|
| FRESH join | `879C05C0` `02040000 status_rcvd` → `02060002 member,selected,status_rcvd`, State `01 open` | admitted, `XITDONE=1` |
| after crash | `879C05C0` State `09 wait`, still `member,selected,status_rcvd` | reconnect-wait for the old incarnation |
| after removal | `879C05C0` `06040005 long_break,removed,status_rcvd` | departure reconfiguration removes the old incarnation |
| op02-driven RETURN | **new** CSB `879CEA40` `02040000 status_rcvd`, State `03 reconnect` | `status_rcvd` (op 0x02 received) but **never `selected`, never `member`** — `XITDONE=0` |

The coordinator DEALLOCATES the old CSB and builds a fresh one for the returner (exactly the reclaim
§4(O.23)/§4(O.24) saw on the member), which reaches `status_rcvd` but **stalls in State `reconnect`
and is never `selected`** — the JOIN CLUSTER SELECT (Davis p. 7-38) never runs. So the p. 7-38
admission tests are not *failing*; they are **never reached**.

### 9.3 CNXMAN never proposes — the decisive datum

The coordinator's own console (captured by the parked SDA session) states it in VMS's words:

- FRESH: `%CNXMAN, received VAXcluster membership request from system OVX4` → `%CNXMAN, proposing
  addition of system OVX4` → `%CNXMAN, completing VAXcluster state transition`.
- RETURN (op 0x02 provably on the wire): ONLY `%CNXMAN, lost connection to system OVX4` and finally
  `%CNXMAN, timed-out lost connection to system OVX4` — **never** `received … request`, **never**
  `proposing addition`, for the entire return.

The coordinator does not *ignore* a received join request (§4(O.24)'s wording, from Davis p. 7-38);
its connection manager never *receives* one.

### 9.4 Why — the readmission connection never reaches a stable OPEN

`SHOW CONNECTIONS/NODE=OVX4` on the coordinator, across the return, shows its `VMS$VAXcluster`,
`SCS$DIRECTORY` and `VMS$DISK_CL_DRVR` connections to the returner parked in `State: 0007 con_sent`
/ `Blocked State: 0001 con_pend`, `Remote Con. ID 00000000` — the coordinator re-opens its
member-initiated connections to the returner (Figure-2-14, Davis pp. 7-37/7-40/7-46) but they never
reach OPEN. It sent five `VMS$VAXcluster CONNECT_REQ`s over the return; OVMX accepted exactly two to
OPEN (one per member) and drove op 0x02 on the coordinator's — yet the coordinator keys membership on
a reconnect connection that stays `con_sent` and repeatedly `timed-out lost connection`. Wire
(`cmdiff.py`): after the return `OVMX>VAX2 op02-CONFIG` there is no `op12-RELAY`, no `op03-COMMIT`, no
`op06-MEMB` — only the pre-op02 member↔member `op0d-DEPART` that removed the old incarnation.

The `remote_conid=0x00000000` §4(O.24) flagged as "a separate OVMX-side defect (not the gate)" is
this same connection seen from OVMX's side — the readmission connection whose non-settling IS the
gate. **Reframed: fingerprint of the gate, not a separate defect.** (OVMX's own end-of-run
`DISC SENT` on the member VCs is SHUTDOWN teardown, reason 5=SYSAP_SHUTDOWN — a symptom of the daemon
being killed while the connection was stuck, not the cause.)

### 9.5 Relocated frontier + fix design

The gate is a **CONNECTION-layer** failure upstream of CM admission: the coordinator's
member-initiated `VMS$VAXcluster` reconnect to the returning identity never reaches a stable OPEN, so
CNXMAN never proposes the addition. The next isolation: the coordinator opens (at least) two
`VMS$VAXcluster` connects to the returner — one OVMX accepts and drives op 0x02 on, and one that
stays `con_sent` — and CNXMAN waits on the one that never opens. Determine whether OVMX must
COMPLETE/accept the coordinator's *expected* reconnect (an incarnation/connection-identity handling
gap on the return) instead of presenting a separate fresh VC + op 0x02. The fix will live in OVMX's
rejoin connection handling (which member-initiated `VMS$VAXcluster` connect it accepts and keeps
open), NOT in the op02 payload — so it stays inside guard 8's no-wire-change envelope until the
specific connect to accept is isolated. No speculative change is made here (crash-prone op02/START
region); this ships the isolation, the relocated frontier, and a corrected diagnostic.

### 9.6 What `vms-9a7` ships (no wire change, guard 8/23)

(1) This §9 and the spec §4(O.25) note. (2) The coordinator-SDA single-factor bracket above. (3) A
READMITMAP **verdict-text correction**: `JOIN-ABANDONED` no longer says the coordinator "IGNORES the
request (Davis p. 7-38)"; it states the grounded coordinator-SDA finding — CNXMAN never PROPOSES the
addition because the `VMS$VAXcluster` reconnect to the returning identity never settles OPEN
(`con_sent`/`reconnect`, times out). Classifier logic UNCHANGED (`joiner_cfg2_sent &&
cm_responses==0`), kill-switch `OVMX_NO_READMITMAP` unchanged, fail-pre/pass-post case in
`tests/vmsscs/test_scsd_wire.c`, byte-unchanged on the wire. Still a non-admission (never `ADMITTED`;
INV-6). *VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39/7-40/7-46.

## 10. CORRECTION — the member-initiated reconnect DOES reach OPEN; op 0x02 breaks it (`vms-c40`, spec §4(O.26))

§9 (`vms-9a7`) relocated the frontier to the SCS connection layer on the reading that the
coordinator's member-initiated `VMS$VAXcluster` reconnect "never reaches a stable OPEN"
(`con_sent`/`reconnect`, `Remote Con. ID 0`). `vms-c40` re-ran the SAME single-factor bracket with
DENSER connection sampling on the **current** build (re-proven to drive op 0x02 on the return — not a
stale binary; `build-9a7` lived in a since-deleted worktree and could not be trusted line-for-line),
and the reading is CORRECTED one layer back up.

### 10.1 The connection DOES open (coordinator SDA)

`SHOW CONNECTIONS/NODE` on the coordinator across the return, virgin `vaxlab-1`, identity `OVXC0`:

| moment | coordinator's `VMS$VAXcluster` CDT to the returner | note |
|---|---|---|
| fresh join | `State 0002 open`, `Remote Con. ID = OUR fresh handle` | admitted; `CNXMAN proposing addition` |
| return R+4 s | **`State 0002 open`, `Remote Con. ID 653B0001` (OUR return handle)** | opens — with `SCS$DIRECTORY` + `MSCP$DISK` also `open` |
| return R+17 s → end | `State 0007 con_sent` / `03 reconnect`, `Remote Con. ID 0` | broke back to reconnect; times out |

So §9's "never settles OPEN" sampled only the post-break phase. OVMX's op=1 `CONNECT_RSP` echo
(`send_seq 17`, dcid = coordinator handle) + op=2 `ACCEPT_REQ` (`send_seq 18`, supplies our handle
`653B0001`), both wire-proven correctly addressed, DO open the connection.

### 10.2 What breaks it — op 0x02 rides past the coordinator's `recv_ack` ceiling

Coordinator console: `%CNXMAN, lost connection to system OVXC0` at `08:50:23.75`. OVMX daemon:
`CMCONFIG2` drives op 0x02 on the member connection at `08:50:23.74` — the SAME 10 ms. Per-`send_seq`
decode of `d94-c40repB.pcap` on the membership VC:

| CM op | `send_seq` | coordinator `recv_ack` ceiling on this VC |
|---|---|---|
| op 0x01 params | 21 | **18** |
| op 0x02 config | 22 | **18** |

OVMX's OWN `SCS$DIRECTORY` + `MSCP$DISK` client discovery to the coordinator bloats the shared per-VC
`send_seq` (spec §4(O.14)), so op 0x01/op 0x02 land at 21/22 — 3–4 past the coordinator's `recv_ack`
ceiling of 18. The coordinator never delivers them, `CNXMAN` never proposes (no second
`received VAXcluster membership request`), and the OPEN connection reverts to `03 reconnect`. The
`vms-9af` lean-VC suppression (§4(O.15)) fired at `08:50:25` — ~1.3 s AFTER the op 0x02 and the break,
too late to keep the VC lean. Post-break, op 0x05/op 0x06 are addressed to `Remote Con. ID 0`.

### 10.3 Relocated frontier + fix design

The gate is NOT the SCS connection layer (it opens) and NOT CM admission (CNXMAN is willing — it
proposed the fresh join identically). It is **op 0x02 readmission DELIVERY on an already-OPEN member
connection**: op 0x02 rides past the coordinator's `recv_ack` ceiling because OVMX's own discovery
traffic precedes it, and lean-VC engages too late. This is the same `send_seq`-ceiling / lean-VC-timing
family as §4(O.15) `vms-9af`, §4(O.17) `vms-46f` and §4(O.10) `vms-e15` — now PROVEN open-then-break
with coordinator SDA rather than inferred from the joiner side. The next increment (deferred to
`vms-694`): make lean-VC / credit-first engage BEFORE op 0x02 is driven on the member-initiated
connection (deterministically via `OVMX_CFG2_PEER`, or defer op 0x02 until the coordinator's VC is
lean) so op 0x02 rides UNDER the ceiling. That is a wire-visible sequencing change in the crash-prone
op02/START region and is NOT made here (guard 8; "no speculative wire change until isolated").

### 10.4 What `vms-c40` ships (no wire change)

(1) This §10 and the spec §4(O.26) note. (2) The denser-sampled coordinator-SDA bracket above,
grounding the open-then-break. (3) A `READMITMAP` verdict-text correction: `JOIN-ABANDONED` no longer
says the reconnect "never settles OPEN"; it states the grounded finding — the connection settles OPEN
then breaks when op 0x02 rides past the `recv_ack` ceiling. Classifier logic UNCHANGED
(`joiner_cfg2_sent && cm_responses==0`), kill-switch `OVMX_NO_READMITMAP` unchanged, fail-pre/pass-post
case in `tests/vmsscs/test_scsd_wire.c`, byte-unchanged on the wire. Still a non-admission (never
`ADMITTED`; INV-6). *VAXcluster Principles* pp. 2-43/2-44/2-45/7-37/7-40/7-46.


## 11. The §10 fix IMPLEMENTED — op 0x02 now rides ≤18 and is DELIVERED, but the frontier relocates to returning-identity non-admission (`vms-3aba`, spec §4(O.27))

§10 (`vms-c40`) named the next increment: make lean-VC / credit-first engage BEFORE op 0x02 on
the member-initiated connection so op 0x02 rides UNDER the coordinator's `recv_ack` ceiling of 18.
`vms-3aba` ships it and brackets it live. Full record: `docs/cluster-protocol-spec.md` §4(O.27).

**The fix.** `cm_peer_is_coordinator()`'s `lower_peer_seen` guard could not identify an
appears-first coordinator (higher node, START completes before the non-coordinator's), so the
`vms-9af` suppression fired ~1.3 s too late and OVMX's own dir/MSCP client half bloated the
coordinator's VC. `cm_rejoin_lean_early_hold()` now HOLDS own-dir on a coordinator CANDIDATE
(`cm_peer_coord_pending()` — top node, no lower peer yet) for a bounded settle window
(`LEAN_COORD_SETTLE_MS`), so the non-coordinator appears and suppression engages BEFORE op 0x02.
Kill-switch `OVMX_REJOIN_LEAN_EARLY=0`; added at both own-dir initiation sites.

**Live result (virgin `vaxlab-0`, identity `OVX3A0`, no `OVMX_CFG2_PEER`).** The daemon emits
`LEANHOLD → CREDITFIRST → CMCONFIG2 (MEMBER-initiated, send_msg=3) → LEANVC` in order; op 0x02
rides `send_seq` 2/8/17 (all ≤18, vs §10's 21/22) and the coordinator's `recv_ack` advances to 13
PAST it — op 0x02 is DELIVERED, and the op6/op7/op8/op9 credit exchange completes. **The §10
send_seq-ceiling gate is CLOSED.** Yet `XITDONE=0`: `cm_responses=0`, and the coordinator console
prints only `lost connection` for the return, never `received membership request` / `proposing
addition` (it prints both for the SAME identity's first join).

**The frontier relocates BACK to §2.2 / §6 / §7.4 / §8 — returning-identity non-admission.** §10's
ceiling was a real intervening gate stacked on the deeper one; closing it returns the frontier to
where §7.4 and §8 left it: the coordinator receives a clean, delivered, credited op 0x02 for an
identity it has held before and runs ZERO CM JOIN handshakes / never proposes the addition, though
it admits a FRESH identity through the identical path. The next isolation is that CM-layer gate,
now with the wire delivery PROVABLY clean (op 0x02 delivered, `recv_ack` past it, credit flowing) —
a confound §4 through §10 could never fully remove.


## 12. The coordinator retains NO persistent per-identity block — the gate is the departed node's own reconnect/wait CSB, kept alive by OVMX's rejoin-form return (`vms-358`, spec §4(O.28))

§11 (`vms-3aba`) left the frontier at "the coordinator receives a clean, delivered, credited
op 0x02 for an identity it has held before and proposes nothing." `vms-358` puts `ANALYZE/SYSTEM`
on the **coordinator** across three same-boot single-factor brackets on freshly-booted virgin
`vaxlab-0` (current-source daemon `md5=2518a52a`, `scsd.c` == `origin/main` HEAD), and the answer
CORRECTS the "returning-identity non-admission" framing this document has carried since §5.

**Bracket 1 (`358flip`) — the fresh-SCSSYSTEMID flip does NOT flip.** `A` FRESH join `OVX3S0` →
ADMITTED; `B` fast return SAME `OVX3S0` → REFUSED; `C` fast return presenting a FRESH `OVX3F0` the
coordinator never held → **REFUSED** (`scsd-358flip-C.log` `READMITMAP-SUMMARY admitted=0`). A fresh
id is refused too, so the block is **not** keyed on the exact held SCSSYSTEMID. (The tag's
`358flip.status` SUMMARY says `C=1` — a `grep -ac XITDONE` false positive counting the literal
"XITDONE STILL 0" in the verdict text; the per-run daemon `admitted=` count is authoritative.)

**Bracket 2 (`cq358`) — `qf_failed_node` is a red herring.** Four fresh first-joins on one pristine
boot; `qf_failed_node` latches after the first departure and PERSISTS, yet a fresh join after a
graceful departure (`B`, `admitted=2`) AND a fresh join after an **unclean SIGKILL crash + 26 s
settle** (`D`, `admitted=2`) both ADMIT with it set. A fresh join admits once the prior node's
teardown has **settled**. `358flip` arm C is thus re-read: refused because it arrived ~3 s after
arm B's 70 s SAME-id reconnect-hammer was killed — mid-churn, Davis "abandon if a rebuild is
pending" (pp. 7-40/7-41) — not because it was fresh-during-`qf_failed_node`.

**Bracket 3 (`sameid358`) — the same id is self-perpetuating.** One id `OVX3X0`: `F1` first-join →
ADMITTED → crash (cleanly `removed`); `R` fast return in REJOIN form → REFUSED, and R's op 0x02
drive re-creates the coordinator's CSB for `OVX3X0` in State `03 reconnect`→`09 wait`,
`Flags 02040001 long_break,status_rcvd`, CSID `0`, which OUTLIVES a 35 s no-daemon settle; `J`
return as a FRESH FIRST-JOIN onto that residual → REFUSED (`verdict=JOIN-ABANDONED`, request
received on VAX1, never proposed). **No arm achieved `XITDONE 0→1` for a same-id return.**

**The retained record, named from SDA.** Not a "known-system" cluster-DB marker, not the
`qf_failed_node` quorum flag — the departed node's **own per-SCSSYSTEMID CSB held in a reconnect/wait
connectivity state** (`SHOW CLUSTER/NODE` State `03 reconnect`→`09 wait`, `long_break`, CSID 0, never
`member`; `SHOW CONNECTIONS/NODE` the coordinator's member-initiated VCs to it `con_sent`/`con_pend`,
`Remote Con. ID 0`). The coordinator holds it because it believes the node is *reconnecting*
(RECNXINTERVAL/`long_break`, pp. 7-29/7-30) and will not PROPOSE a fresh JOIN for a SCSSYSTEMID whose
CSB it is still reconnecting. OVMX's rejoin apparatus (`cm_rejoin_target_mode`, deferred op 0x02, the
reconnect/readmit path) KEEPS this CSB alive: by fast-returning as the same id and driving op 0x02
onto the coordinator's still-open reconnect — which can never complete (OVMX returns a fresh
incarnation/Con.ID) — OVMX perpetually re-arms the `long_break` wait so the CSB is never removed.

**Relocated frontier — OVMX-side, the reconnect/readmit apparatus itself.** Davis grants a rejoin no
distinct transaction: the old CSB is deallocated and a new one built "just as if joining for the
first time," new CSID (pp. 7-24/7-25). **Fix:** abandon the reconnect/readmit rejoin path; make
OVMX's departure DRIVE the coordinator to fully REMOVE the prior incarnation's CSB (a clean CM
self-departure / last-gasp — which OVMX does NOT emit today, only a per-connection SCS DISCONNECT —
forces immediate removal, p. 7-29), THEN drive a plain FRESH first-join exactly as `cq358` arm D
does. The one arm that proves this for the SAME id — clean unhammered removal then a same-id
first-join — is the next increment; it is a wire-visible change in the crash-prone op02/START region
(guard 8) and awaits the last-gasp emit, so `vms-358` ships the isolation, the corrected verdict
text, and this record, not a speculative wire change.

Evidence (host, tank volume): `/data/training/vax/k8s-labs/vaxlab-0/logs/scsd-{358flip-*,cq*,sid*}.log`,
`/data/training/vax/cluster/work/{358flip,cq358,sameid358}.csb` (coordinator SDA timelines),
`d94-{358flip,cq358,sameid}.pcap`; runners `tests/lab/tools/{coord358,cq358,sameid358}.sh`.
*VAXcluster Principles* pp. 7-24/7-25/7-29/7-30/7-37/7-38/7-39/7-40/7-41.

## 13. The fix SHIPPED — fresh-first-join return (safe, default) advances the coordinator to PROPOSE+SELECT the return; the CM-layer op-0x0d last-gasp CRASHES the VAX and is defaulted OFF (`vms-ab1`, spec §4(O.29))

§12 named the OVMX-side frontier and the two-part fix. `vms-ab1` implements both,
kill-switched, and the LIVE lab arbitrates each:

**Part 2 — RETURN AS A PLAIN FRESH FIRST-JOIN (SAFE, DEFAULT ON).** `ovmx_rejoin_cleanleave()`
(default on; `OVMX_REJOIN_CLEANLEAVE=0` restores the legacy apparatus) neutralises the
whole reconnect/readmit apparatus at ONE point each: `cm_rejoin_target_mode()` returns 0
(own-outbound first-join topology) and `cm_apply_rejoin_form()` is a no-op (plain first-timer
op 0x02). Every sub-lever (lean-VC, credit-first, lean-early-hold, ackhold, the proactive
CMREADMIT burst) chains through those two, so both are neutralised together. Fail-pre/pass-post
unit test in `tests/vmsscs/test_scsd_wire.c`
(`test_rejoin_cleanleave_default_is_a_fresh_first_join`).

**Live result (GROUNDED, fresh `vaxlab-2`, current-source daemon `md5=d5a8946f`, 45 s settle).**
F1 joins (`admitted=2`, coordinator CSB `02060002 member,selected`, `SCSD-I-XITDONE`), departs
cleanly (graceful `--duration` expiry → `scsd_shutdown_teardown` → SCS DISCONNECT), and the
coordinator SDA shows the F1 incarnation move `02060002 member,selected` → `06040005
long_break,removed` across the settle — **the prior incarnation IS REMOVED** by the coordinator's
own RECNXINTERVAL, with no rejoin-form return keeping it alive. That is the load-bearing win over
§4(O.28): the wedge that outlived every settle is gone. **The return, however, is STILL not
admitted.** Rejoining as a plain fresh first-join it builds a NEW CSB (`879D7880`) that reaches
only `01 open` non-member; the authoritative per-run daemon verdict is `READMITMAP-SUMMARY
admitted=0` (`JOIN-ABANDONED` on the peer OVMX drove op 0x02 to, `NO-ENGAGE` on the other), and the
coordinator console prints NO `proposing addition` for the returning identity. (One earlier arm on
a *contaminated* `vaxlab-0` did once reach `02060000 selected` with a `proposing addition`; it did
NOT reproduce on a clean pod and is not relied on.) So fresh-first-join removes the harmful
apparatus and the prior CSB, but a further gate on the RETURN path — the §4(O.26)-class op 0x02
delivery / member CM-JOIN engagement — persists.

**Part 1 — CLEAN CM LAST-GASP (op-0x0d) CRASHES THE VAX; DEFAULT OFF.** OVMX also gained an
emit of the class-0x04 op-0x0d SELF-DEPARTURE open (`scs_member_build_depart` /
`scsd_emit_clean_departure`), header-only, on the OPEN VC at shutdown, to drive IMMEDIATE
removal. The LIVE bracket on a **fresh** `vaxlab-1` (daemon `md5=73c18ef6`) proved this
**CRASHES the real VAX coordinator**: VAX2 bugchecked and rebooted (SDA/console `%SYSBOOT` →
`OpenVMS VAX V7.3` → `%SYSINIT, waiting to form or join`) immediately after F1's
depart-with-last-gasp, and the return then could not admit (`admitted=0`) because the coordinator
was reforming. The Part-2-only clean shutdown (SCS DISCONNECT, no last-gasp) never crashed any
VAX. This is the clean-room-warned crash class (`scs_member.h`): a class-0x04 transition-OPEN
emitted OUT of its grounded `0x12→0x03→0x0d→0x0a` departure choreography drives CNXMAN into an
inconsistent-state bugcheck. **`scsd_emit_clean_departure` is therefore DEFAULT OFF (opt-in
`OVMX_LASTGASP=1`), kept only as an RE probe.** The AUTHENTIC immediate-removal signal is the
PORT-LEVEL last-gasp datagram (p. 7-29), whose byte form is NOT grounded in OVMX and is deferred
to a wire-RE increment.

**Relocated frontier.** Fresh-first-join (safe, default) moves the return from
TOTAL-NON-ADMISSION (§4(O.28): coordinator proposed nothing) to PROPOSED+SELECTED, with removal
of the prior incarnation confirmed on coordinator SDA. What still blocks full `member` commit:
the second member NO-ENGAGEs the returning identity's JOIN barrier (its residual is not cleared
as promptly as the coordinator's), and the only safe way to force IMMEDIATE per-member removal —
the port-level last-gasp datagram — is not yet RE'd (the CM-layer op-0x0d substitute crashes the
VAX). Next increment: ground the port-level last-gasp datagram, OR bracket a longer settle so
BOTH members remove via their own RECNXINTERVAL before the return.

Evidence (host, tank volume): `/data/training/vax/k8s-labs/vaxlab-2/logs/scsd-ab-J.log`
(`READMITMAP-SUMMARY admitted=0`, `JOIN-ABANDONED`/`NO-ENGAGE`),
`/data/training/vax/cluster/work/abrun-p2.csb` (coordinator SDA: F1 `member,selected` → settle
`long_break,removed` → return fresh CSB `879D7880 01 open` non-member);
`/data/training/vax/k8s-labs/vaxlab-1/logs/vax2.log` + `abrun-clean.csb` (the op-0x0d last-gasp
CRASH: VAX2 `%SYSBOOT`/`%SYSINIT` reboot). Code `src/vmsscs/scsd.c`
(`ovmx_rejoin_cleanleave`, `cm_rejoin_target_mode`, `cm_apply_rejoin_form`,
`scsd_emit_clean_departure`), `src/vmsscs/scs_member.c` (`scs_member_build_depart`). Runner
`tests/lab/tools/abrun.sh`. *VAXcluster Principles* pp. 7-24/7-25/7-29.
