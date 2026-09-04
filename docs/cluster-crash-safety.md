# OVMX cluster crash safety — the taxonomy, the gate, and the standing rule

> **The invariant this document serves: OVMX NEVER CRASHES A PEER.**
> A faithful OVMX node must never emit cluster traffic that can bugcheck a real
> VMS Connection Manager. The endgame is joining *production* VMScluster
> systems, where a crashed CM does not fail alone — it takes the transition,
> and then the healthy members, with it. Memory: `ovmx-never-crashes-a-peer`.
> Operator, 2026-09-04: *"better we find all the ways we can crash VAX in dev.
> if we crash a prod cluster we're cooked!"*

## 0. Why this exists, and what changed

We have found two peer-crashing vectors, and we found both of them **the same
way — by crashing a lab VAX**:

| # | bugcheck | what OVMX emitted | note |
|---|---|---|---|
| 1 | `CNXMGRERR`, *Error detected by VAXcluster Connection Manager* | a brand-new SCS dialogue opened at SYSAP send-msg# 8 (and 13), acking a peer message the peer had not sent on it — the per-CSB counter never restarted and was burned on refused sends | E76; **both** reference VAXes bugchecked, dumped, rebooted |
| 2 | `INVEXCEPTN`, *Exception while above ASTDEL or on interrupt stack* | one `cat 0x04` ack per `cat 0x01 op 0x06` frame — 254 acks in 31.6 ms to a coordinator that had just sent a 254-frame membership burst | E78; VAX2 down, no auto-reboot |

Two earlier ones are recorded in `docs/cluster-protocol-spec.md` §4(p) and are
folded into the taxonomy below: `LOCKMGRERR` (the cat-`0x01` body edits applied
to a cat-`0x02` DLM rebuild, corrupting the lock resource name) and
`INCONSTATE` (a cat-`0x06` close echoed back at the peer, reflecting its own
live Con.IDs and cluster id).

Reactive discovery does not scale, does not generalise, and costs a lab VAX per
finding. **This document and its gate make the check proactive**: measure the
envelope the corpus of real VMS nodes actually keeps, then flag every
OVMX-originated frame outside it — *before* the next lab fire, and eventually
before the frame is emitted at all.

### What the gate asserts, and what it does not

The gate never claims a flagged frame *will* crash a peer. It asserts exactly
one thing, which is the thing we can prove:

> **No real VMS node in the reference corpus ever emitted this, and here is the
> sample size.**

Two vectors additionally carry an observed crash and are labelled so. The rest
are honest "outside the measured envelope" reports. That is INV-6 applied to a
diagnostic: report what the wire shows, never a fabricated verdict.

### Provenance (Rule 8, clean-room)

Every offset, threshold and label below is either an on-wire observation of our
own reference lab or a citation of `docs/cluster-protocol-spec.md`. Nothing is
computed from an unpublished VMS algorithm; no VSI/HPE source or binary was
read. Thresholds are **measured**, and re-derivable:

```
tools/cluster/cm_wire_safety_audit.py --measure ~/vax/cluster/captures/*.pcap
```

**The corpus.** 47 captures in `~/vax/cluster/captures`, 306 670 CM-class
frames. The false-positive baseline uses the **19 captures containing only real
VMS nodes** (234 555 CM frames) — a capture where OVMX is on the wire cannot
measure "what real VMS does", because half of what the real node does there is
shaped by OVMX's own defects.

---

## 1. The crash-vector taxonomy

Each class: the mechanism, the plausible VMS failure, the invariant OVMX must
respect, and how the gate detects it from a frame stream.

### C1 — Transaction-envelope inconsistency  ▸ `S1-ENVELOPE-JUMP`, `S2-ENVELOPE-ACK`

**Mechanism.** The 132-byte SYSAP body carries a per-sender message counter at
`body[0:2]` and an acknowledgement of the peer's counter at `body[2:4]` (spec
§4(j)). A node whose counter advances on messages that never left it will ack a
peer message the peer never sent — an assertion about a conversation that did
not happen. The CM validates this and bugchecks.

**Observed failure.** `CNXMGRERR` on both reference VAXes, within 1.2 ms and
0.2 ms of the OVMX burst (E76).

**Invariant.** The SYSAP send-msg# counts *messages actually transmitted*; it is
never advanced by a send that was refused. The ack-msg# never exceeds the
peer's highest send-msg# actually observed.

**Detection.** `S2` — an ack running more than the capture-loss floor above the
peer's node-level high-water. `S1` — a fresh Con.ID pair opened at a send-msg#
that neither restarts at 1 nor continues from the sender's own high-water.

> ⚠ **CORRECTION to the E76 reading, and it matters.** E76 concluded "golden:
> every fresh CDT opens at send_msg=1", and E77 implemented a per-connection
> restart on that premise. **The corpus contradicts it.** In three reference
> captures (`af2-established-rejoin`, `af2-firsttimer-established`,
> `scs-node-leave`) *both* real VMS nodes open a second `VMS$VAXcluster` Con.ID
> pair at send-msg# **9** simultaneously — continuing neither from 1 nor from
> their own high-water (14 760 / 10 065) — and neither peer bugchecks. Why 9 is
> not recoverable from passive capture. So "opened at != 1" is **not** the
> crash, and `S1` is therefore **WARN, not FATAL**. The FATAL half of C1 is the
> unbacked ack, `S2`.
>
> The existing E76 tool `tools/cluster/cm_dialogue_audit.py` still applies the
> refuted per-dialogue rule and **flags real VMS nodes for it** — see the
> findings in §4.

### C2 — Response flood / under-coalesced acknowledgement  ▸ `S3-ACK-COALESCE`, `S4-ACK-RATE`

**Mechanism.** The coordinator publishes membership as a `cat 0x01 op 0x06`
burst of ~254 frames in tens of milliseconds. Spec §4(u) grounds the reference
ack as *prompt, opportunistic, **cumulative***. A node that answers each frame
individually returns the burst at the sender as a same-size burst, at interrupt
level, while it is still transmitting.

**Observed failure.** `INVEXCEPTN, Exception while above ASTDEL or on interrupt
stack, Current process=OPCOM` on VAX2 (E78).

**Invariant — MEASURED, and it is a hard law.** Over **6 549** advancing
`cat 0x04` acks from real VMS nodes across the whole corpus, the SYSAP ack-msg#
advances by **3 or more, every single time**. An advance of **1 occurs zero
times**; an advance of **2 occurs zero times**. During an op-06 burst the ratio
is exact: 6 of 6 real responders across 4 captures answer a 254–255 frame burst
with **84–85 acks, `am` advancing by exactly 3 each** (ratio 0.322–0.333).

> This supersedes the E78 note's "the golden answers the burst with THREE
> frames". The golden answers it with ~84 frames, each acknowledging exactly 3.
> The actionable law is the **coalescing factor of 3**, not a frame count.

**Detection.** `S3` — any OVMX `cat 0x04` ack advancing `am` by 1 or 2. `S4`
(WARN) — more than 111 acks in any 50 ms window, the busiest 50 ms any real node
managed anywhere in the corpus (a node *leaving*, the library's peak).

### C3 — Response-body fidelity: DLM lock rebuild  ▸ `S5-ECHO-DLM`, `S5B-DLM-NAME-HOLE`

**Mechanism.** A `cat 0x02 op 0x0d` rebuild record is echoed **verbatim** with
four edits (spec §4(p): envelope `[0:4]`, response bit `[8]`, result stamp
`[34]` — a recipe that reconstructs 1367 of 1367 real responses byte-for-byte).
`body[47]` is the lock resource-name length and `body[48:]` the name. Applying
the *cat-`0x01`* mutations here writes `body[18]` and `body[55]` — and
`body[55]` is the **8th byte of the resource name**.

**Observed failure.** `LOCKMGRERR` on VAX1 and VAX3. OVMX shipped
`CACHE$cmSYSDSK1` as `CACHE$c\0SYSDSK1` on all eight replies. The in-capture
control is decisive: across the same milliseconds the two real VAXes exchanged
the same records with each other correctly and neither crashed.

**Invariant.** Response shape is **per-category**. A cat-`0x02` echo edits only
`{0,1,2,3, 8, 34}`. The resource name is never touched.

**Detection.** `S5` — a corroborated pairing whose response rewrites anything
outside that set (fires **8/8** on `ovmx-760-lockmgrerr`, zero on the real nodes
in the same capture). `S5B` (WARN, correlation-free) — a non-printable byte
*inside* the name's ASCII run, since the binary sub-key is a suffix.

### C4 — Response-body fidelity: the membership family  ▸ `S6-ECHO-CM`, `S6-ECHO-UNGROUNDED`

**Mechanism.** Once the relay works, non-coordinator members open their own
transactions with the joiner carrying opcodes absent from the pre-relay
dialogue. Their bodies carry **the peer's own live Con.IDs and cluster id**;
answering with a full-body echo reflects that peer's I/O structures back at it.

**Observed failure.** `INCONSTATE, Inconsistent I/O data base` on VAX3 and
`INVEXCEPTN` on VAX1 (`ovmx-760-relay-crash`).

**Invariant.** Answer only (category, opcode) pairs grounded in the reference,
and rewrite only the bytes a reference responder rewrites. **An allowlist, never
a default.** Silence is the safer failure — but not a free one: an unanswered
pair the coordinator gates on strands the transition (spec §4(p)).

**MEASURED allowed-mutation sets** (offsets a real responder *ever* differs from
the request it answers; n = real responses measured):

| response | n | offsets ever rewritten |
|---|---|---|
| `0x81`/`0x03` | 64 | 0,1,2,3, 8, 18 |
| `0x81`/`0x05` | 97 | 0,1,2,3, 8, 18 |
| `0x81`/`0x08` | 11 | 0,1,2,3, 8, 18, 55 |
| `0x81`/`0x09` | 50 | 0,1,2,3, 8, 18, 55 |
| `0x81`/`0x0b` | 809 | 0,1,2,3, 8, 16, 17, 18 |
| `0x81`/`0x0d` | 3 | 0,1,2,3, 8, 18 |
| `0x81`/`0x0f` | 11 | 0,1,2,3, 8, 20 |
| `0x81`/`0x12` | 26 | 0,1,2,3, 8, 17, 18, 20, 21, 22, 23 |

`cat 0x01 op 0x01` is exempt: it is a cluster-parameters **reply** carrying the
responder's own state, not an echo (spec §4(j)/§4(s)). n < 16 is reported at
WARN, not FATAL.

**Detection.** `S6-ECHO-CM` — a rewrite outside the opcode's measured set (fires
4× on `ovmx-760-lockmgrerr`, all `body[55]`). `S6-ECHO-UNGROUNDED` (WARN) — a
response to an opcode whose response shape the corpus never shows.

### C5 — Reflecting a close back at its sender  ▸ `S7-CLOSE-ECHO`

**Mechanism.** A `cat 0x06` close must carry the responder's **own** node
parameters. "Echoing this request's payload bugchecks the peer — it carries
that peer's live Con.IDs and cluster id" (spec §4(p)).

**Observed failure.** `INCONSTATE, Inconsistent I/O data base`.

**Invariant.** Never reflect a close.

**Detection.** MEASURED over 990 real closes, the smallest number of body bytes
differing from the correlated request is **7**. A response differing in fewer is
a near-verbatim reflection.

### C6 — Answering something that is never answered  ▸ `S8-ANSWERED-NOTIFY`, `S9-RESP-TXN-ZERO`

**Mechanism.** `op 0x0a` (barrier GO) and `op 0x0c` (step release) are
notifications: they carry `txn = 0` and draw no response of any kind. A response
to one invents a message VMS never sends, correlated to a transaction the peer
is not holding open — the same class of assertion as C1.

**Invariant.** MEASURED: **zero** responses correlated to `op 0x0a`/`op 0x0c`
in 47 captures; and **zero** response frames anywhere carry `txn = 0` (0 of
103 413 real, 0 of 2 953 non-reference).

> **Deliberately NOT a finding:** a response whose request is merely absent from
> the capture. 5.3% of *real* responses fail to correlate (a capture that began
> mid-transaction, a peer answering from its other leg), so flagging it would be
> five thousand cries of wolf. The rate is printed as context instead.

### C7 — Addressing a Con.ID the peer does not hold  ▸ `S10-CONID-ZERO`

**Mechanism.** A Con.ID identifies a connection endpoint (spec §4(t)). A frame
addressed to Con.ID 0 cannot resolve to a CDT at the receiver. Spec §4(O.26)
records OVMX emitting `op 0x05`/`op 0x06` to `Remote Con.ID 0x00000000` after a
VC break.

**Invariant.** MEASURED: **zero** of 306 670 corpus CM frames carry a zero local
or remote Con.ID.

### C8 — Malformed / short / oversized frame  ▸ `S11-FRAME-SIZE`

**Mechanism.** A frame in the fixed class that is shorter than the class hands
the receiver a body it will index past.

**Invariant.** MEASURED: every corpus CM frame is exactly **204 bytes** —
**306 670 of 306 670** over all sources, **299 224 of 299 224** over reference
nodes only, which is the population `--measure` prints (spec §4(d), the fixed
190-content class).

### C9 — Over-sending against the peer's grant  ▸ `S12-CREDIT-OVERSEND`

**Mechanism.** One message costs one send credit and the acknowledgement returns
it (*VAXcluster Principles* p. 2-43). The peer advertises its grant, byte-exact
to SYSGEN `CLUSTER_CREDITS`, at abs 95 of its `0x41` START (spec §4(g)).
Transmitting with more messages outstanding than granted over-runs its receive
buffering.

**Invariant.** `send_seq − peer's highest recv_ack ≤ the peer's advertised
grant`.

> Two measurement hazards, both handled, both learned the hard way while
> building this gate: `send_seq` is per-**virtual circuit**, not per-connection
> (spec §4(O.14)), so a mid-capture `0x41` opening *another* SYSAP connection
> between the same pair must **not** reset the baseline — doing so read a
> `send_seq` of 17 519 as 17 519 messages outstanding and produced 736 false
> positives on real VMS nodes. And a stream is only judged once the peer has
> acknowledged at least once, which is the only baseline a capture that began
> mid-circuit can honestly have.

### Classes considered and NOT retained

- **Duplicate / replayed dialogue.** No corpus signature separates a legitimate
  retransmit (which reuses its `send_seq`, spec §4(h)(4a)) from a replay. Not
  groundable from passive capture — recorded as a gap, not implemented as a
  check that would guess.
- **An op emitted in a peer state that does not expect it.** Partially covered
  by C4/C6, but the peer's *state* is not on the wire. What is checkable is the
  ordering invariant (`0x0c#N` never precedes `0x0b#N`, spec §4(p)); OVMX is not
  the coordinator, so it cannot violate it yet. Revisit at T3 (electable OVMX).

---

## 2. The gate

**Checker:** `tools/cluster/cm_wire_safety_audit.py` (stdlib only, no deps).
**CI/pre-flight wrapper:** `tools/ci/cluster_wire_safety_gate.sh`.
**Host test:** ctest `cluster_host_wire_safety_audit`.

```sh
# before every lab fire: audit the PREVIOUS run's capture
tools/ci/cluster_wire_safety_gate.sh /path/to/join-<tag>.pcap

# CI / no capture: run the checker's own negative control
tools/ci/cluster_wire_safety_gate.sh

# re-derive every threshold from the corpus instead of trusting it
tools/cluster/cm_wire_safety_audit.py --measure ~/vax/cluster/captures/*.pcap

# judge EVERY source, including the reference nodes (false-positive check)
tools/cluster/cm_wire_safety_audit.py --audit-all <pcap>
```

Exit 0 = clean, 1 = a FATAL finding, **2 = the audit could not run** (a capture
with no CM frames proves nothing and must not read as a pass).

**Who is judged.** By default, every SCA source whose OUI is not a reference one
(`08:00:2b` hardware, `aa:00:04` DECnet-logical). The classification is printed,
so a capture whose OVMX address was misread is visible rather than silently
mis-audited. `--ovmx-mac` pins it; `--audit-all` judges everyone.

### False-positive baseline — the number that makes it usable

Over the **19 pure-reference captures / 234 555 CM frames**:

| severity | vector | n | status |
|---|---|---|---|
| **FATAL** | *(all)* | **0** | **zero false positives** |
| WARN | `S5B-DLM-NAME-HOLE` | 16 | one identifiable name, `UCX$INETACP_` with a binary IP sub-key ending in a printable byte |
| WARN | `S1-ENVELOPE-JUMP` | 6 | the refuted "opens at 1" rule; see C1 |

Both WARN classes are fully characterised, which is why they are WARN.

### Self-test (the teeth)

`--self-test` synthesizes a clean CM dialogue that must produce **zero**
findings, and one **single-factor** violation fixture per vector that must each
be detected — so a fixture that stops firing means the *check* broke, not the
fixture. 11 vectors, all detected, clean fixture clean. This is what CI runs.

---

## 3. Retro-audit: every lab capture we have

**107 OVMX-bearing captures** (`/data/training/vax/k8s-labs/*/logs/*.pcap` plus
the in-tree `tests/lab/captures/`):

| vector | findings | captures |
|---|---|---|
| `S3-ACK-COALESCE` | **112 160** | most |
| `S12-CREDIT-OVERSEND` | 88 | 15 |
| `S4-ACK-RATE` | 63 | — |
| `S5B-DLM-NAME-HOLE` | 60 | 55 (all the benign `UCX$INETACP_` name — **not** a defect) |
| `S10-CONID-ZERO` | 26 | 7 |
| `S1-ENVELOPE-JUMP` | 5 | 3 |

### The two known crashes are both flagged

| run | capture | gate output |
|---|---|---|
| E75/E76 `CNXMGRERR` | `join-e75refire-1788532514.pcap` | `S1-ENVELOPE-JUMP` ×2 — the two fatal bursts, opening at send-msg# **8** and **13** having sent 2 |
| E78 `INVEXCEPTN` | `join-e78refire2-1788549071.pcap` | `S3-ACK-COALESCE` ×**253** + `S4-ACK-RATE` — the 1:1 ack flood |
| (historical) `LOCKMGRERR` | `ovmx-760-lockmgrerr-20260730.pcap` | `S5-ECHO-DLM` ×**8** (exactly the eight replies §4(p) names) + `S6-ECHO-CM` ×4 + `S5B` ×8 |

### And it is silent on every run that did NOT crash a peer

`e55`, `e56`, `e57`, `e60`, `e63`, `e65`–`e74`, `e77`, `op06`, `op06-join`, and
both in-tree captures: **0 findings**. On this corpus the gate is a clean
discriminator — it fires on the crash runs and nowhere else.

### NEW vectors the retro-audit surfaced (not yet hit as a crash)

Each is a **finding for a follow-up item**, not fixed here (E79 owns the op-06
response path; this work item is audit + gate + doc only).

**N1 — `S3-ACK-COALESCE` was present from the very beginning, in almost every
run.** 112 160 findings across the corpus, including runs long predating E78
(`join-milestone-1021`, `1029*`, `db20b*`, `o38*`, `v197*` — 255 per run is the
recurring shape). The E78 crash was not a new defect: **OVMX has always acked
1:1**, and E78 was simply the first time a real coordinator pushed a 254-frame
burst at it. *Fix location:* the `cat 0x04` ack emitter — it must coalesce to
the measured factor of ≥3 rather than answer per frame. **This is E79's
territory; the finding is that the fix must be judged against the coalescing
law, not against "fewer than 254".**

**N2 — `S12-CREDIT-OVERSEND`: OVMX transmits past the peer's advertised grant.**
88 findings in 15 captures; OVMX reaches **11–15 messages outstanding against a
grant of 10** (e.g. `join-milestone-1031b1`, `db20b7`, `db20b8`,
`db20bcomplete_a554`, `ovmx-760-lockmgrerr` ×40). Never yet observed to crash a
peer, but it is an over-run of the receiver's buffering by the p. 2-43 ledger.
*Fix location:* the SCS send path's window check — it must gate on the peer's
abs-95 grant, and the grant must be **read from the peer's START**, never
assumed.

**N3 — `S10-CONID-ZERO`: OVMX emits CM config frames to remote Con.ID
`00000000`.** 26 findings across 7 captures (`d94-3abaB`, `d94-c40repB`,
`d94-f61-B2rejoin`, `d94-0425*`, `d94-sameid`) — `cat 01 op 14` and
`cat 01 op 01` addressed to a Con.ID the peer does not hold. Zero of 306 670
reference frames do this. Confirms §4(O.26) as a live defect class rather than a
one-off. *Fix location:* the CM config emitter must refuse to send when the
connection's remote Con.ID is unbound, and fail honestly instead (INV-6 — an
unbound Con.ID is not a value to be filled in with zero).

**N4 — `S1-ENVELOPE-JUMP` also fires on `d94-sameid` and `d94-c40repB`**, where
OVMX opens a dialogue whose **local** Con.ID is `00000000` at send-msg# 3/5 with
319/323 already sent. Same root as N3.

**N5 — the E77 premise is contradicted by the corpus** (see C1). E77 made the
SYSAP counter restart per connection on the strength of "golden: every fresh CDT
opens at send_msg=1"; three reference captures show real VMS opening a fresh
Con.ID pair at 9. This does not make E77 wrong (restarting at 1 is also
observed), but the *stated grounding* for it is not what the corpus says, and
`tools/cluster/cm_dialogue_audit.py` still enforces the refuted rule and flags
real VMS nodes with it. **Escalation, not a unilateral change** — the
per-connection-vs-per-node counter scope is an architecture question.

---

## 4. The standing rule

1. **The gate runs before any lab fire.** Audit the previous run's capture with
   `tools/ci/cluster_wire_safety_gate.sh <pcap>`. A FATAL finding means the last
   run put a frame on the wire no reference node ever emits; that fix goes ahead
   of the re-fire.
2. **Reset, and crash-check BOTH consoles first, on every milestone re-fire.**
   Read the consoles for `BUG CHECK` / `CNXMGRERR` / `INVEXCEPTN` / `LOCKMGRERR`
   / `INCONSTATE` and for the §4(O.30) `0xb1` departure marker in the capture,
   *before* reading the run as a protocol result. A run against an already-dead
   or rebooting VAX measures nothing — and the E75→E76 session was confounded
   for a full cycle by reading "connection churn" that was actually OVMX
   crashing both peers.
3. **A new (category, opcode) answer is an allowlist entry, never a default.**
   Ground the response shape in the corpus before emitting it; if it is not
   grounded, send nothing and log it, and raise the gap as work.
4. **Never fill an unbound wire field with a placeholder.** An unbound Con.ID,
   an unknown lock id, a credit we were not granted: omit and fail honestly.
   INV-6 — a placeholder lock id bugchecked the real VAX and crashed the
   cluster; N3 above is the same mistake with a Con.ID.
5. **The endgame is a runtime guard.** The gate reads captures *after* the fact.
   The vectors above are all decidable at emit time from executive state, and
   the executive should ultimately **refuse to emit an unsafe frame** rather
   than emit it and have us find it in a pcap. Sequence: audit (this item) →
   the FSM-level invariants as assertions in the host/sim rungs → an emit-path
   guard in `kernel-core`. Until then, the gate is the seatbelt.

---

## 5. Re-deriving everything here

Nothing above is a remembered conclusion; each number has a command.

| claim | re-derive with |
|---|---|
| every threshold | `tools/cluster/cm_wire_safety_audit.py --measure ~/vax/cluster/captures/*.pcap` |
| the gate has teeth | `tools/ci/cluster_wire_safety_gate.sh` (or `ctest -R cluster_host_wire_safety`) |
| the false-positive baseline | `--audit-all` over the 19 pure-reference captures |
| the retro-audit | the gate over `/data/training/vax/k8s-labs/*/logs/*.pcap` |

**Cross-references.** `docs/cluster-protocol-spec.md` §4(j) (envelope), §4(p)
(response shapes, the barrier, the allowlist rule), §4(q) (what is never
answered), §4(t) (Con.ID allocation), §4(u) (ack cadence), §4(g)/§4(h) (credit
and the SCS window), §4(O.26) (Con.ID 0 after a VC break), §4(O.14) (`send_seq`
is per-VC). Memory: `ovmx-never-crashes-a-peer`,
`executive-backed-not-wire-plumbing`, `cluster-promotion-gap`.
