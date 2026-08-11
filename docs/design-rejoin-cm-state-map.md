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

<!-- WIRE-CONFIRMATION SECTION: filled from the oracle/live flow analysis below. -->
