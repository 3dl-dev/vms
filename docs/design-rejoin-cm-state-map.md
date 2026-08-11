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

