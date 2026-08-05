# MSCP support — design & direction (vms-54f)

**Status:** investigation output, 2026-08-05. Owner item: vms-54f (MSCP investigation:
decode the unidentified SCA message types and decide what OVMX must serve).
Blocks: vms-ecff (type 10), vms-07a (types 8/9).

**Sources.** (a) *VAXcluster Principles* (Roy G. Davis, Digital Press, 1993),
chapters 4 ("Digital Storage Architecture") and 5 ("Disk Storage Options"),
transcribed 2026-08-05 to `~/cluster/transcript/ch4-5-part01..10.md` +
`ch4-5-INDEX.md` (host-only, copyrighted, **never commit** — same regime as the
ch.2 transcript; quote with page cites only). Public documentation — Rule 8
compliant. (b) Our own lab captures under `/data/training/vax/cluster/`
(`captures/` + `work/`) and `/data/training/vax/clean-cluster/captures/`.
The DEC-Confidential bitsavers *VAXcluster Disk I/O Internals Manual* was **not**
consulted (excluded under Rule 8).

**Corpus gaps.** pp. 4-22–4-35 and 4-39 were re-shot and transcribed same day
(`ch4-5-part11.md`): HSC hardware internals only — no MSCP wire-format tables.
This settles that **chapter 4 contains no numeric opcode/status enumeration
anywhere**; MSCP numeric values remain capture-grounded only (§2). Still
missing: p. 4-103, Figures 4-29/4-30, pp. 5-58–5-69.

---

## 1. The three unidentified message types — resolved shape

### 1.1 The SCS message envelope unifies across every length class

Measured over the full lab-1 corpus (163 pcaps: `cluster/work/`,
`cluster/captures/`, `clean-cluster/captures/`), every SCS message — connect
dialogues, the 190-content add-member class, and the MSCP command frames —
shares **one** header at SCA-content offsets:

| Content offset | Field | Evidence |
|---|---|---|
| [42:44] | inner length (bytes from [44] to end) | 0x0E on 58-content, 0x32 on 94-content, 0x42 on 110-content, 0x92 (146) on 190-content — exact in every frame checked |
| [44:46] | constant 0x0004 (format word) | every frame |
| [46:48] | **SCS message type (MTYPE)**, LE u16 | see §1.2/§1.3 |
| [48:50] | credit field | already grounded (§4(g) WIRE VERDICT) |
| [50:58] | connection-handle pair (dst, src) | already grounded (§4(g) phase 4, §4(h)); observed swapped by direction on established connections |

This subsumes the spec's separate treatment of the "connection-control classes"
vs the 190-byte class: the 190-content class carries the same MTYPE field —
**173,927 of 173,927** frames in the sampled `work/` corpus carry MTYPE=10,
inner length 146.

**Offset-convention erratum found while measuring:** the spec's §4(h)(1a)
offsets ("[46:48] type", "[62:78] connect data") are **SCA-content-relative**,
while §4(a)/(b)/(d) offsets are frame-absolute (content + 14). On-the-wire
pcap frames are `14-byte Ethernet header + content`; the "58/62/66/110-byte
class" names are **content** lengths (72/76/80/124 on the wire). Recorded in
the spec alongside the byte-offset convention.

### 1.2 Type 10 = the SCS "application message" MTYPE — IDENTIFIED

The book (p. 4-13) defines exactly three SCS MTYPE taxa: SCS sets MTYPE to
"application message" for SYSAP-to-SYSAP traffic on an open connection, the
alternatives being "application datagram" and "SCS control message" (used for
connection formation and management). Figure 4-5 (p. 4-14) shows the worked
example — an MSCP command nested inside a packet whose SCS header carries
`CREDIT — SCS MTYPE, DESTINATION CONID, SOURCE CONID`. Delivery dispatch
(p. 4-15): SCS routes on MTYPE — "message" goes to the message input routine
in the CDT for the connection named by the destination CONID.

Capture grounding, all three legs:

1. The **golden MSCP command frames** (94-content, `af2-firsttimer-established-
   20260728.pcap`, 112 frames — the very frames `scs_mscp.c` replays) carry
   MTYPE=10 at content[46:48], credit=1, valid handle pairs, on two distinct
   connections. The template bytes in `scs_mscp.c` already contain `0a 00` at
   content[46:48].
2. The **190-content add-member dialogue** — the VMS$VAXcluster SYSAP body
   grounded in §4(j) — is uniformly MTYPE=10.
3. The 110-content type-10 population (12,872 frames in this corpus; the 2,889
   of vms-ecff were the same class in the earlier restricted corpus) rides
   open-connection handle pairs with binary bodies — application payloads, not
   connect data.

So the 110-content class splits exactly as {0=CONNECT_REQ, 2=ACCEPT_REQ,
10=application message} — connection formation vs payload, one envelope.

**Consequence for vms-ecff:** "OVMX emits zero type-10 frames" is re-framed:
OVMX *does* emit MTYPE-10 frames wherever it replays captured SYSAP messages
(MSCP commands, add-member bursts — the templates carry the field). What OVMX
has never done is **originate** an application message from a live SCS layer
that knows the field exists. The gap is not a missing frame type; it is that
the SCS envelope (MTYPE, inner length, dispatch-on-MTYPE) is not implemented
as a layer. That is precisely the vms-187 "build the SCA layer" strategy, now
extended one field further.

### 1.3 Types 8/9 — paired control exchange; identification still open, candidates narrowed

Observed (mixed-source corpus; formal per-population split still owed to the
OUI-rule census tool):

- 58-content class, MTYPE ∈ {5, 7, 8, 9} — the class is *response/short
  control* shaped: envelope + handle pair, **no payload** (inner length 14).
- **8→9 is a request/response pair**: 8 from A with handles (X,Y), 9 from B
  with handles swapped (Y,X), on an **established** connection (after
  ACCEPT_RSP). Counts pair across the corpus (616 vs 566).
- Observed sequence at teardown (`work/control-vax3-late.pcap` frames
  5297–5302): `8 (A→B) → 9 (B→A) → DISCONNECT_REQ 6 (A→B) → 7 (B→A)` on one
  handle pair. The 8/9 exchange **immediately precedes disconnect** — the
  position vms-07a flagged.
- Real-VAX-sourced instances exist (e.g. type 8 from `08:00:2b:78:56:b9` =
  VAX2's hardware MAC; type 9 from a DECnet-logical source) — this is not an
  OVMX-only artifact, though the corpus also contains OVMX-sourced 8/9 from
  the worktree-760 builds (which already answer an op6/op8 "credit handshake"
  empirically and observe "the peer never ACKs our op9").
- The only value-bearing field present is the credit field [48:50] = 1 in
  every exemplar inspected.

**Candidate identification (NOT grounded — do not write into the spec as a
name):** the SCA credit-flow control pair. Ch.2 p. 2-44 defines the "special
credit message" — sent when the local Receive Credit count is dangerously low
and Pending Receive Credit > 0, carrying the pending count (vms-1d2, currently
UNGROUNDED with no candidate wire class). An envelope-only control message
whose only payload is the credit field, exchanged on a live connection and
flushed before teardown, fits; the pairing would be notification + ack. The
book's eight drawn control messages occupy 0–7 (and this corpus now shows 5
and 7 — see §1.4), so 8/9 are control-message codepoints beyond the drawn
eight, consistent with "SCS control message" taxon membership.

**Decisive experiment (one lab session, doubles as vms-1d2's missing
capture):** engineer a one-way SYSAP flow on a lab-2 replica, drive Receive
Credit toward SCSFLOWCUSH, and watch for 8/9 emission correlated with the
credit counter; then decode the [48:50] value against the engineered pending
count. If 8/9 emission tracks credit state, both vms-07a and vms-1d2 close
with one decode.

### 1.4 Bonus: types 5 and 7 exist in captures we hold

The spec (§4(h)(1a) and sec 5) records the "total absence of 5 and 7"
(REJECT_RSP / DISCONNECT_RSP halves) and instructs "do not emit either". The
full-corpus census overturns the absence: in the 58-content class, **type 7
appears 988× against 986 DISCONNECT_REQ (type 6)** — a near-exact 1:1 — and
**type 5 appears 4,536× against 4,654 REJECT_REQ (type 4)**. The earlier
absence finding came from the restricted-corpus censuses (the same sampling
family as vms-c11). This bears directly on **vms-abd** (a real VAX refused
OVMX's byte-exact DISCONNECT_REQ): the responder half of teardown is
observable after all, and the observed live sequence (§1.3) shows an 8/9
exchange preceding the accepted teardown. vms-abd's re-run should compare
teardowns with and without the preceding 8/9 exchange — decode, don't
correlate.

---

## 2. What the book gives OVMX for MSCP (reference map)

All page cites into `~/cluster/transcript/ch4-5-*.md`; index in `ch4-5-INDEX.md`.

- **Protocol model** (pp. 4-10..4-15): command / end-message, command
  reference numbers unique per connection, GET COMMAND STATUS, generic command
  layout (Fig 4-4: cmd-ref u32; unit u16; modifiers/CAA/opcode; command-
  dependent info), the full wire nesting (Fig 4-5), MTYPE taxonomy and
  dispatch (p. 4-13/4-15), credit piggyback on delivery (p. 4-15).
- **Command classes** (pp. 4-15..4-17): immediate vs nonimmediate (controller
  timeout period, 75 s on HSC), sequential vs nonsequential ordering rules.
- **Opcodes named in prose** (no numeric enum anywhere in ch.4): ONLINE,
  AVAILABLE, READ, WRITE, GET UNIT STATUS, GET COMMAND STATUS, SET CONTROLLER
  CHARACTERISTICS. Numeric values stay grounded from our captures only
  (0x03 GUS / 0x04 SCC already labeled in `scs_mscp.c`).
- **VMS-based MSCP/TMSCP server** (§4.8, pp. 4-71..4-81): server ⇄ local
  driver interaction via IRPs; serving is a *role*, not a membership
  requirement; load capacity (Table 4-3), LOAD_AVAIL formula, two-phase
  dynamic load balancing, MSCP_LOAD / MSCP_SERVE_ALL and the full SYSGEN
  parameter set (Table 4-4).
- **Mount verification** (Fig 4-38, p. 4-106): ONLINE → GET UNIT STATUS →
  read homeblock → read SCB — the sequence a VAX runs against a served disk;
  this is what an MSCP *server* must answer for a mount to succeed.
- **Server data structures** (pp. 4-114..4-117): DSRV/TSRV, UQB (per served
  unit), HQB (per remote class driver), HULB (per host/unit, load balancing),
  HRB (per outstanding remote request, holds the IRP) — the shape of a real
  serving implementation, useful as the design template if OVMX ever serves.
- **Ch.5** (context, not wire protocol): system disk layout, DUDRIVER/
  DSDRIVER/SHDRIVER flows, shadowing (Phase I/II), striping, UCB/VCB/SHAD/
  CDDB internals.

---

## 3. Direction

**Phase A — make the SCS envelope first-class (extends vms-187, do first).**
The MTYPE field joins the SCA layer being built under vms-187: one header
build/parse path (inner length, format word, MTYPE, credit, handles) shared by
every message OVMX emits or receives; dispatch-on-MTYPE at receive (control
0..7+ → connection state machine; 10 → per-connection message input, the CDT
hook `scs_credit.c` already anticipates). This dissolves the per-class
template special-casing and is the precondition for everything below.

**Phase B — MSCP client on grounded fields.** Promote `scs_mscp.c` from
byte-replay toward Fig 4-4-grounded builds: cmd-ref, unit, opcode, modifiers
as first-class fields; end-message parse keeps the END bit and adds status
handling. Opcode *numbers* remain capture-grounded only. Scope stays
joiner-role (SCC + GUS today); mount-verify read path deferred until OVMX
mounts a served disk.

**Phase C — ground types 8/9.** Run the §1.3 experiment. Until decoded, OVMX
keeps the worktree-760 empirical behavior (answer the op8 handshake, expect no
ack for op9) and does not name the messages in the spec.

**Phase D — MSCP disk-server posture (GATED, Baron's call — rd gate raised on
vms-54f).** Recommendation: **minimal honest responder, flag stays OFF by
default.** OVMX currently accepts the member-opened MSCP$DISK connect and then
cannot serve a single command on it — an accepted connection that black-holes
commands is exactly the dishonest-success shape INV-6 exists to kill, one
layer up. The minimal responder answers SCC with a well-formed end message and
GUS with "no unit" status — a truthful "member that serves no disk", matching
the book's model where serving is optional (§4.8). Full serving (READ/WRITE,
block data, DSRV/UQB/HQB/HRB machinery, vms-941 un-deferral, vms-61b2 LISTEN
registration) stays deferred until OVMX has a disk to serve and a reason to.
Alternatives teed up in the gate: (a) permanent "serves no disk" with the
connect *refused* instead of accepted; (b) full server build now. Absent an
answer, work proceeds on Phases A–C only, which no option forecloses.

**Consequences for open items.**
- vms-ecff: identified (§1.2); close after the spec records it (done in this
  session's spec update) + Phase A lands the emit path.
- vms-07a: observations recorded, name still open, decisive experiment defined
  (§1.3); closes with Phase C.
- vms-abd: type 7 exists in-corpus (§1.4); re-run teardown comparison with the
  8/9 exchange present vs absent.
- vms-1d2: the Phase C capture is the same experiment; potential double close.
- vms-941 stays deferred; vms-61b2 stays blocked on a real responder.
- vms-c11 discipline held: censuses here were length-unrestricted; the type
  field was histogrammed over *all* short classes plus the 190-content class.

**Measurement caveat.** All §1 numbers are from an ad-hoc census
(`$CLAUDE_JOB_DIR/tmp/census_8910.py`, this session) over mixed-source
captures; source attribution used raw MACs, not the OUI rule. Before any
number in §1 is cited as a *per-population* claim (real-VAX vs OVMX), re-run
under `scs_connect_data_measure.py`'s OUI-rule split (tool lives on the
worktree-760 branch) with a checked-in EXPECTED table and a figures gate, per
epic method. The *identification* of type 10 does not depend on the split (the
golden frames are identity-proven af2 real-VAX captures); the 5/7 and 8/9
counts do.
