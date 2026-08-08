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

**SUPERSEDED 2026-08-05 — a PUBLIC wire-level MSCP spec now exists for this
project.** *MSCP Basic Disk Functions Manual* **AA-L619A-TK v1.2 (Apr 1982)**,
part of the customer-orderable UDA50 Programmer's Doc Kit QP-905-GZ — standard
copyright page, **no confidential or restricted-distribution marking**, so it
is admissible public documentation under Rule 8. Archived host-only at
`~/cluster/mscp-spec/` with the appendices transcribed to
`~/cluster/mscp-spec/appendices.md` (opcodes, status/event codes, generic and
per-command message formats, ch.3 credit rules). **MSCP numeric values are
therefore no longer capture-only** — the "capture-grounded only" rule above
applies to the *SCA/SCS envelope*, not to MSCP command internals.
⛔ **EXCLUDED under Rule 8, do not read:** the bitsavers `dec/dsa/mscp`
*Mass Storage Control Protocol* v2.4.0 / Rev 2.4.0 / TMSCP 2.0.2 files are
stamped "DEC CONFIDENTIAL AND PROPRIETARY / RESTRICTED DISTRIBUTION".

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

> **SUPERSEDED IN PART BY `vms-a58` (2026-08-06) — read
> `docs/cluster-protocol-spec.md` §4(h)(1f) and its §5 register entry first.**
> The OUI-rule census this section says is "still owed" now exists
> (`tools/cluster/scs_t89_measure.py`, gated by `ctest -R scs_t89_figures`).
> Three things below are settled and one is overturned:
> - The constant `1` is a **count of one**, not a version or a flag. The
>   p. 2-43 credit ledger predicts `[48:50]` on 938 of 938 frames with zero
>   residuals; the value is `1` because the `SCS$DIRECTORY` dialogue never
>   leaves two buffers outstanding, and the same field reads `0` on the first
>   application message of all 131 dialogues.
> - **Types 8 and 9 are NOT connection-control messages** — they are inside the
>   credit account that types `0`–`7` sit outside. The "control-message
>   codepoints beyond the drawn eight" reading below is **overturned**.
> - The special-credit-message candidate is still **not** restored, but for two
>   NEW reasons that do not depend on the constant: the exchange is
>   unconditional and it is answered.
> - Emission ruling (spec §5): OVMX must keep answering a received `8` with a
>   `9`, and **must emit an `8` before any `DISCONNECT_REQ` it initiates** —
>   a 131-of-131 invariant it does not yet honour, and a first-class candidate
>   for the `vms-abd` refusal.

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

**MEASURED 2026-08-05 — the credit candidate is WEAKENED.** Over 855 real-VAX
type-8/9 frames the credit field `[48:50]` is a **constant 1**, identical to
ordinary application messages (type 10). p. 2-44 requires a special credit
message to carry the *Pending Receive Credit count*, which varies with buffers
released; a constant cannot be one. Meanwhile types 5 and 7 carry credit 0 in
100% of 5,257 frames — the structural signature p. 4-68 predicts for a class
that does not extend credit. So the "8/9 are the credit pair" reading now has
evidence *against* it, and the response-half reading of 5/7 has evidence *for*
it. Full table in spec §4(h)(1c). The §1.3 experiment below stands, with an
added discriminator: a genuine special credit message must show a **varying**
credit value.

**Candidate identification (NOT grounded, and now weakened — do not write into
the spec as a name):** the SCA credit-flow control pair. Ch.2 p. 2-44 defines the "special
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

**RAN 2026-08-06 (`vms-f03`) — TYPE 8 IS THE SPECIAL CREDIT MESSAGE. Full
method, tables and limits: spec §4(h)(1g).** The experiment was run with one
deliberate change of instrument, and the change is why it worked. Engineering a
*one-way SYSAP flow* means driving Receive Credit down to a fixed threshold;
but p. 2-44 states the threshold itself as `local SCSFLOWCUSH + remote Minimum
Send Credits`, and **SCSFLOWCUSH is a documented dynamic SYSGEN parameter**
(V7.3: default 1, min 0, max 16, Dynamic). Moving the threshold up to meet the
credit is the same experiment with a knob that DCL can actually turn, needs no
custom SYSAP, and is reversible inside one session. Load was held at ≈3 500
messages/s by shared-file lock contention while the cushion was varied on VAX1
alone; VAX2 stayed at the default and is the matched same-wire control.

Result, type-8 frames emitted by VAX1 per 120 s run, bracketed on both sides —
SCSFLOWCUSH **1 → 0 frames**, **0 → 0 frames**, **8 → 26 719**, **16 → 64 305**,
back to **1 → 0 frames** — with the type-10 rate flat across the sweep. VAX2
emitted zero type 8 throughout. The decode of `[48:50]`
is an accounting identity rather than a correlation: credit on type-10 plus
credit on type-8, from VAX1, equals the messages VAX1 received, in every run
(139 429 + 57 012 == 196 411 at cushion 16; 179 192 + 36 707 == 215 873 at
cushion 8; error ≤ 0.023%). Raising the cushion moves credit between the two
carriers and changes nothing else, so the field is the **Pending Receive Credit
count**. A second dose-response confirms it on the value rather than the rate —
mean credit per type-8 is **1.3738** at cushion 8 and **0.8866** at cushion 16,
because a higher cushion fires the message earlier, on less accumulated credit.

This also explains the constant-1 measurement that weakened the candidate: at
the default cushion the message only fires at genuine near-exhaustion, where the
pending count is essentially always 1.

**Type 9 is NOT named.** Every type 8 drew exactly one type 9 back (26 719 and
64 305 pairs, zero unmatched, p50 0.13 ms) echoing the identical value — but
reverse accounting shows that value is *not* a credit return, and ch.2 p. 2-44
documents no response to a special credit message at all. Recorded as the
book-unnamed response half; naming it would be the vms-c11 pattern. Two further
honest limits: the (X,Y)/(Y,X) **handle swap could not be tested** — both handle
fields carry the identical value on this lab — and 7 388 cushion-16 type-8
frames carry credit 0, which p. 2-44's "Pending Receive Credit > 0" condition
does not predict.

Re-derive with `tools/scs_flowcush_measure.py`; gate
`tests/vmsscs/test_scs_flowcush_figures.py`. Captures (lab-2, host-only):
`/data/training/vax/cluster/captures-lab2/vms-f03/`.

**Consequence for vms-abd — the reframed hypothesis is CONFIRMED, and §1.4's
suggested next move is retired.** The 8/9 exchange seen immediately before an
accepted DISCONNECT_REQ is credit flushing on a draining connection, not a
teardown handshake, so it is not a disconnect precondition. Comparing teardowns
with and without a preceding 8/9 exchange would measure nothing; §4's CSB state
mismatch is the live hypothesis.

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
- **Opcode numerics — now DOCUMENTED, and OVMX's capture-derived labels
  VALIDATE.** AA-L619A-TK Table A-1 gives the full enumeration; the values
  OVMX carries are exact: `0x03` GET UNIT STATUS (`MSCP$K_OP_GTUNT`), `0x04`
  SET CONTROLLER CHARACTERISTICS (`MSCP$K_OP_STCON`), `0x09` ONLINE, `0x21`
  READ, `0x22` WRITE, and the end-message flag `0x80` (`OP.END`; an endcode is
  `command | 0x80`) — matching `scs_mscp.h`'s `SCS_MSCP_OP_GET_UNIT_STATUS`,
  `SCS_MSCP_OP_SET_CTLR_CHAR` and `SCS_MSCP_END_BIT` byte for byte.
  **Wire cross-check:** filtering the 94-content type-10 population by the
  SYSAP that owns the connection, every frame on the **VMS$DISK_CL_DRVR**
  (disk class driver = MSCP client) connection decodes to a valid Table A-1
  opcode at body[8] — 5 245/5 245, and only `0x03`/`0x04`. This simultaneously
  confirms the Figure 4-4 body layout (cmd-ref u32, unit u16, reserved u16,
  then modifiers/CAA/opcode with opcode in the low byte at body[8]) against a
  public spec. *Caveat:* body[8] is an MSCP opcode **only** on MSCP
  connections — the same offset on `SCS$DIR_LOOKUP`/`SCS$DIRECTORY` frames
  yields `0x24`/`0x56`, which are not Table A-1 opcodes at all. An unfiltered
  body[8] census is a category error; filter by SYSAP first.
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

**Phase B — MSCP client on DOCUMENTED fields (now cheaper than planned).**
*LANDED as `vms-533`.* The 36-byte MSCP body is built from `struct scs_mscp_cmd`
at Table A-6 offsets — cmd-ref, unit, opcode, modifiers and the sec 6.16 SCC
parameter area are named fields, and no captured byte survives in `[58:94]`. The
end-message parse splits the sec 5.6 status word into its 5-bit major code and
11-bit sub-code, which fixed a live defect: the GET UNIT STATUS walk compared the
whole word against `3`, so any Table B-2 Unit-Offline sub-code (`0x23`, `0x43`,
`0x83`, `0x103`) failed to terminate the enumeration. Wire-neutral: the
field-built frames reproduce the golden af2 commands byte for byte. What is
still replayed is the 58-byte SCA/PPD header ahead of the body, and P.TIME (a
frozen 2026-07-28 capture timestamp — replacing it with live host time is a wire
change and is deliberately not in this refactor).

AA-L619A-TK supplies the opcode, status/event and per-command formats, so this
is no longer a decode exercise — it is transcription from a public spec, with
the captures as the conformance check. Promote `scs_mscp.c` from byte-replay
toward Fig 4-4-grounded builds: cmd-ref, unit, opcode, modifiers
as first-class fields; end-message parse keeps the END bit and adds status
handling. Opcode *numbers* remain capture-grounded only. Scope stays
joiner-role (SCC + GUS today); mount-verify read path deferred until OVMX
mounts a served disk.

**Phase C — ground types 8/9.** Run the §1.3 experiment. Until decoded, OVMX
keeps the worktree-760 empirical behavior (answer the op8 handshake, expect no
ack for op9) and does not name the messages in the spec.

**Phase D — MSCP disk server. RULED 2026-08-06 (vms-34b): BUILD THE FULL
SERVER, superseding this section's own recommendation below.** The operator
overrode the "minimal honest responder" recommendation; the done condition is a
real VAX running MOUNT against an OVMX-served unit and reading files on it.

*PART 1 LANDED as `vms-291`* — `src/vmsscs/scs_mscp_srv.{h,c}`, the responder:
SET CONTROLLER CHARACTERISTICS, GET UNIT STATUS (with the MD.NXU walk and its
Unit-Offline terminator), ONLINE, READ and WRITE handling; a raw block image as
the backing store; the sec 3.4 Controller-Online gate; UQB/HQB analogues only
(no HULB — nothing to load-balance against). The SCC end message is built from
fields and reproduces a **real captured VAX server answer byte for byte**,
which is possible because the 86-content MTYPE-10 class turns out to be the SCC
END and its 954 frames pair exactly with 954 SCC commands in the same corpus.
Its parameter area is constant over all 954: `P.CNTF` `0xa004`, `P.CTMO` `20`,
and `0x0547` in the word Table A-7 calls reserved — none of the three explained
by AA-L619A-TK, all three recorded as observations rather than named, and
`P.CNTF` notably **not** an echo of what the class driver requested.

Two design decisions worth carrying forward. **The backing store is a raw block
image and OVMX's `vmsfs` is deliberately not used**: MSCP is a block protocol
and `vmsfs_ondisk.h` states it is not byte-compatible with real ODS-2, so a
served volume's *content* must be a genuine VMS-made ODS-2 volume while OVMX
serves its blocks verbatim — which keeps Phase D on the protocol and leaves the
on-disk format story untouched. **v1 is read-only and says so on the wire**
(`UF.WPS` advertised; WRITE answered Write Protected `0x1006`).

**WHAT PART 1 DOES NOT DO.** Block data transfer is still unimplemented, so a
READ with no transfer hook returns Controller Error and a zero byte count
rather than a Success it cannot back up. `vms-941` (block data transfer) and
`vms-61b2` (LISTEN registration) both stay open behind that — registering
`MSCP$DISK` before the responder can honour a mount is the exact facade
`vms-61b2` refused to build.

**THE TWO WIRE-FIDELITY GAPS ARE CLOSED — BY MEASUREMENT, NOT BY THE BOOK.**
Part 1's first draft carried two gaps that blocked a real MOUNT and that
**neither the documentation nor the joiner-side corpus could close**: (a) the
GUS end message the builder emitted was 106-content where every real VAX emits
110, and the four undecoded bytes past Table A-7 had to, per §5's standing
instruction, be *copied from an observation rather than composed*; (b) `P.UNFL`
bit 15 is set on all 404 valid-unit frames and Table A-5 defines no bit 15.
Both needed **a lab capture of a real VAX serving a disk to another VAX**,
which the stock lab-2 configuration cannot produce — both nodes attach the same
disk images and therefore never generate MSCP serving traffic between
themselves. **That capture was taken inside part 1** (next section), and it
settled both: the GUS end message is **110-content, 52 bytes**, with
`body[48:50]` carrying the observed `0x006e` (copied, not composed) and
`body[50:52]` left zero because a real server demonstrably leaves it as stale
garbage; and `P.UNFL` bit 15 is **host-originated** — the class driver's ONLINE
*command* carries `0x8000` and the server echoes it, which settles it as a
design question without decoding its meaning. The same capture caught a third
length the book had also not given away: **WRITE end messages are 36 bytes, not
READ's 32**, which part 1 had assumed equal. All of these corrections are in
`scs_mscp_srv.c`, are asserted by `test_scs_mscp_srv.c`, and are held there by
the `scs_mscp_srv_mutants` battery.

### Phase D part 1's lab capture — SCA block data transfer, DECODED

**A real VAX serving a disk to another real VAX was captured for the first
time** (lab-2 `vaxlab-9`, 2026-08-06). VAX2 ran `MOUNT` against a unit VAX1
served and read a marker file back. Host-only artifacts, never in git:

| path under `/data/training/vax/k8s-labs/vaxlab-9/logs/` | what |
|---|---|
| `vms291-mount-A.pcap` | the MOUNT, TYPE of the marker file, DIRECTORY, DUMP |
| `vms291-control-B.pcap` | **negative control** — MOUNT of a nonexistent unit |
| `vms291-boot-C.pcap` | cold vax2 boot → rejoin → SCC → enumeration → MOUNT |

The control is what makes the mount capture attributable: with the volume
dismounted and `MOUNT` aimed at a nonexistent unit it carries **zero** READ,
**zero** WRITE, **zero** block-transfer frames and zero marker bytes.

**The stock lab-2 config cannot produce this** — both nodes attach the same
disk images, so they never serve to each other. The asymmetry was made by
giving vax1 an `rq2` RA81 on a new `d2.dsk` and disabling `rq2`/`rq3` on vax2.
`MSCP_LOAD=1` and `MSCP_SERVE_ALL=1` were already set in the golden image.

**A block-transfer message is a 28-byte header followed by N data bytes**, and
it deliberately FAILS the SCS envelope conformance test (`content[44:46] ==
0x0004`) that the command/end class passes:

| offset | size | field |
|---|---|---|
| +0 | 4 | destination connection ID (same value the MSCP envelope carries) |
| +4 | 2 | constant per connection (9 on one, 13 on another). **NOT a message type** |
| +6 | 2 | constant across all frames of one transfer; increments between transfers |
| +8 | 4 | **bytes remaining in this transfer**, including this frame's data (counts *down*) |
| +12 | 4 | source buffer name |
| +16 | 4 | destination offset within the destination buffer |
| +20 | 4 | destination buffer name |
| +24 | 4 | source offset |
| +28 | N | the data |

**The MSCP buffer descriptor** (Table A-6 offset 16, 12 bytes) is
`{ u32 offset, u32 SCS buffer NAME, u32 SCS connection ID }`. The name in the
READ/WRITE command is exactly the name that appears in the block-transfer
frames — that is the correlation, and it is the named-buffer mechanism
*VAXcluster Principles* pp. 2-32..2-41 describes without giving bytes.

**READ** streams standalone block frames server→client and **piggybacks the
final partial chunk into the same Ethernet frame as the MSCP end message**; the
SCS inner length declares only the 32-byte end message and everything past it
is a second message. Observed READ-END SCA contents 118/194/448/630/1142 all
declare MSCP length 32. **WRITE** is a two-frame request/response whose two
28-byte headers are *byte-identical* — only the presence of data distinguishes
them.

**Mount-verify ordering, confirmed from a cold node:** `SCC → SCC-END` ×2 at
connection setup → `GUS` walk with MD.NXU terminated by status `0x0003` → ~20 s
periodic `GUS` polls → at MOUNT: `ONLINE → ONLINE-END → GUS → GUS-END → READ
LBN 1 (home block) → READ LBN 0x40a → READ/WRITE of the INDEXF/BITMAP extent`.

**End-message lengths measured:** SCC 28, ONLINE 44, READ 32, WRITE 36, GUS 52.
Phase D part 1 had three right from Table A-7 alone and **two wrong** — GUS
(48) and WRITE (assumed equal to READ) — both corrected.

**Two decode targets did NOT close.** `GUS body[48:50]` is `0x006e` even on an
RA81 with different geometry, so length-echo vs plain-constant remains
undecidable; what the capture *did* show is that the field is written
deliberately (invalid-unit replies carry visible stale garbage in the
surrounding tail while `[48:50]` still reads `0x006e`), which kills the
"all padding" reading. And `P.UNFL` bit 15 turns out to be **host-originated** —
the class driver's ONLINE *command* carries `0x8000` and the server echoes it —
which settles it as a design question without decoding its meaning.

**Still ungrounded, do not build on:** the block header's `+4` and `+6` fields;
the SCC controller-flags bits 15/13/11/2; whether credit/flow control interacts
with block transfers. Also: the connection carrying 190-content frames matched
MSCP opcodes **by byte collision only** and is NOT MSCP — do not feed it to an
MSCP parser.

*The superseded recommendation is preserved below for the record.*

~~Recommendation: **minimal honest responder, flag stays OFF by
default.**~~ OVMX currently accepts the member-opened MSCP$DISK connect and then
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

### Phase D part 2 — the live daemon attaches a real backing store (vms-6e1)

`vms-34b` wired the responder into the live daemon (`scsd_mscp_srv_msg_input()`)
but deliberately attached no unit — "no backing store attached... a shipped
node honestly serves zero units", left as named follow-up. `vms-6e1` does that
follow-up, entirely inside `src/vmsscs/`: `scsd_mscp_attach_unit0()` (scsd.c)
opens — creating it, sized to a placeholder 10 MiB, if absent — a real regular
file and attaches it as served unit 0 via `scs_mscp_srv_attach_fd()`, the same
function `scs_mscp_srv.c`'s own unit tests use. `OVMX_MSCP_UNIT0_IMAGE` lets an
operator (or `vms-600`'s lab harness) point this at a real image instead — a
lab disk, or one a lab VAX `INITIALIZE`d — without a code change. `scsd_mscp_
live_xfer()` (scsd.c) is the live counterpart of `scs_mscp_srv_blk_sink_xfer`:
it sends each READ's block-transfer frame on the actual connection instead of
into a test buffer, so a live READ now moves real backing-store bytes instead
of drawing the "no hook installed" Controller Error.

**What this does not do**, matching design decisions (1) and (2) exactly:
absent an operator-supplied image the served content is an ARBITRARY
zero-filled block image, not a genuine ODS-2 volume, so a real VAX's MOUNT
verification still needs `vms-600`'s lab-supplied image — that acceptance
stays separate, on purpose (this item's brief: "do NOT need the lab here").
And v1's WRITE opcode is unchanged — still Write Protected unconditionally,
because REQDAT (the wire mechanism a live WRITE needs) is still not
wire-grounded. `tests/vmsscs/test_scsd_wire.c`'s
`test_mscp_srv_read_serves_real_backing_store_bytes` proves the round trip
end to end: known bytes written into the live unit's backing store via
`scs_mscp_srv_write_blocks()`, then read back both through a real wire READ
(SCC → ONLINE → READ, block-transfer frames parsed and compared byte-for-byte)
and through a raw `pread()` of the image file itself.

---

## 4. Ground-source answers to standing questions (2026-08-05)

Chapters 2, 4, 7 and 8 of the transcript were queried against the open rd
questions. Answers below are book-grounded with page cites; measurements are
from the corpus. Items are *informed*, not closed, unless stated.

**vms-c35 — RESOLVED.** p. 2-12 names the round-1 106-byte frame a **STACK**
("a STACK acknowledges receipt of the START… each node uses the STACK to again
supply the other node with a description of itself"), which is why it re-carries
the config body; the 46-byte round-2 frame is the **ACK**, "simply discard[ed]"
on receipt. Spec §4(g) now uses START/STACK/ACK, agreeing with `scs_vc.c`.
Caveat preserved: p. 2-14 shows a real retransmitted START exists in the
asymmetric-timing case — it just is not our round-1 frame.

**vms-abd — CONFOUND CLEARED (the missing-precondition theory is dead).** ch.2
documents **no** precondition on sending DISCONNECT_REQ: "SCA permits either
SYSAP to unilaterally request that the connection be broken by invoking the
DISCONNECT service" (p. 2-26), and no draining, credit-return or quiescing rule
appears anywhere in the chapter (nor is a CDT drain-before-free ordering stated
at p. 2-28 — only that CDTs are queued to the Path Block). So "Inappropriate SCA
Control Message" is **not** explained by an undocumented precondition OVMX
skipped. What ch.2 *does* give is the state ladder the peer runs
(OPEN→DISC SENT / DISC RECEIVED→DISC MATCH→CLOSED, pp. 2-26/2-27), and ch.7
gives the CSB's own 10 connectivity states. **Reframed hypothesis for the
re-run:** the refusal is a *state* mismatch — the peer's CSB was not in a state
where a disconnect is legal — not a missing message. Also actionable: type 7
(DISCONNECT_RSP) **exists** in our corpus and carries credit 0, so OVMX should
expect and accept one rather than treating its absence as normal.

**vms-7e7 — a fourth candidate rule REFUTED.** `0x4b13` vs `0x5b13` is not the
message-vs-datagram distinction: p. 4-68 requires datagrams to carry credit 0,
and both words carry mostly-nonzero credit (24.2% vs 29.9% zero over 930k
real-VAX frames). The msgtype-selection rule remains unknown; the corpus-limit
note on the item (need a capture with a served tape or a fourth SYSAP name)
still stands as the cheapest way forward.

**vms-da1 — REFRAMED, and the reframing is the answer to look for.** The four
SDA CSB fields (Next seq. number / Last seq num rcvd / Last ack. seq num /
Unacked messages) appear **nowhere** in ch.2, ch.7 or ch.8. ch.2 defines only
the three credit counters (Send / Receive / Pending Receive, p. 2-45), and
ch.8's SHOW CLUSTER **CREDITS** class maps 1:1 onto exactly those four credit
quantities (p. 8-33) — not onto sequence numbers. Critically, ch.2 p. 2-55
assigns "the protocol necessary to ensure the guarantees associated with the
message… service" to the **PPD layer**, not SCS. **Therefore the CSB sequence
counters are almost certainly PPD/NISCA (PEDRIVER virtual-circuit) state, not
SCS connection state** — which is why no SCS-level wire field matched them, and
why the magnitude-match inference failed. Re-run the correlation against
VC-level counters, not SCS message counters.

**vms-1d2 — the special credit message is a distinct MTYPE, and that is new.**
p. 4-68 enumerates four SCS message types for SSP ports: "datagrams, regular
messages, **special credit messages**, and node name packets." So a special
credit message is its own message-type value, not a flavour of ordinary
message — meaning it should be findable as a distinct `[46:48]` value. Our
observed value set is 0–10; if the special credit message is on our wire it is
one of these, and 8/9 are now the *weakened* candidates (constant credit, §1.3).
ch.2 names no acknowledgment or response to it (p. 2-44), which argues against a
*paired* reading like 8/9.

**Type 5/7 corroboration.** Beyond the count-pairing in §1.4, 5 and 7 partition
perfectly from every other type on credit==0 (5,257/5,257) — the structural
signature p. 4-68 predicts for a class that does not extend send credits, and
what one expects of response halves.

**Method confound found and fixed (self-caught).** An earlier scratch census in
this session read `[46:48]` in the 70-content class and printed "types 1..22".
That class does **not** share the envelope (`[42:44]` is not an inner length,
`[44:46]` is not `0x0004`), so those were a misread field. Corrected in spec
§4(h)(1d); the envelope claim is scoped to the 58/62/66/110/94/190-content
classes only. This is the vms-c11 failure mode in the opposite direction —
over-generalising a field across classes rather than under-sampling them.

**Reference now available for vms-187.** ch.7 supplies what the epic said was
missing: the CSB 10-state connectivity machine (pp. 7-23/7-24), CSID structure
and reuse (16-bit CSV index + 16-bit sequence, new CSID on every rejoin,
p. 7-25), the JOIN CLUSTER Phase 1 field list and Phase 2 commit tasks
(pp. 7-40..7-42), and RECNXINTERVAL timeout formulas (p. 7-30). Note two
explicit **NOT IN BOOK** answers that stop further searching: there is no
cluster-wide generation/configuration sequence counter (only timestamps and the
per-node CSID sequence), and ch.7 says nothing about the cluster group
number/password or CLUSTER_AUTHORIZE.DAT — so the join-nonce derivation gap in
§5 cannot be closed from this book.

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
