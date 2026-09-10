# The cat-0x01 membership frames — op-0x05 and op-0x06, field by field

> **STATUS (rd vms-fc7 / vms-3a7c / vms-9c99, 2026-09-10): both closed.** The
> op-0x05 MEMBERSHIP RECORD -- not op-0x06 -- carries the grounded
> {SCSSYSTEMID -> CSID} pairing, and it is what a joiner adopts its identity
> from. §5 was rewritten when the lab oracle and the op-0x05 decode settled the
> assignment rule (round-robin CSV slot); §11 records what shipped and the
> symmetric CN=2 it produced. The interim "ambiguity gate" §5 proposed is GONE
> -- the oracle's own behaviour is a case it would have refused.

## (original title) The cat-0x01 op-0x06 MEMBERSHIP builder

**rd vms-f6b (rig) / vms-1ee (DLM proof) / vms-3a7c (CSID assignment, open).**
Status: **an honest burst IS buildable.** This note is the evidence for that
claim and the one-read basis for the vms-3a7c decision.

Everything below was re-decoded from the captures by a throwaway script that
uses **no project code** — it re-applies only the frame offsets the codec
header names (ethertype at 12, SYSAP body at 72, category at body[8], opcode at
body[9]) and reads raw bytes. Where a number here agrees with
`vms_cluster_codec_cm.h`, that is a confirmation, not a citation.

Captures used:

| capture | role | op-0x06 frames |
|---|---|---|
| `tests/lab/captures/op06-join-20260903.pcap` | VAX1 coordinator → VAX2 joiner, real VAX/VAX join | 255 (from VAX1) |
| `tests/lab/captures/cn3-achieved-20260905.pcap` | **VAX2 coordinator → the OVMX joiner**, the run that reached CN=3 | 254 |
| `scs-tier0.txt` (steady-state tcpdump) | LAN-address grounding | — |

The second capture matters more than its "cross-ref" billing suggests: it is a
**different cluster, a different coordinator and a different epoch**, and it is
the burst OVMX itself consumed when it joined a real VMScluster. Every header
field below is identical in both.

---

## 1. What op-0x06 is, and the one field a joiner consumes

E30 (falsified-and-replaced by these captures) established that op-0x06 is
**not** a membership *list*. It is a stream of one-record-per-frame
re-assertions from the sender's own tables — transport names, SCS process
names, a node model string, a node-parameter block, a member-name record — and
only some of those records carry a CSID.

OVMX's receive path reads **exactly one field**:

```
vms_cm_membership_coordinator_csid()   (src/kernel-core/vms_cluster_codec_cm.c)
    -> a CSID-shaped u32 at body[24:28] (form A), else body[36:40] (form B)
join_learn_csid_from_membership()      (src/kernel-core/vms_cnxman_join_fsm.c)
    -> generation = coord_csid >> 16          (the ONLY thing taken from the wire)
    -> own_csid   = vms_cm_csid_of(generation, OUR OWN SCSSYSTEMID)
```

The joiner does **not** look for its own record, does not parse a list, and
does not copy a CSID. It learns a **generation** and derives its own identity
from its own real SYSGEN state. That is what makes a minimal burst sufficient:
**one genuine CSID at one grounded offset teaches a joiner everything op-0x06
has ever taught OVMX.**

---

## 2. The 132-byte body, field by field

`VMS_CM_BODY_LEN = 132`, `body[0] == abs 72`. Counts are over the 255 VAX1
frames of `op06-join`; the `cn3` column is the independent re-measurement.

### 2a. The 24-byte CM header — fully grounded

| offset | field | op06-join | cn3 | GROUNDED? | what OVMX writes |
|---|---|---|---|---|---|
| `body[0:2]` | send-msg # | per-frame counter | same | **YES** (envelope) | the envelope stamper's real counter (`cnxman_envelope_originate`) |
| `body[2:4]` | ack-msg # | per-frame | same | **YES** (envelope) | ditto — the peer's real high-water |
| `body[4:6]` | txn | `0000` 255/255 | 254/254 | **YES** | 0 — a NOTIFICATION, never answered with 0x81 (allowlist row: `VMS_WIRE_ACT_CONSUME`) |
| `body[6:8]` | token | `0000` 255/255 | 254/254 | **YES** | 0 |
| `body[8]` | category | `0x01` 255/255 | 254/254 | **YES** | `0x01` |
| `body[9]` | opcode | `0x06` 255/255 | 254/254 | **YES** | `0x06` |
| `body[10:12]` | — | **uninitialised buffer residue**: `0000` ×164, `"AN"` ×24, `"RE"` ×24, `"Xc"` ×23, … | same character | **NO (and must not be)** | 0 — sec 4(p): *do not reproduce another implementation's uninitialised memory* |
| `body[12:16]` | epoch | `0x00000006` 255/255 | `0x00000005` 254/254 | **YES** | the CLUB's **real** transition epoch (`c->epoch`) |
| `body[16]` | role | `0x20` 255/255 | 254/254 | **YES** | `VMS_CM_ROLE_COMMIT` (0x20) |
| `body[17]` | class | `0x02` 255/255 | 254/254 | **YES** | `VMS_CM_CLASS_ADD` (0x02) |
| `body[18:20]` | — | `0000` 255/255 | 254/254 | **YES** | 0 |
| `body[20:24]` | per-frame countdown | `0xff, 0xfe, 0xfd …` descending to `0x00`, then the burst repeats | same, starts `0xff` | **offset yes, SEMANTICS NO** | **HONEST-OMIT**: 0, counted. `0x00` is an observed value; what the counter *means* (records remaining? buffer index?) is not established, so OVMX asserts nothing by it |

That the entire header is byte-identical in shape across two different
clusters, two different coordinators and two different epochs is the strongest
grounding any CM opcode in this codebase has.

### 2b. The 108-byte payload `body[24:132]`

| offset | field | measurement | GROUNDED? | what OVMX writes |
|---|---|---|---|---|
| `body[24:28]` | **form-A CSID** | 24/255 frames carry a CSID-shaped u32 here, and it is **only ever `0x00010001`** — VAX1's own CSID (sysid 1025, `1025 & 0x3ff = 1`, generation 1). Independently in `cn3`: 24/254, only `0x00010001`. **Zero false positives in either capture.** | **YES** | **the coordinator's OWN REAL CSID**, read from `club->local_csid` |
| `body[28:36]` | incarnation quadword | the fixed point both forms share; real values (VAX boot times) | **offset YES, value NOT AVAILABLE** | **HONEST-OMIT**: 0, counted. This node *has* a real incarnation, but it lives in the port's identity (`pe_incarnation()`), and the coordinator FSM is a pure TU that may not reach across the seam. A named, fixable gap — not a guess |
| `body[36:40]` | **form-B CSID** | 69/255 (`0x00010003` ×46, `0x00010001` ×23); `cn3` 92/254 (`0x00010003` ×46, `0x00010001` ×23, `0x00010002` ×23 — all three real members) | **YES** | not written — OVMX builds **form A only** (see §4) |
| `body[40:132]` | the rest of whichever sub-record this frame carries: ASCII names (`"TRANSPORT"`, `"DIR_LOOKUP"`, `"SYSTEM$VAX1"`, `"er 3900 Series"`), a params block with `"V7.3    "` at `body[88:96]`, **and VAX kernel pointers** (`0xc198a160`, `0x00bc1e91`, `0xbf12bdc0`) | **NO** — and the pointer-bearing bytes are another implementation's memory, which Rule 8 forbids reproducing | **HONEST-OMIT**: 0, counted |

---

## 3. Is a zero remainder a FABRICATION? — measured: **no**

`vms_cluster_codec_cm.h:700-720` warns that a zero-filled op-0x06 "would assert
an empty membership list … a fabrication with a cluster-breaking failure mode".
That warning was written under the premise E30 **falsified**: that op-0x06
carries a membership list. The captures refute it directly, twice:

> **23 of 255** VAX1 op-0x06 frames (9.0 %) have an **entirely zero** 108-byte
> payload `body[24:132]`. In `cn3`, **23 of 254** (9.1 %).
>
> They are **interleaved through the burst** — the first at burst position 5 of
> 255, then every ~11 frames (positions 5, 15, 25, 35, 46, 57, 68, …), with
> ordinary countdown values (`0xfa, 0xf0, 0xe6, …`). They are **not** trailing
> padding.

So the reference coordinator itself emits zero-payload op-0x06 frames, ~9 % of
the time, **in the middle of a join that succeeded** — in the `cn3` case, the
very join in which OVMX became a member of a real VMScluster. A zero payload is
therefore demonstrably *not* an assertion of "the cluster is empty"; it is a
shape the reference puts on the wire as a matter of course.

Further: **162 of 255** op-0x06 frames (138/254 in `cn3`) carry **no** CSID at
either grounded offset. A joiner that reads nothing from a given op-0x06 frame
is the *normal* case, and OVMX's reader already handles it (`csid_unpinned++`,
one console line, no state change).

The `:715` concern remains correct for the two opcodes it also names — the
op-0x05 lock/resource rebuild burst and the originating cat-0x02 op-0x0d
record. Nothing here changes those; they still have no builder.

---

## 4. The minimal grounded burst

```
cat 0x01 / op 0x06, one 132-byte body:

  body[8]      = 0x01                  GROUNDED (509/509 frames, both captures)
  body[9]      = 0x06                  GROUNDED (509/509)
  body[12:16]  = the CLUB's real epoch GROUNDED (offset + role; value is ours)
  body[16]     = 0x20  ROLE_COMMIT     GROUNDED (509/509)
  body[17]     = 0x02  CLASS_ADD       GROUNDED (509/509)
  body[24:28]  = THIS COORDINATOR'S    GROUNDED (form A; 48/48 CSID-bearing
                 OWN REAL CSID                   form-A frames across both
                                                 captures carry only the
                                                 sender's own genuine CSID)
  everything else = 0, and COUNTED as omitted
```

**Form A, not form B**, for three reasons: it is the offset the reader tries
**first**; it is the record in which the sender asserts **its own** CSID (form B
is used to re-assert *other* members' records, which OVMX cannot honestly do —
it holds no incarnation for them); and it is the offset with the cleaner
measurement (one distinct value, both captures).

**One frame, not a burst.** The joiner needs one. Integration note E78 records
that a 254-frame membership burst is what provoked the ack storm that
bugchecked VAX2 and kept it down. Emitting the fewest frames that carry the
fact is both the honest minimum and the smallest crash surface (⭐⭐ *never
crash a peer*).

**Refusal, not zero-fill, when there is nothing to say.** If the coordinator's
own CSID fails the shape test, the builder returns `VMS_CODEC_E_RANGE` and
**no frame is sent**. A burst that names no real CSID teaches a joiner nothing;
sending one anyway would be noise asserting a shape we do not have.

---

## 5. The CSID assignment question — vms-3a7c, stated for one read

The builder settles what goes *on the wire*. It does **not** settle **which
CSID a joiner ends up with**, because the joiner does not take the CSID from
the wire — it takes the *generation* and derives the rest. Two rules are in
play and **they must agree or the joiner never becomes a member**:

* the **coordinator** stamps the subject's CSB with a CSID
  (`coord_assign_slot`), and that low word is the bit it sets in the
  transition nodemap;
* the **joiner** derives its own CSID as `generation << 16 | (SCSSYSTEMID &
  0x3ff)`, and `phase2_csb_in_nodemap()` looks for *that* low word as its bit.

### Option A — round-robin CSV slot (what ships today, **provisional**)

`coord_next_slot()` = `max_slot_seen + 1`, never reused, slot 0 never used
(p. 7-25). `coord_assign_slot()` writes `(1 << 16) | slot`.

* Grounded by: p. 7-25's CSV-slot model, and the fact that slot 0 is unused.
* **Not** grounded by the captures — see below.
* Note two latent details a reader should see: it hardcodes generation `1`
  rather than reading the CLUB's generation, and it bypasses
  `vms_cm_csid_of()`, the "one spelling of the published construction". Both
  are harmless while the only cluster generation is 1; both should be folded
  into whatever vms-3a7c decides.

### Option B — joiner self-derive (`SCSSYSTEMID & 0x3ff`)

The construction E30 quotes as *published*: `generation << 16 | SCSSYSTEMID &
0x3ff`. The coordinator would assign that low word rather than the next slot.

* Grounded by: the CSID construction itself; the shape test
  (`VMS_CM_CSID_SHAPE_LO_MASK = 0xFC00`) is *derived* from it, i.e. the codec
  already assumes the low word is a 10-bit SCSSYSTEMID.

### What the captures actually ground — and what they cannot

Every real CSID observed:

| node | SCSSYSTEMID | `sysid & 0x3ff` | observed CSID |
|---|---|---|---|
| VAX1 | 1025 | 1 | `0x00010001` |
| VAX2 | 1026 | 2 | `0x00010002` |
| VAX3 | 1027 | 3 | `0x00010003` |

**The two rules are indistinguishable on this data.** The lab's SCSSYSTEMIDs are
consecutive from 1025, so "next free slot" and "`sysid & 0x3ff`" produce the
same number for every node in every capture we hold. That is the whole of
vms-3a7c, and no capture in the repository can separate them.

### RESOLVED (rd vms-fc7): the rule is the ROUND-ROBIN CSV SLOT

A first pass at cn3 looked for OVMXJ1's assigned CSID in the op-0x06 burst and
did not find it (the burst that reached OVMXJ1 was composed before its
admission completed, so its record was not in it yet). The answer is not in the
burst — **it is in the transition-open NODEMAP**, where the bits *are* the CSV
slots, and there it is unambiguous by causality inside a single capture:

| epoch | frame | transition | nodemap `body[55]` | bits |
|---|---|---|---|---|
| 5 | 273 | VAX2 → VAX1, op-09 **ADD** | `0x0e` | {1, 2, **3**} |
| 5 | 834 | VAX2 → OVMXJ1, op-09 **ADD** | `0x0e` | {1, 2, **3**} |
| 6 | 11347 | VAX1 → VAX2, op-08 **REMOVE** | `0x06` | {1, 2} |

Three stations are ever on that segment: 1025 (VAX1), 1026 (VAX2) and 1986
(OVMXJ1, first frame t+15.247 s, gone by epoch 6). **Bit 3 is present in
exactly the transition that admits OVMXJ1 and absent in exactly the transition
that removes it**, and no other station enters or leaves. So CSV slot 3 is
OVMXJ1's.

OVMXJ1's SCSSYSTEMID is **1986**, and `1986 & 0x3ff = 962 (0x3C2)` — a bit index
that is not expressible in the nodemap byte at all.

> **The real VAX coordinator assigned the round-robin CSV slot (3), not
> `SCSSYSTEMID & 0x3ff` (962). vms-3a7c is CLOSED: Option A is correct.**

**Independent corroboration** from `op06-join-20260903.pcap`, a different
cluster incarnation with only 1025 and 1026 on the wire: its epoch-6 ADD open
(frame 450) carries nodemap `0x0a` = {1, **3**}. Both bits must be real members
— VAX1 coordinates (op-09/0a/81-0b/0c) and VAX2 is the barrier participant
(12 × op-0b) — so one of `1025` / `1026` holds slot 3, which neither
`1025 & 0x3ff = 1` nor `1026 & 0x3ff = 2` can produce. (p. 7-25's "a rejoining
system gets a NEW CSID, never its old one back" is the natural explanation for
the vacant slot 2.)

### Two claims in the first draft of this note were WRONG — corrected here

1. ~~"`0x00010003` belongs to a system absent from the segment."~~ **False.** In
   `op06-join` slot 3 is held by one of the two VAXes that ARE present. The
   record is an ordinary member's, not a retained ghost.
2. ~~"A burst never carries its recipient's own CSID."~~ **False**, and it was
   the load-bearing step of the old §8 argument. `op06-join`'s burst
   (VAX1 → VAX2) carries slot 3's record ×46, and slot 3 is one of those two
   nodes — so the burst *does* carry the recipient's own record. The reason
   cn3's burst lacks OVMXJ1's record is **temporal**: frames 245–833 precede the
   op-09 (834) that admitted it.

### What this DOES and does NOT license in code

It settles what the COORDINATOR must assign — and OVMX's `coord_next_slot()`
round-robin was already right, so **no coordinator change is needed**; the §5
gate below can eventually be retired rather than widened.

It also proves the **JOINER's self-derive is WRONG**:
`join_learn_csid_from_membership()` computes
`generation << 16 | (own SCSSYSTEMID & 0x3ff)`, which for OVMXJ1 is
`0x000103C2` while the cluster assigned `0x00010003`. With a CSID whose low word
is 962, `phase2_csb_in_nodemap()` finds slot 962 inexpressible, answers
"unknown", and **OVMX can never select itself into the map** — which is exactly
why OVMX's own executive could not have read MEMBER in the CN=3 run even while
VAX1's `SHOW CLUSTER` counted it. That is a real, grounded defect.

**It does NOT, however, tell the joiner HOW to learn its slot**, and that is the
open design question: the nodemap names slots but not systems, so a joiner
receiving `0x0e` cannot tell which of the three bits is its own without either a
pre-transition map to diff against or a grounded {system → slot} association.
**Escalated, not implemented.**

### The safety gate this note recommends (and the implementation uses)

Rather than a kill-switch, gate on the **ambiguity itself**:

> A subject is admitted **only when the two candidate rules agree** for it —
> that is, when the assigned CSV slot equals `SCSSYSTEMID & 0x3ff`. When they
> differ, the transition is refused with a named reason and counted; nothing is
> stamped and nothing is emitted.

Why this is the right shape:

* It is not a guess. Under the gate, the CSID OVMX asserts is correct **under
  either rule**, so however vms-3a7c settles, no frame OVMX has emitted becomes
  retroactively wrong. That is the only form of "safe toward a real VAX" that
  does not require knowing the answer first.
* It degrades honestly: a configuration the executive cannot name unambiguously
  gets a refusal an operator can read, not a plausible-looking CSID.
* It is one predicate in one place, deleted or narrowed in one edit when the
  oracle lands.
* It subsumes the VAX-facing question without identity-sniffing the peer: no
  emission toward *anyone* — VAX or OVMX — can carry an assignment the two
  rules disagree about.

Its cost, stated plainly: a cluster whose SCSSYSTEMIDs are not congruent with
its CSV slots cannot admit members until vms-3a7c settles. Today it cannot
admit anyone at all, so this is strictly forward progress, and the refusal says
exactly why.

---

## 6. Honest omissions, all counted

| omitted | why | counter |
|---|---|---|
| `body[20:24]` countdown | offset grounded, semantics not | `membership_fields_omitted` |
| `body[28:36]` incarnation | offset grounded; no accessor for *our own* incarnation from a pure FSM TU | `membership_fields_omitted` |
| `body[40:132]` sub-record body | not grounded; partly another implementation's memory (Rule 8) | `membership_fields_omitted` |
| form-B records for *other* members | OVMX holds no incarnation for a peer; asserting one is the E76/E78 vector | not built at all |
| the 254-frame burst cadence | not needed; E78's crash vector | one frame per admission |

---

## 7. What the builder actually did — measured on two real executives

Built, and run on the 2-node rig (`tests/qemu/run_cluster_genesis_2node.sh`,
on the k3s worker under KVM). All values below are read back out of the
executive through `VMS_IOCTL_CLUSTER_DIAG_CSB` / `_GETSYI` / `_MEMBER_GET`.

```
RIG-A-FINAL role=founder                member=1 state=MEMBER csid=0x00010001 cn=2 epoch=2
RIG-B-FINAL role=member-no-coordinator  member=1 state=MEMBER csid=0x00010002 cn=1 epoch=2
```

Node A's own transcript:

```
%CNXMAN, this node has quorum by its own votes: forming an OpenVMS Cluster
%CNXMAN, this node is a member of the cluster
%CNXMAN, proposing addition of a system to the cluster
%CNXMAN, system 0000000000000402 was added to the cluster        <- 0x402 = 1026 = node B
```

Node B's own transcript:

```
%CNXMAN, committed member count differs from the transition nodemap
%CNXMAN, this node is a member of the cluster
%CNXMAN, system 0000000000000402 was added to the cluster
```

**The admission is real.** Node B holds CSID `0x00010002`. It did not receive
that value: it read a *generation* out of the op-0x06 record node A built,
and computed `1 << 16 | (1026 & 0x3ff)` from its own SYSGEN state. Phase 2
then committed its membership off the nodemap bit node A really asserted.

**The control that makes that a measurement.** `RIG_MODE=ambig` moves node B's
SCSSYSTEMID to 1030 and changes nothing else. The two candidate CSID rules then
disagree (slot 2 vs `1030 & 0x3ff = 6`), node A refuses -- *"%CNXMAN, this
system's cluster system id cannot be assigned unambiguously; membership request
not proposed"* -- **sends no op-0x06**, and node B stays `role=none member=0
csid=- JOINING`. Same code, same wire, one SYSGEN digit apart: without a real
op-0x06 there is no membership. (`RIG_MODE=negctl`, VOTES=0 on both, likewise
leaves both nodes with no membership and no CSID.)

Two executive defects had to be fixed before any of this could be observed, and
both were found by the rig rather than by reading:

1. **The joiner's advert handler swallowed the op-0x02 membership request.**
   `join_h_peer_advert()` consumed cat-0x01 op-0x02 as a "peer advert", and the
   router offers a body to the join FSM first -- so the coordinator's one
   selection edge (`[IDLE][RX_TR_REQUEST]`, and being asked is what MAKES a node
   the coordinator, book pp. 7-37/7-38) was never reached. Both nodes put their
   op-0x02 on the wire for a whole run and neither ever proposed anything.
2. **A member's own membership was overwritten by a later join drive**
   (`cnxman_join_drive` wrote `cl->state = JOINING` unconditionally), so the
   founder reported `member=0` the moment a second node appeared.

## 8. THE REMAINING GAP: a joiner cannot count the OTHER member

`cn` disagrees: node A counts 2, node B counts 1. This is not a defect in the
builder and it is **not** something that can be closed honestly today.

`cnxman_club_recount_members()` counts CSBs carrying SELECTED, and
`phase2_csb_in_nodemap()` can only match a CSB to a nodemap bit if that CSB's
**CSID is known** (`csb->csid_valid`). Node B holds a CSID for itself. It holds
**none for node A**, so A's CSB cannot be matched, is not SELECTED, and is not
counted -- and B's own quorum cells (`cevotes`, `quorum`) stay empty for the
same reason. B's executive says exactly this on the console: *"committed member
count differs from the transition nodemap"*. The same single cause makes B's
CLUB carry no coordinator CSID, which is why its role reads
`member-no-coordinator` rather than `joiner`.

**Why B cannot simply take the CSID off the op-0x06 it just read.** E30 is
explicit, and both captures confirm it: the CSID a burst carries is *sometimes
the sender's own and sometimes another already-admitted member's*. Measured
directly -- in `cn3-achieved-20260905.pcap` the sender is VAX2 (SCSSYSTEMID
1026) and its form-A record carries `0x00010001`, **VAX1's** CSID, not its own.
Attributing a form-A CSID to the sending CSB is therefore a fabrication that the
capture refutes outright, and it is not done.

Three candidate closures, for the record:

| # | closure | honest today? |
|---|---|---|
| 1 | Derive a peer's CSID as `generation << 16 \| (peer SCSSYSTEMID & 0x3ff)` from state the executive really holds (the peer's SCSSYSTEMID off its CSB, the generation off the wire) | **NO, not unilaterally.** It asserts the unresolved vms-3a7c rule *about a system this node was never told about*. Inside an OVMX-coordinated cluster the §5 ambiguity gate makes it self-consistent; against a **real VAX coordinator** there is no such guarantee, and a wrong CSID becomes a wrong nodemap bit -- the class of error that has bugchecked real VAXes. **Escalated, not implemented.** |
| 2 | Attribute the form-A CSID to the sender only when its low word equals that sender's own `SCSSYSTEMID & 0x3ff` | Measurably better than #1 -- it attributes 24/24 correctly in `op06-join` and correctly REFUSES 24/24 in `cn3`, where attribution would have been wrong -- but it still decides attribution by assuming the vms-3a7c construction. Also **escalated, not implemented.** |
| 3 | Ground the association from a real cluster | The clean answer, and a **lab item**: see below. |

**SUPERSEDED IN PART (rd vms-fc7).** The paragraph that used to stand here
proposed a lab capture and justified it with "a burst never carries its
recipient's own CSID" — which the re-decode **refuted** (see §5). The gap
itself is unchanged, but its shape is now much better understood:

* The **slot** assignment rule is settled: round-robin CSV slot (§5). What is
  still missing is the *association* — which system holds which slot.
* The **nodemap already names the slots** (`body[55]`, bits = CSV slots, and
  §5 uses exactly that to settle vms-3a7c by causality). What it does not carry
  is which system each bit is. A node that has watched a cluster since before an
  admission can diff two nodemaps and learn the new member's slot; a node that
  joins cold cannot.
* So the honest closure is either (a) a grounded {system → slot} field, or
  (b) a nodemap-diff rule for a node that really did observe the earlier map —
  which is real state, not an assumption, but is a design question about when a
  joiner may rely on it. **Escalated, not implemented.**

## 9. Scope this note does **not** claim

* **Not interop-verified.** No OVMX-built op-0x06 has been put in front of a
  real VAX. The evidence here is that the frame is *grounded*, not that a VAX
  accepts it.
* The op-0x05 lock/resource rebuild burst and the originating cat-0x02 op-0x0d
  record still have **no builder** and are untouched.
* The CSID assignment rule remains **provisional pending oracle (vms-3a7c)**,
  marked as such in the code.

---

## 10. What completes a VMS state-transition commit (rd vms-fc7)

The question came from `cn3-achieved-20260905.md`'s honest note: OVMXJ1 became a
counted MEMBER with only one `01/0b`/ack pair on the wire and no `01/0c` at all,
so "whatever completed the transition did not require an on-wire op-0c". Two
findings, one of which corrects that note.

### 10a. The premise was scoped wrong — cn3 DOES contain a full 12-step barrier

Whole-capture census of cn3: **`01/0b` ×13, `01/0c` ×12.** They belong to a
*second* transition the note does not mention — the **epoch-6 class-0x03 REMOVE
at t+1584.23 s**, VAX1 coordinating, VAX2 participating, after OVMXJ1 departed:

```
11349  VAX1->VAX2  01/0a  GO
11350  VAX2->VAX1  01/0b  step 1     11351  VAX1->VAX2  81/0b   11352  VAX1->VAX2  01/0c  release 1
11353  VAX2->VAX1  01/0b  step 2     11354  VAX1->VAX2  81/0b   11355  VAX1->VAX2  01/0c  release 2
  ...  (step index in body[16:20] walks 1..12, no gaps)  ...
11394  VAX2->VAX1  01/0b  step 12    11395  VAX1->VAX2  81/0b   11396  VAX1->VAX2  01/0c  release 12
```

`op06-join-20260903.pcap` shows the same walk for a class-0x02 **ADD** (frames
450–1107): 12 × `01/0b`, 12 × `81/0b`, 12 × `01/0c`, steps 1..12.

**So the 12-step 0b/ack/0c barrier is real VMS, for ADD and for REMOVE, and
OVMX's E85 model of it is NOT an over-model.** The `12 × (M−1)` law holds in
both: nodemap `{1,3}` → M=2 → 12 steps; nodemap `{1,2}` → M=2 → 12 steps.

### 10b. The membership commit is Phase 2 at the GO, NOT op-0c #12

The cn3 **epoch-5 ADD** genuinely did stall its barrier (nodemap `{1,2,3}` →
M=3 → 24 `01/0b` expected; exactly **1** observed, from OVMXJ1, acked by VAX2 at
frame 856, never released). VAX1 sent **no** `01/0b` for epoch 5 — and VAX1↔VAX2
traffic is definitely in this capture, since the entire epoch-6 barrier between
them is. The coordinator cannot release step N until every participant reports
it, so the barrier stopped at step 1.

**And the membership committed anyway.** VAX1 received the GO (frame 850,
t+18.9821 s) and counted `CN_3` from then, sustained across every poll to
t+600 s, and 26 minutes later ran a clean epoch-6 REMOVE. Nothing else passed
between them for that epoch.

That is p. 7-42 exactly: Phase 2 — nodemap into the CSBs, quorum, the count,
the CLUSTER flag — runs **at the GO**, and the 12-step barrier is the
lock-database rebuild synchronisation that *follows* the commit.

**OVMX already models this correctly.** `barrier_h_go()`
(`src/kernel-core/vms_cnxman_barrier_fsm.c`) calls `barrier_commit_phase2()`
with the comment *"The count commits HERE, before step 1 goes out and before a
single rebuild record is answered (p. 7-42)"*, and
`phase2_commit_local_membership()` sets `cl->state = VMS_CLUSTER_MEMBER` there.
So OVMX's `cl->state` does not wait for op-0c. What waits for op-0c #12 is the
**join FSM's own promotion** (`join_h_transition_done`, gated on
`cnxman_barrier_commits()`, which `barrier_finish()` moves only on a genuine
release #12, E79) — a narrower thing than "am I a member", and a deliberate E79
choice. Worth a decision, not a silent change.

### 10c. Why the epoch-5 barrier stalled: AMBIGUOUS on the wire

The pcap does not say why VAX1 stayed silent. What can be ruled out:

* **Not the DLM rebuild records.** E85's hypothesis was that the barrier gates
  on `cat-02 op-0d`. cn3 contains **zero** `02/0d` frames in the whole capture,
  yet its epoch-6 barrier walked all 12 steps. Refuted.
* **Not a capture blind spot.** VAX1↔VAX2 CM traffic is present throughout.

What correlates across the three observed transitions is only this: the two that
walked 12 steps each had exactly **one** participant and it was a real VAX; the
one that stalled had **two** participants, one of them OVMX. That is n=1 for the
failing case and is **not** sufficient to implicate OVMX.

**The run that would settle it:** a 3-VAX ADD with no OVMX present. If the
barrier walks 24 steps, OVMX's presence is implicated; if it also stops early,
M≥3 ADDs simply behave differently from the M=2 case both captures show.

### 10d. Verdict

| question | answer |
|---|---|
| Does OVMX over-model op-0c #12? | **No.** Both captures show the full 12-step 0b/ack/0c walk, for ADD and REMOVE. |
| Does OVMX under-model the commit? | **No.** OVMX commits Phase 2 at the GO, which is what cn3 shows the real cluster doing. |
| Code fix warranted from this? | **No.** The barrier and Phase 2 models both match the wire. |
| Doc correction warranted? | **Yes** — `cn3-achieved-20260905.md`'s "no op-0c ever appears" is corrected in place. |
| Anything left open? | Why VAX1 skipped the epoch-5 barrier (needs the 3-VAX ADD run), and whether the join FSM's op-0c #12 promotion gate should move to the GO (an E79 decision, not a decode question). |

---

## 11. What shipped, and the symmetric CN=2 it produced (rd vms-fc7 / vms-9c99)

Three changes, together:

**(a) The coordinator originates op-0x05 membership records.**
`vms_cm_membership_rec_build()` writes only the grounded fields (§5c) and
`coord_send_membership_set()` distributes them the way the reference does --
the FULL member set to the joiner, the DELTA to each already-present member.
Every field is a projection of the CSB this coordinator really holds: the
SCSSYSTEMID the port learned, the CSID `coord_assign_slot()` actually stamped,
that CSID's own slot for the index, that member's real incarnation. A member
this node holds no complete identity for gets NO record (counted). Its
`body[42:132]` -- uninterpreted stale buffer in the reference -- is emitted
ZERO and counted, never reproduced (Rule 8).

**(b) The joiner ADOPTS rather than derives.** `join_adopt_membership_rec()`
takes the CSID out of the record that names its own SCSSYSTEMID, re-adopting on
every admission and caching nothing (p. 7-25: a rejoining system gets a NEW
CSID). Records about OTHER members are filed on the block this CLUB already
holds for that SCSSYSTEMID -- which is what lets a joiner COUNT the cluster --
and a record about a system it holds no block for is counted and dropped, never
invented. The old `generation << 16 | (SCSSYSTEMID & 0x3ff)` self-derive is
gone; op-0x06 now only confirms a generation, counted.

**(c) The joiner promotes at the GO, not at op-0x0c #12** (§10b). The barrier
gains `phase2_commits`, moved by `barrier_commit_phase2()`, and the join FSM
promotes on that -- the same criterion membership and the coordinator already
use. A real VAX commits an ADD with no on-wire op-0x0c to the joiner, so the
old trigger was an interop hang.

**The interim §5 ambiguity gate was REMOVED**, and had to be: it admitted a
system only when the round-robin slot and `SCSSYSTEMID & 0x3ff` agreed, and the
oracle's own behaviour (1986 -> slot 3) is exactly a case it would have refused.

### Measured, on two real executives (rig on the k3s worker, KVM)

```
RIG-A-FINAL role=founder member=1 state=MEMBER csid=0x00010001 cn=2 epoch=2 projections=agree
RIG-B-FINAL role=joiner  member=1 state=MEMBER csid=0x00010002 cn=2 epoch=2 projections=agree
  GENESIS 2-NODE PROOF PASSED
```

Node B's own CSB table now names both systems:

```
RIG-B-CSB i=0 node=OVMXB sysid=1026 csid=0x00010002 state=MEMBER
RIG-B-CSB i=1 node=-     sysid=1025 csid=0x00010001 state=MEMBER
RIG-B-GETSYI member=1 nodes=2 votes=0 quorum=0 csid=0x00010002
```

and its transcript says how it got there:

```
%CNXMAN, the cluster assigned this node a cluster system id
%CNXMAN, this node is a member of the cluster
%CNXMAN, this node is now a VAXcluster member
%CNXMAN, system 0000000000000402 was added to the cluster
```

### The controls

* **no-derive** (`RIG_MODE=noderive`): node B's SCSSYSTEMID is **1030**, whose
  low ten bits are **6** -- and its executive reports CSID **0x00010002**, CSV
  slot **2**, the slot the coordinator assigned. A value it could not have
  computed, so the identity was demonstrably ADOPTED off the wire. HELD.
* **negctl** (VOTES=0 on both): neither founds, neither reaches MEMBER, neither
  holds a CSID. HELD.

### Two things this run does NOT claim

* **Node B's quorum arithmetic is still empty** (`quorum=0 cevotes=0`). B counts
  both members but has not learned node A's VOTES, which ride op-0x01 PARAMS
  rather than the membership record. Out of scope here; recorded, not hidden.
* **Node A logs two "unroutable VMS$VAXcluster frame" lines** -- node B's
  `0x81/0x05` echoes of the records A sent it. The response is correctly
  *answered* by B; A's coordinator simply owns no edge for a 0x81/0x05 and says
  so rather than inventing one. Counted and honest; closing it means grounding
  what a coordinator does with that echo, which no capture shows.
