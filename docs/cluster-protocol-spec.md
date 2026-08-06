# OpenVMS VMScluster Wire Protocol — Clean-Room Specification

> Status: DRAFT, derived entirely from wire observation (vms-ci.2).
> Companion dissector: `tools/cluster/dissect_sca.py`.

## 0. Clean-room provenance

Everything in this document is derived ONLY from:

1. **Observing the wire bytes** in pcaps captured off our own, wholly
   ours-to-operate 2-node SIMH OpenVMS VAX 7.3 reference cluster (`~/vax/cluster/`,
   VAX1 `[SYS0]` + VAX2 `[SYS1]`, one shared disk, bridged on `br0`), plus a
   3rd diskless satellite node VAX3 for the boot-SOLICIT specimens.
2. **Public OpenVMS documentation and documented tool output**: SDA
   (`ANALYZE/SYSTEM`) `SHOW CLUSTER` / `SHOW CONNECTIONS` / `SHOW PORTS`,
   SYSGEN `SHOW/CLUSTER`, and SYSMAN `CONFIGURATION SHOW
   CLUSTER_AUTHORIZATION` — all captured verbatim in
   `~/vax/cluster/captures/sda-scs-extract-vax1.txt`, indexed and
   cross-referenced in `~/vax/cluster/captures/RE-specimens-2026-07-26.md`.

**No VSI/HPE VMS source or binary was ever disassembled, decompiled, read,
or consulted.** Every field below is labeled either:

- **GROUNDED** — the byte value matches, exactly and reproducibly, a value
  documented by the SDA/SYSGEN/SYSMAN decoder ring, or the field's presence/
  absence correlates perfectly with a known wire condition (e.g. multicast
  vs. directed) across every sampled frame.
- **inferred** — a plausible reading based on position, constancy, or a
  documented tunable's *numeric* coincidence, but not independently
  confirmed. Presented as a hypothesis, not fact.
- **unknown** — no grounding at all. The bytes are reported as raw hex and
  nothing more.

This document and `tools/cluster/dissect_sca.py` are the vms-ci.2
deliverables. See CLAUDE.md rule 8 (clean-room cluster RE hard invariant)
and `docs/design-cluster-node.md` §2 for how this fits the wider effort.

### Byte-offset convention

All offsets in this document are **absolute frame offsets**, counted from
byte 0 = the first byte of the Ethernet destination address (i.e. exactly
what `tcpdump -xx` / `tcpdump -e -xx` shows, and what
`tools/cluster/dissect_sca.py` prints in its `[off..off]` column). The SCA
payload begins at offset 14 (after the 14-byte Ethernet header).

**Convention erratum (vms-54f, 2026-08-05):** §4(h) — including (1a)/(1b)/(2) —
uses **SCA-content-relative** offsets (frame-absolute − 14), and its
"58/62/66/110-byte class" names are SCA-content lengths (72/76/80/124 on the
wire). Verified by re-measurement: the (1a) type field sits at frame-absolute
[60:62] = content [46:48], and the CONNECT_REQ SYSAP name at content [62:78]
(`MSCP$DISK       ` observed there byte-exact, and binary at frame-absolute
[62:78]). All other sections follow the frame-absolute convention above.

---

## 1. Specimens used

**`vax3-2to3-established-join-20260730.pcap`** (17 705 frames, 83.7 s) — **the authority
for an established join.** A third *real* VAX (VAX3, `SCSNODE=VAX3`,
`SCSSYSTEMID=1027`, `VOTES=0`, root `[SYS2]`, MAC `08:00:2b:11:22:33`) built on the lab
disk and booted into the **running 2-node cluster**, reaching MEMBER
(`CLUSTER_NODES=3`). Every other join specimen in this library is a 1→2 *formation*;
this is the only capture of a node being **admitted to an existing cluster**, which is
the operation OVMX must reproduce. Grounds §4(m) and §4(n).

| pcap | Frames (0x6007) | Used for |
|---|---|---|
| `scs-idle-baseline.pcap` | 36 | §4(b) HELLO baseline, §4(d) SCS envelope baseline |
| `formation-ci1-joinwindow.pcap` | 2992 | §4(c) connect/directory-lookup phase, §4(g) membership handshake, §4(h) SCS$DIRECTORY connect + 0x5b/0x48 bodies (vms-560), §4(j) CM add-member transaction envelope on the 190-byte VMS$VAXcluster VC (vms-f85) |
| `ci3-join-membership-live2node-20260728.pcap` | 58 | §4(g) join-nonce cross-boot stability (vms-b6c) |
| `cd0-bootB-zk1099-join-20260728.pcap` | 18297 | §4(g) phase-2 START/config body grounding — joiner reconfigured to SCSNODE `"ZK"`/SCSSYSTEMID 1099/VOTES 0 (vms-cd0) |
| `cd0-bootC-zk1099-votes2-20260728.pcap` | ~18k | §4(g) vote-varying diff — same node, VOTES 2 (vms-cd0); §4(j) VOTES grounding at 190-byte VC body[22:24] via the VOTES 0↔2 byte-diff (vms-f85) |
| `af2-established-rejoin-20260728.pcap` | 16340 | §4(i) joining an ESTABLISHED cluster — VAX2 drop-and-rejoin while VAX1 stays up (Member State Seq 2→3→4); rejoin `0x41` START = SCA 2850–2855 (vms-af2) |
| `af2-firsttimer-established-20260728.pcap` | 51072 | §4(i).B — fresh-identity **first-timer** VX3/1050 joins established VAX1 (STARTs SCA 2558–2563), then **2nd** (20170–20175) and **3rd** (33591–33596) incarnations; grounds the `[22:24]` incarnation counter 1→2→3 and its `[78:80]` HELLO advertisement (vms-af2) |
| `formation-ci1.pcap` | 18541 | §1 message-class census (Table 2), §4(e) MSCP, large block-transfer frames, §4(h) 0x5b/0x48 scale re-validation (605/622 frames, vms-560), §4(k) the 2 padded-HELLO channel-size-verification frames (idx 5990/7534) — the acked case |
| `ci3-addmember-20260728.pcap` | 1922 | §4(k) NISCA channel packet-size verification: 25 unacked padded directed HELLOs VAX1→OVMX (1500/1069/853/745B), the retransmit ladder, the stalled-join wall (vms-84f) |
| `scs-dlm-lockconflict.pcap` | 100 | §4(f) DLM section |
| `scs-node-leave.pcap` | 2901 | cross-check only (teardown, not separately decoded here) |
| `satellite-niscs-boot-solicit.pcap` | 231 | §4(c) SOLICIT |
| `sda-scs-extract-vax1.txt` | — | decoder ring for all ConID/credit/tunable citations |

All frame-index citations below (e.g. "frame 9") are 0-based indices into
the pcap's SCA (`0x6007`) frame sequence as read by `read_pcap()` in the
dissector, i.e. reproducible by running
`tools/cluster/dissect_sca.py <pcap> --frame N`.

---

## 2. Datalink / SCA framing — GROUNDED

Every VMScluster frame we observed rides raw Ethernet II with:

| Offset | Size | Field | Observed values |
|---|---|---|---|
| 0 | 6 | Ethernet destination | unicast peer's real/logical MAC, OR the cluster multicast `AB-00-04-01-01-01` |
| 6 | 6 | Ethernet source | sender's real HW MAC (VAX2 before DECnet remaps it, e.g. `08-00-2b-78-56-b9`) or logical LAVC MAC (`AA-00-04-00-<node>-04`) |
| 12 | 2 | Ethertype | `0x6007` — DEC SCA/LAVC (the entire protocol stack we dissect rides this one ethertype) |
| 14 | 2 | **SCA length field** | LE `uint16`; see below |

**SCA length field, corrected finding.** The initial specimen index
(`RE-specimens-2026-07-26.md`) provisionally called the 2 bytes at offset
14 a "frame type word" (e.g. "`7600` = HELLO", "`4c00` = SOLICIT"). Byte-exact
validation against every SCA frame in 4 pcaps (6029 frames total: idle
baseline, join window, DLM conflict, node leave) shows this is **not an
enum** — it is a **little-endian length field**:

```
total_SCA_content_bytes = LE_uint16(bytes[14:16]) + 2
```

Re-validated against the full specimen set (**24,570 SCA frames** across 5
pcaps: idle baseline, join window, DLM conflict, node leave, full
formation): the length identity `LE_uint16(bytes[14:16]) + 2 ==
total_SCA_content_bytes` held for 23,642 frames exactly, and **every one of
the 928 mismatches was a runt frame zero-padded to the exact 60-byte
Ethernet minimum** (predicted payload ≤ actual, frame total == 60) — i.e.
the field reports the *true* payload length while the wire pads the runt.
**0 unexplained residuals.**

**Disambiguation — why this is a length and not a type (definitive).** A
fixed message *type* enum would permit the same offset-14 value to appear at
more than one frame size. Across all 24,570 frames spanning **24 distinct
SCA-content sizes, zero offset-14 values were ever observed at more than one
size.** A type field cannot produce that; a length field must. Combined with
the padding rule accounting for 100% of the mismatches, this pins the
interpretation to the VMS oracle: offset 14 is a little-endian payload-length
field, not a frame-type enum. **GROUNDED (24,570/24,570 frames explained).**

The apparent "type" values (`7600`, `4c00`, `bc00`, …) are simply the
length-minus-2 of each fixed-size message class, which is why they looked
enum-like: HELLO/keepalive frames are always exactly 120 bytes, SOLICIT
frames always exactly 78 bytes, and so on. `dissect_sca.py` classifies
frames by this length instead (see Table 2).

### Table 2 — message-class census (`formation-ci1.pcap`, 18541 frames)

| Total SCA len | Count | Class (this doc) | Notes |
|---|---|---|---|
| 190 | 17557 | **SCS fixed message** | DLM traffic + directory lookups; only class with a GROUNDED Con.ID location (§4(d)) |
| 41 | 622 | SCS short (ack/credit) | Ethernet-padded to 60 bytes total |
| 120 | 151 | **HELLO** | §4(b) |
| 70, 110, 94, 62, 58, 66, 106, 86 | 158 combined | SCS connect/directory-lookup & MSCP req/resp | §4(c), §4(e) |
| 526, 398, 634, 462, 270, 82, 369, 302, 590, 718 | 50 combined | **MSCP bulk block-transfer** (`0x4b`/`0x13` sequenced-application, nonzero data body, up to `NISCS_MAX_PKTSZ`=1498, GROUNDED against SYSGEN tunable) | header NOT decoded (see §4(d)/§4(e) caveat) |
| 1500 | 2 | **padded directed HELLO** (channel packet-size verification, zero-pad body) | **GROUNDED in §4(k)** (`vms-84f`) — distinct from the MSCP class above |

`78` (SOLICIT) doesn't appear in `formation-ci1.pcap` (no satellite boot in
that capture); it's the majority class in `satellite-niscs-boot-solicit.pcap`
(40/231 frames, the rest HELLO).

---

## 3. Node/connection identity used throughout (decoder ring)

From `sda-scs-extract-vax1.txt` (SDA `SHOW CLUSTER` / `SHOW CONNECTIONS`,
SYSGEN `SHOW/CLUSTER`, SYSMAN `CONFIGURATION SHOW CLUSTER_AUTHORIZATION`):

| Node | CSID | SCSSYSTEMID | Logical LAVC MAC |
|---|---|---|---|
| VAX1 | `00010001` | 1025 | `aa-00-04-00-01-04` |
| VAX2 | `00010002` | 1026 | `aa-00-04-00-02-04` (HW MAC `08-00-2b-78-56-b9` until DECnet remaps it) |
| VAX3 (satellite) | — | 1027 | `aa-00-04-00-03-04` |

Cluster group `1` → multicast `AB-00-04-01-01-01` (SYSMAN CONFIGURATION
SHOW CLUSTER_AUTHORIZATION: `Cluster group number: 1`, `Multicast address:
AB-00-04-01-01-01`).

SCS connections (SDA `SHOW CONNECTIONS` CDTs):

| Local SYSAP | Remote | Local Con.ID | Remote Con.ID | Credit (Send/Recv) |
|---|---|---|---|---|
| `VMS$VAXcluster` | `VAX2::VMS$VAXcluster` | `62C50009` | `33580008` | 10 / 8 |
| `MSCP$DISK` | `VAX2::VMS$DISK_CL_DRVR` | `62C6000A` | `33590009` | 10 / 8 |
| `VMS$DISK_CL_DRVR` | `VAX2::MSCP$DISK` | `62D4000B` | `3367000B` | 7 / 10 |

Tunables (SYSGEN `SHOW/CLUSTER`), cross-validated below:
`CLUSTER_CREDITS 10`, `MSCP_CREDITS 8`, `NISCS_MAX_PKTSZ 1498`,
`NISCS_LAN_OVRHD 18`, `LOCKDIRWT 1`. PEDRIVER port (SDA `SHOW PORTS`
PDT `PEA0`): `Msg Header Size 32`, `DG Header Size 320`, `Poller Sweep 31`.

---

## 4. Field-by-field decode

### 4(a) shared "SCA discovery header" (HELLO and SOLICIT), offsets 14–71

HELLO and boot-time SOLICIT frames share an identical 58-byte template
(payload-relative offset 0–57, absolute frame offset 14–71), verified
byte-exact across `scs-idle-baseline.pcap` frames 1–3 (VAX1/VAX2 HELLO,
multicast and directed) and `satellite-niscs-boot-solicit.pcap` frame 1100
(VAX3 SOLICIT):

| Abs. offset | Size | Field | Grounding |
|---|---|---|---|
| 14 | 2 | SCA length field | GROUNDED (§2) |
| 16 | 6 | Dest/group logical LAVC addr | GROUNDED (matches multicast group or peer's logical MAC) |
| 22 | 2 | Connect flag, constant `0x0001` | observed constant |
| 24 | 6 | Src logical LAVC addr (sender's own) | GROUNDED |
| 30 | 2 | per-frame word: `a000` on multicast HELLO / `b600` on VAX3 SOLICIT / **`b200`,`b300`,`b400` on directed HELLO = the NISCA channel-verify request/response counter** | **GROUNDED (directed values) in the offset-30 subsection below (`vms-d94`)**; the multicast `a0`/SOLICIT `b6` values remain inferred-constant |
| 32 | 4 | constant prefix `08 00 00 80` | unknown/inferred |
| 36 | 1 | **message-class byte**: `0x05` on every HELLO, `0x02` on every SOLICIT | inferred (consistent 100% split across all captures, but not documented anywhere — a working label, not a confirmed opcode enum) |
| 37 | 3 | constant suffix `01 00 00` | unknown/inferred |
| 40 | 1 | node-name length prefix (observed `6`) | GROUNDED (matches the following ASCII name's byte count in every frame) |
| 41 | *namelen* | node name, ASCII, space-padded (`"VAX1  "`, `"VAX2  "`, `"VAX3  "`) | GROUNDED |
| 33+14=47 | 17 | constant capability/version-ish span, differs slightly HELLO vs. SOLICIT | unknown/inferred |
| 64 | 1 | constant `0x03` | unknown |
| 65 | 3 | zero | unknown |
| 68 | 4 | **connect/join nonce** | **GROUNDED**: `0x00000000` on every multicast HELLO, and the identical non-zero shared token (e.g. `ee 05 39 5b`) on every directed HELLO between VAX1/VAX2 *and* on the VAX3 boot SOLICIT — the same cluster-wide token the initial RE-specimens doc flagged. Confirmed frame examples: `scs-idle-baseline.pcap` frame 1 (zero, multicast) vs. frame 2/3 (`ee05395b`, directed); `satellite-niscs-boot-solicit.pcap` frame 1100 (`ee05395b`). |

#### 4(a).0 Directed-HELLO addressing: abs 16 is the peer's LOGICAL address, not its HW MAC (GROUNDED, `vms-760`)

On a **directed** HELLO the Ethernet destination (abs 0–5) and the SCA
destination-logical address (abs 16–21) are **two different addresses**:

| field | value |
|---|---|
| abs 0–5 | the peer's **hardware** MAC (where the frame is delivered) |
| abs 16–21 | the peer's **cluster-logical** LAVC address `aa:00:04:00:<LE16(sysid)>` |
| abs 24–29 | the **sender's own** cluster-logical address (§4a) |

**GROUNDED**, `vax3-2to3-established-join-20260730.pcap` **frame 182** — VAX3
answering VAX2's channel probe carries eth-dst `08:00:2b:78:56:b9` (VAX2's HW
MAC) and abs 16 `aa:00:04:00:02:04` (VAX2's logical address).

> **This corrects an earlier reading.** The rule was previously recorded as "the
> wire mirrors abs 0 into abs 16." That inference came from a **2-node** lab in
> which VAX1's HW MAC *is* its logical address (`aa:00:04:00:01:04`), so the two
> fields were indistinguishable in every specimen available at the time. It is
> wrong for any node whose HW MAC is not a DECnet `aa:00:04:..` address.
>
> **Failure signature if you mirror instead.** The peer silently DROPS the
> reply. It re-sends its `0xb2` probe indefinitely (51 times in a 100 s OVMX
> run) and never sends the `0xb4` that finalises the channel; a correct
> exchange is **one** `0xb2` → `0xb3` → `0xb4` and then steady `0xb3`/`0xb4`
> keepalives. With the channel unverified that peer never opens SCS connections
> to the joiner at all, so the cluster-wide reconfiguration cannot run and the
> joiner is stuck at `NEW` no matter how correct its SCS layer is. Because a
> 2-node lab cannot exhibit this, **a third node with a non-DECnet HW MAC is
> required to observe it** — which is how it was found.

#### 4(a).1 The directed-HELLO offset-30 per-frame word — the NISCA channel-verify handshake (GROUNDED, `vms-d94`)

The abs-30 word (SCA payload `[16:18]`; the state lives in the **low byte** at
abs 30, high byte abs 31 is constant `0x00`) on **directed** HELLOs is a
**two-phase channel-verify REQUEST/RESPONSE counter**, not an opaque constant.
This was left `unknown/inferred` in the table above and in §4(k); it is now
**GROUNDED byte-exact across two independent fresh formations**:

- `~/vax/clean-cluster/captures/formation-clean-2node.pcap` — VAXA
  (`08:00:2b:fb:72:36`, member) / VAXB (`08:00:2b:94:ca:47`, joiner), the clean
  isolated-bridge reference.
- `~/vax/cluster/captures/formation-ci1-joinwindow.pcap` — VAX1 (member) / VAX2
  (joiner), the golden lab formation.

**The bootstrap (byte-identical in both captures).** The channel opens with a
fixed three-step exchange, the member initiating:

```
formation-clean-2node.pcap (directed HELLOs):        formation-ci1-joinwindow.pcap:
  +26.9909  VAXA(mem) ->VAXB(join)  abs30 = b2         +0.0000  VAX1(mem) ->VAX2(join)  abs30 = b2
  +26.9911  VAXB(join)->VAXA(mem)   abs30 = b3         +0.0003  VAX2(join)->VAX1(mem)   abs30 = b3
  +26.9922  VAXA(mem) ->VAXB(join)  abs30 = b4         +0.0012  VAX1(mem) ->VAX2(join)  abs30 = b4
```

**The rule (GROUNDED):** on receiving a directed HELLO carrying word `X`, a node
replies with `X + 1`, **saturating at b4**: `b2 → b3`, `b3 → b4`. The values are:

| word | meaning |
|---|---|
| **b2** | channel **INIT** — the member's *first* directed contact only. The **joiner never originates b2** (0 of 213 joiner directed HELLOs in the clean capture carry b2). |
| **b3** | channel-verify **REQUEST** / probe. A node initiates a verify by sending b3 (as a plain directed HELLO **or** a §4(k) padded HELLO). |
| **b4** | channel-verify **CONFIRM** / ack — terminal. Sent in immediate (~0.2 ms) response to a received b3. There is no b5. |

**The ongoing keepalive (GROUNDED).** After the bootstrap reaches b4 the channel
is confirmed, and the two nodes run an indefinite **b3↔b4 oscillation**: each
node periodically re-initiates the verify with a fresh b3 REQUEST (on its
poller-sweep timer, ~1–10 s apart) and the peer immediately acks it with a b4
CONFIRM. The initiator role alternates between the two nodes. Frame counts over
`formation-clean-2node.pcap` (directed HELLOs, class-`0x05`):

| sender | b2 | b3 | b4 |
|---|---|---|---|
| VAXA (member) | 1 | 72 | 70 |
| VAXB (joiner) | **0** | 70 | 72 |

The near-even b3/b4 split on **both** nodes is the oscillation; the joiner's
`b2 = 0` confirms only the member ever INITs. The **padded** §(4k) size-verify
HELLOs are simply b3 REQUESTs (each acked by a plain b4): in the clean capture
VAXB→VAXA padded(1514) carries b3 and is answered `+0.2 ms` later by VAXA→VAXB
plain b4, and symmetrically for VAXA's own padded probe. This **grounds the
§4(k) "op-0xb3"**: it is exactly this REQUEST word on a size-padded frame.

**The OVMX gate (`vms-d94`).** OVMX's directed-HELLO builder (`vms-5fe`)
hard-held abs-30 at a fixed b3 and **never emitted b4**, so the member never saw
OVMX CONFIRM the channel; VAX1's NISCA handshake never finalized and it looped
the padded-HELLO flood + START round-0 forever (the §4(k) symptom). The
corrective is the response rule above: OVMX reads the received word `buf[30]` and
replies `b2→b3`, `b3→b4` (the fix), `b4→b3` (re-initiate), so it reaches b4 and
toggles b3↔b4 like the real joiner VAXB. Implemented as `scs_hello_response_pfw()`
+ the `per_frame_word` arg of `scs_hello_build_directed_frame()` in
`src/vmsscs/scs_hello.c`, driven from the `scsd.c` `--respond` path.

**Clean-room note:** the b2/b3/b4 rule is derived purely from the request/response
timing and frame-count structure observed on the reference-lab wire (two
independent captures) plus the saturating-increment pattern; no VSI/HPE source or
binary was read. The low-byte-carries-state / high-byte-`0x00` split and the
INIT/REQUEST/CONFIRM labels are OVMX working labels for the observed values.

### 4(b) HELLO frame — offsets 72–133 (HELLO-specific tail)

Total frame 134 bytes (120-byte SCA content + 14-byte Ethernet header).
Verified against `scs-idle-baseline.pcap` frames 1 (VAX1→multicast), 2
(VAX2→VAX1 directed), 3 (VAX1→VAX2 directed).

| Abs. offset | Size | Field | Grounding |
|---|---|---|---|
| 72 | 20 | zero padding | unknown |
| 92 | 2 | **directed-HELLO flag / node-incarnation counter** | GROUNDED: `0x0000` on multicast HELLOs; on **directed** HELLOs it is the **node-incarnation number the sender attributes to the peer** — `0x0001` on a fresh/first contact, and it increments `0x0002`, `0x0003`, … each time that peer re-forms its channel (established member advertises the returning node's incarnation here; the joiner then echoes this value into its `0x41` START `[22:24]`, §4i.B). Byte-exact 1↔0x0001, 2↔0x0002, 3↔0x0003 across the `vms-af2` first-timer/2nd/3rd-incarnation specimens. Every fresh-formation specimen shows `0x0001`, which is why it originally read as a plain "directed=1" flag. |
| 94 | 2 | constant trailer `0x9205` | unknown/inferred |
| 96 | 4 | changing 4-byte value (increases across the capture) | unknown/inferred — plausibly a local timer/tick, not confirmed |
| 100 | 12 | constant tail `99 00 bc 00 03 58 51 41 00 00 00 00` | unknown/inferred |
| 112 | 8 | zero padding | unknown |
| **120** | **6** | **sender's real hardware LAN MAC** | **GROUNDED**: matches the actual Ethernet source MAC used on the wire (e.g. VAX2's `08-00-2b-78-56-b9`, VAX1's `08-00-2b-4a-b7-15`) before/regardless of DECnet's logical-address remap. Confirmed in every HELLO across both nodes. |
| 126 | 2 | constant trailer `0x2600` | unknown/inferred |
| **128** | **2** | poller-sweep/directed marker | **GROUNDED**: `0x0000` on multicast HELLOs, `0x001F` (31 decimal) on directed HELLOs — the value `31` is byte-exact against SDA `SHOW PORTS` PDT `PEA0` **"Poller Sweep 31"**. Frame examples: `scs-idle-baseline.pcap` frame 1 (`0000`, multicast) vs. frame 2 (`1f00`, directed). |
| 130 | 2 | constant `0x0064` (100 decimal) | unknown/inferred — not corroborated against any decoder-ring value |
| 132 | 2 | trailer, always `0x0000` | unknown |

### 4(c) SOLICIT / connect-and-directory-lookup phase

**Boot-time SOLICIT** (satellite VC establishment on disk-server discovery),
total frame 92 bytes (78-byte SCA content). Verified against
`satellite-niscs-boot-solicit.pcap` frame 1100 (VAX3 → multicast group 1).
Shares offsets 14–71 with HELLO (§4a); diverges after the nonce:

| Abs. offset | Size | Field | Grounding |
|---|---|---|---|
| 72 | 4 | zero | unknown |
| 76 | 1 | target device spec length prefix (observed `9`) | GROUNDED (matches following string) |
| 77 | *len* | target device spec, ASCII (observed `"_$2$DUA0:"`) | GROUNDED — literal "serve me this system disk" request from the RE-specimens doc, confirmed byte-exact here |
| 86 | 6 | trailing zero pad | unknown |

**Directory lookup / connect handshake** (pre-membership, SYSAP name
resolution — e.g. asking who serves `SCS$DIRECTORY`, `MSCP$DISK`,
`VMS$VAXcluster`). Seen throughout the early part of
`formation-ci1-joinwindow.pcap` as a family of small variable-size frames
(58/62/66/70/106/110 bytes total) sharing the offset 14–29 dst/flag/src
preamble (see §4d) but with **no valid Con.ID yet** at the generic
offset — consistent with the SDA "listen" CDT state, which shows
`Remote Con. ID 00000000` before a connection is established. These frames
carry literal ASCII SYSAP names in the body, GROUNDED by direct
observation, e.g.:

- frame 29 (`formation-ci1-joinwindow.pcap`, 110 bytes): body contains
  `"SCS$DIRECTORY   "` and `"SCS$DIR_LOOKUP        "`
- frame 37/39 (94 bytes): body contains `"MSCP$TAPE"`, then
  `"NOT PRESENT HERE"` (a negative lookup response)
- frame 41–46 (94 bytes): body contains `"MSCP$DISK"` repeated (both
  local and remote SYSAP name in the same message)
- frame 45/46/50 (94–110 bytes): body contains `"VMS$VAXcluster"`
- frame 58 (110 bytes): body contains `"MSCP$DISK"` and
  `"VMS$DISK_CL_DRVR5.0"` (5.0 looks like a class-driver version string)

The exact opcode/field layout of this lookup exchange was originally left
`unknown` here; it is **now grounded in §4(h)** (`vms-560`): the `0x5b`
directory frames carry a `SCS$DIRECTORY` SYSAP connect handshake (handle pair
at [50:58], same offsets as §4g phase 4) followed by a name-resolution body
whose 16-byte result field carries the literal `"NOT PRESENT HERE"` on a
negative lookup, and each is credit-acked by a `0x48` short (§4h).

### 4(d) SCS message header (the 190-byte fixed class)

This is the single most solidly grounded structural finding of this
document: **every** SCS message that totals exactly 190 bytes shares one
header layout, and it is the only length class where the Connection-ID
location was independently confirmed against the SDA decoder ring at scale
(17557/17557 frames in `formation-ci1.pcap`; see §2 Table 2 and the
dissector's `decode_scs_envelope()` docstring for the full validation
numbers across all other length classes, which do **not** reliably match
this layout and are therefore left undecoded).

Verified against `scs-idle-baseline.pcap` frames 9–14 (idle-time SCS
directory/status traffic riding `VMS$VAXcluster`) and reconfirmed in the
DLM and MSCP sections below.

| Abs. offset | Size | Field | Grounding |
|---|---|---|---|
| 14 | 2 | SCA length field (`0x00BC` = 188 → 190 total) | GROUNDED |
| 16 | 6 | Destination logical LAVC addr | GROUNDED |
| 22 | 2 | Connect flag | constant `0x0001` |
| 24 | 6 | Source logical LAVC addr (sender's own) | GROUNDED |
| 30 | 2 | SCS sequence/type word (varies per-message, e.g. `4b13`) | unknown/inferred |
| 32 | 32 | SCS sequence-number region: two 16-bit counters, each repeated up to 3×, zero-padded to 32 bits; a constant `0x0012` (=18 decimal) sits at offset 38–39 | the `18` is **GROUNDED**: byte-exact match to SYSGEN `NISCS_LAN_OVRHD 18`. The repeated 16-bit values plausibly correspond to the CSB's "Next seq. number" / "Last seq num rcvd" / "Last ack. seq num" triad documented in SDA `SHOW CLUSTER`, but the specific mapping of which repeat is which CSB field is **inferred**, not independently confirmed. |
| **64** | **4** | **Remote Connection ID**, LE `uint32` | **GROUNDED**: e.g. bytes `09 00 c5 62` = `0x62C50009`, byte-exact to VAX1's `Local Con. ID` for `VMS$VAXcluster` in the SDA decoder ring, appearing in a frame sent BY VAX2 — i.e. this field is the *destination's* Con.ID as the sender addresses it. |
| **68** | **4** | **Local Connection ID**, LE `uint32` | **GROUNDED**: the complementary value (e.g. `0x33580008`, VAX2's own Con.ID) — confirmed the pair swaps consistently with send direction (VAX1→VAX2 frames show `remote=0x33580008 (VAX2)`, `local=0x62C50009 (VAX1)`, and vice versa for VAX2→VAX1 frames). This directly matches CDT terminology "Local Con. ID" / "Remote Con. ID" from `SHOW CONNECTIONS`. |
| 72 | 132 | SYSAP-specific message body | **the transaction envelope is now GROUNDED in §4(j)** (`vms-f85`): a SYSAP-level send-msg#/ack-msg# counter pair, a transaction-id/checksum correlation token, a message-category/flags byte (bit `0x80` = response), and an opcode. The `VMS$VAXcluster` connection-manager add-member dialogue rides here, and the joiner's **VOTES** field is grounded at body-offset 22 (abs 94). The per-opcode DLM/MSCP sub-fields (lock mode, resource-id, status; §4f) remain undecoded. |

### 4(e) MSCP disk-serving request/response framing

MSCP$DISK ↔ VMS$DISK_CL_DRVR traffic was identified **by connection
identity**, not by decoding MSCP command-block fields (which we could not
ground). In `scs-dlm-lockconflict.pcap`, a request/response pair rides on
the exact §4(d)-style dst/flag/src preamble but at 94/110-byte total
lengths (not the 190-byte class, so the Con.ID location is *not* grounded
for these — see §4(d) caveat):

- frame 17 (94 bytes, VAX1→VAX2, t=+3.1004s): SCS envelope, dst/src match
  VAX1/VAX2 logical MACs; body 76 bytes, undecoded.
- frame 18 (110 bytes, VAX2→VAX1, t=+3.1007s): SCS envelope, response to
  the above; body 92 bytes, undecoded.
- A second such pair occurs at frame 21/22 (t=+4.2359s), symmetric
  (VAX2 requests, VAX1 responds).

These request/response pairs are consistent with `MSCP$DISK`/
`VMS$DISK_CL_DRVR` polling activity riding alongside the DLM burst — timing
alone (not payload content) is the basis for calling this class MSCP; we
did **not** locate a grounded MSCP opcode field. `formation-ci1.pcap`
additionally shows a large **block-transfer class** (206–1500 bytes,
Table 2) that we believe carries bulk MSCP disk I/O data (its size caps
out at exactly `NISCS_MAX_PKTSZ`=1498, GROUNDED against the SYSGEN
tunable), but its header layout is unknown — see the frame-5990 example in
`formation-ci1.pcap` (1500-byte total): the bytes at what would be the
190-byte class's Con.ID offset instead echo the constant HELLO/SOLICIT
discovery-header prefix (`08 00 00 80 05 01 00 00`) plus a
`"VAX2  "`-style embedded node name, meaning this frame class has an
**entirely different header shape** that was not further decoded. Marked
unknown in the dissector rather than mislabeled.

**Correction (`vms-84f`, §4k):** frame 5990 is **not** an MSCP bulk transfer at
all — it is a **padded directed HELLO** (a PEDRIVER channel packet-size
verification frame), which is why it carries the HELLO discovery-header prefix
and an embedded node name and a body that is **pure zero padding**. It is now
GROUNDED in **§4(k)**. The genuine MSCP bulk block-transfer frames are the
separate `0x4b`/`0x13` sequenced-application class (206–718 bytes here, nonzero
data body); only the two 1500-byte frames in `formation-ci1.pcap` (idx 5990,
7534) are padded HELLOs. This distinguishes the two large-frame families the
census (Table 2) had lumped together.

### 4(f) DLM: enqueue / deny / release / grant (`scs-dlm-lockconflict.pcap`)

The specimen index describes the captured scenario as: **T1** VAX2 issues
an `OPEN` that is denied (`RMS-E-FLK`, enqueue conflict, VAX1 is lock
master); **T2** VAX1 releases; **T3** VAX2 `OPEN`s again and succeeds.

**Connection identity** (GROUNDED via §4(d)'s Con.ID field, applicable
here because all DLM traffic in this pcap is carried in 190-byte frames):
100% of the 190-byte frames in this capture carry
`remote/local ∈ {0x62C50009, 0x33580008}` — i.e. **all DLM traffic rides
the `VMS$VAXcluster` connection**, exactly as the decoder ring states
("DLM lock traffic rides the VMS$VAXcluster↔VMS$VAXcluster connection").

**Burst timing** (GROUNDED by direct frame-timestamp observation): three
distinct bursts of 190-byte `VMS$VAXcluster` frames appear, cleanly
separated in time and matching the documented T1/T2/T3 narrative:

| Burst start | Frame range | Frame count | Correlates to |
|---|---|---|---|
| t=+2.966s | frames 9–16 | 8 | T1 — VAX2's denied `OPEN` attempt |
| t=+4.967s | frames 25–30 | 6 | T2 — VAX1's release |
| t=+11.024s | frames 41–58 (with a gap) | ~18 | T3 — VAX2's successful re-`OPEN` |

(A 4th, larger burst at t=+12.8s–19.9s reflects continued lock-database
housekeeping/directory traffic after the grant — not separately
attributed to T1/T2/T3.)

**Resource names observed in message bodies** (GROUNDED — directly
observed ASCII, cross-referenced against the well-documented public
OpenVMS RMS/XQP lock-resource naming convention, i.e. the `F11B$` prefix
for Files-11 volume-allocation locks, documented in OpenVMS System
Management / RMS reference material — this is a naming *convention*
citation, not a claim about undocumented internal structure):

- `"F11B$sSDSK1     *"` / `"F11B$sVAX2   *"` / `"F11B$aSYSDSK1     *"` /
  `"F11B$aVAX1DATA    *"` — Files-11 volume-lock resource names for the
  `SYSDSK1`/`VAX1DATA` volumes, seen in frames 9–16, 27–30, 41–58 of
  `scs-dlm-lockconflict.pcap`.
- `"DTI$SYSTEM$VAX2   *"` and `"SYSTEM$VAX2   *"` — a second resource
  namespace, seen in frames 25/26, 48/50, 92/93.
- `"CACHE$cmVAX1DATA    "` — an extent-cache lock resource, frames 47/70.
- The literal text `'LOCK BLOCKED: "+F$MESS'` (a truncated DCL
  `F$MESSAGE()` invocation) appears in frames 11–16, 27/28, 45/46, 54/55,
  57/58, 72–75 — almost certainly a `SYS$OUTPUT`/OPCOM broadcast from an
  interactive DCL session on the lab (visible because it rides the same
  `VMS$VAXcluster` SCS connection as the lock traffic, not because it is
  itself a raw `$ENQ`/`$DEQ` wire field).

**What is NOT grounded**: the byte offsets for lock mode (NL/CR/CW/PR/PW/
EX), resource-ID/lock-ID values, and completion status
(`SS$_NORMAL`/`SS$_ENQLKOVF`-equivalent) within the 132-byte message body
following the Con.ID pair. We could not locate a field we could
confidently map to "lock granted" vs. "lock denied" purely from wire
diffing — the resource-name strings and burst timing are the only
grounded correlation to the T1/T2/T3 narrative. This is flagged as an open
RE gap (see report to PM / follow-on item).

### 4(g) Connection-manager membership handshake (VMS$VAXcluster join)

This subsection isolates the SCS exchange by which a joining node is admitted
to the cluster and the `VMS$VAXcluster`↔`VMS$VAXcluster` connection (the
connection manager's own SCS connection, which subsequently carries all DLM
and membership traffic — §4(f)) is established. It was captured for `vms-b6c`.

**Specimens.** The byte-level grounding below is taken from
`formation-ci1-joinwindow.pcap` (VAX2 joining VAX1's running cluster), and
every structural claim was re-validated, byte-for-byte at the same payload
offsets, against the independent full-run `formation-ci1.pcap` (different
capture session, 18 541 SCA frames). A fresh third specimen,
`ci3-join-membership-live2node-20260728.pcap`, was captured for the nonce
stability finding below. All three agree.

**The join is a fixed phase sequence** (GROUNDED — directly observed, 0-based
**SCA-frame** indices into `formation-ci1-joinwindow.pcap`, per the §1 index
convention). Note: `dissect_sca.py --frame N` counts *all* pcap records, and
this pcap has 8 non-SCA records early (raw indices 2,4,5,6,8,12,16,17), so for
frames past SCA#17 the raw `--frame` argument is the SCA index **+ 8** — e.g.
the keystone CONNECT-REQUEST (SCA#39) and CONNECT-RESPONSE (SCA#42) are
`--frame 47` and `--frame 50`:

| Phase | Frames | What happens on the wire |
|---|---|---|
| 0. Discovery | 0–11 | VAX1 multicasts HELLO to group 1 (`AB-00-04-01-01-01`); the joiner (VAX2) appears with its own multicast HELLO (frame 11) |
| 1. Directed-VC channel handshake | 12–14 | 3 **directed** HELLO frames, dst = peer's *hardware* MAC, distinguished by the offset-16 per-frame word stepping `b200`→`b300`→`b400` (§4a offset-30). Carry the join nonce (below). |
| 2. START / config exchange | 15–20 | opcode-`0x41` 106-byte frames carrying ASCII **software version + hardware type + node name** (below), plus a 46-byte opcode-`0x41` ack |
| 3. Directory lookup | 21–36 | opcode-`0x5b` frames resolving SYSAP names: `SCS$DIRECTORY`/`SCS$DIR_LOOKUP`, then `MSCP$TAPE`→`"NOT PRESENT HERE"`, `MSCP$DISK` |
| 4. **SYSAP connect / accept** | 37–44 | opcode-`0x4b` frames establishing the `VMS$VAXcluster` connection and binding the Connection-ID pair (below) |
| 5. Credit / ack settle | 45–57 | opcode-`0x48` 41-byte credit-return shorts + opcode-`0x4b` 58/62-byte acks; steady-state 190-byte VC (§4d) begins |

**SCS envelope opcode/format bytes** (offsets 16–17, payload-relative; abs
30–31). Across **all 2 975 directed SCS-envelope frames** in
`formation-ci1-joinwindow.pcap`:

| Offset | Field | Grounding |
|---|---|---|
| 16 | **SCS message-type byte** | inferred label, GROUNDED correlation: value partitions the exchange 100%/0-residual — `0x41`=START/config (6 frames), `0x5b`=directory lookup (9), `0x4b`=sequenced-application message = connect **and** all VC/DLM data (2 951), `0x48`=credit-return short (9). No public SCS opcode *table* was used, so the numeric→name mapping is inferred; the value↔phase partition itself is a grounded fact. Reconfirmed on `formation-ci1.pcap` (same four values dominate; the bulk-block-transfer class additionally shows `0x7b`/`0xb3`, left unknown as in §4e). |
| 17 | **format/version constant `0x13`** | **GROUNDED: 2 975/2 975 directed SCS-envelope frames carry `0x13` here, 0 residuals** — a fixed protocol-format byte, not a counter. |

**NAMING — START / STACK / ACK, corrected (`vms-c35`, 2026-08-05).** Earlier
revisions of this section called the round-1 106-byte `0x41` frame a
"START-retransmit". *VAXcluster Principles* p. 2-12 names it a **STACK** (Start
Acknowledgment) and gives it two jobs: "a STACK acknowledges receipt of the
START… And second, each node uses the STACK to again supply the other node with
a description of itself" — which is exactly why the round-1 frame is 106 bytes
and re-carries the config body, where a pure retransmit would be redundant. The
round-2 46-byte frame is the **ACK**, normally discarded on receipt ("each port
driver simply discards the ACK it receives… because it already considers the
virtual circuit to be OPEN", p. 2-12). This spec now uses START/STACK/ACK,
matching `src/vmsscs/scs_vc.c` and `scs_vc.h`. **Caveat kept:** p. 2-14 shows a
genuine *retransmitted START* is possible in the asymmetric-timing case ("A
response of START advances the circuit only to the START RECEIVED state,
causing the port driver to issue a STACK") — a retransmitted START exists in
SCA, it just is not what our round-1 frame is.

**Phase 2 — START/config body — GROUNDED field map** (opcode `0x41`,
106-byte class; captured for `vms-cd0`). The identity a joining node presents
for membership rides here. This was grounded by **controlled reconfiguration**
of the lab: the joiner (VAX2/`[SYS1]`) was rebooted twice with SYSGEN-varied
`SCSNODE`/`SCSSYSTEMID`/`VOTES` and the bodies byte-diffed. New specimens:
`cd0-bootB-zk1099-join-20260728.pcap` (SCSNODE `"ZK"`, SCSSYSTEMID **1099**,
VOTES 0) and `cd0-bootC-zk1099-votes2-20260728.pcap` (same, VOTES **2**),
diffed against the golden `formation-ci1-joinwindow.pcap` (VAX1 1025 / VAX2 1026)
and re-validated on the full-run `formation-ci1.pcap`.

All payload offsets are **payload-relative** (payload byte 0 = abs frame
offset 14; add 14 for the absolute offset). The 106-byte START body:

| Pay off | Size | Field | Grounding |
|---|---|---|---|
| 16 | 1 | opcode `0x41` (START/config) | §4g partition (inferred label) |
| 17 | 1 | format constant `0x13` | GROUNDED (§4g) |
| 18 | 2 | SCS counter region begins (`0x0000`) | see "counters" below |
| 20 | 2 | SCS sequence counter A (also mirrored at [30:32]) | GROUNDED mechanism / inferred CSB mapping |
| 22 | 2 | SCS counter B (ack-side) | GROUNDED mechanism / inferred CSB mapping |
| 24 | 2 | constant `0x0012` = 18 = SYSGEN `NISCS_LAN_OVRHD` | **GROUNDED** (byte-exact to tunable; 42/42 frames) |
| 26 | 4 | zero | constant observed |
| 30 | 2 | SCS sequence counter A (mirror of [20:22]) | GROUNDED mechanism |
| 32 | 6 | zero | constant observed |
| 38 | 2 | constant `0x0001` | constant observed |
| 40 | 2 | zero | constant observed |
| **42** | **2** | **inner length** = (payload length − 44) | **GROUNDED**: `0x003e`=62 for the 106-byte START and `0x0002`=2 for the 46-byte ack — the length identity `LE16([42:44]) == len(payload) − 44` holds **42/42 frames, 0 residuals** across both length classes (same proof style as §2's outer length). The config descriptor section starts at payload [44]. |
| **44** | **2** | **config-round counter** | **GROUNDED**: increments `0 → 0 → 1 → 1 → 2 → 2` across START(round 0), **STACK**(round 1) and the 46-byte ACK(round 2); both nodes carry the same round. |
| **46** | **2** | **SCSSYSTEMID** (LE `uint16`) | **GROUNDED**: `0x0401`=1025 (VAX1), `0x0402`=1026 (VAX2), `0x044b`=**1099** (the reconfigured ZK node) — byte-exact to the SYSGEN/SDA-reported SCSSYSTEMID in **28/28** 106-byte frames across three distinct values. The joiner's logical LAVC src addr [10:16] tracks it (`aa:00:04:00:4b:04`, node `0x4b`=75 = 1099 & 1023). |
| 48 | 4 | zero (SCSSYSTEMIDH region) | constant observed |
| 52 | 2 | constant `0x0001` | 28/28 |
| 54 | 2 | constant `0x0240` = 576 | 28/28; inferred (SCS transport param, no tunable match) |
| 56 | 2 | constant `0x00d8` = 216 | 28/28; inferred |
| **58** | **8** | **software version string** `"VMS V7.3"` (ASCII) | **GROUNDED**: byte-exact `56 4d 53 20 56 37 2e 33` in **28/28** frames. *Correction to the earlier §4g note:* the field is `"VMS V7.3"` for **all** nodes; the previously-reported `"VMS V7.3f"` was a misread — the `f` (`0x66`) is the first byte of the per-boot token at [66:], which happened to be printable in the golden VAX1 frame (it is `0xd8`/`0x5d`/`0xae` in other boots). |
| **66** | **8** | **THIS SYSTEM'S INCARNATION** — a single VMS absolute-time quadword (LE, 100 ns units since 17-NOV-1858), = the time this system was booted | **GROUNDED** (`vms-2f3`, 2026-08-01) four ways: (1) SDA on VAX1 rendered OVMX's CSB as `Incarnation 26-JUL-2026 14:35:33` — decoding our replayed template bytes `bb 8e 67 7a 94 00 bc 00` to the second; (2) the same dump gives real peers their own boot times (VAX3 `1-AUG-2026 00:02:21`, VAX1 `30-JUL 08:54:26`, which had not rebooted); (3) after OVMX started emitting a live value, VAX1 read back `Incarnation 1-AUG-2026 15:25:12`, matching the `0x00bc05526906b4a1` we emitted; (4) **public doc** — VSI *System Management Utilities Ref. Vol. II*, SHOW CLUSTER SYSTEMS class: *"INCARNATION: Unique 16-digit hexadecimal number established when the system is booted."* Sixteen hex digits **is** this quadword. |
| **74** | **4** | **hardware-type string** `"VAX "` (ASCII) | **GROUNDED**: 28/28 frames |
| 78 | 2 | constant `0x0006` | 28/28 |
| 80 | 2 | `0x0a` = 10 = SYSGEN `CLUSTER_CREDITS` at [81] | GROUNDED numeric match (as §4g credit) |
| 82 | 6 | zero | constant observed |
| 88 | 2 | constant `0x0077` | 28/28 |
| **90** | **8** | **node name** (ASCII, **fixed 8-byte, blank-padded, left-justified**) | **GROUNDED**: `"VAX1    "`, `"VAX2    "`, and `"ZK      "` — the 2-char `"ZK"` name occupies the same 8-byte field with 6 trailing spaces and **the following bytes do not shift** (28/28), proving a fixed-width blank-filled field, *not* the length-prefixed encoding HELLO uses (§4a). Distinct encoding from §4a. |
| **98** | **8** | **frame-composition time** — a second VMS absolute-time quadword, distinct from [66:74] | **GROUNDED as a live timestamp** (`vms-2f3`): real peers carry two or three *different* values here inside a single capture, and one of VAX3's matches — to the second — the OPCOM line it printed as it built the frame. Its precise *role* is **not** grounded and OVMX does not claim one. What **is** grounded is the negative: **no real node ever sends a stale one.** |

> ### ⚠ CORRECTION (2026-08-01, `vms-2f3`) — this table previously split
> **[66:74]** into three fields (a 5-byte token at 66, a flag at 71, a "constant
> `0x00bc`" at 72) and **[98:106]** into two. **Both are single 8-byte VMS
> absolute-time quadwords.** The upper bytes only *look* constant because every
> 2026-era VMS timestamp ends `bc 00`. The old "per-boot token, not derivable
> from passive capture" reading was wrong in a way that mattered: OVMX replayed
> the captured template's [66:74] on **every** boot, forever, advertising a
> 26-JUL-2026 boot time for six days. Per VSI *OpenVMS Cluster Systems* App.
> C.7.1, a connection reestablished after `RECNXINTERVAL` *without the node
> having rebooted* earns a **CLUEXIT bugcheck** on the surviving side — a node
> whose incarnation never changes is exactly that node. Fixed in `c302b7d`.
>
> **Method note for the rest of this spec:** any remaining "observed constant"
> in a replayed template that decodes as a plausible 2020s VMS quadword should
> be re-audited the same way before it is trusted. The honesty debt (`vms-70c`)
> and this bug turned out to be the same defect class.

**These quadwords are NOT node identity.** They change across reboots of the
*same* node: VAX1's values differ between the days-old golden capture and the
fresh `cd0-boot*` captures although VAX1's name/SCSSYSTEMID are unchanged. That
observation was always correct — it is *why* they are timestamps.

**SCS counters in the START phase.** Region [18:32] carries the same
sequenced-message counters as §4d: a 16-bit counter at [20:22] mirrored at
[30:32], and a second counter at [22:24]. On a fresh join they read `0x0001`;
on VAX1 (already running, higher sequence) they read e.g. `0x1a29`. They advance
per message in lockstep with the flow, driving the reliable START→ack exchange.
The **mechanism** is GROUNDED; the precise next-seq/last-ack CSB assignment is
**inferred** (same honesty caveat as §4d/§4g). Note these per-node counters are
what makes a captured START **non-replayable** — a valid START must advance
OVMX's *own* counters, not echo a VAX's.

**Phase 4 — the connect→accept handshake and Connection-ID binding**
(opcode `0x4b`). This is the keystone finding, and it is GROUNDED against the
SDA `SHOW CONNECTIONS` decoder ring (§3). The **110-byte** connect frames
carry the Local/Remote Connection-ID pair at the *same* payload offsets as
the 190-byte class (§4d): **remote at [50:54], local at [54:58]**, LE `uint32`.
Both `VMS$VAXcluster` SYSAP names (local endpoint, then remote endpoint)
follow in ASCII at [62:]. The pair is negotiated exactly as a connection
manager would:

| Frame | Dir | remote Con.ID [50:54] | local Con.ID [54:58] | Reading |
|---|---|---|---|---|
| 39 | VAX1→VAX2 | `0x00000000` (peer's not yet known) | `0x62C50009` (VAX1's own) | **CONNECT-REQUEST**: VAX1 offers its local Con.ID, remote still zero |
| 42 | VAX2→VAX1 | `0x62C50009` (VAX1's, now learned) | `0x33580008` (VAX2's own) | **CONNECT-RESPONSE/ACCEPT**: VAX2 echoes VAX1's Con.ID and supplies its own |

Both values are **byte-exact** to the SDA `SHOW CONNECTIONS` CDT pair
`Local Con. ID 62C50009` / `Remote Con. ID 33580008` for
`VMS$VAXcluster`↔`VAX2::VMS$VAXcluster` (§3). The `remote=0 → filled`
transition across the request/response is the admission act: after frame 42
both nodes share the bound pair, and every subsequent 190-byte VC/DLM frame
(§4d, §4f) addresses it. Identical offsets and values reproduced in
`formation-ci1.pcap` (frames 39/42). **GROUNDED.**

**SCS sequenced-message counters** (offsets 18–19 and 20–21, two LE `uint16`).
Across the connect exchange (frames 37→44) the pair advances monotonically in
lockstep with the message flow — VAX1 emits `(5,6),(6,7),(7,8),(8,9)`; VAX2
emits `(6,6),(7,7),(7,8),(8,9)` — i.e. a sender's counter reappears as the
peer's other counter one frame later. This GROUNDS the *mechanism* (a
reliable sequenced-message send/acknowledge pair driving the handshake); the
precise send-vs-ack assignment is **inferred** (candidate: the CSB
`Next seq`/`Last seq num rcvd` triad from SDA `SHOW CLUSTER`, §3), not
independently confirmed — the same honesty caveat as §4d.

**Credit.** The value `0x0a` (10 = SYSGEN `CLUSTER_CREDITS`, §3) is present in
the connect/VC frames (e.g. frame 37 payload [46], frame 39 payload [48]) and
is the dominant value at that region across directed frames. GROUNDED as a
numeric match to the tunable, but its offset shifts between message classes,
so it is not pinned to a single fixed field — reported as a grounded value,
not a grounded offset.

> **⭐ SUPERSEDED — the credit field IS pinned, at SCA `[48:50]` LE u16**
> (absolute frame offset `[62:64]`), i.e. the two bytes immediately preceding
> the remote/destination Con.ID at `[50:54]`. `vms-76e` re-measured this over
> `formation-ci1.pcap` (18 558 frames) and `formation-ci1-joinwindow.pcap`
> (3 000 frames), both under `/data/training/vax/cluster/captures/`.
> **Re-derive it:** `tools/scs_credit_measure.py --quick` (the two grounding
> captures, ~5 s) or without `--quick` for all 47 (~5–10 min). It re-measures
> every figure below from the raw pcaps and PASS/FAILs each against a
> checked-in `EXPECTED` table — last full run 2026-08-03, **30 checks, 0
> failures**. The captures are host-only and not in git, so ctest runs only the
> cheap half (`scs_credit_figures`), which asserts these numbers still appear
> verbatim here and in `scs_credit.h`.
>
> **Method — two populations, and each line says which it uses.** Take
> `sca = frame[14:]`. **(A)** keep on `len(sca) == <class>` alone, no marker
> filter; **(B)** additionally require `sca[16:18] == 4B 13`. Over the two
> captures there are 20 459 190-byte frames, marker split
> `{0x4B13: 19 860, 0x5B13: 591, 0x7B13: 8}` — every one is an SCS message of
> the `0x?B13` family, all carrying the same credit field at the same offset, so
> at this length (A) *is* "the whole `0x?B13` family" (the script asserts that
> equality rather than assuming it). Three independent lines:
>
> 1. **Conservation** over the 190-byte class — **population (A), no marker
>    filter, and it must be**: a debit/credit account only balances if every
>    message on the connection is counted. Summing `[48:50]` across every
>    190-byte frame a node *sends* against the count of 190-byte messages it
>    *received*: `formation-ci1` VAX1 granted 10 842 vs peer sent 10 842
>    (Δ0), peer granted 6 712 vs VAX1 sent 6 715 (Δ3); `joinwindow` 1 601 vs
>    1 602 (Δ1) and 1 300 vs 1 300 (Δ0). All four reproduce exactly. That is the
>    debit/credit identity of *VAXcluster Principles* p. 2-43 and no other
>    header field satisfies it.
>
>    > **Correction (`vms-76e`, adversary-caught).** An earlier revision of this
>    > note headed the `0x4B13` filter "the counts below do not reproduce
>    > without it" and applied it to all three lines. For line 1 that is
>    > **inverted**: filtering *destroys* the identity, giving 10 817 vs 10 266
>    > (Δ−551) and 6 369 vs 6 695 (Δ+326) — a refutation. The four figures
>    > printed were always the population-(A) numbers and are correct; only the
>    > recorded method was wrong. **The offset conclusion is unaffected.**
>
> 2. **Value shape** — **population (B)** — over 19 860 190-byte `0x4B13` frames the field takes only
>    `{0:5174, 1:10696, 2:2582, 3:1405, 4:3}`: a piggybacked Pending Receive
>    Credit, not a counter. Unfiltered (population (A)) the same histogram over
>    all 20 459 frames is `{0:5418, 1:11042, 2:2587, 3:1409, 4:3}` — the same
>    shape, which is the independent reason the `0x5B13`/`0x7B13` siblings
>    belong in line 1. (Note `[46:48]` in the 190-byte class is a
>    *constant* `0x000a` — that is the value the older note above was reading,
>    and it is a different field.)
> 3. **Tunable match at formation** — **population (B)** — in the 110-byte `CONNECT_REQ`/
>    `ACCEPT_REQ` class the same field carries the Send Credits that SYSAP
>    extends, byte-exact to **two distinct** SYSGEN parameters in one capture:
>    `VMS$VAXcluster`↔`VMS$VAXcluster` = **10** (`CLUSTER_CREDITS`),
>    `MSCP$DISK`→`VMS$DISK_CL_DRVR` accept = **8** (`MSCP_CREDITS`), plus
>    `SCS$DIRECTORY` 3, `SCS$DIR_LOOKUP` 1, `SCA$TRANSPORT` 6.
>
> **Scope of the grounding — every admitted class is measured.** Offset 48 is
> asserted only for the SCS *message* classes **58/62/66/86/94/110/190** SCA
> bytes, and all seven were tabulated over **all 47** `.pcap` files in
> `/data/training/vax/cluster/captures/` under **population (B)**
> (n / distinct / max at `sca[48:50]`): 58 → 1212/2/1 · 62 → 1087/1/0 ·
> 66 → 944/1/0 · 86 → 194/1/1 · 94 → 3670/2/1 · 110 → 3999/5/10 ·
> 190 → 288 484/5/4. The block-data-transfer classes
> (70/82/206/270/398/462/526/…) and 50/122/126/142 carry large unrelated values
> there (e.g. 70 → 752 distinct, max 65 447) and are refused, and the 41-byte
> `0x48` short does not reach offset 48 at all — which is the residue of truth
> in the "offset shifts between message classes" note above.
>
> **Correction (`vms-76e`, adversary-caught): 106 is NOT one of these classes.**
> An earlier revision of this note and of `scs_credit.h` listed 106. There are
> **zero** 106-byte SCA frames with the `0x4B13` marker in any capture; all
> **792** that exist are marker `0x4113` — the §4(j) START/config frames, a
> different layer with no credit field (`sca[48:50]` is a constant 0 in 792/792).
> The entry came from misreading the §4(c)/§4(e) *frame*-length listing of the
> `0x41` START class as an SCA message class. It has been deleted from
> `scs_credit_header_offset()`, not relabelled. Note this makes the §4(c) table
> row "70, 110, 94, 62, 58, 66, 106, 86" and the §4(m) list a mix of two
> markers; only the `0x?B13` members are SCS messages.
>
> Field map, evidence and the reader/stamper:
> `src/vmsscs/include/scs_credit.h`. **OVMX stamps a live credit on the wire as
> of `vms-aa1`**, on outbound MTYPE-10 (application message) frames only: the
> connection's Pending Receive Credit, debited and reset per p. 2-44, written at
> the choke point `send_frame_vc()`. Every other MTYPE is transmitted with the
> builder's own bytes at `[48:50]`, because there the field is a different
> quantity (extension count for 0/2; 0 for 5/7; an unnamed constant 1 for 8/9).
> Kill switch `OVMX_NO_CREDIT_ACCOUNTING=1` restores the pre-`vms-aa1` bytes.

**Vote / quorum — GROUNDED NEGATIVE RESULT** (the vote-varying capture
recommended below was **done** for `vms-cd0`, subsuming `vms-41d`). The joiner
was rebooted with `VOTES 0` (`cd0-bootB`) and then `VOTES 2` (`cd0-bootC`),
everything else identical (SCSNODE `"ZK"`, SCSSYSTEMID 1099). **No byte of the
phase-2 0x41 START/config body cleanly tracks the node's votes.** Two independent
lines of evidence:

1. The golden capture already carried a vote contrast — VAX1 `VOTES 1` vs VAX2
   `VOTES 0` — and the full byte-diff of their 0x41 bodies shows **no
   vote-isolated byte** (only SCSSYSTEMID [46], node-name char [93], and the
   per-boot tokens differ).
2. In `cd0-bootC` the joiner's `VOTES 0→2` change *appeared* to flip payload
   byte [22] `0x01→0x02` — but this is a **false positive**: byte [22] is a
   per-node SCS sequence counter (§4d region), and in the *same* Boot-C cluster
   the VAX1-sourced 0x41 frames carry `[22]=0x01` while the ZK-sourced frames
   carry `[22]=0x02` for the **same** cluster quorum. Ground truth from
   `F$GETSYI` on VAX1 during Boot C: `QUORUM=1`, `EXPECTED_VOTES=1` (VMS held
   expected-votes at the configured 1), so quorum was `1` in **every** captured
   config — byte [22]'s value is therefore uncorrelated with quorum too.

**Conclusion:** the connection manager unquestionably reconciles votes/quorum at
membership time (VAX1's cluster state reflects ZK's configured votes), but it is
**not carried as a locatable field in the phase-2 START/config body** — it is
exchanged later, most plausibly in the post-connect 190-byte `VMS$VAXcluster`
sequenced-message VC traffic (§4d), whose SYSAP body is not yet decoded. For
OVMX this means the START/config response does **not** need to encode a vote
field; votes are negotiated on the established VC. **GROUNDED (negative):
vote/quorum absent from the 0x41 body, validated across 4 node-vote
configurations {1,0,0,2} with quorum held at 1.**

**The join nonce and the credential question — the central finding.**
The 4-byte join nonce (§4a offset 68, abs; payload [54:58] of the discovery
header) was observed as the single value **`ee05395b`** on *every* directed
frame that carries it:

- `formation-ci1-joinwindow.pcap`: 3/3 directed HELLOs → `ee05395b`
- `formation-ci1.pcap`: 59/59 directed HELLOs → `ee05395b`
- `ci3-join-membership-live2node-20260728.pcap` (a **completely fresh boot**,
  captured this session on an independently re-booted cluster): 20/20 directed
  HELLOs → `ee05395b`
- and the diskless-boot SOLICIT (§4c): `ee05395b`

Because the identical value survives a full cluster reboot, the nonce is
**derived from the persistent cluster credential** (the cluster group number +
password stored hashed in `CLUSTER_AUTHORIZE.DAT`, per the public SYSMAN
`CONFIGURATION SHOW CLUSTER_AUTHORIZATION` documentation, §3) — it is **not** a
per-session or per-boot random challenge. **GROUNDED (presence + cross-boot
stability, 82/82 directed-nonce frames across three independent captures).**

**What is NOT grounded — the derivation.** How `(group#, password)` maps to
`ee05395b` is **unknown and not derivable from passive capture.** Only one
credential (group 1, the lab password) was ever on the wire, so there is no
input/output contrast; and the transform is a one-way hash of the cluster
password (documented conceptually as a hashed `CLUSTER_AUTHORIZE.DAT` secret),
which cannot be inverted from observed outputs. **This means OVMX can, for a
*known* cluster, replay a captured nonce to interoperate on the lab wire, but
a lab-replay is NOT a general credential implementation** — an OVMX node that
must join an *arbitrary* cluster needs the documented `CLUSTER_AUTHORIZE` hash
algorithm, and that algorithm is not present in any wire byte. Deriving it by
varying the password on the lab and correlating outputs is clean-room-legal
but was out of scope here (it mutates `CLUSTER_AUTHORIZE.DAT` on the shared
disk) and, being a hash, would at best yield correlation points, not the
algorithm. **Recommended follow-up:** locate the hash construction in public
OpenVMS security/cluster documentation (not on the wire); until then, treat
nonce handling as *observe-and-replay for a known cluster only*.

### 4(h) SCS$DIRECTORY connect + directory-lookup (0x5b) + credit-return (0x48)

This subsection grounds the frame BODIES the joiner runs *between* the phase-2
0x41 START (§4g phase 2) and the phase-4 0x4b `VMS$VAXcluster` connect (§4g
phase 4): the SCS$DIRECTORY SYSAP connection + name-resolution exchange (opcode
`0x5b`, `0x7b` is its retransmit) and the per-message credit-return short
(opcode `0x48`). Captured for `vms-560`. **Specimen:**
`formation-ci1-joinwindow.pcap`, the golden VAX2-joins-VAX1 handshake — the
directory phase is SCA frames 21–31 (raw `--frame` 29–39, per the §4g +8 raw
offset) interleaved with the credit shorts 22–32, and every structural claim
was re-validated byte-for-byte on the independent full run `formation-ci1.pcap`
(different session; 605 `0x5b` frames + 622 `0x48` frames). All offsets are
**payload-relative** (payload byte 0 = abs frame offset 14; add 14 for
absolute).

**The SCS envelope is one shape.** `0x5b` and `0x48` reuse the same
[0:18] envelope as §4d/§4g — length [0:2], dst-logical [2:8], connect-flag
`0x0001` [8:10], src-logical [10:16], opcode [16], format constant `0x13` [17]
(GROUNDED 605/605 for `0x5b`, 622/622 for `0x48` in the full run, 0 residuals) —
and the same SCS sequenced-message counter region beginning at [18].

**(1) The `0x5b` connection-handle pair is at [50:58] — the SAME offsets as
§4g phase-4 and §4d.** The directory exchange opens with a full SYSAP
connect handshake for `SCS$DIRECTORY`, structurally identical to the
`VMS$VAXcluster` connect: a remote/local connection-handle pair sits at
**remote [50:54], local [54:58]** (LE `uint32`), and it fills by the same
`remote = 0 → learned` admission act (spec §4g phase 4). Directly observed
sequence:

| SCA | Dir | remote [50:54] | local [54:58] | Reading |
|---|---|---|---|---|
| 21 | V1→V2 | `0x00000000` | `0x63050008` | CONNECT-REQUEST: VAX1 offers its `SCS$DIRECTORY` handle, remote still zero |
| 23 | V2→V1 | `0x63050008` | `0x00000000` | VAX2 echoes VAX1's handle, its own not yet assigned |
| 25 | V2→V1 | `0x63050008` | `0x33590007` | CONNECT-RESPONSE: VAX2 supplies its own handle — pair now bound |
| 27 | V1→V2 | `0x33590007` | `0x63050008` | both endpoints carry the bound pair (swapped by direction) thereafter |

**GROUNDED (mechanism + offsets):** the handle location [50:58] and the
`remote = 0 → filled → swaps-with-direction` binding reproduce §4g phase-4
byte-for-byte. The specific values `0x63050008` (VAX1) / `0x33590007` (VAX2) are
**not** in the §3 decoder ring because that ring is an *idle-state* SDA snapshot
that only lists `SCS$DIRECTORY` in its **listen** CDT (`62C50000`); a formed
`SCS$DIRECTORY` connection allocates a new open Con.ID exactly as
`VMS$VAXcluster` listen `62C50003` becomes connected `62C50009` (§3). So the
handle *identity* is **inferred** (the dynamically-allocated `SCS$DIRECTORY`
CDT Con.IDs), the handle *binding* is **GROUNDED**.

**(1a) [46:48] IS THE SCA CONNECTION-CONTROL MESSAGE TYPE — GROUNDED (`vms-dd5`),
and it corrects this section.** The four-frame handshake tabulated in (1) is not
peculiar to `SCS$DIRECTORY`, and [46:48] is not a counter. Measured over
`formation-ci1.pcap` (the full independent run), restricting to the
connection-control length classes 110/66/62 and excluding the application value
`10`: **60 frames, 16 complete connection dialogues, 0 residuals.**

| [46:48] | Frames | Length | Con.ID pair | Reading |
|---|---|---|---|---|
| `0` | 16 | 110 | remote `0`, local supplied, SYSAP name present | **CONNECT_REQ** |
| `1` | 16 | 66 | remote echoed, local still `0` | **CONNECT_RSP** |
| `2` | 6 | 110 | both filled, SYSAP name present | **ACCEPT_REQ** |
| `3` | 6 | 62 | both bound, original direction | **ACCEPT_RSP** |
| `4` | 10 | 62 | both bound | **REJECT_REQ** (see below) |
| `6` | 6 | 62 | both bound, matched pairs both directions | **DISCONNECT_REQ** (see below) |

**Why 0/1/2/3 are GROUNDED, not merely labelled.** The counts pair exactly —
16 CONNECT_REQ, 16 CONNECT_RSP, and each dialogue terminated by exactly one of
6 ACCEPT_REQ or 10 REJECT_REQ (**6 + 10 = 16, with zero Con.IDs receiving
both**) — and each value's Con.ID fill pattern is what *VAXcluster Principles*
Figure 2-14 requires of that message and of no other: `0` carries destination
Con.ID 0 because the target's CDT does not exist yet (p. 2-28: the CONIDs are
exchanged *during* formation); `1` echoes it while leaving its own at 0, which
is an acknowledgement that structurally *cannot* be the accept; `2` supplies the
responder's own handle, the admission act; `3` carries the bound pair back in
the original direction. Five SYSAPs exercise the same four values in the same
order in a single capture — `SCS$DIRECTORY` (×3), `VMS$VAXcluster`,
`MSCP$DISK`, `VMS$DISK_CL_DRVR`, `SCA$TRANSPORT` — so the pattern is a property
of SCA, not of one SYSAP. A per-dialogue counter is **REFUTED** by two
observations: the `MSCP$DISK` dialogues run `0,1,4` (a counter cannot skip to 4
as the third message), and the `SCS$DIRECTORY` connection runs
`0,1,2,3,10,10,…,6,6` (a counter cannot go back down to 6 after 10).

**`4` = REJECT_REQ — strongly grounded, one caveat.** All 10 occurrences follow
a CONNECT_RSP and terminate the dialogue with no ACCEPT_REQ ever sent, and the
partition against ACCEPT_REQ is exact (no Con.ID gets both). All 10 are the
target refusing `MSCP$DISK` — precisely Figure 2-15. **Caveat, stated:** the
`4` frame carries the *responder's own* Con.ID, which a pure rejection does not
obviously need. The label is the best reading of a decisive behavioural
partition, not a decoded field.

**And its 16-bit reason code (p. 2-26) is NOT located.** Both `4` and `6` are
supposed to carry an optional reason code. Measured over the two carrier
populations with `tools/cluster/scs_reason_measure.py` — the two 16-bit words
that follow the Con.ID pair at `[50:58]`:

<!-- CENSUS-A: parsed by tests/vmsscs/test_scs_reason_figures.py. One digit per
     figure, nowhere else in this section. Re-run the script; do not hand-edit. -->

| msgtype | frames | pcaps | payload `[58:60]` | payload `[60:62]` |
|---|---|---|---|---|
| `4` REJECT_REQ | 453 | 19 | `0x0000` × 453 | `0x0001` × 453 |
| `6` DISCONNECT_REQ | 220 | 25 | `0x0000` × 220 | `0x0000` × 131, `0x0001` × 89 |

So `[58:60]` never carries a nonzero value in either frame — and `[60:62]`,
which **an earlier revision of this paragraph wrongly described as invariant**,
*varies* on DISCONNECT_REQ. It is a live field with an undecoded meaning, not a
reason code we can read. SDA's `Rej/Disconn Reason` likewise reads zero on every
CDT. With no nonzero reason code anywhere in the data, the offset cannot be
derived from anything we hold. See the `vms-6b3` entry in §5 for the neighbour
census — which shows `[58:60]` is *not* dead space across the envelope — and for
the LABELED OVMX placement that stands in for the undecoded offset.

**`6` = DISCONNECT — plausible, NOT grounded.** All 6 occurrences are on
connections that completed `0,1,2,3` and finished their work, and they appear in
matched pairs one per direction, which is Figure 2-16's matched
`DISCONNECT_REQ`. **THIS PARAGRAPH IS SUPERSEDED BY §4(h)(1b)
(`vms-591`), which supplies the decisive part it lacked:** across all 47
captures a type-`6` frame is answered by a type-`7` frame 262 times and by
nothing else. Do not quote the "plausible" verdict above without it.

**(1b) `5` AND `7` ARE ON OUR WIRE, AND THIS SECTION SAID THEY WERE NOT —
CORRECTED BY `vms-591`.**

The claim this replaces, REFUTED by the census below and kept only so a reader
who saw the old text recognises what went:

<!-- REFUTED-QUOTE-BEGIN -->
> REFUTED by §4(h)(1b):
> ~~"`5` and `7` DO NOT EXIST ON OUR WIRE … Do not build a `5` or `7` frame."~~
<!-- REFUTED-QUOTE-END -->

**Why it survived three revisions: a sampling error, not a decoding error.**
Every census of [46:48] had been restricted to the SCA length classes
**{62, 66, 110}**, because those are the classes the four *formation* messages
occupy — the restriction is written into the table above and into
`tools/cluster/scs_reason_measure.py`'s `CONNCTL_CLASSES`. **Both RESPONSE
messages are shorter than all three.** They are **58 bytes**: the envelope, the
message type and the Con.ID pair, and nothing else. They fell outside the filter
that was looking for them.

Re-measured with no length restriction, over all 47 captures, pairing each
connection-control frame with **the first frame in the reverse direction whose
Con.ID pair is this frame's pair with the two handles swapped**:

<!-- CENSUS-D: parsed by tests/vmsscs/test_scs_disc_figures.py. One line per
     figure, nowhere else in this document. Re-run
     tools/cluster/scs_disc_measure.py; do not hand-edit. -->

| request | | response | | frames | pcaps |
|---|---|---|---|---|---|
| 110 B | `0` CONNECT_REQ | 66 B | `1` CONNECT_RSP | 1115 | 34 |
| 110 B | `2` ACCEPT_REQ | 62 B | `3` ACCEPT_RSP | 381 | 33 |
| 62 B | `4` REJECT_REQ | 58 B | `5` **REJECT_RSP** | 696 | 26 |
| 62 B | `6` DISCONNECT_REQ | 58 B | `7` **DISCONNECT_RSP** | 262 | 25 |

All four SCA request/response pairs, in figure order. No other
(request → response) combination occurs for any of these four request classes.

**This upgrades `6` from "plausible" to grounded, and it grounds `7`.** `6` is
now supported by the same kind of decisive behavioural partition that carried
`4`: it is answered, 262 times in 25 captures, by a distinct message type that
answers nothing else. And `4` gains its response half on the identical
argument. The `5`/`7` labels themselves still rest on **figure order** — the
book draws REJECT_RSP after REJECT_REQ and DISCONNECT_RSP after DISCONNECT_REQ,
and the values land exactly there — which is an inference, not a decoded field
name. **State it that way and no more strongly.**

**A COMPLETE TEARDOWN, frame by frame.** `formation-ci1.pcap` raw frames
61/63/64/65 (SCA #53/#55/#56/#57), between VAX1 (Con.ID `63050008`) and VAX2
(`33590007`) — Figure 2-16 end to end, both halves:

| SCA # | direction | class | msgtype | `[60:62]` |
|---|---|---|---|---|
| 53 | VAX1 → VAX2 | 62 B | `6` DISCONNECT_REQ | `0x0000` |
| 55 | VAX2 → VAX1 | 58 B | `7` DISCONNECT_RSP | — |
| 56 | VAX2 → VAX1 | 62 B | `6` DISCONNECT_REQ | `0x0001` |
| 57 | VAX1 → VAX2 | 58 B | `7` DISCONNECT_RSP | — |

**AND `[60:62]` IS DECODED — the MATCHING flag (`vms-591`).** This section
recorded it above as "a live field with an undecoded meaning". Ranking each
VMS-origin DISCONNECT_REQ within its own Con.ID-pair dialogue partitions it
exactly, with **zero residuals** over all 220 frames:

<!-- CENSUS-E: parsed by tests/vmsscs/test_scs_disc_figures.py. -->

| rank within the dialogue | `[60:62]` | frames | pcaps |
|---|---|---|---|
| 0 — the first DISCONNECT_REQ on the pair | `0x0000` | 131 | 25 |
| 1 — the matching one, from the other end | `0x0001` | 89 | 18 |

No frame of either rank carries the other value and no pair carries a third
DISCONNECT_REQ. (131 ≠ 89 because 42 dialogues' matching half falls outside the
capture window.) So the field distinguishes *initiating* a disconnect from
*matching* one, which is Figure 2-16's DISC SENT vs DISC MATCH. **Same honesty
bound as `4`:** this is the best reading of a decisive behavioural partition,
not a decoded field; a peer that ignored the flag would look identical here.

**A NEW GAP THIS OPENS, recorded rather than smoothed over.** The same
unrestricted census finds message types **`8` and `9`** in the 58-byte class,
paired the same way — 131 frames each, in 25 captures — and positioned inside
the connection lifetime between the ACCEPT_RSP and the DISCONNECT_REQ (the
observed order on a full connection is `0,1,2,3,…,8,9,6,7`). **They are not
named here.** *VAXcluster Principles* ch. 2 draws eight connection-control
messages and these are a ninth and tenth; nothing we hold says what they are.
See §5.

**What OVMX now builds on this section:** `6` and `7`, from byte-exact captured
templates, in `src/vmsscs/scs_disc.c` (`vms-591`). Not `5` — OVMX has no
production REJECT caller to answer, so a REJECT_RSP builder would be untested
code.

**(1b) THE WIDER CORPUS CLOSES THE 5/7 GAP AND IDENTIFIES 10 — vms-54f,
2026-08-05.** Re-measured over the full lab-1 corpus (163 pcaps:
`cluster/work/`, `cluster/captures/`, `clean-cluster/captures/`; ad-hoc census,
mixed sources — per-population claims still owed to the OUI-rule split; offsets
below content-relative per the §0 erratum):

- **The envelope unifies across every length class.** Every SCS message —
  the short classes here, the 94-content MSCP commands, and the 190-content
  §4(d)/§4(j) class — carries inner-length [42:44] (= content length − 44),
  constant `0x0004` [44:46], **message type [46:48]**, credit [48:50], handle
  pair [50:58]. The 190-content class is uniformly type **10** with inner
  length 146 (173,927/173,927 in the sampled `work/` corpus): the "SCS
  sequence-region" reading of §4(d) for those bytes is superseded — they are
  this same header.
- **Resolution (iii) was correct: `5` and `7` exist.** 58-content class:
  type `7` 988× against 986 `DISCONNECT_REQ` (type 6) — 1:1; type `5` 4,536×
  against 4,654 `REJECT_REQ` (type 4). The 58-content class is the short
  response/control class: envelope + handle pair, no payload (inner length 14).
  The earlier "absent in 18,541 frames" finding was a property of the single
  `formation-ci1.pcap` corpus, not of VMS.
- **Type `10` = the SCS "application message" MTYPE — IDENTIFIED.**
  *VAXcluster Principles* p. 4-13 defines the three-way MTYPE taxonomy
  (application message / application datagram / SCS control message) and
  Figure 4-5 (p. 4-14) shows an MSCP command nested under the SCS header
  `CREDIT — SCS MTYPE, DEST CONID, SRC CONID`; p. 4-15 grounds dispatch-on-
  MTYPE into the CDT message-input routine. On our wire: the golden
  94-content MSCP command frames (`af2-firsttimer-established-20260728.pcap`,
  112 frames, identity-proven real-VAX) carry type 10, and the 110-content
  class partitions exactly {0 CONNECT_REQ, 2 ACCEPT_REQ, 10 application
  message}. This closes the vms-ecff identification: type 10 is not a tenth
  connection-control message — it is the carrier of *all* SYSAP payloads.
- **Types `8`/`9` — REGISTERED, still unnamed.** 58-content class, paired
  request/response on established connections (8 from A handles (X,Y), 9 from
  B handles swapped), envelope-only, credit field = 1 in every inspected
  exemplar; observed immediately preceding teardown
  (`work/control-vax3-late.pcap` frames 5297–5302: `8 → 9 → 6 → 7` on one
  handle pair); real-VAX-sourced instances exist. Candidate (NOT grounded, do
  not name): the credit-flow pair around the p. 2-44 "special credit message"
  (vms-1d2). Decisive experiment and full observations:
  `docs/design-mscp-direction.md` §1.3.

**(1c) CREDIT-FIELD MEASUREMENT — one candidate confirmed structurally, one
WEAKENED, one REFUTED (`vms-54f`, 2026-08-05).** *VAXcluster Principles*
p. 4-68 (the SSP section) enumerates **four** SCS message types — "datagrams,
regular messages, **special credit messages**, and node name packets" — and
states a testable rule: "This field must contain a 0 in datagram packets since
messages must be used to extend send credits." Measured over the full corpus,
**real-VAX sources only** (OUI `08:00:2b` + DECnet-logical `aa:00:04`), credit
field at content `[48:50]`:

| Type | n | credit == 0 | credit values seen |
|---|---|---|---|
| 0 (CONNECT_REQ) | 6 081 | 0 (0.0%) | 3, 6, 10 |
| 2 (ACCEPT_REQ) | 1 109 | 0 (0.0%) | 1, 6, 8, 10 |
| **5** | **4 523** | **4 523 (100%)** | **0 only** |
| **7** | **734** | **734 (100%)** | **0 only** |
| 8 | 608 | 0 (0.0%) | **1 only** |
| 9 | 247 | 0 (0.0%) | **1 only** |
| 10 (application msg) | 12 759 | 0 (0.0%) | **1 only** |

- **Types 5 and 7 partition perfectly on credit==0** against every other type,
  which is what p. 4-68's rule predicts of a class that does not extend credit.
  This is an independent structural corroboration that 5/7 are the *response*
  halves (a response acknowledges; it does not extend buffers). Recorded as an
  observation — the p. 4-68 rule is stated for SSP ports, and we do not assert
  it verbatim for the LAN/NISCA path.
- **The 8/9 = "special credit message" candidate is WEAKENED, not confirmed.**
  p. 2-44 requires a special credit message to *carry the local Pending Receive
  Credit count* — a quantity that varies with how many buffers were released.
  Types 8 and 9 carry a **constant 1** across 855 real-VAX frames, identical to
  ordinary application messages (type 10, constant 1). A constant is not a
  pending count. Do not name 8/9 as credit messages on present evidence; the
  §1.3 engineered one-way-flow experiment is still the way to settle it, and it
  should now also look for a *varying* credit value as the signature.
- **REFUTED for `vms-7e7`:** the `0x4b13`/`0x5b13` msgtype words at content
  `[16:18]` are **not** the message-vs-datagram distinction. If they were, one
  of them would carry credit 0 throughout; instead both carry mostly-nonzero
  credit at similar rates (`0x4b13`: 24.2% zero of 864 193; `0x5b13`: 29.9%
  zero of 66 096). That is a fourth candidate rule killed for vms-7e7's
  lookup-response msgtype question.

**(1d) CORRECTION — the 70-content class does NOT share this envelope.** An
earlier scratch census for this item printed "types 1..22" for the 70-content
class by reading `[46:48]` there. That reading is **wrong**: in the 70-content
class `[42:44]` is not an inner length (observed 9,10,11,12,13 — never 26) and
`[44:46]` is not the `0x0004` format word (observed `522f`, `532f`, `2abe`, …).
The envelope in (1b) is asserted for the 58/62/66/110-content connection
classes, the 94-content MSCP class and the 190-content class **only**. The
70-content class is a different shape and remains undecoded. Do not extend the
(1b) offsets to it.

**(2) SCS$DIR_LOOKUP body — name resolution with a grounded negative marker.**
Past the handle pair the body carries fixed-position, blank-padded ASCII SYSAP
name fields beginning at [62]. Two observed shapes, selected by the field at
**[46:48]**, which this section previously called a "per-dialogue message
counter" — **that reading is REFUTED, see §4(h)(1a) immediately below**; it is
the SCA connection-control **message type**. A companion flag/status word sits
at [48:50] and remains inferred.

- **connect frame** (SCA 21): target SYSAP `"SCS$DIRECTORY   "` (16-byte field
  [62:78]) + operation `"SCS$DIR_LOOKUP"` (blank-padded, [78:]).
- **lookup req/resp** (SCA 29/31): queried name `"MSCP$TAPE       "` (16-byte
  [62:78]) + a 16-byte **result field [78:94]**: all-zero in the request, and
  the literal ASCII **`"NOT PRESENT HERE"`** in the negative response.

**The 16-byte name field at [62:78] is also the CONNECT_REQ's TARGET SYSAP
name**, which is what `vms-7fe` scans the p. 2-48 SDIR queue with
(`scs_sdir_target_name()`): the connect frame row immediately below records
SCA 21 carrying `"SCS$DIRECTORY   "` there, and the 110-byte `[46:48] == 0`
CONNECT_REQ class of §4(h)(1a) carries the target name in the same field.

**GROUNDED (directly observed ASCII):** the queried SYSAP name and, decisively,
the `"NOT PRESENT HERE"` result string that signals a negative resolution — the
same string §4c reported but now pinned to the [78:94] result field. The exact
byte width of each name field varies by operation (the operation name in the
connect frame runs longer than 16 bytes), so field *widths* are reported
as-observed, not asserted as a fixed schema; the *presence, position, and
negative-marker semantics* are grounded.

**(2a) THE ASK SIDE — the two REQUEST frames, and what the reference VAX does
with them (GROUNDED, `vms-66f`).** Everything §4h recorded before this entry was
a frame OVMX *answers*. The SCS Process Poller of *VAXcluster Principles* p. 2-50
(SYSAP name `SCS$DIR_LOOKUP`) has to *ask*, so both request classes were dumped
byte-for-byte out of `formation-ci1-joinwindow.pcap` and are now baked into
`src/vmsscs/scs_dir.c`:

| Frame | raw pcap | SCA | class | key fields |
|---|---|---|---|---|
| `SCS$DIRECTORY` CONNECT_REQ | 29 | 21 | 110 B | `[46:48]=0`, remote `0`, local `0x63050008` offered, `[48:50]=3` |
| lookup REQUEST | 37 | 29 | 94 B | `[46:48]=0x0a`, `[48:50]=0`, `[58:62]=0` marker, result `[78:94]` all-zero |

*(Raw pcap indices in this section are **0-based** record indices, counting every
record in the file including non-SCA ones. Wireshark/tcpdump frame numbers are
these **plus one** — the same two frames are 30 and 38 there. Stated because a
review of this section cited the 1-based numbers and the two sets do not
overlap by accident.)*

**`[48:50]` IN BOTH ROWS IS THE GROUNDED SCA CREDIT FIELD, NOT A FLAG — and the
capture refutes reading it as a request/response discriminator (`vms-66f`,
corrected).** §4(d)'s ⭐ block pins `[48:50]` as the piggybacked credit for the
SCS message classes 58/62/66/86/94/110/190, and line 3 of that grounding names
`SCS$DIRECTORY` = 3 / `SCS$DIR_LOOKUP` = 1 among the extended Send Credits in
the 110-byte `CONNECT_REQ` class — which is exactly the `3` in row 1 above. On
the 94-byte lookup class the same field is the ordinary piggybacked credit, and
the census below is the refutation of the "flag" reading. Selecting **by
connection identity, not by SCA length** (the Con.ID pair of each observed
`SCS$DIRECTORY` connection — see `tools/cluster/scs_dir_role_measure.py`, and
`vms-c11` on why length-keyed censuses in this epic are not trusted), the
capture holds **12** lookup messages, all of length 94, split by the grounded
`[58:62]` marker into 6 REQUESTs and 6 RESPONSEs:

| side | `[48:50]` histogram | frames |
|---|---|---|
| REQUEST (`[58:62]=0`) | `{0: 2, 1: 4}` | `0` at 37, 1244; `1` at 41, 43, 45, 1248 |
| RESPONSE (`[58:62]=1`) | `{1: 6}` | 39, 42, 44, 46, 1247, 1250 |

So `[48:50]` is **not** constant in requests and **does not** separate requests
from responses: the value `1` appears on four requests and all six responses.
The only structure the capture *does* show is positional — the two zeros are
exactly the FIRST lookup message on each of the two directory connections (37
opens VAX1's, 1244 opens VAX2's) — and **that is a correlation over n=2, not a
grounded rule**. Which condition sets the field to 0 rather than 1 (no credit
owed yet, a credit already returned by the intervening `0x48` short, or
something else) is **NOT separated by this capture** and is recorded as §4h gap (f).
What *is* grounded is the negative: the "request/response flag" reading is dead.

**Why OVMX pins 0 anyway, and what is NOT grounded.** `dir_lookupreq_tmpl` in
`src/vmsscs/scs_dir.c` is a byte-exact replay of SCA 29 (raw 37), the first
lookup on its connection, so its `[48:50]` is 0 for the same reason the wire's
is. OVMX **does not stamp a live credit on a directory inquiry** — `vms-aa1` wired
the p. 2-44 piggyback for MTYPE 10 (application messages) ONLY, and these
inquiries are not that class — so every inquiry OVMX sends still carries 0
whether it is the first on the connection or the fourth. That is a KNOWN DEVIATION from the
reference wire, recorded as §4h gap (f), and it is one of the unseparated
candidates for the unanswered-inquiry gap below.

**The two 16-byte name fields are (DESTINATION SYSAP, SOURCE SYSAP), and the
request/response PAIR is what grounds it.** SCA 21 carries
`[62:78]="SCS$DIRECTORY   "` then `[78:94]="SCS$DIR_LOOKUP  "`; SCA 25, the
answer travelling the other way, carries exactly those two strings **swapped**.
A pair that swaps with direction is an endpoint pair. The alternative reading —
a fixed "target, operation" schema — is **REFUTED** by SCA 25, since
`SCS$DIR_LOOKUP` is not an operation performed on `SCS$DIRECTORY`. This confirms
`scs_sdir_target_name()`'s use of `[62:78]` on the 110-byte CONNECT_REQ class.

**WHAT THE REFERENCE VAX DID WHEN OVMX ASKED — measured, and it is half good
news.** OVMX drove its poller against VAX1 (VAX 7.3, lab-2 replica `vaxlab-1`,
2026-08-05, 8 cycles over 60 s, `PRCPOLINTERVAL=8`):

- **The VAX ACCEPTS an OVMX-initiated `SCS$DIR_LOOKUP → SCS$DIRECTORY`
  connection.** It answered the CONNECT_REQ with a message-type-1 `CONNECT_RSP`
  and, on the second cycle, a message-type-2 `ACCEPT_REQ`, taking OVMX's CDT to
  OPEN through the §4(h)(1a) sequence. This is the first time OVMX has been the
  ACTIVE half of any SCA connection on the reference wire.
- **It did NOT answer the inquiry that followed.** Zero lookup RESPONSEs over 8
  cycles; the VAX instead retransmitted `CONNECT_RSP`. So the inquiry OVMX
  builds is not yet the inquiry the VAX will answer, and the reason is **not
  identified** — candidates not separated by this run are the Con.ID the inquiry
  addresses (`[50:54]`, which OVMX learned as `0`), the `[48:50]` **credit**
  (OVMX stamps a constant 0 there; the reference wire sends 1 on every inquiry
  after the first — §4h gap (f)), and the sequence state. **Recorded as an open
  gap, not as a working exchange.**
- **Enabling the poller COST THE JOIN.** Same binary, back-to-back, poller on
  vs. gated: on → `dir_connected=no dir_lookups=0 cm_config=no`, no join at all
  (the member never ran its own directory phase, `connect_scans=0`); gated →
  `dir_connected=YES dir_lookups=4 cm_config=YES joiner=OPEN`. OVMX's poller
  therefore ships **OFF by default** (`OVMX_PROCESS_POLLER=1` opts in,
  `OVMX_NO_PROCESS_POLLER=1` forces off) until the unanswered-inquiry gap is
  closed. Logs: `/data/training/vax/k8s-labs/vaxlab-1/logs/vms66f-{on,off}.log`.

**WHO IS THE ACTIVE HALF — corrected, because the first version of this
paragraph was refuted by the capture it cited (`vms-66f`).** The rejected text
said:

<!-- REFUTED-QUOTE-BEGIN -->
> (revision 1, quoted here only to kill it — every clause below is REFUTED)
> the directory exchange "runs in ONE direction", VAX2 "only answers", and VAX2
> "opens its own `VMS$VAXcluster` connection without having polled anybody".
<!-- REFUTED-QUOTE-END -->

**Both halves of that are false**, and the roles were inverted. Census
over the whole file, keyed on the `[62:94]` SYSAP name pair and the `[46:48]`
message type with **no SCA length filter**
(`tools/cluster/scs_dir_role_measure.py`; `vms-c11`):

| what | count | frames (0-based raw pcap) |
|---|---|---|
| `VMS$VAXcluster` ↔ `VMS$VAXcluster` CONNECT_REQ (`[46:48]=0`) | **1** | 47, **VAX1 → VAX2** |
| …of those, sent by the JOINER (VAX2) | **0** | — |
| `VMS$VAXcluster` ACCEPT_REQ (`[46:48]=2`) | **1** | 50, **VAX2 → VAX1** |
| `SCS$DIRECTORY` ← `SCS$DIR_LOOKUP` CONNECT_REQ (`[46:48]=0`) | **2** | 29, VAX1 → VAX2; **1237, VAX2 → VAX1** |

Read off that table:

- **The joiner never opens the `VMS$VAXcluster` connection.** There is exactly
  one `VMS$VAXcluster` CONNECT_REQ in the file and the **established member**
  sends it (47, VAX1 → VAX2). The joiner's frame 50 is `[46:48]=2`, an
  ACCEPT_REQ. The established member is the ACTIVE half of `VMS$VAXcluster`
  formation and the joiner is the PASSIVE half — the opposite of the rejected
  reading. Scope: **n=1 formation**, so this is what the reference wire did
  here, not a proven protocol rule about who may connect.
- **The joiner DOES poll.** Frame 1237 is VAX2 → VAX1, `[46:48]=0`, name pair
  `("SCS$DIRECTORY", "SCS$DIR_LOOKUP")` — VAX2 opening its own Process Poller
  connection — and its own lookup round follows on it (requests 1244/1248,
  answers 1247/1250, the second half of the §4(h)(2a) census above). The
  directory exchange is **bidirectional**; it just is not simultaneous.
- **What is true is only the ORDERING.** VAX2's poll is at t+33.804 s, 0.36 s
  *after* the `VMS$VAXcluster` connection formed at t+33.444 s (frames 47/50).
  So a joiner polls **after** it is in, not before — which still refutes gating
  a joiner's connect on a poller answer, but for a reason about *sequence*, not
  about *direction*. That was the only defensible part of the rejected
  paragraph and it is all that survives.

**(3) `0x48` credit-return short — the 41-byte body.** Every credit-return is a
fixed 41-byte SCA frame (Ethernet-padded to 60). Its distinguishing feature vs.
a sequenced message: **[20:22] (send-seq) is 0** — a credit-return carries no
new sequence number of its own; it purely acknowledges. The acknowledged
sequence number (the sender's `recv_seq` = the peer's last `send_seq`) is
carried at **[18:20] and doubled at [26:28]** (GROUNDED: `[18:20] == [26:28]`,
**622/622** frames, 0 residuals), with a third repeat usually at [34:36]
(**616/622** — the §4d "up to 3× repeat" pattern; the 6 exceptions are
steady-state shorts on the established VC where [34:36] reads 0). Field map of
the clean archetype (SCA 26, an idle-directory credit-ack):

| Pay off | Size | Field | Grounding |
|---|---|---|---|
| 16 | 1 | opcode `0x48` | §4g partition (inferred label) |
| 17 | 1 | format constant `0x13` | GROUNDED (622/622) |
| 18 | 2 | **acknowledged sequence** (= receiver's `recv_seq`) | **GROUNDED**: pairs 1:1 with the peer's just-sent `send_seq` (see lockstep below) |
| 20 | 2 | send-seq — **`0x0000`** (credit-return emits no new seq) | **GROUNDED (622/622)** |
| 22 | 2 | constant `0x0001` | GROUNDED (622/622) |
| 24 | 2 | `0x0012` = 18 = SYSGEN `NISCS_LAN_OVRHD` | **GROUNDED (622/622)** |
| 26 | 2 | acknowledged-sequence mirror (== [18:20]) | **GROUNDED (622/622)** |
| 28 | 2 | zero | observed |
| 30 | 2 | secondary counter (sender's own outstanding seq) | inferred — small in the connect phase, large on the steady VC; not cleanly a single function of [18:20] |
| 32 | 2 | zero | observed |
| 34 | 2 | acknowledged-sequence 3rd repeat | GROUNDED (616/622; §4d triple-repeat) |
| 36 | 2 | zero | observed |
| 38 | 2 | constant `0x0001` | inferred (598/622; not fully constant) |
| 40 | 1 | zero pad | observed |

The **credit count is not a locatable multi-value field** in the 41-byte short:
the connection's Send/Recv credit is 10/8 (§3), but no `0x0a`/`0x08` byte tracks
it here. The observable credit behavior is **strict 1-for-1**: every sequenced
message is answered by exactly one `0x48` returning exactly one message's worth
of credit, which is what drives the tight per-message lockstep below. A bulk
credit-grant field, if one exists, is not on the wire in this class — reported
as an RE gap.

**(4) Seq/ack lockstep — grounded across the whole phase (extends §4g).** For
every sequenced message (`0x41`/`0x5b`/`0x4b`) the sender stamps
`send_seq` at [20:22] **mirrored byte-exact at [30:32]** (GROUNDED:
`[20:22] == [30:32]`, **17,758/17,758** such frames in the full run, 0
residuals) and its `recv_ack` (the peer's last `send_seq`) at [18:20]. Reading
each frame as `(recv_ack, send_seq)`, the golden phase advances in exact
lockstep (directly observed, SCA 33→44):

```
SCA33 V1→V2 (3,4)   SCA37 V1→V2 (5,6)   SCA41 V1→V2 (7,8)
SCA34 V2→V1 (4,4)   SCA38 V2→V1 (6,6)   SCA42 V2→V1 (7,8)
SCA35 V1→V2 (4,5)   SCA39 V1→V2 (6,7)   SCA43 V1→V2 (8,9)
SCA36 V2→V1 (5,5)   SCA40 V2→V1 (7,7)   SCA44 V2→V1 (8,9)
```

This reproduces the §4g phase-4 example `(5,6)(6,7)(7,8)(8,9)` / `(6,6)(7,7)…`
exactly and extends the grounding backward through the directory phase to the
first sequenced message. **The rule (GROUNDED mechanism):** a node holds
`send_seq` (its own next number) and `recv_seq` (highest peer `send_seq` seen);
a sequenced message stamps `send_seq` (+mirror) and `recv_ack = recv_seq`, then
increments `send_seq`; a `0x48` credit-return stamps `[18:20] = recv_seq` with
`send_seq = 0` (no advance). A VC engine can reproduce the exchange from this
state alone — no captured counter needs replaying. (This is exactly what the
`vms-21e` `scs_seq_state` machine in `src/vmsscs/scs_start.c` already tracks;
§4h grounds the `0x5b`/`0x48` classes it must also drive.) The precise
next-seq/last-ack CSB assignment (§3 `SHOW CLUSTER` triad) remains **inferred**,
same honesty caveat as §4d/§4g.

**(4a) SEQUENCE CONTINUITY — GROUNDED census over every capture we hold
(`vms-abc`).** §4h(4) grounds that a sender increments `send_seq` by one per new
sequenced message. `vms-abc` needed to know how often that is *violated* on the
wire, because OVMX now breaks a virtual circuit when it sees a gap (VAXcluster
Principles p. 2-31) and a detector that fires on healthy traffic would tear down
working joins. Re-derive with
`python3 tools/cluster/scs_seqgap_measure.py --all` (lab host; the captures are
host-only). The rule applied is exactly the one in
`src/vmsscs/scs_vc.c::scs_vc_check_recv_seq()`, restated in the script.

| Population | Value |
|---|---|
| pcaps scanned (every `.pcap` in the lab capture dir, 0 skipped) | 47 |
| sequenced messages examined (format `0x13`, `send_seq != 0`, `0x41` excluded) | **321,599** |
| duplicate / retransmit frames (`send_seq` at or behind `recv_seq`) | 506 |
| **gaps** (`send_seq` ahead by more than 1) | **41** |
| gaps whose source MAC is a VAX | **0** |
| gaps whose source MAC is `b6:16:8a:dc:3a:53` (OVMX) | **41** |

Per-VC counters are keyed on the ordered (src,dst) MAC pair, reset to 0 on any
`0x41` START in either direction (sec 4i.A: "the post-START SCS VC resets to
`send_seq = 1` on both sides"), and the FIRST sequenced message on a pair
ANCHORS the counter rather than being scored — a capture, like a node attaching
to a circuit already carrying traffic, cannot know what preceded the first frame
it sees. Without that anchor the same scan reports 147 "gaps"; the extra 106 are
all anchor cases (the first sequenced message seen on a VC, typically at a
capture that starts mid-stream on an established circuit, e.g. `recv_seq=0
send_seq=11142`). That 147 is a measurement artefact and is recorded here so
nobody re-derives it and reads it as wire loss.

**What this establishes:** on the RECEIVE side — the only side a port driver can
police — the reference wire never breaches sequentiality. `formation-ci1-
joinwindow.pcap` (the golden fresh join) contributes 2,960 sequenced messages
with 0 gaps, `formation-ci1.pcap` 17,760 with 0, and
`vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` 18,881 with 0.

**What it also exposes — an OVMX TRANSMIT defect, not fixed by `vms-abc`:** all
41 gaps are OVMX's own outbound frames, in three captures
(`vms246-scsdir-0x4b-reached`, `ovmx-e81-newcomer-ignores-us`,
`ovmx-e81-newcomer-refuses-60retx`). 40 of the 41 are one shape: OVMX emitting a
`0x5b` with `send_seq = 10` immediately after the member's round-0 `0x41` START
carried `send_seq = 10` — i.e. OVMX copying the member's *continuation*
`send_seq` into its own counter, which §4i.A states in as many words a joiner
must **not** do ("A correct joiner must not treat the member's `send_seq ≠ 1` as
an error, and must not copy it into its own `send_seq`/`recv_ack`"). Recorded
here as an open defect; it is a transmit-path bug and `vms-abc` is the
receive-side guarantee only.

**RE gaps left in §4h (honest):** (a) the [46:48] field is **GROUNDED as the
connection-control message type for values 0–3** (§4(h)(1a), `vms-dd5`), with
`4` and `6` supported by decisive behavioural partitions and `5` and `7`
grounded as their answers by the §4(h)(1b) pairing census (`vms-591`) but
LABELLED by figure order only; message types `8` and `9` in the same 58-byte
class are observed and **unidentified**. *(The clause that used to end this item
— "and the companion [48:50] flag remains inferred" — is **withdrawn**:
`[48:50]` is not a companion flag of `[46:48]`, it is the §4(d) credit field,
and reading it as a flag is refuted in §4(h)(2a), `vms-66f`.)*
(b) the `0x48` secondary counter [30:32] and the
early-phase shorts' non-zero residual at [30:40] (SCA 22/24 carry printable
leftover bytes) are not grounded to a field; (c) the affirmative
(non-`"NOT PRESENT HERE"`) lookup *result* encoding — the capture's directory
lookups that resolve carry the resolved SYSAP name back, but no separate
status/handle-return field was isolated; (d) the absolute `SCS$DIRECTORY`
connection Con.IDs are inferred (dynamically-allocated, absent from the
idle-state decoder ring). **(e) `vms-66f`: the reference VAX accepts an
OVMX-initiated directory connection but does not answer the lookup REQUEST that
follows — see §4(h)(2a). The request frame's bytes are a byte-exact replay of
SCA 29; what is NOT grounded is what makes the VAX answer one.** **(f)
`vms-66f`: on the 94-byte lookup class, WHAT SETS `[48:50]` TO 0 RATHER THAN 1
is not decoded. The field is the §4(d) credit (the "flag" reading is refuted,
§4(h)(2a)); over the 12 lookup messages in the golden capture the only two zeros
are the first message on each of the two directory connections, which is a
correlation over n=2, not a rule. Consequently — and this is the OVMX-side half
of the same gap — **OVMX stamps a constant 0 there on every inquiry it sends**,
because `vms-aa1` wired the live piggyback for MTYPE-10 application messages
only and a directory inquiry is not one (`scs_credit.h` reachability note). A reference exchange longer than one inquiry would show a `1` OVMX never
sends. Listed as a KNOWN DEVIATION and as one of the unseparated candidates
for (e).**

### 4(i) Joining an ALREADY-ESTABLISHED cluster (member-state-seq > 1)

All the §4g/§4h grounding above comes from **fresh 2-node formations**, where
both nodes boot into the cluster at about the same time and every node's
phase-2 `0x41` START carries `send_seq` (`[20:22]`) = **1**. OVMX's real target,
however, is a node joining a cluster that is **already up** — a running member
whose connection manager has already reconfigured one or more times, and/or that
already holds a *residual* connection block for the joining node from a prior
attempt. This subsection grounds what a joiner does differently in that case.
Captured for `vms-af2`.

There are **two distinct differences** between a fresh formation and a join into
an established member. They live in different fields, are driven by different
state, and only one of them is the join *gate*:

- **(A) member-side, informational — `send_seq[20:22]`:** the established
  member's first START carries a large `send_seq` (its prior VC's continuation),
  not 1. The joiner ignores this. (§4i.A below.)
- **(B) joiner-side, the GATE — incarnation counter `[22:24]`:** the joiner must
  stamp its `0x41` START `[22:24]` with the **node-incarnation number** the
  member advertises for it (1 for a first contact, 2 for the 2nd, …), derived
  from the member's directed-HELLO flag. Sending the wrong value stalls the
  join. (§4i.B below — this is the OVMX corrective.)

> **Correction to an earlier draft of this subsection.** A first pass reported
> "the joining node does nothing different — byte-identical to fresh." That was
> **wrong**: it missed field (B). The joiner's `[22:24]` *does* change (1→2→3
> across incarnations). The claim below is the corrected, experimentally-forced
> result.

**Method — controlled drop-and-rejoin + fresh-identity first-timer + repeated
rejoins (clean-room: reconfiguring our own lab).** A passive `br0` capture was
run while (i) VAX2 was cleanly `@SYS$SYSTEM:SHUTDOWN` **REMOVE_NODE**'d so it
left while **VAX1 stayed up** and reconfigured, then rebooted to **rejoin the
established VAX1**; (ii) a **fresh identity** VAX1 had never seen (`SCSNODE
"VX3"`, `SCSSYSTEMID 1050`, via `MC SYSGEN` on root `[SYS1]`) was booted as a
**first-timer** into the same established VAX1; and (iii) that VX3 identity was
dropped and rejoined **twice more** (2nd, 3rd incarnation) with VAX1 staying up
throughout. The SDA oracle (`SHOW CLUSTER` CLUB **Member State Seq. Num**) was
read at each transition and confirms establishment:

| transition | Member State Seq. Num |
|---|---|
| VAX1+VAX2 steady state | **0002** |
| VAX2 REMOVE_NODE departs | **0003** |
| VAX2 rejoins (established) | **0004** |
| … later, VAX2 departs again; VX3/1050 first-joins established VAX1 | 3 → **4** |

**Specimens:** `af2-established-rejoin-20260728.pcap` (VAX2 rejoin; 16 340 SCA
frames; `0x41` START = SCA 2850–2855) and
`af2-firsttimer-established-20260728.pcap` (VX3/1050 first-join + 2nd + 3rd
incarnations; 51 072 SCA frames; STARTs at SCA 2558–2563, 20170–20175,
33591–33596).

#### 4(i).A — member-side `send_seq` continuation (informational, NOT the gate)

The established member's **round-0** `0x41` START carries a large `send_seq`
instead of 1. Byte-diff of the round-0 START `[16:48]`, fresh vs. established
(payload-relative offsets):

```
FRESH   (formation-ci1-joinwindow SCA 23, VAX1->VAX2, round 0):
  41 13 00 00 01 00 01 00 12 00 00 00 00 00 01 00 ...  3e 00 00 00 01 04
                 ^^^^^ send_seq[20:22]=0x0001    ^^^^^ mirror[30:32]=0x0001
ESTAB.  (af2-established-rejoin  SCA 2851, VAX1->VAX2, round 0):
  41 13 00 00 c6 2e 01 00 12 00 00 00 00 00 c6 2e ...  3e 00 00 00 01 04
                 ^^^^^ send_seq[20:22]=0x2EC6=11974  ^^^^^ mirror[30:32]=0x2EC6
```

**GROUNDED:** this `send_seq` is the prior VC's continuation — VAX1's **last**
`send_seq` to VAX2 on the old VC was **11973** (`af2` SCA 2624); the round-0
rejoin START carries **11974 = 11973 + 1**. The member does not reset its
channel send-sequence to 1 for a node it has seen before; its round-1 START
(`af2` SCA 2852) then resets to `send_seq = 1`, and the post-START SCS VC
restarts from 1. The `[20:22] == [30:32]` mirror holds even at the elevated
value (6/6 af2 `0x41` frames). Confirmed independently on the VX3 rejoins: VAX1's
round-0 START `send_seq` = **6510** (2nd incarnation) and **5087** (3rd) —
each the residual continuation of the just-torn-down VC.

**The joiner ignores field (A).** Every joiner `0x41` frame carries
`send_seq = 1`, `recv_ack = 0`, `mirror = 1`; the config-round `[44:46]` walks
`0 → 1 → 2` regardless of the member's `send_seq`. A correct joiner must **not**
treat the member's `send_seq ≠ 1` as an error, and must **not** copy it into its
own `send_seq`/`recv_ack` — the START handshake is driven by the config-round,
and the post-START SCS VC resets to `send_seq = 1` on both sides and runs the
§4h lockstep byte-identical to fresh (`af2` SCA 2856→ reproduces
`formation-ci1-joinwindow` SCA 29→, 0 residuals). So field (A) is receive-side
tolerance only; it is **not** what a first pass thought — it is **not** the
join gate.

#### 4(i).B — joiner-side incarnation counter `[22:24]` (THE GATE) — GROUNDED

The field the joiner must set correctly is the second SCS counter in the START
body, **`[22:24]`** (payload-relative; the §4g/§4d "counter B"). It is **not** a
sequence counter here — it is the **node-incarnation number**, constant across
the joiner's three START frames, and it counts how many times this node has
(re)connected to this member:

| specimen (joiner) | joiner `0x41` `[22:24]` | member directed-HELLO flag `[78:80]` | member round-0 `send_seq` | cluster established? | result |
|---|---|---|---|---|---|
| fresh formation (VAX2) | **1** | 0x0001 | 1 | no (fresh) | join ✓ |
| OVMX-21e success (OVMX/1030) | **1** | 0x0001 | 1 | fresh channel | join ✓ |
| **VX3/1050 first-timer → established** | **1** | 0x0001 | 1 | **yes (seq 3)** | join ✓ |
| VAX2 established-rejoin (2nd) | **2** | 0x0002 | 11974 | yes | join ✓ |
| VX3/1050 2nd incarnation | **2** | 0x0002 | 6510 | yes | join ✓ |
| VX3/1050 3rd incarnation | **3** | 0x0003 | 5087 | yes | join ✓ |
| OVMX stall (`vms-691`, per orchestrator diff) | **1** (wrong) | (0x0002 advertised) | 2 | yes | **STALL** |

Two grounded conclusions from this table:

1. **It is NOT the cluster generation.** A genuine **first-timer** (VX3/1050,
   an `SCSSYSTEMID` VAX1 had never seen) joining a **confirmed-established**
   VAX1 (Member State Seq 3) sends `[22:24] = 1` — the fresh value — and the
   join **completes**. If `[22:24]` encoded the cluster's generation, a
   first-timer would have to present the established value; it does not. VAX1
   opened a **new** CSB (CSID `00010003`) for VX3 and its round-0 START to VX3
   carried `send_seq = 1` (no residual), confirming VAX1 treats a fresh
   `SCSSYSTEMID` as incarnation 1 no matter how established the cluster is.

2. **It is a per-node incarnation counter, and the member advertises it.** The
   same VX3/1050 identity, dropped and rejoined against an up-the-whole-time
   VAX1, sends `[22:24] = 1 → 2 → 3` across successive incarnations (byte-exact,
   e.g. VX3 3rd-incarnation START SCA 33592 `[22:24] = 03 00`). And the value
   the joiner stamps is **exactly the value the member advertised** in its
   **directed-HELLO flag at payload `[78:80]`** during the pre-START
   channel-formation exchange (§4b): VAX1's directed HELLOs to the joiner carry
   `[78:80]` = `0x0001 / 0x0002 / 0x0003` for the 1st / 2nd / 3rd incarnation
   (byte-exact, e.g. VX3 3rd directed HELLO SCA 33588 `[78:80] = 03 00`), and
   the joiner echoes that number into `0x41 [22:24]`. The joiner's *own*
   directed HELLO always carries `[78:80] = 0x0001`; it is the **member** that
   supplies the incarnation. (This refines §4b, where `[78:80]`≈abs-92 was
   labeled merely "directed-HELLO flag 0x0001": the "directed" value is in fact
   the incarnation counter, which is simply 1 in every fresh-formation
   specimen.)

The incarnation counter is *coupled* to field (A) — whenever the member holds a
residual VC (its `send_seq > 1`), it also advertises incarnation ≥ 2 — but the
gate is the incarnation echo in `[22:24]`, not the `send_seq`. The member's
`send_seq` is a large VC byte-count; the incarnation is a small 1,2,3 counter.

**Concrete answer for OVMX (the deliverable).** To join an established member,
OVMX must, during the pre-START directed-HELLO exchange, **read the member's
directed-HELLO flag at payload `[78:80]` (LE `uint16`) = N**, and stamp its
`0x41` START `[22:24] = N` (in all three START frames; the `[20:22]` send_seq
stays 1, `recv_ack` 0, config-round walks 0→1→2). For a genuine first contact
the member advertises `N = 1` and OVMX sends 1 (which is why the fresh-formation
and first-timer cases already work). OVMX's stall (`vms-691`) is because it
hard-coded `[22:24] = 1` while the member — holding a residual CSB for OVMX from
a prior aborted attempt — advertised `[78:80] = 0x0002` and expected
`[22:24] = 2`; the member will not advance its config-round past round 0 until
the joiner's incarnation echo matches. **GROUNDED** byte-exact across 6
specimens spanning `[22:24] ∈ {1,2,3}` with the member's `[78:80]` advertisement
matching 1-for-1, and the SDA oracle confirming admission (Member State Seq
2→3→4, new CSID per fresh identity). Field (A) receive-tolerance (§4i.A) is also
required, but it is *not* what distinguishes a stalling joiner from a succeeding
one — the incarnation echo is.

**RE gaps left in §4i (honest):** (a) the member's internal rule for *when* it
retires a residual CSB and resets the advertised incarnation back to 1 for a
given `SCSSYSTEMID` is not derivable from passive capture (across this run VAX1
never reset VX3/1050 below the running count); (b) the reason the member emits
exactly one round-0 START at the continued `send_seq` before resetting to 1
(§4i.A) is observed, not explained; (c) the upper bound / wrap behavior of the
incarnation counter is unprobed (grounded only for N ∈ {1,2,3}).

---

### 4(j) Connection-manager add-member transaction (the 190-byte VMS$VAXcluster SYSAP body)

This subsection grounds the **132-byte SYSAP body** (abs frame offset 72+,
payload-relative [58:]) of the 190-byte `VMS$VAXcluster`↔`VMS$VAXcluster`
message class (§4d) — the channel the connection manager uses to admit a joiner
as a full cluster **MEMBER** *after* the `0x4b` connect binds the VC (§4g phase
4). Everything through the `0x4b` CONNECT-RESPONSE (a bound Con.ID pair, §4g)
establishes only a *transient* SYSAP connection; the joiner does not appear in
SDA `SHOW CLUSTER` as a CSB, and the Member State Seq does not advance, until
this add-member dialogue completes. Captured for `vms-f85`.

> **Body-offset convention for this subsection.** All offsets here are
> **SYSAP-body-relative**: `body[0]` = payload offset 58 = **abs frame offset
> 72** (the first byte after the Local Connection-ID at §4d abs [68:72]). Add 72
> to get an absolute frame offset, or 58 to get a §4d payload-relative offset.
> The 132-byte body spans `body[0..131]` = abs [72..203] of the 204-byte frame.

**Specimens & method (pure passive analysis + the vms-cd0 controlled-reconfig
captures — no new lab mutation).** The transaction envelope was grounded on the
golden `formation-ci1-joinwindow.pcap` (VAX2 joins VAX1; 2902 `VMS$VAXcluster`
190-byte VC frames, isolated by the §3 Con.ID pair `{0x62C50009, 0x33580008}`)
and re-validated on the full run `formation-ci1.pcap`. The **VOTES** field was
grounded by byte-diffing the two `vms-cd0` vote-varying captures
(`cd0-bootB-zk1099-join` VOTES 0 vs `cd0-bootC-zk1099-votes2` VOTES 2, same
`ZK`/1099 joiner) and cross-validated against the golden (VAX1 VOTES 1 / VAX2
VOTES 0) and `af2-established-rejoin` — **five captures / four vote
configurations**. The **CSID** finding uses `af2-established-rejoin` (where the
rejoining node is assigned a *new* CSID `00010003`, SDA-confirmed §4i).

#### The SYSAP transaction envelope — GROUNDED field map

Every 190-byte `VMS$VAXcluster` VC frame carries this fixed 10-byte header at the
top of the SYSAP body, followed by an opcode-specific payload:

| body off | abs | Size | Field | Grounding |
|---|---|---|---|---|
| 0 | 72 | 2 | **SYSAP send-msg#** — the sender's own application-level message counter (distinct from the §4d/§4h SCS `send_seq` at abs [46:48]) | **GROUNDED**: strictly monotonic **per sender**, 2902/2902 golden VC frames (VAX1: 1,2,3,… independent of VAX2: 1,2,3,…). Starts at 1 on the first VC message. On the full run `formation-ci1.pcap` it stays monotonic across 17 541 VC frames with only 2 single-step (−1) retransmit dips — i.e. 17 539/17 541. |
| 2 | 74 | 2 | **SYSAP ack-msg#** — acknowledges the peer's highest send-msg# seen | **GROUNDED**: `ack-msg# ≤ peer's max send-msg#` in 2902/2902 frames; advances in lockstep with the peer's sends (application-layer analogue of the §4h SCS seq/ack). |
| 4 | 76 | 2 | **transaction number** — small per-dialogue id, **shared by a request and its matching response** | **GROUNDED**: see the req/resp correlation below. |
| 6 | 78 | 2 | **transaction checksum / correlation token** — a per-transaction value, **shared by a request and its response** | **GROUNDED as a correlation token** (17/17 responses match their request on `(txn, checksum, opcode)`); the *derivation* of the checksum from the transaction content is **unknown** (one-way, not recoverable from passive capture). |
| 8 | 80 | 1 | **message-category / flags byte** — bit `0x80` = **response**; the low bits select the category | **GROUNDED**: every response category pairs with its request (`0x01/0x81`, `0x02/0x82`, `0x05/0x85`, `0x06/0x86`). Categories observed: **`0x01`** = membership/config (the add-member dialogue, small count — 2× each opcode per join), **`0x02`** = steady-state DLM traffic (the 613+306+272+… bulk), **`0x04`** = credit/commit ack, plus `0x05`/`0x06`. |
| 9 | 81 | 1 | **opcode** (within the category) | **GROUNDED value↔role partition** (inferred numeric→name labels; no public SCS opcode table used). Category-`0x01` add-member opcodes: **`0x14`** node CPU/model advertisement, **`0x01`** cluster-parameters (carries VOTES + version), **`0x02`** config/topology, **`0x03`** membership-commit transaction, **`0x05`** lock/resource rebuild. Category-`0x04` acks carry opcode `0x49`/`0x00`/`0x01`/`0x02`. |

**Request/response correlation — GROUNDED.** Reading each frame as
`(category, opcode, txn, checksum)`, **every one of the 17 category-response
frames (`flag & 0x80`) in the golden VC matches a prior request frame with the
identical `(txn, checksum, opcode)` triple, 17/17, 0 residuals.** This is what
proves `body[4:8]` is a request/response correlation token and `body[8]` bit
`0x80` is the response direction.

#### The joiner's add-member SEND sequence — GROUNDED (the deliverable)

After the `0x4b` CONNECT-RESPONSE binds the VC (§4g phase 4), the 190-byte VC
opens with a **symmetric category-`0x01` config exchange** — each node sends
three messages advertising its own identity, then the *member* drives the commit
transactions and the *joiner* responds. Directly observed order (golden
SCA#45→70; reproduced in both `cd0` captures):

| # | Dir | body[8] | body[9] | Payload / role |
|---|---|---|---|---|
| 1 | J→M **and** M→J | `0x01` | `0x14` | **node CPU/model advertisement**: a length-prefixed ASCII string at `body[16]` (len `0x15`=21) + `"VAXserver 3900 Series"`. |
| 2 | J→M **and** M→J | `0x01` | `0x01` | **cluster-parameters**: the node's **VOTES** at `body[22:24]` (below) + software version `"V7.3"` (fixed 8-byte field further into the body). |
| 3 | J→M **and** M→J | `0x01` | `0x02` | **config/topology** (acks the peer's msg#2 via `ack-msg#`). |
| 4 | M→J | `0x04` | `0x49`/`0x00` | member **commit/credit ack**. |
| 5 | M→J req / J→M resp | `0x01`/`0x81` | `0x03` | **membership-commit transaction** (first `(txn,checksum)`-correlated exchange; member requests, joiner echoes the token in its `0x81` response). |
| 6 | M→J req / J→M resp | `0x01`/`0x81` | `0x05` | **lock/resource-database rebuild** transactions (member requests, joiner responds). |

The category-`0x01` dialogue occurs **once per join** (2× per opcode = the two
nodes); immediately afterward the VC transitions to the steady-state
category-`0x02` DLM traffic (§4d/§4f). The joiner's *active* contribution is the
three category-`0x01` config messages (opcodes `0x14`, `0x01`, `0x02`) that
carry its identity + votes, plus its `0x81` responses to the member-driven
`0x03`/`0x05` transactions; it advances its own `send-msg#`/`ack-msg#` in
lockstep (never copying the member's counters), exactly as the §4h SCS seq/ack
rule but at the SYSAP layer.

#### VOTES — GROUNDED across four configurations (resolves the §4g deferral)

§4g proved (grounded negative) that votes are **not** in the phase-2 `0x41`
START body and are "exchanged later on the established VC." This subsection
locates them. In the category-`0x01` opcode-`0x01` cluster-parameters message,
the sender's **VOTES** is an LE `uint16` at **`body[22:24]`** (payload [80:82],
**abs frame offset 94**):

| specimen | node | configured VOTES | `body[22:24]` |
|---|---|---|---|
| `cd0-bootB` (joiner ZK/1099) | ZK | 0 | `0x0000` |
| `cd0-bootC` (joiner ZK/1099) | ZK | 2 | `0x0002` |
| `formation-ci1-joinwindow` | VAX1 (member) | 1 | `0x0001` |
| `formation-ci1-joinwindow` | VAX2 (joiner) | 0 | `0x0000` |
| `af2-established-rejoin` | VAX1 / VAX2 | 1 / 0 | `0x0001` / `0x0000` |

**GROUNDED**: the `cd0-bootB`→`cd0-bootC` byte-diff of the joiner's op-`0x01`
message differs in exactly **two** bytes, and only `body[22]` (`0x00`→`0x02`)
tracks the controlled VOTES change 0→2; every other byte is identical. Combined
with the golden VAX1=1/VAX2=0 and the af2 re-confirmation, `body[22:24]` matches
the SYSGEN/SDA-reported VOTES byte-exact in **all four vote values {0,1,2}**.
The second differing byte, `body[84]`, is a **false positive** — it reads
`0x0e`(14)/`0x0d`(13) between the two `cd0` boots but `0xd4`(212) for the golden
VAX1 (VOTES 1) vs `0x0e`(14) for the golden VAX2 (VOTES 0), i.e. **non-monotonic
in votes** — so `body[84]` is an unrelated per-boot/timing field, not a vote
field. `body[18:20]` is likewise **not** votes (it stayed `0` for ZK across the
VOTES 0→2 change, though it happens to read 1 for the coordinator VAX1). Only
`body[22:24]` survives controlled variation. **The OVMX takeaway: a non-voting
OVMX node sends `body[22:24] = 0x0000` in its op-`0x01` cluster-parameters
message.**

**EXPECTED_VOTES — RE gap.** `EXPECTED_VOTES` was held at 1 (SDA/`F$GETSYI`
ground truth, §4g) in **every** captured configuration, so no wire contrast
exists to locate it. It is almost certainly a sibling field in the same op-`0x01`
body. **Recommended next step:** the identical controlled-reconfig method used
here for VOTES — reboot the joiner with `SET EXPECTED_VOTES` varied (e.g. 1 vs
3) via `MC SYSGEN`, byte-diff the op-`0x01` bodies. (Disk-mutating: snapshot
`d0.dsk` first, restore golden after — same runbook as `vms-cd0`.)

#### CSID assignment — GROUNDED that it rides this VC (field not fully isolated)

The connection manager assigns each admitted member a **CSID** (VAX1
`00010001`, VAX2 `00010002`; §3). That the assignment rides the 190-byte VC body
is proven by the `af2-established-rejoin` capture: when VAX2 drops and rejoins it
is assigned a **new** CSID `00010003` (SDA `SHOW CLUSTER`, §4i), and that exact
value — which did **not** exist on the wire before the rejoin — appears as an LE
`uint32` in the rejoin VC body at **`body[30]`** (owner-CSID slot, 302 frames)
and `body[108]` (379 frames), while the peer VAX1 keeps `00010001` at the same
slot. In the fresh golden formation the same slots carry `00010001`/`00010002`.
**GROUNDED (presence + the fresh-assignment `00010003` appearing exactly where
SDA says a new CSID was minted).** What is **not** isolated is the discrete
"here is your CSID" *assignment* field in the commit handshake (candidate: the
op-`0x03` transaction body): the coordinator CSID `00010001` doubles as the
**cluster ID** and appears pervasively (2099 frames at `body[30]` in af2), which
confounds a clean single-field pin. Reported as an owner/peer **routing tag** at
`body[30:34]` (abs 102:106), grounded; the assignment-vs-routing distinction is
inferred.

#### RE gaps left in §4j (honest)

- **EXPECTED_VOTES** — not isolable (held at 1 in all captures); recommended
  controlled reconfig above.
- **Member State Seq bump** (SDA `0001→0002` on join, `2→3→4` on af2
  drop/rejoin) — not pinned to a specific body field. **Recommended next step:**
  correlate an af2-style drop/rejoin (where SDA shows the seq stepping) against a
  candidate body field via the same controlled-reconfig method.
- **CSID assignment field** — the CSIDs are grounded as owner/peer routing tags
  (`body[30:34]`), but the discrete assignment field in the `0x03` commit body is
  not isolated (coordinator CSID = cluster ID confound).
- **transaction checksum `body[6:8]`** — grounded as a correlation token; its
  derivation is one-way and not recoverable from passive capture.
- **opcode `0x02`/`0x03`/`0x05`/`0x06` sub-field semantics** beyond the envelope,
  and the category-`0x04` ack opcode meanings (`0x49`/`0x00`), remain undecoded —
  they are the same DLM/MSCP body-decode gap flagged in §4d/§4f.

**What `vms-224` needs to drive OVMX to MEMBER (byte-level).** After the `0x4b`
connect binds `VMS$VAXcluster`, OVMX must, on the 190-byte VC: (1) send
category-`0x01` op-`0x14` (send-msg#1) with its CPU/model string; (2) send
category-`0x01` op-`0x01` (send-msg#2) with **`body[22:24] = 0x0000`** (non-voting)
and the version field; (3) send category-`0x01` op-`0x02` (send-msg#3); each
stamping `body[0:2]` = its own send-msg# and `body[2:4]` = ack of the member's
last send-msg#; then (4) **respond** to the member's op-`0x03` commit and op-`0x05`
lock-rebuild requests by echoing their `(txn, checksum)` at `body[4:8]` with
`body[8] = 0x81`. Success is confirmed by the SDA oracle: a CSB with OVMX's
assigned CSID appears in `SHOW CLUSTER` and the Member State Seq advances.

---

### 4(k) NISCA channel packet-size verification — the padded directed HELLO ("op-0xb3")

This subsection grounds the frame class that `vms-224` wire-observed as VAX1
"flooding op-0xb3 block-transfer frames to the joiner and retransmitting until
acked" — the last thing an established VAX1 waits on before it opens the
joiner's CSB. Captured for `vms-84f`. **The finding corrects the working name:
these frames are NOT a data/state block transfer. They carry no cluster state —
their body is pure zero padding. They are PEDRIVER *channel packet-size
verification* HELLOs: a directed HELLO (§4a/§4b) zero-padded up to
`NISCS_MAX_PKTSZ`, used to prove the LAN channel can carry a full-size packet.
What VAX1 waits for is the joiner to *reciprocate* the padded HELLO — the "ack"
is the joiner's own padded HELLO on the reverse channel, not an acknowledgement
of transferred data.**

> **Provenance / clean-room label.** "PEDRIVER", "channel", and
> `NISCS_MAX_PKTSZ` are public (SDA `SHOW PORTS` PEDRIVER port `PEA0`, §3; the
> documented SYSGEN tunable; the public *OpenVMS Cluster Systems* description of
> PEDRIVER LAN channels being characterized by a maximum packet size before the
> VC will use them). The **byte-level frame structure and the ack contrast below
> are GROUNDED** from the wire. The **mechanism name** ("packet-size
> verification / channel-usability handshake") is **inferred** from the observed
> behavior + that public channel concept — no VSI/HPE binary was consulted.

**Specimens & method (pure passive analysis — no lab mutation).**
- **`ci3-addmember-20260728.pcap`** (1922 SCA frames) — an OVMX joiner
  (`b6:16:8a:dc:3a:53`) attempting to join the established VAX1
  (`aa:00:04:00:01:04`). The join reaches the `0x41`/`0x5b`/`0x4b` VC-setup phase
  but then **stalls**: VAX1 floods **25** padded directed HELLOs to OVMX and OVMX
  never reciprocates. This is the block-transfer-**without**-a-receiver-ack case
  (ideal for grounding the header + the retransmit ladder).
- Golden **`formation-ci1.pcap`** (full run, 18541 SCA frames) — a *successful*
  fresh formation containing exactly **2** padded HELLOs, one each direction
  (the block-transfer-**with**-ack case). Diffing the two isolates the ack.
- `formation-ci1-joinwindow.pcap` (the 3000-frame join window) contains **zero**
  padded HELLOs — a fresh 2-node formation whose channels come up without a
  size-verification stall, matching the `vms-224` observation that only an
  established-cluster join exhibits this wall.

All offsets are **payload-relative** (payload byte 0 = abs frame offset 14).

#### The padded-HELLO frame — GROUNDED structure

Every one of the 25 `ci3` frames and the 2 golden frames decodes as a **directed
HELLO with a zero-pad tail**. Verified byte-exact:

| Pay off | Size | Field | Grounding |
|---|---|---|---|
| 0 | 2 | SCA length field (LE u16 + 2 = total) | GROUNDED (§2): `0x05da`→1500, `0x042b`→1069, `0x0353`→853, `0x02e7`→745 |
| 2 | 6 | dest logical LAVC addr (the joiner) | GROUNDED (§4a) |
| 8 | 2 | connect flag `0x0001` | observed constant |
| 10 | 6 | src logical LAVC addr (VAX1 `aa:00:04:00:01:04`) | GROUNDED (§4a) |
| **16** | **1** | **per-frame word `0xb3`** (§4a offset-30) | GROUNDED value: VAX1's directed-HELLO per-frame word (the `b2/b3/b4` channel-handshake stepping, §4g phase 1). **This is `vms-224`'s "op-0xb3": it is a directed-HELLO per-frame word, NOT a distinct block-transfer opcode** — a genuine 120-byte directed HELLO carries the same `0xb3` (a `0xb2`-step directed HELLO differs from the padded frame in this one byte only). The padded frame's distinguishing feature is its **size**, not this byte. |
| 17 | 1 | `0x00` | observed (note: **not** the `0x13` SCS-envelope format constant — this is a HELLO-family frame, not an `0x4b` sequenced message) |
| 22 | 1 | **message-class byte `0x05`** (HELLO) | GROUNDED (§4a): every padded frame carries the HELLO class byte, not SOLICIT `0x02` |
| 26 | 1 + n | node-name length (`6`) + name (`"VAX1  "`) | GROUNDED (§4a) |
| 54 | 4 | join nonce `ee05395b` | GROUNDED (§4a/§4g) |
| 78 | 2 | node-incarnation counter (§4b/§4i.B) | GROUNDED: `0x0009`/`0x000a` in `ci3` (VAX1 has seen this OVMX node reconnect 9–10×); `0x0001` in the golden (fresh). Separate concern from this section — see the §4i.B gate. |
| 82 | 4 | changing timer/tick (§4b abs-96) | the **only** field that varies across a retransmit (below) |
| 106 | 6 | sender's real hardware LAN MAC (§4b) | GROUNDED: VAX1 `08:00:2b:4a:b7:15` |
| 114 | 2 | poller-sweep marker `0x001f`=31 (§4b) | GROUNDED (SDA `Poller Sweep 31`) |
| 116 | 2 | constant `0x0064` (§4b) | observed |
| **120** | **total−120** | **zero pad** (see caveat for the 745-byte class) | **GROUNDED for the 1500/1069/853-byte classes (21/25 `ci3` frames + both golden frames): pl[120:total] is entirely zero** — `1380/1380` bytes zero in the 1500-byte frame, `0` nonzero. The frame is a genuine §4b 120-byte directed HELLO followed by a run of zeros. **Caveat:** the four smallest-step **745-byte** frames (round 2) are *not* pure zero — they carry a deterministic ~55-nonzero-byte structured blob at `pl[393:532]` (byte-identical across all four retransmits); its content is **unknown** (it *echoes* opcode-like bytes `0x14`/`0x01`/`0x02` and a `bc 00 03…` run reminiscent of the §4j add-member config opcodes and the §4b HELLO tail, but this is not grounded). See the caveat below. |

**Proof it is a pure padded HELLO (byte-exact):**
- A retransmit of the *same* padded frame differs in **only 4 bytes** — the
  timer/tick at `[82:86]` (`ci3` idx 85 vs 109, 1500B: `4/1500` bytes differ, all
  in `[82:86]`). The ~1440-byte body is identical across every retransmit.
- The 1069/853-byte frames are byte-identical **prefixes** of the 1500-byte frame
  (differ only in the length field `[0:2]` and the timer `[82:86]`) — the pad is
  simply truncated, confirming there is no length-dependent content.
- Two VAX1-sourced padded HELLOs to *different* joiners (`ci3` idx 85 → OVMX vs
  golden idx 7534 → VAX2, both 1500B) differ in **12 bytes only**: the dest addr
  `[2:8]`, the incarnation `[78]`, and the timer `[83:88]`. Same class.
- `pl[120:total]` is **pure zero** in 21/25 `ci3` frames (the 1500/1069/853-byte
  classes) and in both golden 1500-byte frames. The exception is the four
  745-byte frames (see the offset-120 caveat above and the RE-gaps note below):
  they carry a small ungrounded structured blob at `pl[393:532]`, identical
  across their four retransmits. The pure-zero result holds for **every frame at
  the sizes VAX1 actually probes first (1500 → 1069 → 853)** and for the golden
  ack pair — i.e. the *ack-relevant* frames are pure padded HELLOs.

#### The retransmit ladder (unacked) — GROUNDED (`ci3`)

With OVMX never reciprocating, VAX1 sends the **largest** size first and steps
**down**, ~4 frames per size at a steady **~6.0 s** interval, across two join
attempts (~380 s apart):

```
1500 ×4  →  1069 ×4  →  853 (×1, attempt 1 ends)        [attempt 1]
1500 ×4  →  1069 ×4  →  853 ×4  →  745 ×4                [attempt 2]
```

Size ladder `1500 → 1069 → 853 → 745`; the pad-decrement roughly halves each step
(`431, 216, 108`). The max, **1500 = `NISCS_MAX_PKTSZ` 1498 + 2**, is byte-exact
against the SYSGEN tunable (§3, matches the §4e cap). Interpretation
(**inferred**): PEDRIVER probes the channel at full size, and on no
acknowledgement steps the verification size down looking for a size the peer will
confirm. Retransmit interval **6.010 s ± 0.15** (24/24 inter-frame gaps), a
distinct timer from `RECNXINTERVAL 20`.

#### The ACK — GROUNDED by the golden-vs-`ci3` contrast (the deliverable)

**The frame that appears in the golden and is missing in `ci3` is the joiner's
own padded HELLO on the reverse channel.** This is the load-bearing deliverable
for `vms-9f3`:

| capture | result | padded HELLOs, member→joiner | padded HELLOs, joiner→member | member retransmits? |
|---|---|---|---|---|
| golden `formation-ci1` | **join succeeds** | **1** (VAX1→VAX2, idx 7534, 1500B) | **1** (VAX2→VAX1, idx 5990, 1500B) | **no** |
| `ci3-addmember` | **join STALLS** | **25** (VAX1→OVMX, 1500/1069/853/745) | **0** | **yes, forever** |

In the golden the **joiner (VAX2) sends its padded HELLO first** (idx 5990,
t≈…736), and the **member (VAX1) reciprocates** with its own identical-size
padded HELLO (idx 7534, t≈…737) — one exchange, no retransmit, and the CSB opens.
The reciprocal frame (idx 7534) is a padded directed HELLO carrying VAX1's *own*
identity (name `"VAX1  "`, HW MAC `08:00:2b:4a:b7:15`), zero-padded to the same
1500 bytes — structurally identical to the `ci3` VAX1→OVMX frames but on the
reverse channel. **GROUNDED: exactly one padded HELLO per direction in the
successful case, zero from the joiner in the stalled case.**

**An unpadded 120-byte HELLO is NOT the ack (GROUNDED negative).** In `ci3`, OVMX
is alive and *does* send directed HELLOs throughout the retransmit window (23
multicast + 23 directed 120-byte HELLOs in the idx 80–310 window), yet VAX1 keeps
retransmitting the padded probe. **OVMX's largest frame in the entire capture is
190 bytes** — it never emits a frame ≥745 bytes. So VAX1's requirement is
specifically a *padded* HELLO at (or near) the probe size; the ordinary 120-byte
directed HELLO that completes the §4g phase-1 channel handshake does not satisfy
the size-verification, and the join wall stands.

**Concrete answer for OVMX (`vms-9f3`).** OVMX already builds a 120-byte directed
HELLO (it runs the §4g phase-1 `b2/b3/b4` handshake). The fix is to
**participate in the packet-size verification** with a *padded* HELLO:

1. **Reciprocate (the unstick):** on receiving a padded directed HELLO of total
   size *N* from VAX1 (a HELLO-class `pl[22]=0x05` frame with `total > 120` and a
   zero pad tail), reply with OVMX's **own** directed HELLO — its identity, its
   `pl[16]` per-frame word, incarnation per §4i.B — **zero-padded to the same
   size *N*** (the largest OVMX received successfully; up to `NISCS_MAX_PKTSZ`),
   sent to VAX1 on the reverse channel. This is the exact frame that is present
   (golden) / absent (`ci3`) and it stops VAX1's retransmit and lets it open the
   CSB.
2. **Initiate (match the golden joiner):** ideally OVMX also *sends first* — emit
   a padded HELLO up to `NISCS_MAX_PKTSZ` to advertise its own channel size, as
   golden VAX2 did (idx 5990), so the member reciprocates and both directions
   verify.

The pad is pure zeros appended after the standard 120-byte directed HELLO; no new
field content is required. Success is confirmed by the SDA oracle exactly as
§4j: a CSB with the joiner's CSID appears in `SHOW CLUSTER` and the Member State
Seq advances.

#### RE gaps left in §4k (honest)

- **Why the joiner initiates in the golden but the member initiates in `ci3`** is
  not pinned — plausibly the established member starts probing a new/unverified
  channel when the joiner has not, but that is an inference from two captures.
- **The exact size OVMX must echo to satisfy VAX1** is grounded as "the probe
  size, up to `NISCS_MAX_PKTSZ`" (both golden directions used the full 1500); it
  was not independently varied. Whether a smaller reciprocal (e.g. VAX1's stepped
  853/745) would also satisfy VAX1 is untested (would need an OVMX that acks at a
  reduced size).
- **The step-down algorithm** (why `1500→1069→853→745`, ~4 per size, 6 s) is
  reported as-observed; the precise PEDRIVER size-search rule is not derivable
  from passive capture.
- The incarnation counter `pl[78]` = 9/10 in `ci3` shows the OVMX join has also
  been cycling the §4i.B incarnation gate; that is a **separate** field and
  concern (see §4i.B) — the padded-HELLO ack grounded here is what *this* capture
  shows VAX1 retransmitting on.
- **The 745-byte class's `pl[393:532]` blob is unknown.** When VAX1 stepped its
  probe all the way down to 745 bytes (round 2's final size) it appended a small
  deterministic ~55-nonzero-byte structure instead of pure zeros — it *may* be a
  compact channel/config descriptor VAX1 tries at the smallest size (the bytes
  loosely echo §4j add-member opcodes `0x14`/`0x01`/`0x02` and a `bc 00 03…`
  run), but the content is **not grounded** from passive capture. It does not
  affect the ack (which is a reciprocal padded HELLO, and the golden ack pair is
  pure-zero 1500-byte). Grounding it would need an OVMX that acks the larger
  probes so VAX1 never reaches the 745-byte step, or a controlled capture that
  isolates what that size carries.

---

### 4(L) The active-joiner drive sequence — reaching `SHOW CLUSTER` membership (GROUNDED live, `vms-d94`)

§4(g)–§4(k) decode the individual frame classes of the join. This section
grounds the **choreography and timing** a joiner must reproduce to be admitted —
i.e. *who must initiate what, and when* — derived from the clean 1→2-node
reference `formation-clean-2node.pcap` (VAXB joining an established VAXA) and
byte-verified **live** against a real VAX 7.3 by driving an OVMX joiner to each
observed state. It corrects a first-pass model (that the joiner is a passive
responder) that stalled the join indefinitely.

**(1) The joiner actively DRIVES the post-START sequence; it is not a passive
responder — GROUNDED.** The moment the phase-2 START/config handshake completes
(§4g/§4i: config-round `0→1→2`, both nodes' 46-byte round-2 ack), the joiner
VAXB *immediately* (Δt ≈ 0.1 ms in the clean reference) begins issuing its own
requests — it does **not** wait to be asked. In order it: opens its **own**
`SCS$DIRECTORY` connection to the member and looks up SYSAP names (§4h); sends
its **own** `VMS$VAXcluster` **CONNECT-REQUEST** (§4g phase 4, JOINER→MEMBER,
remote Con.ID = 0, offering its local handle); and streams the add-member config
(§4j) on **that joiner-initiated VC**. A node that only *answers* the member's
directory queries and sends its config on the **member-initiated** connection is
never admitted (see (2)). The corollary, grounded on both captures: `VMS$VAXcluster`
is symmetric but the **join is joiner-driven** — in a formation there is exactly
one `VMS$VAXcluster` CONNECT-REQUEST and it is JOINER→MEMBER; the member does not
open the cluster-manager connection to the joiner.

**(2) The member's connection manager re-issues START on a timeout unless the
joiner promptly drives the connect — GROUNDED live.** After START completes the
member holds the channel open and waits for the joiner's `VMS$VAXcluster`
CONNECT-REQUEST. If it does not arrive **promptly** — observed member timeout
≈ 1.4 s — the member abandons the attempt and **re-issues the phase-2 `0x41`
START at config-round 0**, looping indefinitely (observed 65 re-issues over
425 s against a joiner that delayed its connect ~7 s). A joiner that fires its
CONNECT-REQUEST the instant the post-START directory phase begins is accepted and
the member **stops** re-issuing START. **START completing is therefore necessary
but NOT sufficient**: the earlier "member loops START forever" symptom (mistaken
at times for a START-handshake or 190-byte-VC defect) is the connection manager
timing out while waiting for a joiner that never drove the connect.

**(3) The member accepts the joiner's connect by Con.ID signature, not by
opcode — GROUNDED live.** The member accepts the joiner's `VMS$VAXcluster`
CONNECT-REQUEST by returning a frame whose **remote Con.ID == the joiner's
offered local handle** and whose **local Con.ID is the member's freshly-supplied
handle**, carrying the `VMS$VAXcluster` name and the affirmative descriptor
(§4h `01 1b 01 03 …`). The observed message-type byte on that acceptance is
**`0x5b`** (the directory/resolution class, §4g/§4h) — **not** the `0x4b`
sequenced-application byte a connect-response was assumed to use. A joiner must
therefore recognize its connect being bound by the **Con.ID pair**
(`remote == own-handle, local != 0`), independent of the opcode, then treat the
pair `{own-handle, member-handle}` as the bound cluster-manager VC.

**(4) The joiner-connect sequence semantics — GROUNDED.** The CONNECT-REQUEST is
one sequenced message on the **shared per-channel** VC sequence (§4h): it advances
the channel `send_seq` exactly once, and **retransmissions REUSE that same
`send_seq`** (a retransmit is not a new message — advancing it per retransmit
desynchronizes the peer). `recv_ack` tracks the member's latest `send_seq` per
the §4h lockstep. The joiner's directory responses and its connect-request share
this one counter.

**(5) `SHOW CLUSTER` status `NEW` — the transitional membership state, GROUNDED
live.** Once the member accepts the joiner's `VMS$VAXcluster` connect and receives
its add-member burst (§4j op `0x14/0x01/0x02`), it opens a CSB for the node and
`SHOW CLUSTER` reports it with status **`NEW`** — the documented transitional
state a node occupies before `MEMBER`. Contrast the observed progression by joiner
completeness: a node that forms the channel (§4a.1) + answers START but never
drives the connect appears in the SYSTEMS table with **blank** status; a node that
promptly drives the connect + add-member reaches **`NEW`**; reaching **`MEMBER`**
requires the reciprocal transaction in (7).

**(6) The `[58:66]` software-version field is display-only — GROUNDED live.** The
START/config software-version string (§4g `[58:66]`, `"VMS V7.3"` in every
captured VMS node) is what `SHOW CLUSTER` prints in its **SOFTWARE** column. It is
**not validated for admission**: a node advertising **`"VMX V0.1"`** (not a
`"VMS Vx.y"` string) still reaches `NEW`, and `SHOW CLUSTER` prints `VMX V0.1`
verbatim. OVMX advertises its own identity here rather than impersonating VMS
(authenticity INV-0 / trademark ceiling). The field is a fixed 8-byte ASCII span.

**(7) `NEW → MEMBER` — the remaining RE gap (honest).** Promotion from `NEW` to
`MEMBER` was **not** achieved and is the open frontier. After the joiner's
add-member burst the member **credits** it (§4h `0x48`) but does **not**
reciprocate config on the joiner VC — instead it directory-looks-up the joiner
(`MSCP$DISK`, then `VMS$VAXcluster`) and idles. In the clean 1→2-node formation
the member instead **reciprocates** its own `0x14/0x01` on the joiner VC and then
drives the interactive `0x02 → 0x03` (commit) `→ 0x05` (lock-rebuild) `→ 0x06`
sequence of §4j to `MEMBER` (Member State Seq bump). **Ruled out live** as the
cause of the stall: the joiner's **VOTES** value (tested `0` and `1`; both stall
at `NEW`; `VOTES=0` is legitimate — an existing member ran `VOTES=0`); sending the
`0x02` **prematurely** in the initial burst (held it — still `NEW`); and
**3-node reconfiguration coordination** (zero member↔member 190-byte VC traffic
followed the joiner's connect, so the member is **not** blocked waiting on the
peer member's ack).

The missing predicate is now **GROUNDED** as the **full joiner-CLIENT connection
choreography** (`vms-760`, live 2026-07-29). Byte-anchored against the clean
1→2-node formation (`formation-clean-2node.pcap`, joiner `08:00:2b:94:ca:47`), the
real joiner, on **one shared monotonic per-channel `send_seq`**, does — in order:
(a) open its **own `SCS$DIRECTORY` CLIENT connection** (SCA idx20, `send_seq=1`,
local handle `0x4e630007`); (b) **look up each SYSAP on the member as a client**
before connecting to it — `MSCP$TAPE`/`MSCP$DISK` (idx31, `seq4`) and
`VMS$VAXcluster` (idx41, `seq7`), each answered **affirmatively** by the member on
that dir-client connection; (c) only **then** open the `MSCP$DISK` client
connection `VMS$DISK_CL_DRVR→MSCP$DISK` (idx35, `seq6`, local `0x4e620008`); (d)
open the `VMS$VAXcluster` VC (idx47, `seq10`, local `0x4e620009`); (e) send the
add-member burst (idx54, `seq14`, `cat=0x01/op=0x14`). The member reciprocates its
own `0x14/0x01` (idx59) within ~1 ms of receiving (e), **independently of the
joiner's later `0x02`** (idx97, +3.5 s). The single element OVMX has **never**
presented in any capture is the joiner acting as a **directory + disk CLIENT** —
0 `VMS$DISK_CL_DRVR` frames vs the clean joiner's 41.

**Shared-sequence deadlock — the mechanism, live-grounded (`d94-760mscp.pcap`).**
The per-channel `send_seq` is **shared across all Con.ID pairs** (clean joiner
draws `1,3,4,5,6,7,9,10,14…` across its dir/MSCP/VC connections from one counter);
OVMX's single-counter model is therefore **correct**, not the bug. The consequence:
a connect the member **cannot yet process** — e.g. OVMX firing the `MSCP$DISK`
connect **without first resolving `MSCP$DISK` via a dir-client lookup** — occupies
a slot in that shared sequence and creates an **in-order hole**. Observed live: the
member froze its `recv_ack` at `2` (the last dir-response) and **never** accepted
the `VMS$VAXcluster` connect at the next `seq`, regressing OVMX **below `NEW` to
blank** status. This falsifies the "inject the `MSCP$DISK` connect standalone"
shortcut and proves the SYSAP **resolution ordering** (lookup-before-connect) is
load-bearing, not cosmetic. The byte-exact `MSCP$DISK` connect builder
(`scs_connect_build_mscp_request`, template = clean idx35) is built and verified,
but must not be driven until the dir-client resolution precedes it.

**Next deliverable:** implement the full dir-client resolution choreography (a),
(b), (c), (d) with correct shared-`send_seq` ordering. This subsumes the earlier
own-`SCS$DIRECTORY`-connect attempt (`vms-760`/d94-760b), which regressed only
because OVMX's own-dir drive **mis-sequenced** and suppressed the member's parallel
dir probe (the clean member opens **its own** dir connect regardless, idx76) — an
OVMX drive bug, **not** a protocol incompatibility.

**Clean-room provenance:** every claim here is from (a) observing the reference
lab wire (`formation-clean-2node.pcap` + live `SHOW CLUSTER`/`SDA` output on the
lab VAX) and (b) public OpenVMS documentation on cluster connection management and
the `NEW`/`MEMBER` SDA states. No VSI/HPE binary was disassembled (CLAUDE.md
Rule 8).

---

### 4(M) Peer silence: how long a live node may say nothing (GROUNDED, `vms-17f`)

**What this grounds and why.** OVMX now declares a peer *departed* after a listen
timeout, tears its Path Block down and releases its peer slot (VAXcluster
Principles pp. 2-17/2-21/2-28; `src/vmsscs/scs_depart.h`). That threshold decides
whether a node that is merely quiet gets thrown out of OVMX's configuration
database, so it is bounded by measurement rather than chosen. Nothing here is a
frame layout — it is a **timing** property of the wire, measured the same way.

**Method.** `tools/scs_peer_silence_measure.py` (re-runnable; PASS/FAILs each
figure against a checked-in table). Over every `0x6007` frame in a capture, the
gap between consecutive frames with the same **source MAC**, regardless of
destination — multicast beacons included, because a node that is beaconing has
not departed. Leading/trailing gaps at the capture window's edges are reported
separately and excluded. Captures are classified as *healthy* or *departure* by
what the run was for, never by the numbers.

| population | captures | frames | wire span | **max per-source silence** |
|---|---|---|---|---|
| healthy (no node left) | `ovmx-760-persist-10min-20260730`, `cd0-baseline-current-20260728`, `formation-ci1-joinwindow` | 13 392 | 747 s | **3.153 s** |
| departure (VAX2 dropped and rejoined, VAX1 up throughout) | `af2-established-rejoin-20260728` | 16 340 | 606 s | **395.955 s** (VAX2) |

The two populations do not overlap, and they do not overlap **inside** the
departure capture either: VAX1, which stayed up the whole time, never exceeds
3.12 s in the same window as VAX2's 395.955 s gap.

**What OVMX does with it (OVMX design choice, labeled per Rule 8).** The default
listen timeout is **20 000 ms** — the value of the lab's SYSGEN `RECNXINTERVAL`
(20, §3) — which is 6.3× the longest healthy silence measured and 20× below the
observed departure. This is **not** a claim that VMS uses 20 s as a listen
timeout: `RECNXINTERVAL` governs removal *after* a circuit breaks, not the timer
that breaks it. The book (ch. 2) describes circuit loss but publishes no
detection timer; that lives in the port drivers, which ch. 2 is not about.
`OVMX_PEER_LISTEN_TIMEOUT_MS` overrides it and SCSD logs the value at startup, so
a capture is never read as a spontaneous departure.

**Explicit non-claim.** 3.153 s is the largest silence in 747 s of captured wire
from a 2–3 node lab, not an upper bound. A larger cluster, a loaded node or a
lossy link could exceed it; the choice rests on the margin, not on the maximum.

---

### 4(N) The 16-byte SCA connect data (GROUNDED location + census, `vms-fdd`)

*VAXcluster Principles* p. 2-25: the initiating SYSAP may supply **up to 16
bytes** of connect data in `CONNECT_REQ`, and the target SYSAP up to 16 in
`ACCEPT_REQ`. "This option is used to limit which versions of VMS can
coexist… When two Connection Managers form a connection with each other, they
use this data to effectively identify to each other which version of VMS each
is associated with. If the target does not approve of the source Connection
Manager VMS version, it rejects the request. If the source does not approve of
the target, it explicitly breaks the connection that the target accepted."
p. 2-28 puts the field in the CDT. This section locates it on our wire.

**Re-derive everything below:** `tools/scs_connect_data_measure.py` (lab host;
the captures are host-only and not in git). Last full run **2026-08-05: 67
checks, 0 failures.** `ctest -R scs_connect_data_figures` needs no captures — it
asserts these figures still appear verbatim here and in
`src/vmsscs/include/scs_connect.h`.

**THE POPULATION IS VAX-ONLY — OVMX's own frames are not evidence about VMS.**
The lab captures are recorded on a LAN where OVMX itself is a talker, so a
quarter of the library's connect frames were **transmitted by OVMX** and are
**excluded here: 466 of 1891, of which 55 are `VMS$VAXcluster`**.
Counting those toward "what a
real VAX puts in this field" is circular grounding: the census could not then
distinguish *"every VMS node does this"* from *"we do this, and so do the VAXes
we recorded alongside us"*. **Every GROUNDED figure in this section is therefore
measured over VAX-sourced frames only.** The split is on the Ethernet source MAC
and is a first-class filter in the tool, which reports both populations and the
counts it dropped.

Identification is sound: OVMX never spoofs its Ethernet source — `scsd` takes
`our_hw_mac` from `SIOCGIFHWADDR` and every builder copies it into abs `[6:12]`
— and its MACs are the `hwmac=` values `scsd` itself logged in the lab work
directory. That blocklist is backed by a **structural rule**: a real lab VAX
sources from `08:00:2b` (DEC NIC OUI) or `aa:00:04` (DECnet logical address);
anything else, in particular a locally-administered Linux tap MAC, is not a VAX.
The tool **fails** on any source it cannot place, so a future OVMX run on a new
MAC reds the measurement rather than silently rejoining the sample.

OVMX-sourced frames remain legitimate evidence about exactly one thing — **what
OVMX's own encoder emits** — and the tool reports that population separately.
They are never evidence about a real VAX.

**A MAC IS NOT A NODE — how many sources agree is counted from identity.** The
source MAC is the right axis for *"is this frame ours"* and the wrong axis for
*"how many independent nodes agree"*; an earlier revision of this section used
it for both, and **both directions of that error are real in this library**:

- **one node, two MACs** — `08:00:2b:4a:b7:15` (VAX1's DEC NIC address) and
  `aa:00:04:00:01:04` (the DECnet logical address that replaces it once DECnet
  starts) are the same machine; both name themselves `VAX1`/1025 in their own
  START frames;
- **one MAC, three nodes** — `08:00:2b:78:56:b9` was reconfigured across reboots
  and appears as `VAX2` (1026), `VX3` (1050) and `ZK` (1099).

Identity is therefore read **out of the frames**. Every connect frame carries
its sender's own LAVC logical address at `[10:16]` in the form
`aa:00:04:00:NN:04`, `NN` = `SCSSYSTEMID & 1023` (§4(g)) — **0 residuals** over
the VAX population, and **0 mismatches** where the Ethernet source is itself an
`aa:00:04` address. `NN` is resolved to the ASCII node name through the 106-byte
START frames (`[90:98]` name, `[46:48]` SCSSYSTEMID, §4(g)); the tool requires
that map to be 1:1 and to cover every node number in the census:
`{1: VAX1, 2: VAX2, 3: VAX3, 26: VX3, 75: ZK}`.

Two counts follow, and they are **not** interchangeable:

| count | what it is | value |
|---|---|---|
| **node identities** | distinct cluster members | **5** |
| **hardware sources** | connected components of the MAC ↔ identity graph — distinct SIMH instances. `{VAX1}`, `{VAX3}`, `{VAX2, VX3, ZK}` | **3** |

**No "sources agree" claim in this section may use the source-MAC count**,
because `VX3` and `ZK` are the same reconfigured box as `VAX2` and are not
separate observations of VMS behaviour. **The independence figure dropped**:
this section previously said *"four independent real VAX nodes"*, which was the
source-MAC count; the graph count is **3**.

A by-product is a second, independent check on the population split: the node
numbers the VAX population emits and the ones OVMX emits must be **disjoint
sets**, and they are. A misclassified source would surface as a node number in
the wrong population.

**…and 3 is not an independence count either — what the attestation actually
rests on.** *"3 independent hardware sources"*, *"distinct lab machines"* is
what this section said next, and that is **also false**. It matters more than
the first error, because it was the conservative figure the first correction
retreated to. The lab's real configuration
(`/data/training/vax/cluster/README-lab.md`, `cluster/vax.ini`) is:

| | | |
|---|---|---|
| **emulator instances** | `vax1`, `vax2`, `vax3` — all the same emulated model (MicroVAX 3900 / KA655) under the same SIMH binary on **one** Linux host. No physical hardware diversity exists in this lab at all. | **3** |
| **system roots** | `[SYS0]` = VAX1 · `[SYS1]` = VAX2 and, re-identified with `MC SYSGEN` between reboots, VX3 and ZK · `[SYS11]` = VAX3, a diskless satellite whose root is MSCP-served | **3** |
| **system disk images** | all three roots live in the single file `data/d0.dsk`, which `vax1` and `vax2` attach at the same time (dual-ported) | **1** |
| **VMS installations** | one OpenVMS VAX V7.3 install, whose `SYS$COMMON` executive images all three roots share | **1** |

The last row is **measured**, not merely read off the lab notes: all **668**
VAX-sourced 106-byte START frames report version `[58:66]` = `"VMS V7.3"` on
hardware `[74:78]` = `"VAX "` — **one** distinct version string across all 5
node identities (§4(g) grounds both fields).

> **So the honest attestation behind every GROUNDED figure in this section is:
> one VMS build, under three system roots, on one system disk image, across
> three emulator instances.** Three roots of one installation agreeing about a
> connect-data byte is much closer to **one observation repeated** than to three
> independent confirmations, and nothing below may be read as the latter.

What the census **does** establish is that the value is stable across node
identity, node number, system root, boot, incarnation and role (joiner vs
member) — that is worth having, and it is the whole of it. What it **cannot**
establish is anything at all about another VMS version, another build, or a
second installation of the same version: the sample holds exactly one of each.
§5 carries that as a standing limit on this section.

**WHERE IT IS — GROUNDED.** The connect data is the **last 16 payload bytes of
the 110-byte connect class, `[94:110]`** (absolute frame `[108:124]`), directly
after the two 16-byte ASCII SYSAP name fields `[62:78]` and `[78:94]` that
§4(h)(2) already grounds. This closes the region §4(g)/§5 previously carried as
an unexplained replay.

**Population, and why it is exactly this one.** Take `sca = frame[14:]`; keep
`len(sca) == 110`, format byte `sca[17] == 0x13`, opcode `sca[16]` in
`{0x4b, 0x5b, 0x7b}`; drop OVMX-sourced frames per the guard above; then split
on the SCA connection-control message type at `[46:48]` (§4(h)(1a)). Over every
`.pcap` in the lab capture directory (**48 files**) the VAX-sourced part of that
class carries message types **`{0: 1101, 2: 324, 10: 2889}`**. The
1101 + 324 = **1425** type-0/type-2 frames all carry an ASCII SYSAP name at
`[62:78]` — **0 non-ASCII residuals** — while the type-10 frames carry binary
there and are not connect frames. So the field is claimed for `CONNECT_REQ` and
`ACCEPT_REQ` **only**; a length test alone would over-claim by 2 889 frames.
(OVMX's own 466 connect frames are `{0: 396, 2: 70}` — it emits **no** type-10
frame at all. The two populations are not scaled copies of one another, which is
why the exclusion has to be applied before the split rather than to the totals.)

**WHAT IS IN IT — GROUNDED, and it is per-SYSAP, not per-node.** Census of
`[94:110]` over those 1 425 VAX-sourced frames, keyed on the local SYSAP name
`[62:78]`. The last column records what OVMX contributed and the guard removed:

| local SYSAP | VAX frames | distinct values | value(s) | OVMX frames excluded |
|---|---|---|---|---|
| `MSCP$DISK` | 809 | 1 | ASCII **`"V5.0          + "`** | 243 |
| `SCS$DIRECTORY` | 201 | 1 | 16 ASCII spaces | 113 |
| `SCS$DIR_LOOKUP` | 134 | 1 | 16 ASCII spaces | 55 |
| `SCA$TRANSPORT` | 32 | 2 | `02 02 01 03 …` | 0 |
| `VMS$DISK_CL_DRVR` | 101 | 5 | `00 00 04 a0 00 00 00 00 NN 04 00 00 00 00 04 01` | 0 |
| `VMS$VAXcluster` | 148 | 5 | `01 1b 01 03 …` | 55 |

**`MSCP$DISK` is the decisive row**: a printable ASCII **version string** in the
connect data of the disk-server SYSAP — p. 2-25's "which version" read straight
off the wire, at these offsets, with **809** VAX-sourced frames from **5 node
identities on 3 emulator instances** — i.e. 1 VMS build, 3 system roots, 1 disk
image — and one distinct value. (The
earlier "four distinct VAX nodes" here was the source-MAC count.) Every SYSAP
row in the table above is emitted by all 5 identities; the tool pins that
per-row. The `VMS$DISK_CL_DRVR` row is the
corroborating one, and it is **untouched by the guard** — OVMX runs no such
SYSAP, so all 101 frames are VAX-sourced: its byte `NN` takes
`{01, 02, 03, 1a, 4b}` = `{1, 2, 3, 26, 75}`, exactly the LAVC node numbers of
VAX1/VAX2/VAX3/VX3-1050/ZK-1099 (`SCSSYSTEMID & 1023`, §4(g)) — a per-node datum
landing inside the 16-byte window, which is a second, independent check that the
window's boundaries are right.

**The `VMS$VAXcluster` value — what is invariant.** Across **all 148**
VAX-sourced `VMS$VAXcluster` connect frames, every boot and every capture we
hold (all VAX/VMS V7.3), which by node identity are

| node | VAX1 | VAX2 | VAX3 | VX3 | ZK | total |
|---|---|---|---|---|---|---|
| frames | 74 | 32 | 36 | 3 | 3 | **148** |

— **5 node identities on 3 emulator instances**, which is 1 VMS build under 3
system roots on 1 disk image:

| span | value | grounding |
|---|---|---|
| `[94:98]` | `01 1b 01 03` | **GROUNDED 148/148, 0 residuals** — the version quad |
| `[98:105]` | 5 distinct | **NOT GROUNDED** — see the gap below |
| `[105:110]` | `08 00 00 06 00` | **GROUNDED 148/148, 0 residuals** |

Excluding OVMX moved these counts from `203/203` to `148/148`; it did **not**
weaken either claim below what we would accept from a stranger's capture — the
two spans still hold with zero residuals over **3 emulator instances
carrying 5 node identities**, many boots and 48 captures, and the
distinct-value count for `[98:105]` is unchanged at 5 (OVMX's 55 frames carried
only values the VAXes already emit). No claim in this section had to be
downgraded to a §5 gap as a result. **The independence count itself did drop**,
from the 4 this paragraph previously claimed to 3 — that figure was a source-MAC
count, not a node count (see "A MAC IS NOT A NODE" above).

**What OVMX sends, and why that value.** OVMX joins an existing cluster.
`vax3-2to3-established-join-20260730.pcap` is the library's only capture of a
**real node being admitted to an already-running cluster** (§1), and in it the
joiner VAX3 (`08:00:2b:11:22:33`) emits **one** connect-data value for **both**
message types — raw frame 132 (its `CONNECT_REQ` to VAX1) and raw frame 210 (its
`ACCEPT_REQ` answering VAX2):

```
01 1b 01 03 00 00 00 00 00 00 00 08 00 00 06 00
```

That is `scs_connect_data_vaxcluster[]` in `src/vmsscs/scs_connect.c`, byte for
byte, stamped by **both** OVMX builders — because OVMX occupies exactly VAX3's
role in exactly that exchange. The established **members** in the same capture
emit a different value (VAX1 raw 136, VAX2 raw 208, both
`01 1b 01 03 01 00 01 00 02 00 01 08 00 00 06 00`), and that contrast is what
the decode test in `tests/vmsscs/test_scs_connect.c` asserts. **Both ends of
that contrast are real VAXes.** OVMX is present in the specimen as a bystander
(209 SCA frames) but sources no `VMS$VAXcluster` connect frame in it, and the
tool asserts that — the joiner/member contrast is VAX-to-VAX throughout.

**Not just the specimen.** Two frames would be thin evidence for a value a peer
is documented to *reject* on, so the adopted value is attested independently:
**40 VAX-sourced `VMS$VAXcluster` connect frames carry it, 38 of them outside
the specimen, from 5 distinct node identities on 3 emulator
instances, across 18 captures.** All five counts are pinned by the tool. (The
earlier "3 distinct VAX nodes" here was a source-MAC count that coincided with
the graph count by accident, not by derivation.) Read with the configuration
above, those 3 instances are still **one VMS build under 3 system roots on 1
disk image**: this is repetition across roles, boots and roots, not
corroboration by independent VMS systems. (OVMX's own excluded frames also carry it 54 times —
that is evidence about an OVMX build, deliberately not counted here.)

**What changed on OVMX's wire.** Before `vms-fdd` the region was a labelled
replay of whichever golden frame the template came from. The `CONNECT-REQUEST`
template is VAX1's — an established **member's** frame — so OVMX, a joiner, was
presenting a member's connect data (`[98:105]` = `01 00 01 00 01 00 01`). This
is a claim about the code as it stood immediately before `vms-fdd`, measured by
`scsd_wire_diff.sh`, **not** a claim about OVMX's whole recorded history: the
OVMX frames in the capture library predate the `vms-561` refactor that routed
the joiner request through this template, and 54 of their 55 already carried the
joiner value. The `CONNECT-RESPONSE` template is VAX2's joiner frame and already
carried the stamped bytes, so that direction is byte-unchanged. Kill switch
`OVMX_NO_CONNECT_DATA=1` suppresses the stamp and restores the template bytes
exactly; both directions are asserted in the unit test.

**RE gap left in §4(N) (honest).** What `[98:105]` **encodes is unknown.** All
five values it takes over the 148 VAX-sourced frames, exhaustively:

| `[98:105]` | VAX frames | note |
|---|---|---|
| `00 00 00 00 00 00 00` | 40 | the joining form — the value OVMX adopts |
| `01 00 01 00 02 00 01` | 59 | |
| `01 00 01 00 03 00 01` | 37 | |
| `01 00 01 00 01 00 01` | 11 | |
| `01 00 00 00 02 00 01` | 1 | **does not fit the shape below** |

The nodes emitting the all-zero form are the ones joining. Four of the five fit
`01 00 01 00 NN 00 01` with `NN ∈ {1,2,3}`, and "`NN` = the count of cluster
members the sender currently sees" fits every capture and is the best reading —
but it is **INFERRED**, the fifth value does not even fit the shape, and one
frame is too few to say whether that is a sixth state or a transient. Nothing
OVMX does depends on any of it: OVMX copies a real joiner's observed bytes
rather than computing them. Two candidate readings are **REFUTED**: it is not
the member-state sequence (`af2-established-rejoin` runs Member State Seq 2→3→4
while VAX1 sends `NN=1` throughout) and not the node number (VAX1, node 1, sends
`NN=2` in the 2-member specimen). **Consequence, and it is a real limit:** OVMX
cannot yet *generate* connect data for a role it has not observed, and it must
not claim to.

**OVMX does NOT act on the peer's value.** It decodes and logs it
(`SCSD-I-CONNDATA`) and nothing more. p. 2-25's rejection behaviour is real, but
every node on our wire is VMS V7.3, so we have never observed a refusal and have
no basis for a version policy. Implementing one on a single data point would be
inventing it.

---

<!-- vms-578 INTEGRATION NOTE ON SECTION ORDER.
     Section 4(O) is placed LAST before section 5 on purpose, and moving it is
     not cosmetic. test_scs_join_capability_figures.py pins its figures by
     slicing the document from the 4(O) heading to the section-5 heading, so
     anything that lands between them is searched for those figures too.
     Merging worktree-760`s
     4(m)..4(r) sections in AFTER 4(O) widened that slice by ~38 KB and a
     mutant that changed A0`s cm_190_rx from 583 to 584 SURVIVED -- the string
     "584" occurs in the widened slice as the item id `vms-584`. The gate was
     right and the document was wrong; the sections are reordered rather than
     the slice loosened. Keep 4(O) immediately above section 5. -->

### 4(m) SCS connection lifecycle — the `op` verb set at abs 60 (GROUNDED, `vax3-2to3-established-join-20260730.pcap`)

Every SCS connection-control frame carries a little-endian verb at **abs 60**
(`sca[46:48]`). Prior sections decoded individual frames; this is the complete verb
set and state machine, grounded on a real VAX (VAX3, `08:00:2b:11:22:33`) joining the
live 2-node cluster — the only capture in the library of an **established** join by a
genuine VMS node, and therefore the authority for connection semantics.

| op | name (inferred) | SCA len | sent by | meaning |
|----|-----------------|---------|---------|---------|
| 0 | CONNECT-REQUEST | 110 | initiator | opens a connection to a named SYSAP; `remote_conid`=0, `local_conid`=own handle; `name@76` = target SYSAP, `result@92` = offered local SYSAP |
| 1 | CONNECT-ECHO | 66 | acceptor | "received"; echoes `remote_conid`=initiator's handle, `local_conid` still 0. **Every** accept emits this first |
| 2 | CONNECT-RESPONSE | 110 | acceptor | the ACCEPT — supplies the acceptor's handle in `local_conid`; binds the Con.ID pair |
| 3 | CONNECT-CONFIRM | 62 | initiator | initiator acknowledges the bind. **Load-bearing**: without it the connection stays half-open and the peer will not accept the initiator's *next* connect — and, on the `VMS$VAXcluster` VC specifically, will not run the add-member dialogue on it at all (see below) |
| 4 | CONNECT-ACCEPT (alt) | 62 | acceptor | the accept form used when answering a **member-initiated `MSCP$DISK`** connect (in place of op 2) |
| 5 | CONNECT-CONFIRM (alt) | 58 | initiator | confirm paired with op 4 |
| 6 | DISCONNECT-REQUEST | 62 | either | tears the connection down. **Bidirectional**: each side sends its own op 6 and answers the peer's with op 7 |
| 7 | DISCONNECT-RESPONSE | 58 | either | acks an op 6 |
| 8 | CREDIT/READY-REQUEST | 58 | either | post-bind flow-control/ready exchange |
| 9 | CREDIT/READY-RESPONSE | 58 | either | answers op 8 |
| 10 | DATA / DIRECTORY-OP | 94/110/190 | either | directory lookup (§4h), MSCP command (§4e), and the 190-byte SYSAP config dialogue (§4j) all ride op 10 |

**Response construction (ops 7/9):** a standard reflection — swap Ethernet src/dst,
swap the cluster-logical addresses at abs 16 / abs 24, swap the Con.ID pair
(`rc`↔`lc`), set `op = op+1`, `recv_ack` = the request's `send_seq`, and take a fresh
`send_seq` from the shared counter.

**Directory connections are TRANSIENT and serially reused.** A `SCS$DIRECTORY`
connection is opened, used for lookups, then closed by the op8/9 + op6/7 sequence. A
node opens a *new* directory connection later rather than keeping one alive. Do not
model it as long-lived.

#### The msgtype phase rule (abs 30) — supersedes the §4(d) note for connection frames

`msgtype` tracks the **connection's** phase, not the node's:
- **`0x5b`** while a connection is being established — the joiner's own `SCS$DIRECTORY`,
  `MSCP$DISK` **and** `VMS$VAXcluster` CONNECT-REQUESTs are all `0x5b`, as are its
  op 3 confirms and its first directory lookups.
- **`0x4b`** once traffic is data-phase — later lookups on an established directory
  connection, MSCP commands, and all 190-byte SYSAP config frames.

The acceptor answers in the phase it has reached, so a `0x5b` request is commonly
answered with a `0x4b` echo/response. Sending a connect as `0x4b` when the peer expects
an establishing connection, or a post-establishment lookup as `0x5b`, causes the member
to **echo (op 1) but never accept (op 2)** — the signature failure mode.

#### Connect-class at abs 22 (`sca[8:10]`)

Connection-control frames carry **`0x0001`** here. (`0x03e8` appears in some
fresh-formation captures and is *not* accepted by an established member — a member that
receives it echoes and stalls.) The same field carries the node-incarnation echo on
`0x41` START frames (§4i); it is phase-dependent, not a single global constant.

#### Ordering invariant

The joiner's connects are **pipelined on one shared, contiguous `send_seq`** — it issues
the next connect/lookup before earlier responses arrive. It is *not* stop-and-wait. What
is strictly ordered is the **confirm**: a connection must be confirmed (op 3) before the
initiator's next CONNECT-REQUEST will be accepted.

#### The VC confirm gates the ENTIRE membership dialogue (GROUNDED, `vms-760`)

The op-3 confirm on the joiner's own `VMS$VAXcluster` VC is not merely
housekeeping for the *next* connect — it is what makes the peer's connection
manager treat the VC as usable at all. Reference ordering:

```
frame 132  ss=10  J->M  CONNECT-REQUEST  VMS$VAXcluster
frame 136  ss=11  M->J  op 2 ACCEPT
frame 139  ss=13  J->M  op 3 CONFIRM            <-- load-bearing
frames 142/143 ss=14,15 J->M  the 190-byte MODEL+PARAMS config
frames 145/146          M->J  the peer's reciprocal config      (+0.3 ms)
frame 162  +0.9543      M->J  the peer opens its OWN SCS$DIRECTORY connect back
```

**Omit frame 139 and everything from 145 onward disappears.** Observed
directly (`d94-disc2.pcap`, `d94-disc3.pcap`): a joiner that sent only op 10 on
its VC Con.ID got its config burst **bound and silently discarded** — no
reciprocal config, and — the diagnostic tell — **no member-initiated connections
back**, which a real member opens to a real joiner within ~15 ms. Restoring the
confirm restored both immediately.

> This is worth stating plainly because the failure looks nothing like a missing
> acknowledgement: every frame the joiner sends is accepted at the SCS layer,
> the Con.ID pair binds, `SHOW CLUSTER` shows the node as `NEW`, and the peer
> simply never speaks again. It is easy to misread as an admission policy
> decision taken *above* SCS. It is not — it is a half-open connection.

### 4(n) MSCP disk-client command layer (GROUNDED, same capture)

§4(e) decoded MSCP request/response *framing*; this is the client command sequence a
joiner must execute, and the MSCP message layout carried in the op-10 body at **abs 72**.

| body offset | field |
|-------------|-------|
| `[0:2]` | class token — `0x0002` SET CONTROLLER CHARACTERISTICS, `0x0001` GET UNIT STATUS |
| `[2:4]` | message id — increments per command, **echoed verbatim** by the server |
| `[4:6]` | unit word (GUS: the unit being queried; END: the unit returned) |
| `[8]` | MSCP opcode — `0x04` SET CTLR CHAR, `0x03` GET UNIT STATUS; **END response = opcode \| 0x80** |
| `[9]` | flags |
| `[10:12]` | modifiers on a command (`0x0001` = NEXT-UNIT); **MSCP status** on an END |

MSCP status majors observed: `0x0000` SUCCESS, `0x0004` UNIT AVAILABLE, `0x0003` UNIT
OFFLINE.

**The client sequence (complete — this is all a joiner does):**
1. `SET CONTROLLER CHARACTERISTICS` **twice** → END `0x84`, status SUCCESS.
2. `GET UNIT STATUS` walk with the NEXT-UNIT modifier. **The first command seeds unit
   word `0x0001`**; each subsequent command uses *the previous END's returned unit word
   + 1*. Each real disk answers status AVAILABLE; the walk ends when an END returns
   status **OFFLINE**, which is the end-of-list terminator, not an error.
3. Nothing else — there is **no MSCP INIT handshake** before it (the SCS
   connect/accept/confirm subsumes it) and **no ONLINE or READ** after it. The joiner
   never mounts or reads the disk during the join.

Seeding the first GUS with unit `0x0000` makes the server answer OFFLINE immediately and
the enumeration terminates after one exchange — a silent, plausible-looking failure.

### 4(o) The joiner's category-0x01 membership dialogue, end to end (GROUNDED, `vms-760`)

§4(j) grounded the SYSAP envelope and the field map. This is the **order of
events** on an established join, and in particular *when* each of the joiner's
three config messages goes out — the part that decides whether admission starts.

| # | t (ref) | dir | cat | op | meaning |
|---|---------|-----|-----|----|---------|
| 1 | +0.9394 | J→M | `0x01` | `0x14` | model advertisement |
| 2 | +0.9394 | J→M | `0x01` | `0x01` | cluster parameters (VOTES, `"V7.3"`) |
| 3 | +0.9397 | M→J | `0x01` | `0x14`+`0x01` | the peer reciprocates in kind |
| 4 | **+5.8774** | J→M | `0x01` | **`0x02`** | **config/topology — this starts admission** |
| 5 | +5.8777 | M→J | `0x04` | `0x00` | peer ack (0.3 ms later) |
| 6 | +5.8804 | M→J | `0x01` | `0x03` | membership **COMMIT** request (`txn`,`cksum`) |
| 7 | +5.8806 | J→M | `0x81` | `0x03` | joiner echoes the token |
| 8 | +5.8808… | M→J | `0x01` | `0x05` | lock/resource-database rebuild requests |
| 9 | +5.8815… | J→M | `0x81` | `0x05` | joiner echoes each token |
| 10 | +5.8827… | M→J | `0x01` | `0x06` | burst, acked by the joiner with `0x04/0x49`,`0x04/0x00`,`0x04/0x02` |

**The initial burst is MODEL+PARAMS only — but `0x02` is deferred, not
omitted.** Sending `0x02` inside the initial burst leaves the peer silent
(grounded previously); never sending it leaves the dialogue permanently
half-finished.

**⚠ UPDATE 2026-08-01 (`vms-2f3`): the gap is not a fixed delay and it is not
idle.** It measures **1.44 s** (`af2-firsttimer`, VX3's rejoin at SCA 20170) and
**4.4 s** (`vax3-2to3`), so "~4.9 s" was one specimen, not a constant. **GROUNDED:
what the real joiner does in that window** (`af2-firsttimer` frames 20212–20243,
32 frames) is a complete **client run of its own**:

1. opens **its own** `SCS$DIRECTORY` connection — it does **not** reuse the
   member's — and confirms it;
2. looks up `MSCP$TAPE` and `MSCP$DISK` on it;
3. opens an `MSCP$DISK` connection (op 0/1/2/3);
4. runs 2× SET CONTROLLER CHARACTERISTICS, then the full GET-UNIT-STATUS
   NEXT-UNIT walk (10 command/END pairs);
5. tears the directory connection down;
6. *then* sends `op 0x02`.

So the rule is not "wait N seconds" — the joiner sends `op 0x02` when its own
disk-client discovery is finished. **OVMX implements none of steps 1–5** and
substitutes a `JOIN_CFG2_DELAY_MS` timer. Whether the run is a *gate* on
admission is **not** decidable from passive capture — `vax3-2to3#285` carries an
all-zero topology body and is acked in 0.3 ms, so the MSCP walk is not *encoded*
into `op 0x02`. But it is the largest ungrounded behavioural gap left between
OVMX and a real joiner.

**Do NOT "fix" this by matching the `op 0x02` ack-msg alone.** A real joiner's
admission `op 0x02` is `(smsg=3, amsg=2)` and OVMX's bundled one is
`(smsg=3, amsg=0)` — a shape that occurs 104× from OVMX and **0× in 196 real
specimens**, so the observation is correct. But `OVMX_PURE_SERVER=1` already
emits the reference shape (2-frame burst, deferred, coordinator-only, correct
`amsg`) and **the coordinator answers it not at all** — no ack, no COMMIT — where
the malformed bundled form draws an ack in 0.4 ms. Tested 2026-08-01, run `p1A`,
fresh identity, no code change. See `docs/HANDOFF-vms-2f3.md` §4c.2.

**Which VC carries it.** A member opens its own `VMS$VAXcluster` VC back to the
joiner **only if the joiner has not already opened one to it**. In the reference
VAX3 opened its own VC to VAX1 (so VAX1 reused it) but not to VAX2 (so VAX2
opened one, frame 208, and the whole commit dialogue rode VAX2's). Either way
the dialogue rides **one** VC per peer; answer on whichever the request arrived
on.

**Two body fields of the admission `0x02` are REPLAYED, not decoded** —
`body[10:12]` = `0x5041` and twelve `0x20` spaces at `body[40:52]`
(frame 285). They are **not constants**: the same node's later `0x02`
(frame 8658) carries `0x0004` and binary data in those places. One specimen of
one variant; see §5(z).

### 4(p) The cluster-wide state-transition BARRIER (GROUNDED, `vms-760`)

After the add-member commit the coordinator runs a **12-step barrier**. It is not
joiner-private work: the coordinator runs the same dialogue with **every** member
and releases step *N* to nobody until **all** of them have sent their step-*N*
request.

#### Does the step count scale with membership? NO — the FRAME count does (GROUNDED, `vms-584`)

This mattered enough to be measured rather than assumed: if the step count grew
with cluster size, OVMX would strand the first larger cluster it met and nothing
in the earlier evidence would have warned us. A census of **41 captures** finds
**40 transitions, of which 30 ran a barrier to completion**, and:

- **Every completed barrier tops out at exactly step 12** — indices 1…12, no
  gaps, no 13. M=2 (16 barriers), M=3 (11), M=4 (3): **zero variance.** The four
  partial barriers all stop at an OVMX defect, not at a protocol boundary.
- **The frame count scales exactly: `#0x0b = #0x0c = 12 × (M−1)`, in 30 of 30.**
- The topology is a **star**. Every member exchanges `0x0b`/`0x0c` only with the
  coordinator; members never barrier with each other.
- It is **one cluster-wide lock-stepped barrier**, not `M−1` independent runs:
  `0x0c#N` never precedes the last `0x0b#N` — **0 violations out of 12 steps in
  every transition**.
- A class-`0x03` (remove-a-failed-node) transition runs the **same 12 steps and
  the same `12 × (M−1)` law**; only its opening differs (§4(r)). A class-`0x04`
  self-departure emits its `op 0x0a` and starts **no barrier at all**.

> **Implementation consequence, and it is the reassuring one:** a joiner or an
> ordinary member always sends exactly **12** `0x0b` frames and receives exactly
> **12** `0x0c`, *regardless of cluster size*. Its cost is flat and it does not
> need to know M. The `12 × (M−1)` scaling is entirely the **coordinator's**
> obligation — which OVMX will inherit only at T3, when it can be elected.
> The one member-side effect of a large cluster is **latency**: the coordinator
> holds `0x0c#N` until the slowest member reports, so per-step wait grows with M.
> **Do not time out on a step merely because it is slow.**

> **No peer ever announces the step total.** An exhaustive scan of all 54 `op
> 0x09` opens and all 65 class-`0x02` `op 0x0a` GOs finds no byte equal to 12 or
> 13, and no LE u16 equal to 12, at any constant offset. **12 must be a
> constant** — but instrument for a mismatch rather than trusting it, because…

> **…the honest bound on this evidence is FOUR MEMBERS.** The largest cluster in
> the entire library is VAX1+VAX2+VAX3+OVMX (bitmap `0x1e`), and OVMX is one of
> the four; the largest all-reference-VAX cluster is **three**. Treat "12" as
> GROUNDED-to-M=4. Nothing above 4 is grounded, and `vms-584` item 1 exists to
> extend it.

| step | dir | cat | op | note |
|---|---|---|---|---|
| open | M→J | `0x01` | `0x09` | `body[16:18]=0x0240`; carries the transition **epoch** at `body[12:16]` and the membership **bitmap** at `body[55]` |
| | J→M | `0x81` | `0x09` | echo + **three** mutations (below) |
| go | M→J | `0x01` | `0x0a` | `body[16:18]=0x0260`. **Never answered.** Start the barrier at N=1 |
| ×12 | J→M | `0x01` | `0x0b` | epoch at `body[12:16]`, step N (LE u32) at `body[16:20]` |
| | M→J | `0x81` | `0x0b` | the coordinator's ack — **not** the release |
| | M→J | `0x01` | `0x0c` | release of step N. **Never answered.** N<12 → N+1; N=12 → complete |

**GROUNDED across 6 joins / 4 clusters / 3 different joiner nodes**: exactly 12
steps, indices 1…12, and the `0x0240`/`0x0260`/`0x0210` tags are invariant. The
count 12 is the only termination signal — `0x0c#12` is byte-identical to earlier
releases apart from its index.

**The gating is directly observable.** At step 5 the reference coordinator held
the joiner's completed request for **89 ms** and released `0x0c#5` to *both*
members 0.8 ms after the last member caught up. `0x0c#N` never precedes `0x0b#N`
anywhere — 72 ordered pairs, 0 residuals.

> **A joiner that ignores the barrier does not merely fail to join — it breaks
> the cluster.** The coordinator's barrier stays permanently one member short, so
> the transition times out, `%CNXMAN, aborting VAXcluster state transition` is
> logged, and the healthy members are dropped. Observed twice before this was
> implemented.

**Notifications carry `txn=0` and are NEVER answered.** `op 0x0a` and `op 0x0c`
get no response of any kind — no `0x8a`/`0x8c` exists in any capture, and no
cat-`0x04` ack is attributable to them. Only the `op 0x06` burst is acked. An
`op 0x0a` whose `body[16:18]` is not `0x0260` (e.g. `0x0460`, seen on a running
cluster) is **not** a barrier start.

#### The `0x81` echo takes THREE mutations — corrects §4(j)

§4(j) says a response echoes the request body with the response bit set. It is
echo **plus three** edits, verified on `0x03`, `0x05` and `0x09` over 6/6
responses in 5 captures with 3 different responder nodes:

```
body[8] |= 0x80        response bit
body[18]  = 0x01       response marker    (NOT on op 0x0f -- see §4(r))
body[55]  = 0x00       cleared            (op 0x09 only)
```

> **`body[55]` is not a "mutation slot" — it is the coordinator's MEMBERSHIP
> BITMAP, and the responder is refusing to assert it** (GROUNDED, `vms-584`).
> `popcount(body[55])` of the `op 0x09` open **equals the post-transition member
> count in 54 of 54 opens, zero residuals**; bit *k* is the member holding CSID
> index *k*, and bit 0 is never set. The three values this section originally
> recorded as "held `0x0e` / `0x0a` / `0x06`" are exactly the M=3, M=2 and M=2
> bitmaps.
>
> | bmap | bits | M | context |
> |---|---|---|---|
> | `0x06` | 1,2 | 2 | fresh 2-node formation |
> | `0x0a` | 1,3 | 2 | a node's 2nd incarnation (slot 3) |
> | `0x0e` | 1,2,3 | 3 | 3-node |
> | `0x12` | 1,4 | 2 | 3rd incarnation (slot 4) |
> | `0x16` | 1,2,4 | 3 | a node joins after the slot-3 holder departed |
> | `0x1e` | 1,2,3,4 | 4 | VAX1+VAX2+VAX3+OVMX |
> | `0x22` | 1,5 | 2 | 4th incarnation (slot 5) |
>
> Slot allocation is self-consistent across independent runs: `af2-firsttimer`
> shows `0x0a → 0x12 → 0x22` as one node rejoins three times taking slots 3, 4, 5,
> and a vacated slot is **not** reused by the next joiner.
>
> **A joiner can therefore read the expected barrier-participant set out of the
> open it receives.** Two cautions: a class-`0x03` removal has **no `op 0x09` at
> all** (it starts directly at `op 0x0a` / tag `0x0360`) and so carries no bitmap;
> and one byte holds only 8 slots while the library already reaches slot 5.
> `body[52:55]` and `body[56:60]` are all-zero in every specimen, so the field is
> certainly **wider than a byte**, but its extent and endianness are UNDETERMINED
> — a BE u32 at `body[52:56]` fits the data as well as an LE map based at
> `body[55]`. **Do not assume 8 slots.**

#### Category is per-SYSAP, and the response SHAPE is per-category

`cat`/`op` are namespaces scoped to the SYSAP the Con.ID resolves to; the same
numbers mean different things on `SCA$TRANSPORT` and `VMS$VAXcluster`. Response
shape likewise does not generalise:

- **cat `0x01`** — echo the whole body (+ the three mutations).
- **cat `0x06`** — closes the transaction. Carry only the `(txn,checksum)`; send
  your **own** node-parameter block, the same one carried in the `op 0x01`
  PARAMS message (`body[72:76]=0x10`, `body[76:80]=0x01`,
  `body[88:96]="V7.3    "`). **Echoing this request's payload bugchecks the
  peer** — it carries that peer's live Con.IDs and cluster id, and reflecting
  them back produced a fatal `INCONSTATE, Inconsistent I/O data base`.
- **cat `0x02`** (DLM) — during the join the coordinator replays lock-resource
  records as token-correlated transactions **interleaved with the barrier**, and
  gates the next step on them being answered. Five unanswered cat-`0x02`
  requests froze the barrier at step 5.
  **`op 0x0d` is the ONLY cat-`0x02` opcode that occurs during a join** (216/216
  in the reference), and its response is now **GROUNDED to an unusual degree**:
  the recipe below reconstructs **1367 of 1367** real responses byte-for-byte,
  from four responder nodes across two captures, with zero residuals.

  ```
  memcpy(resp_body, req_body, 132);      /* VERBATIM echo            */
  resp[0:2] = own SYSAP send-msg#        /* envelope                 */
  resp[2:4] = ack of the peer's send#    /* envelope                 */
  resp[8]  |= 0x80                       /* 0x02 -> 0x82             */
  resp[34]  = 0xf9                       /* MANDATORY, unconditional */
  /* everything else -- txn/cksum, opcode, body[12:16], the L1 region
     and the resource name -- echoed byte for byte                   */
  ```

  `body[34]` is written **unconditionally**: requests carried `0xf9`(209),
  `0x00`(3), `0x20`(2), `0x72`(1), `0xbc`(1) and *every* response carried `0xf9`,
  landing mid-ASCII in two specimens — a fixed-offset stamp, not a payload field.
  INFERRED to be a per-opcode result code; `op 0x01/0x07/0x15` use `0xfa`.

  **Request layout (GROUNDED):** `body[12:14]=0x0001` and `body[14:16]=0x0003`
  invariant · `body[16]` = L1 length · `body[47]` = **resource-name length** ·
  `body[48 : 48+len]` = the **lock RESOURCE NAME** in ASCII + binary sub-key.
  Observed verbatim: `"F11B$aSYSDSK1     "`, `"CACHE$cmSYSDSK1     "`,
  `"VCC$vSYSDSK1     "`, `"SYS$_$2$DUA0:"`, `"SYS$SYS_ID"` — the documented
  Files-11 / extent-cache / VCC namespaces (§4(f)).

  > ⚠ **DO NOT apply the cat-`0x01` mutations here.** `body[18]` is the 2nd byte
  > of the L1 region and **`body[55]` is the 8TH BYTE OF THE RESOURCE NAME** for
  > every observed length (13–24). OVMX applied both and shipped
  > `"CACHE$cmSYSDSK1"` as `"CACHE$c\0SYSDSK1"` on all eight replies; VAX1 and
  > VAX3 took a fatal **`LOCKMGRERR`**. The in-capture control is decisive:
  > across the same milliseconds VAX1 and VAX3 exchanged the *same* records with
  > each other correctly and neither crashed. Specimen:
  > `ovmx-760-lockmgrerr-20260730.pcap`.

  > **The plausible-sounding theory that was WRONG.** "A joiner holds no locks,
  > so echoing a rebuild record asserts lock state it does not have" was carried
  > for three sessions and is false. The echo returns the **coordinator's own
  > record** with a result code and claims nothing — which is exactly why a
  > lock-less joiner answers all 216. Refusing them instead pins the barrier at
  > step 5 forever: the coordinator **retransmits each unanswered record up to
  > 3×** (measured, `ovmx-760-dlm-refused-20260730.pcap`). Step 5 *is* the
  > lock-rebuild barrier step — in the reference it is held for 89 ms while 216
  > of these transactions run.

#### Residue: several "fields" are uninitialised buffer contents

`op 0x02`'s `body[10:12]` and `body[40:52]`, and the varying opcode of a
cat-`0x04` ack, are **not data**. 9 of 12 genuine `op 0x02` specimens carry zeros
there and are acked identically; the outliers hold printable digraphs (`"AP"`,
`"IS"`) and ASCII spaces. A real cat-`0x04` ack reads
`04 49 "IR_LOOKUP  SCS$DIRECTORY"` — a leftover `"DIR_LOOKUP SCS$DIRECTORY"`
whose first two bytes were overwritten by the category and opcode. An
implementation should send zeros; do not reproduce another implementation's
uninitialised memory.

#### Admission is single-coordinator — and the peer must be THE COORDINATOR

The joiner sends its `op 0x02` to **exactly one** peer, which relays the new node
to the rest (`op 0x12`) and then runs the barrier across all members.

**A non-coordinator peer SILENTLY DISCARDS `op 0x02`.** GROUNDED, `d94-e15`
byte-verified: all three members received a byte-identical `op 0x02` inside
400 ms; VAX1 and VAX2 each answered only a cat-`0x04` ack and did nothing further
— VAX1 had a **383 ms head start** — while VAX3 relayed `op 0x12` to VAX1 1.0 ms
later and drove `0x03`/`0x05`/`0x09`/`0x0a` to **both** peers. The reference
joiner behaves identically: it sent its `op 0x02` to **VAX2, not VAX1**
(frame 285), and VAX2 relayed to VAX1 in 0.3 ms (286). So the reference picks
**the coordinator**, not "one peer arbitrarily".

> **CORRECTION to the previous text.** Fan-out does **not** start N competing
> transitions. In `d94-e15` exactly **one** transition ran, started by VAX3;
> the other two contributed only acks, and no abort occurred. The earlier
> "competing transitions / cluster abort" reading is **not reproduced**. Fan-out
> appeared to work only because it happened to include the coordinator.

*How* a joiner identifies the coordinator is **NOT grounded**. No wire-visible
coordinator flag was found: the coordinator is a **zero-vote** node in both
specimens, and every field distinguishing VAX3 from VAX1/VAX2 in our lab is
all-zero on the reference's coordinator VAX2 — so those are node-local
properties, not a role marker. The only predicate surviving both specimens is
**highest DECnet node number** (VAX2 of {VAX1,VAX2}; VAX3 of {VAX1,VAX2,VAX3}),
which is confounded with "highest SCSSYSTEMID" and "last to have joined".
OVMX implements that observable and labels it INFERRED (`cm_pick_coordinator`).

#### Never answer a (category, opcode) pair you have not grounded

Once the relay works, the **non-coordinator members open their own
token-correlated transactions with the joiner**, carrying opcodes that never
appear in the pre-relay dialogue — `0x12`, `0x0f`, `0x08`, `0x00` were all
observed (`ovmx-760-relay-crash-20260730.pcap`).

> ⚠ OVMX answered every one of them with the cat-`0x01` full-body echo and
> **crashed two real VAXes**: VAX3 `INCONSTATE, Inconsistent I/O data base` and
> VAX1 `INVEXCEPTN, Exception while above ASTDEL or on interrupt stack`. These
> request bodies carry the **peer's own live Con.IDs and cluster id**; echoing
> one reflects that peer's I/O structures back at it. It is the same failure as
> generalising the cat-`0x01` echo to cat-`0x06`.

The rule is an **allowlist, never a default**: answer only (category, opcode)
pairs grounded in the reference; for anything else send **nothing** and log it.
Silence is the safer failure — but not a free one. A joiner that fails to answer
something the coordinator gates on strands the transition, which times out and
drops healthy members. An unanswered pair is a gap to close, not a resting state.

### 4(q) After the barrier — what makes a node a MEMBER (GROUNDED, `vms-760`)

**There is no "you are now a member" message, and no joiner-emitted field flips.**
Membership *follows from the transition completing*. Grounded three ways:

- Every non-DLM message in the 500 ms after `op 0x0c`#12 recurs elsewhere in the
  capture; none is unique to the transition. `cat 0x06`/`0x86 op 0x00` is a
  recurring member poll (~1/s, and one occurs *before* the barrier);
  `cat 0x02 op 0x02` is an **OPCOM broadcast relay** (ASCII `"OPCO"` at
  `abs 82 = 0x00bb`), not a membership message and never answered.
- The joiner's HELLOs are **byte-identical across the boundary** except the
  padded-vs-plain framing (abs 14–15), the §4(a).1 channel-verify oscillator
  (abs 30) and a free-running tick (abs 96–100). Incarnation (`abs 92`) and
  poller sweep (`abs 128`) are unchanged. Sampled over +390 s.
- The joiner's **CSID is already on the wire ~160 ms *before* the barrier opens**
  (frame 297), and it never appears in a HELLO (0 hits in 741 sampled).

> `SHOW CLUSTER`'s `NEW` → `MEMBER` is **member-side state produced by the
> transition**. There is nothing extra for a joiner to emit to be *rendered* as
> `MEMBER`.

**`op 0x0c`#12 must NOT be answered**, exactly like every other release. No
`0x81`/`0x0c` exists anywhere in the capture, and `op 0x0c` carries `txn=0`, so
there is nothing to correlate. The joiner's next frame is a **standalone
`cat 0x04` credit ack** — for steps 1–11 that ack rode piggyback on the next
`op 0x0b`; after step 12 there is no next step, so it goes out alone. Answering
the release would invent a message VMS never sends (Rule 10).

**Ongoing MEMBER obligations** (a node that stops meeting them may be dropped):
HELLO cadence is *unchanged* (~2.3 s); **SCS credit return (`mt 0x48`) becomes
continuous** (~0.75 frames/s combined, vs a handful during setup); members
re-issue an `MSCP$DISK` CONNECT-REQUEST every ~10 s **indefinitely**; fresh
`SCS$DIRECTORY` connections open post-join; `SCA$TRANSPORT` is opened once by a
peer; and `mt 0x7b` (len 204) frames appear on the `VMS$VAXcluster` Con.ID pair
— **payload undecoded**; accept and ack at the SCS level, answer nothing above it.

**What a LOCK-LESS member actually receives — MEASURED, not inferred.** Over a
7-minute OVMX membership with full CM tracing, the *entire* post-`XITDONE`
inbound inventory was: **5×** `cat 0x06 op 0x00` (answered `0x86`), **1×**
`cat 0x04 op 0x00` (notification, correctly unanswered), **1×** `cat 0x02
op 0x01` (refused, see §4(p) — free). **Zero** `cat 0x02 op 0x12`.

> The reference joiner answers 411 `cat 0x02 op 0x12` and 138 `op 0x01` post-join
> because it is a real node **with lock activity**. A node holding no locks never
> provokes them. So the reference's post-join answer table is a **superset scoped
> to a lock-holding member**, not a checklist every member must meet — and the
> steady-state obligations of a lock-less member are far smaller than it implies.
> Do not implement the whole table on the strength of the reference alone;
> measure what actually arrives first.

**The post-barrier DLM burst is a consequence, not an obligation** (INFERRED):
membership is reached at the release, before any of it; all 338 cat-`0x02` frames
in the window are joiner-initiated and every resource is Files-11/MOUNT
(`MOU$_`, `F11B$*`, `VCC$v`, `DMT$_`), never `CNX$`/quorum. A lock-less OVMX
likely needs to emit **no** outbound DLM to reach or hold `MEMBER`.

### 4(r) The connection-manager ROLE SLOT and TRANSITION CLASS — `body[16]`, `body[17]` (GROUNDED, `vms-e4b`)

Census over 26 captures in both capture trees, all 204-byte `0x6007` frames.
`body[0]` = abs 72, so `body[16:18]` = abs 88:90.

**`body[16]` is a stable ROLE SLOT.** It partitions the category-`0x01`
connection-manager opcodes with zero residuals:

| role | opcodes | meaning |
|---|---|---|
| `0x10` | `0x12` (and the coordinator's `0x81/0x0b`) | announce / relay |
| `0x20` | `0x03`, `0x05`, `0x06`, joiner's `0x02` | commit / lock push |
| `0x30` | `0x0f` | the extra step of a class-`0x03` transition |
| `0x40` | `0x09`, `0x08`, `0x0d` | **transition-open** |
| `0x60` | `0x0a` | barrier GO |

**`body[17]` is the TRANSITION CLASS — not a generation.** The epoch is
`body[12:16]` (LE u32), where §4(j) and §4(p) already put it.

| class | transition | has `0x05`/`0x06`? | has the 12-step barrier? | opened by |
|---|---|---|---|---|
| `0x02` | ADD a member | yes | **yes** | `op 0x09`, tag `0x0240` |
| `0x03` | REMOVE a failed member | no | **yes** | `op 0x08`, tag `0x0340` |
| `0x04` | a node announces its OWN departure | no | **no** | `op 0x0d`, tag `0x0440` |

A class-`0x04` departure is `0x12` → `0x03` → `0x0d` → `0x0a` and then nothing;
an `0x81/0x0b` carrying class `0x04` occurs in **no** capture.

The `op 0x0a` tag is `(class << 8) | role`, i.e. `0x0260` / `0x0360` / `0x0460`.
Two of the three start a barrier.

**Two things this REFUTES, both of which had been believed:**

1. *`body[16:18]` is `<generation><role>`* — no. `af2-firsttimer-established-20260728.pcap`
   contains six successive transitions whose epoch runs **3, 4, 6, 7, 9, 11**
   (monotone) while `body[17]` runs **`0x04, 0x04, 0x02, 0x04, 0x02, 0x02`** — it
   goes *down*. Frames 1826/1828/1830/1832, 2638/2986/2989, 19431/19433/19435/19440,
   20247/20595/20598, 33670/34018/34021.
2. *The transition-open opcode varies with generation (`0x09` gen-2, `0x08` gen-3,
   `0x0d` gen-4)* — no. That triple is the three **classes**, which merely look
   like consecutive small integers. The same capture runs three successive ADD
   transitions at epochs 6, 9 and 11 and opens every one with `op 0x09` / tag
   `0x0240`; `0x81/0x09` is 54/54 library-wide.

**The role slot must NOT be used as the response key.** Role `0x20` alone spans
`op 0x03`/`0x05` (full-body echo), `op 0x06` (answered with a cat-`0x04` ack and
**never** an echo — 7882 frames) and the joiner's own `op 0x02`. Keying on the
role would fire a 132-byte echo at the entire `op 0x06` burst. Role tags do not
exist at all on categories `0x02` and `0x06` — the two categories that have
already bugchecked real VAXes. **The response key is `(SYSAP, category, opcode)`;
the role slot is a corroborating cross-check and the place the class is read
from.**

**And an opcode alone is not an identifier either.** `op 0x0d` is the
class-`0x04` transition-open in category `0x01` and the DLM lock-resource rebuild
record in category `0x02`, and a single join carries **216** of the latter.

**Response recipes by opcode** (cat `0x01`), each a verbatim body echo plus
`body[8] |= 0x80` and then:

| opcode | extra mutations |
|---|---|
| `0x03`, `0x05`, `0x08`, `0x09`, `0x0d` | `body[18] = 0x01`; `body[55] = 0x00` on `0x09` only (§4(p)) |
| `0x0f` | **none** — `body[18]` is *echoed*, not forced |
| `0x12` | `body[18] = 0x01`; `body[17]` = the responder's own current class; `body[20:24]` = LE u32 copy of the request's `body[12:16]` (the epoch) |
| `0x06` | never `0x81` — answered with cat-`0x04` acks |
| `0x0a`, `0x0c` | never answered (`txn = 0`) |

> The `0x0f` row reconciles two censuses that looked contradictory. One found a
> single real `0x0f` response with `body[18] == 1`; the other found six that leave
> it `0`. The first specimen's **request** already carried `1` — so both are
> echoes, and neither is a node setting the byte.

**Not accounted for:** what `op 0x0f` (role `0x30`) *means*, and why one responder
additionally flipped `body[20]` `0x0e`→`0x1e` where another answering the same
byte did not. `scs-node-leave.pcap` contains no transition frames at all and is
not usable as a departure specimen; the `af2-*` captures are.

### 4(O) What an OVMX daemon must put on the wire to be admitted — the capability bracket (GROUNDED live, `vms-70e2`)

**What this grounds and why.** `vms-70e2` set out to prove a same-identity
REJOIN end to end and never reached the question, because its POSITIVE CONTROL
failed: an SCSD built from `work/vms-187-closure` cannot complete a FIRST join
either. This section records what that binary does and does not transmit,
measured against a binary that joins, so the gap is a wire fact rather than a
branch comparison. Nothing here is a new VMS field — every offset used is
already grounded above.

**Method.** `tools/cluster/scs_join_capability_measure.py` (re-runnable;
PASS/FAILs every figure against a checked-in table). Three runs on ONE lab-2
pod, `vaxlab-4`, a pod that had never seen an OVMX node, inside twelve minutes
on 2026-08-05, with only the BINARY varying. Guardrail 18: each identity is
proven from the capture bytes, not from a log. Guardrail 20: the closing
control ran AFTER the joining one, so "the lab stopped admitting anyone" is
excluded. Frames are counted between the OVMX MAC and a peer only; peer↔peer
traffic is not counted, because the two VAXes talk to each other at the same
rate whatever OVMX does.

| run | binary | identity on the wire | CM 190-byte tx | CM 190-byte rx | OVMX ACCEPT_RSP | verdict |
|---|---|---|---|---|---|---|
| `A1` | `work/vms-187-closure` | `OVMXA1` | **3** | **0** | **0** | NOT JOINED |
| `A0` | `worktree-760-active-directory` | `OVMXA0` | **514** | **583** | **2** | **JOINED, t+13 s** |
| `A3` | `work/vms-187-closure` | `OVMXA3` | **3** | **0** | **0** | NOT JOINED |

The 190-byte class is §4(d)'s fixed SCS class, the one §4(g)/§4(j)'s membership
dialogue rides. The failing binary emits its three-frame add-member burst
(op `0x14`/`0x01`/`0x02`) to one peer at t+2.85 s and then transmits nothing
further in that class for the rest of the run, and **the peer answers with
none**. The joining binary sustains the dialogue in both directions to t+25.7 s
and is a member by t+13 s.

**The connection-control repertoire, at payload `[46:48]` (§4(h)(1a)).** OVMX-
sourced frames only, over the grounded classes `{58, 62, 66, 110}`:

| run | message types OVMX transmitted |
|---|---|
| `A1` | `0` CONNECT_REQ ×1 · `1` CONNECT_RSP ×1 · `2` ACCEPT_REQ ×1 · `6` DISCONNECT_REQ ×2 |
| `A0` | `0` ×2 · `1` ×6 · `2` ×2 · **`3` ACCEPT_RSP ×2** · `4` REJECT_REQ ×4 · `6` ×2 · `7` DISCONNECT_RSP ×2 · `9` ×2 |
| `A3` | `0` CONNECT_REQ ×1 · `1` CONNECT_RSP ×1 · `2` ACCEPT_REQ ×1 · `6` DISCONNECT_REQ ×2 |

**The one named frame.** In `A1` and `A3` the peer's `ACCEPT_REQ` (type `2`)
arrives on OVMX's own `VMS$VAXcluster` connection at t+2.852 s (Con.ID pair
`4F580002`/`B74F000D`), the `vms-dd5` state machine resolves it to
CONNECT ACK → OPEN with the action *send ACCEPT_RSP*, and the daemon prints
`SCSD-W-CONNNOACT … OVMX has no builder for it -- nothing was sent` and closes
the run with `actions-required-but-not-emitted=1`. The joining binary sends that
frame twice, one per peer, at t+3.412 s.

<a name="sec4o-refutation"></a>
**⛔ AND THIS REFUTES A LOAD-BEARING SOURCE COMMENT.**
`src/vmsscs/include/scs_sdir.h` justified returning a listening CDT to LISTEN on
*emit* rather than on the response — a deliberate deviation from p. 2-50 — partly
on the ground that
<!-- REFUTED-QUOTE-BEGIN -->
*"OVMX HAS NEVER OBSERVED AN ACCEPT_RSP ADDRESSED TO ITSELF … A listening CDT
that waited for a frame that never comes would wedge in CONNECT RECEIVED with no
timeout"* — **REFUTED by run `A1` below; quoted only to kill it.**
<!-- REFUTED-QUOTE-END -->
It has. In `A1`, at t+2.851 s, VAX1 answers OVMX's own `ACCEPT_REQ`
on the `SCS$DIRECTORY` connection with a 62-byte type-`3` `ACCEPT_RSP` addressed
to the OVMX MAC and carrying OVMX's own Con.ID pair `4F580007`/`B751000C`, and
the daemon logs the matching `ACCEPT SENT --RCV_ACCEPT_RSP--> OPEN` transition.
The same frame is present in `A3`. The latency is **0.5 ms**, so "a frame that
never comes" is not the situation. **The deviation may still be the right design
— a synchronous ACCEPT has no one to wait for — but that premise is dead and may
not be restated.** `tests/vmsscs/test_scs_join_capability_figures.py` reds if it
comes back.

**Explicit non-claim, and it is the important one.** This does **not** establish
that the missing `ACCEPT_RSP` causes the failed join. Both differences —
the unsent `ACCEPT_RSP` and the absent CM dialogue — are present together in
both failing runs and absent together in the joining one, and no run isolates
either. The CM layer that drives §4(g)'s dialogue is simply not present in the
failing binary at all, so there is nothing to attribute. Naming a cause here
would be the correlation §3 of the `vms-2f3` handoff exists to prevent.

---

#### 4(O.1) The integrated tree joins — the `vms-578` bracket (GROUNDED live, `vms-578`)

`vms-578` merged `work/vms-187-closure` (the SCA architecture) with
`worktree-760-active-directory` (the layer that joins). This is the acceptance
measurement, taken the same way as the bracket above, on the **same pod**
(`vaxlab-4`, lab-2) on **2026-08-05**, minutes apart, with the control
**between** the two runs of the binary under test rather than after them.

Identity is proven ON THE WIRE
(`strings -a <pcap> | grep -oE 'OVMX[A-Z0-9]{2}'`), not from a log. All three
runs used the **default** environment — no `OVMX_JOIN_SEQ`, no
`OVMX_PURE_SERVER`, no `OVMX_MSCP_SERVER`.

| run | binary | identity | CM 190 tx | CM 190 rx | ACCEPT_RSP | verdict |
|---|---|---|---|---|---|---|
| `B1` | `work/vms-578` (integrated) | `OVMXB1` | **509** | **575** | **2** | **JOINED, t+13 s** |
| `B3` | `worktree-760-active-directory` | `OVMXB3` | **513** | **579** | **2** | **JOINED, t+26 s** |
| `B2` | `work/vms-578` (integrated) | `OVMXB2` | **508** | **571** | **2** | **JOINED, t+13 s** |

Every arm reached `CLUSTER_NODES=3` with `XITDONE=1`. Compare the `A1`/`A3` rows
above — the same closure code, on the same lab, reaching `cm_190_rx=0` and
`accept_rsp_tx=0` and never joining. The merge is what closes that gap.

**What differs between the integrated tree and the `worktree-760` control, and
it is not nothing.** The control emits `1` CONNECT_RSP ×8 and `4` REJECT_REQ ×6;
the integrated tree emits ×2 and none, and emits one more DISCONNECT pair (×3
against ×2). Both join. The reduction is the `vms-7fe` p. 2-48 SDIR scan and the
`vms-561` ACCEPT service replacing `worktree-760`'s open-coded echo-per-connect:
one CONNECT_RSP per accepted connection instead of one per received frame, and
no speculative REJECT. **NOT CLAIMED: that the reduction is an improvement.** It
is a difference, both shapes are accepted by this peer, and nothing here
measures which one a different peer would prefer.

**The `B1` capture carries two identities**, `OVMXA0` and `OVMXB1`. `OVMXA0` is
residue — `VAX1` still held the `vms-70e2` `A0` run's CSB on this pod and names
it on the wire. `B2`, run a cycle later, is clean. Recorded rather than filtered.

**Not in scope and not claimed:** the rejoin (`vms-2f3`). Every run above is a
FIRST join by an identity that had never been admitted anywhere
(`no prior-admission sidecar` in each run log).

Re-derive every figure: `tools/cluster/scs_join_capability_measure.py`
(`EXPECTED_578`). Captures:
`vms578-{B1,B3,B2}-lab2-vaxlab4-20260805.pcap`.

**The two kill switches were RUN, not asserted (guardrail 23).** Same pod, same
runner, one extra arm each.

| switch | what it is supposed to gate | measured OFF | measured ON |
|---|---|---|---|
| `OVMX_CONNREQ_LEGACY_MSGTYPE=1` | msgtype at `SCA[16]` of the joiner's own VMS$VAXcluster CONNECT-REQUEST | `0x5b`, `0x5b` | `0x4b`, `0x4b` |
| `OVMX_MSCP_SERVER=1` | MSCP$DISK LISTEN + lookup answer + inbound connect accept | `SDIR listening=2`, `MSCP-SERVER-ACCEPTS-SENT=0` | `SDIR listening=3`, `MSCP-SERVER-ACCEPTS-SENT=4` |

The msgtype figures are read out of the capture (`d94-B2.pcap` /
`d94-B5.pcap`), selecting OVMX-sourced 110-byte SCA frames whose `[46:48]` is
message type 0 and whose `[62:78]` is `VMS$VAXcluster` — not from a log line.

**AND BOTH SETTINGS OF BOTH SWITCHES JOINED**, `CLUSTER_NODES=3`, `XITDONE=1`
(`B4`, `B5`). That is stated because it CONTRADICTS a tempting reading of the
merge: neither the `0x5b` msgtype nor the MSCP$DISK affirmative is, on this
lab, the thing that decides a first join. `worktree-760` grounded each on a
reference capture and both are kept; what this bracket measured is only that
the integrated tree joins, not WHICH of the merged pieces was necessary.
Isolating that is a separate experiment and no claim is made about it here.

---

#### 4(O.2) The rejoin question, ASKED AND ANSWERED — the `vms-449` bracket (GROUNDED live, `vms-449`)

**The answer is NO.** A returning OVMX identity is refused readmission by a
cluster it was admitted to minutes earlier, on the tree that joins, on a pod
that had never seen an OVMX node. This is the first time the question was put
to a tree capable of completing a first join: `vms-70e2`'s positive control
failed (§4(O)), `vms-578` fixed that (§4(O.1)), and nobody re-ran the triple.

**Method.** Nine runs on ONE lab-2 pod, `vaxlab-6`, scaled up fresh for this
bracket and verified `CLUSTER_NODES=2` before the first run, over 27 minutes
29 seconds on 2026-08-05 (A1 start to C4 end, with the last rejoin B4 at the
21-minute mark) — **one binary throughout** (a build of `main` at
`f874b04`), **default environment**, no switches. `SCSSYSTEMID`s 1500–1504.
Identity is proven from the capture bytes on every run, never from SCSD's log
(guardrail 18). A control sits **between** every pair of test runs, not merely
before and after them (guardrail 20). The last two runs are a matched pair
driven by `tests/lab/tools/csbwatch.sh`, which parks VAX1 in SDA and samples the
peer's CSB for our identity **while OVMX is still running** (guardrail 22).

| run | role | identity | CM 190 tx | CM 190 rx | peer DISCONNECT_REQ/RSP rx | verdict |
|---|---|---|---|---|---|---|
| `A1` | first join | `OVMXJ0` | **510** | **579** | 3 / 3 | **JOINED, t+13 s**, `XITDONE=1` |
| `B1` | rejoin #1 | `OVMXJ0` | **14** | **11** | **0 / 0** | **REFUSED**, `XITDONE=0` |
| `C1` | control | `OVMXK1` | 510 | 583 | 3 / 3 | JOINED, t+13 s |
| `B2` | rejoin #2 | `OVMXJ0` | **14** | **11** | **0 / 0** | **REFUSED** |
| `C2` | control | `OVMXK2` | 510 | 578 | 3 / 3 | JOINED, t+13 s |
| `B3` | rejoin #3 | `OVMXJ0` | **14** | **11** | **0 / 0** | **REFUSED** |
| `C3` | control | `OVMXK3` | 510 | 574 | 3 / 3 | JOINED, t+13 s |
| `B4` | rejoin #4 | `OVMXJ0` | **14** | **11** | **0 / 0** | **REFUSED** |
| `C4` | control | `OVMXK4` | 513 | 583 | 3 / 3 | JOINED, `completed` at t+9 s |

Four consecutive rejoins of one identity, four fresh identities admitted around
them. **Every arm — refused ones included — emits the `ACCEPT_RSP` whose absence
was the `vms-70e2` failure signature**, which is what establishes that the thing
measured here is the rejoin and not §4(O)'s first-join defect.

**The discriminator at the SCA connection-control layer, 4/4 against 5/5.** On
every joining run the peer sends `6` DISCONNECT_REQ ×3 and `7` DISCONNECT_RSP ×3
— it tears down its own `SCS$DIRECTORY` connection to us and then re-probes,
opening a *new* one (`0` CONNECT_REQ rises to ×9). **On every refused rejoin it
sends none of the three**: no DISCONNECT_REQ, no DISCONNECT_RSP, and `0`
CONNECT_REQ stays at ×2. OVMX is left holding both connections open and, at
exit, tears them down itself:

```
SCSD-W-CONNSTUCK, conid=…0002 VMS$VAXcluster parked in DISC SENT
SCSD-W-CONNSTUCK, conid=…0007 SCS$DIRECTORY  parked in DISC SENT
SCSD-I-CONNSTUCK, 2 of 2 in-use connection(s) parked off OPEN
```

against `0 of 0` on the control. This is the same divergence the `vms-2f3`
handoff located by hand in §4k.5 — *the peer's directory teardown never comes* —
now expressed as a message-type census and as a named state in a state machine
that did not exist when it was first found.

**The peer's own dialogue, off VAX1's console (`B4` vs `C4`).** Only the
timestamped `Node X (csid …)` OPCOM lines are used (guardrail 24):

| | fresh identity `OVMXK4` | returning identity `OVMXJ0` |
|---|---|---|
| | `received membership request` | `received membership request` |
| | `proposed addition` | `proposed addition` |
| | **`aborted VAXcluster state transition`** (+20 ms) | — *nothing, ever* |
| | *second* request 6.2 s later → `proposed addition` | — *no second request* |
| | **`completed VAXcluster state transition`** (+170 ms) | — |

The cluster neither completes **nor aborts** the transition it opened for a
returning identity; the CLUB is still `quorum,transition` at T+130 s. And the
peer's CSB for our identity tells the same story from the other side:

| sample | fresh `OVMXK4` | returning `OVMXJ0` |
|---|---|---|
| T‑PRE | `SCSNODE … not found` | `09 wait` `long_break,status_rcvd,send_status`, CSID `00010006` |
| T+5 s | `01 open` `status_rcvd`, CSID `00000000` | `01 open` `status_rcvd`, CSID `00000000` |
| T+10 s | **`01 open` `member,selected,status_rcvd`, CSID `00010007`** | `01 open` `status_rcvd`, CSID `00000000` |
| T+125 s | (member) | **unchanged — `01 open`, CSID `00000000`** |

Both identities reach the *same* intermediate CSB state. The fresh one advances
out of it in 5 s; the returning one never does.

**⛔ THE p. 2-21 REFRESH PATH IS ELIMINATED as the mechanism here.** `vms-17f`
made `SCS_OPEN_EXISTING_REFRESHED` reachable and it was the standing candidate
for what would let a returning identity back in. It did not fire: SCSD's own
`PB-OPEN:` summary reads `new-sb=2 refreshed=0 existing-sb=0` on **every** run
of this bracket, joining and refused alike, with `PEER-DEPARTURES=0`. The branch
is about OVMX refreshing an SB for a *peer* that departed and returned, not
about a peer refreshing OVMX, and no peer departed in any of these runs. It is
symmetric across the discriminator and therefore cannot be it.
(`tests/vmsscs/test_scsd_wire.c`'s `test_rejoin_reaches_the_p221_refresh` is the
positive control that the counter is live and can read non-zero.)

**Explicit non-claims.** (1) The missing peer DISCONNECT pair is **not** shown
to *cause* the refusal — it is the peer's own behaviour, and what it responds to
is unknown; naming it a cause is exactly the correlation §3 of the `vms-2f3`
handoff exists to prevent. (2) Nothing here isolates *which* byte of the
returning identity's traffic the peer keys on; §4M.16 established that nothing
OVMX transmits differs by one non-per-run byte from a successful join, and this
bracket does not revisit that. (3) `type 9` ×4 / `type 8` ×4 on a rejoin against
×2 / ×2 on a join is recorded as a figure and is **not** interpreted — the 8/9
pair is undecoded (§3 of the followup design note).

Re-derive every figure: `tools/cluster/scs_join_capability_measure.py`
(`EXPECTED_449`). Captures:
`vms449-{A1,B1,C1,B2,C2,B3,C3,B4,C4}-lab2-vaxlab6-20260805.pcap`.

> **⚠ THE OVMX TAP MAC IS PER-POD.** `vaxlab-6` puts OVMX on
> `26:8b:49:99:95:3c`; `vaxlab-4` (§4(O), §4(O.1)) used `4e:83:cd:c4:fe:54`. The
> lab-2 README's "every replica reuses the same node MACs by design" is true of
> the **VAX** nodes (`aa:00:04:00:01:04`) and not of the OVMX tap. Measuring
> this bracket with the other bracket's MAC returns zero for every figure and
> reads as a total wire failure. Each `EXPECTED*` dict carries its own.

---

#### 4(O.3) The refusal REPLICATES on a second pod — the `vms-449` replication (GROUNDED live, `vms-449`)

§4(O.2) answered the rejoin question on one pod. A single pod cannot separate
*"a returning OVMX identity is refused"* from *"`vaxlab-6` was sick"*. This is
the same experiment on **`vaxlab-7`** — a lab-2 pod scaled up fresh for it,
`CLUSTER_NODES=2` verified before the first run, never having seen an OVMX node
— with the **same binary** as §4(O.2) (a build of `main` at `f874b04`), default
environment, no switches. `SCSSYSTEMID`s 1520–1523. Identity is proven from the
capture bytes on every run (guardrail 18).

| run | role | identity | CM 190 tx | CM 190 rx | peer DISCONNECT_REQ/RSP rx | verdict |
|---|---|---|---|---|---|---|
| `A1` | first join | `OVMXM0` | **504** | **568** | 3 / 3 | **JOINED, t+13 s**, `XITDONE=1` |
| `B1` | rejoin #1 | `OVMXM0` | **14** | **11** | **0 / 0** | **REFUSED**, `XITDONE=0` |
| `C1` | control | `OVMXN1` | **502** | **568** | 3 / 3 | JOINED, t+13 s |

**Both discriminators reproduce, and the refused census is identical.** The
returning identity's whole message census on `vaxlab-7` — `cm190 tx=14 rx=11`,
OVMX-sent `0`×2 `1`×2 `2`×2 `3`×2 `6`×2 `9`×4, peer-sent `0`×2 `1`×2 `2`×2
`3`×2 `8`×4 — is **the same in every field** as all four refused rejoins on
`vaxlab-6`, on a pod that shares no state with it. The peer again sends **no**
DISCONNECT_REQ and **no** DISCONNECT_RSP on the rejoin while sending ×3 of each
on both joining runs. So the finding of §4(O.2) is a property of OVMX, not of
one pod.

**⚠ FOUR FURTHER RUNS WERE STARTED AND ARE VOID — recorded, not hidden.**
`B2`/`C2`/`B3`/`C3` were to extend this to three rejoins. The lab pod terminated
at `2026-08-05T18:58:50Z` (exit 255, `RESTARTS=1`) and restarted at `18:59:01Z`,
rebooting both VAXes *during* `B2`; the console had already begun returning
empty `CLUSTER_NODES` reads. `A1`/`B1`/`C1` all completed before `18:56:58` and
are unaffected. **The void runs are discarded on harness grounds — the restart,
not their figures.** That order matters: a run whose lab rebooted under it does
not get to vote either way (guardrail 19). Their captures are not archived and
no figure from them appears in this spec.

**What this therefore does and does not establish.** It establishes that the
refusal and both wire discriminators reproduce on an independent virgin pod. It
contains **one** rejoin, so it does **not** independently establish the "three
consecutive rejoins, so a single refusal is not a fluke" property — that rests
on §4(O.2)'s four. `check_449r_bracket_shape()` asserts this weaker shape
deliberately and must not be conflated with `check_449_bracket_shape()`.

> **⚠ A THIRD DISTINCT OVMX TAP MAC.** `vaxlab-7` mints
> `3a:ad:35:5d:23:80`, against `vaxlab-6`'s `26:8b:49:99:95:3c` and
> `vaxlab-4`'s `4e:83:cd:c4:fe:54`. Three pods, three taps — independent
> confirmation of the per-pod trap recorded in §4(O.2).

Re-derive every figure: `tools/cluster/scs_join_capability_measure.py`
(`EXPECTED_449R`). Captures:
`vms449r-{A1,B1,C1}-lab2-vaxlab7-20260805.pcap`.

---

#### 4(O.4) Disk discovery keeps ONE trigger — the `vms-ebb` bracket (GROUNDED live, `vms-ebb`)

`vms-096` deleted the immediate disk-discovery trigger together with the
unreachable `cm_op == 6` block that gated it, leaving the
`OVMX_DISKRUN_GATE_MS` ungate as the only entry point, and explicitly left the
question open: is one trigger correct, or should an immediate one be re-attached
to the architected DISCONNECT path (`scs_disc_*`, the `vms-591`/`vms-dd5`
classifier)? §4(O.1) could not answer it — **all three of its arms ran the
default environment, so none of them entered the pure-server disk-client path at
all.** This is the missing bracket.

**Method.** One lab-2 pod, `vaxlab-1`, restored to a healthy 2-node cluster
first (`CLUSTER_NODES=2` on `VAX1` before any arm; the replica had been sitting
at `CN_1` with `vax2` halted at `?06 HLT INST`, so its `d0`/`d1` were re-cloned
from the golden images). One binary for all three arms, `md5 9fc8451f…`,
**verified in-pod before and after every arm**. Three fresh identities, none of
which had ever been admitted anywhere. Control run **between** the two test arms
(guardrail 20). Identity read off the capture, never off `SCSD`'s log
(guardrail 18). SCSSYSTEMIDs `1387`/`1388`/`1389`.

| run | env (plus `OVMX_PURE_SERVER=1`) | identity | `PSC-UNGATED` | PS `SCS$DIRECTORY` `CONNECT_REQ` on the wire (slot `0x000C`) | peer `DISCONNECT_REQ` → our slot `0x0007` | join |
|---|---|---|---|---|---|---|
| `E7` | — | `OVMXE7` | **2** | t+3.113, t+3.558 | 2 — t+0.939, t+1.420 | **`CN_3`, t+13 s** |
| `E8` | `OVMX_NO_DISKRUN_UNGATE=1` | `OVMXE8` | **0** | **none** | 2 — t+3.821, t+4.295 | **`CN_3`, t+13 s** |
| `E9` | — | `OVMXE9` | **2** | t+5.110, t+5.999 | 2 — t+2.272, t+3.827 | **`CN_3`, t+13 s** |

**The kill switch was RUN, not asserted (guardrail 23).**
`OVMX_NO_DISKRUN_UNGATE=1` takes `PSC-UNGATED` from 2 to 0 *and* removes the PS
`CONNECT_REQ` from the capture — the counter and the wire agree, so the gated
behaviour is genuinely suppressed rather than merely unlogged.

**THE IMMEDIATE TRIGGER IS NOT DEAD FOR WANT OF A SIGNAL.** The peer *initiates*
a p. 2-26 symmetric teardown of OUR `SCS$DIRECTORY` server connection, twice per
run — once per VAX — in 3 of 3 arms. Microsecond ordering, `E7`, slot `0x0007`:

```
t+0.938545  peer->OVMX  DISCONNECT_REQ   (peer initiates)
t+0.938743  OVMX->peer  DISCONNECT_RSP
t+0.938788  OVMX->peer  DISCONNECT_REQ   (our own disconnect call, p. 2-26)
t+0.938882  peer->OVMX  DISCONNECT_RSP
```

That `DISCONNECT_REQ` is exactly the frame the deleted trigger fired on, and the
`vms-591`/`vms-dd5` classifier now handles it (`scsd_disconnect_dialogue`). An
immediate trigger re-attached there would start the disk run **2.1–2.9 s
earlier** than the gate does (measured, paired per peer node: 2.175 s / 2.138 s
on `E7`, 2.838 s / 2.172 s on `E9`).

Re-derive every figure in this section:
`tools/cluster/scs_diskrun_trigger_measure.py` (`EXPECTED`). Captures:
`vmsebb-{E7,E8-control,E9}-lab2-vaxlab1-20260805.pcap`. `ctest -R
scs_diskrun_figures` holds the prose to that table without needing them.

**RULED: one trigger. It buys nothing on the path this bracket can reach.** All
three arms reached `CLUSTER_NODES=3` at t+13 s — *including the control, in
which disk discovery never ran at all.* Admission on this lab does not wait on
the disk run, so there is no measured effect for 2.1–2.9 s of earlier start to
improve, while a second entry point restores exactly the two-writer shape
`vms-096` deleted. The reason is the bracket, not conservatism, and it is not
"the signal is missing" — the signal is there and is timed above.

**What this does NOT settle, and it is the case that motivated the trigger: the
REJOIN.** Every arm is a FIRST join by an identity that had never been admitted
anywhere. §4e.3's peer-side evidence — the peer holding `VMS$DISK_CL_DRVR` in
`con_sent` with a zero Remote Con. ID during a *refused rejoin*, where a
successful join leaves `MSCP$DISK` `open` — is not exercised here, and whether
the op 6 above even arrives on a rejoin is `vms-449`'s bracket. If it does and
the earlier start matters there, this ruling is what to re-open; the attachment
point and the gain are already measured.

**A SECOND RESULT NOBODY ASKED FOR, RECORDED BECAUSE IT WEAKENS THE UNGATE'S
OWN MOTIVATION.** `E8` joined with disk discovery *entirely* suppressed, on the
same schedule as the arms that ran it. Nothing here says the disk run is
useless — §4c.8 shows a real joiner performing it, and this lab's admission
simply does not gate on it — but "the run must happen inside the 1.4–4.4 s
window or the join suffers" is not a claim this lab supports. Filed as `vms-5c7e`, not fixed.

**A HARNESS DEFECT FOUND WHILE RUNNING THIS, AND EVERY FIGURE ABOVE DEPENDS ON
THE FIX.** `tests/lab/tools/lab2run.sh` stages the daemon to `/lab/SCSD.EXE`, and `/lab`
**is the shared tank volume** — the same file for every pod and for lab-1. Two
sessions bracketing at once overwrite each other's binary between the copy and
the exec, silently, because the copy's errors are discarded. Two vms-ebb arms
(`E1`, `E4`) ran a FOREIGN daemon this way: `md5 fbd553d8…` then `b62bd7cb…`,
183768 bytes against this tree's 398288, and their logs carry
`disk-discovery step 1, post-credit` — **a log line that exists nowhere in this
tree**. Both were discarded. The arms above ran from a per-run
`/lab/ebb-<TAG>/` directory with the md5 checked in-pod before and after. Do not
run a lab-2 bracket through `tests/lab/tools/lab2run.sh` while another session is live.

## 5. Summary of unknown/inferred fields (RE gaps)

For visibility, every field NOT marked GROUNDED above:

- **`PRCPOLINTERVAL` — GROUNDED, and recorded here because a default that
  matters is worth pinning to its oracle (`vms-66f`).** The SCS process polling
  interval of *VAXcluster Principles* p. 2-50 is a real SYSGEN parameter, and it
  was read off the reference system rather than inferred from the prose —
  `MC SYSGEN SHOW/SCS` on VAX1 (VAX 7.3, lab-2 replica `vaxlab-1`, 2026-08-05):
  `PRCPOLINTERVAL  Current 30  Default 30  Min. 1  Max. 32767  Seconds  Dynamic`.
  `src/vmsscs/include/scs_poll.h` carries all four numbers and clamps to the
  range. Raw evidence:
  `/data/training/vax/k8s-labs/vaxlab-1/logs/sysgen-scs-vax1-20260805.txt`.
  **What is NOT grounded and is labeled as an OVMX choice in that header:**
  `SCS_POLL_CYCLE_TIMEOUT_MS` (5 s — SCA states no bound on how long a poll
  cycle may stay open; it is a local timer and puts nothing on the wire), and
  the forced CDT release when a poller cycle ends without the p. 2-26 disconnect
  dialogue completing (counted in `descriptors_forced`, see
  `poll_release_cdt()`).

  > **CORRECTED (`vms-66f` round 4).** The sentence above used to justify the
  > forced release with "OVMX builds no `DISCONNECT_REQ`". That was true when it
  > was written and is FALSE as of `vms-591`, which added
  > `scs_disc_build_request()`. The poller's own emitter in `scsd.c` went on
  > answering `NOBUILDER` for `SEND_DISCONNECT_REQ` anyway, so the p. 2-50 cycle
  > could never end the way the page describes and **every** cycle force-released
  > its descriptor. It now sends the frame, the dialogue completes, and
  > `descriptors_forced` stays at 0 on a normal cycle — the forced release is
  > what is left for a cycle that is ABANDONED (timeout, lost circuit, node
  > dropped mid-cycle), which is a real case and still an OVMX choice. The
  > completed teardowns are counted apart in `disconnects_closed`.

- **The poller's ANSWER READING is two-thirds inferred (`vms-66f`).** p. 2-50
  says the directory answers "Yes" or "No"; only the "No" is on our wire, as the
  literal `"NOT PRESENT HERE"` in `[78:94]` (§4(h)(2)). "Yes" cannot be read —
  §4(h) gap (c) — so `enum scs_dir_answer` is three-valued: `NO` is GROUNDED,
  `YES` means "non-zero and not the negative marker" (INFERRED), and an all-zero
  result is reported `UNKNOWN` and notifies nobody. A boolean here would have
  turned every unreadable response into a discovery.

- **The `CONNECT_RSP` REFUSAL CODES — "no such SYSAP" and "busy, try again
  later" (`vms-7fe`). NOT GROUNDED, IN PLACEMENT OR IN VALUE, AND OVMX SHIPS
  BOTH.** *VAXcluster Principles* p. 2-48 requires a `CONNECT_RSP` "containing
  the 'no such SYSAP' error" when the target SYSAP is not in the list of
  listening SYSAPs, and p. 2-50 requires "a response that essentially says
  'busy … try again later'" when the listening CDT is already in CONNECT
  RECEIVED. **The book publishes no code for either, and neither frame is on any
  capture we hold:** all 16 `CONNECT_RSP` frames in `formation-ci1.pcap` are the
  positive kind (their targets were listening), and the companion word at
  `[48:50]` is recorded as INFERRED in §4(h)(2). OVMX therefore carries its
  refusal in `[48:50]` — the only word the positive `CONNECT_RSP` holds at zero
  and the only one §4(h) named as a status/flag — with **OVMX-invented values**
  `0x0002` (no such SYSAP) and `0x0003` (busy), declared in
  `src/vmsscs/include/scs_sdir.h`. These are **not** VMS status codes and not
  `$SSDEF` values. A capture of a real VAX refusing a connect request supersedes
  both, and the frame class itself (the 66-byte `CONNECT_RSP`, `[46:48] == 1`,
  requester's Con.ID echoed, local Con.ID `0`) IS grounded — only the status
  word is invented. **Measured blast radius:** in the configuration OVMX runs
  neither refusal is ever emitted, because the only two `CONNECT_REQ`s the
  reference VAX addresses to OVMX name `SCS$DIRECTORY` and `VMS$VAXcluster` and
  OVMX LISTENs for both; `tools/cluster/scsd_wire_diff.sh` byte-diffs 34 frames
  across the pre/post trees with zero differences, and the daemon's exit summary
  reports `no-such-sysap-sent` and `busy-sent` every run. Kill switch:
  `OVMX_NO_SDIR=1`.

  > **⚠ CONFLICT RAISED, NOT RESOLVED HERE (`vms-66f`, 2026-08-05).** The
  > sentence above calls `[48:50]` "the only [word] §4(h) names as a
  > status/flag". §4(h)(2a) has since **refuted the flag reading of `[48:50]`**
  > and §4(d)'s ⭐ block grounds it as the SCA credit field for exactly the
  > classes involved, including the 66-byte `CONNECT_RSP` (`66 → 944/1/0`:
  > constant 0 in 944 frames). The *observation* that survives is only "the
  > 66-byte `CONNECT_RSP` holds 0 there" — which is now explained as "no credit
  > extended", not as "an unused status word". **So OVMX's invented refusal
  > codes `0x0002`/`0x0003` are being written into a field the spec elsewhere
  > grounds as a credit count, and a peer that reads credit there would see
  > OVMX extend 2 or 3 send credits it does not honour.** `vms-66f` does not
  > change `vms-7fe`'s frames — that is `vms-7fe`'s decision to revisit — it
  > records the collision so the next reader does not have to rediscover it.

  **The two codes are not equally exercised, and the difference should not be
  glossed.** `0x0002` (no such SYSAP) is emitted by `scsd.c` under a synthesized
  frame — `tests/vmsscs/test_scsd_wire.c` case (2d), a 0x5b `CONNECT_REQ` whose
  16-byte name field is substituted — so the wire shape OVMX would send is at
  least pinned, even though the status *value* in it is an OVMX invention that
  no capture can confirm. `0x0003` (busy) is **never emitted by the daemon at
  all**, in production or in test: reaching it needs a listening CDT still in
  CONNECT RECEIVED when a *different* requester's frame arrives, and `scsd.c`
  answers synchronously and returns the CDT to LISTEN before it reads the next
  frame (OVMX DESIGN CHOICE 3 in `src/vmsscs/include/scs_sdir.h`). That is
  measured, not predicted: `tests/vmsscs/test_scsd_wire.c` sums the daemon's
  `sdir_busy_replies` across every case it runs and asserts the total is `0`,
  and a live daemon prints `busy-sent` in its exit summary. The BUSY path is
  reached only through the `src/vmsscs/scs_sdir.c` module API, only by
  `tests/vmsscs/test_scs_sdir.c`. **Do not read that green test as evidence that
  OVMX sends busy replies.**

- **The AFFIRMATIVE `SCS$DIR_LOOKUP` result encoding stays ungrounded
  (§4(h) gap (c)) — but WHO DECIDES IT is now the SDIR queue (`vms-7fe`).**
  The 16-byte negative marker `"NOT PRESENT HERE"` at `[78:94]` remains the one
  GROUNDED half of the answer. What changed is the source of the yes/no: the
  responder used to decide with a hardcoded `memcmp(name, "VMS$VAXcluster", 14)`
  in `scsd.c`, and now scans the p. 2-48 queue of SCS Directory Entries that
  `LISTEN` populates. **The only queried name whose answer changed is
  `SCS$DIRECTORY`**, which OVMX serves and now affirms; `MSCP$TAPE` and
  `MSCP$DISK` — the names the golden directory phase actually asks about — still
  get the grounded negative marker, byte for byte, because OVMX LISTENs for
  neither and deliberately does not advertise an MSCP server it does not have.

- **SCA connect data `[98:105]` (`vms-fdd`, §4(N)).** The 16-byte field's
  *location*, its *width*, its *per-SYSAP* character and the two spans `[94:98]`
  = `01 1b 01 03` and `[105:110]` = `08 00 00 06 00` are GROUNDED (148/148
  **VAX-sourced** `VMS$VAXcluster` connect frames from 5 node identities on 3
  emulator instances, 0 residuals; OVMX's own 55 frames are excluded — see the
  circular-grounding guard in §4(N)). **GROUNDED IN A NARROW SENSE, AND THE
  NARROWNESS IS THE POINT — see the limit bullet below.** The **seven bytes
  between them are not**: 5 distinct values, four of
  which fit `01 00 01 00 NN 00 01` with `NN ∈ {1,2,3}` and one of which
  (`01 00 00 00 02 00 01`, 1 frame) does not, plus the all-zero form,
  correlated with whether the sender is joining. "`NN` = current member count"
  is the best reading and is **INFERRED**; member-state-sequence and
  node-number are both REFUTED (§4(N)). OVMX copies a real joiner's observed
  bytes and therefore **cannot generate** connect data for a role it has not
  captured. Also not grounded: what a real node's *rejection* on connect data
  looks like — only one VMS version (V7.3) has ever been on our wire, so no
  refusal has ever been observed, and OVMX correspondingly implements no
  version policy.

- **STANDING LIMIT on every "GROUNDED" in §4(N): the sample is ONE VMS BUILD
  (`vms-fdd`).** The GROUNDED figures in §4(N) are re-derivable and their
  residual counts are real, but they must not be read as agreement between
  independent VMS systems, and two earlier revisions of §4(N) said exactly that
  — first *"four independent real VAX nodes"* (a source-MAC count), then
  *"3 independent hardware sources" / "distinct lab machines"*. **Both were
  false.** The lab's actual configuration, per
  `/data/training/vax/cluster/README-lab.md` and `cluster/vax.ini`, is **1
  OpenVMS VAX V7.3 installation, under 3 system roots (`[SYS0]`, `[SYS1]`,
  `[SYS11]`), on 1 system disk image (`data/d0.dsk`), across 3 SIMH instances
  of one emulated model on one host** — the five node identities are those
  three roots, two of them re-identified with `MC SYSGEN` between reboots. The
  one-installation half is measured, not assumed: all 668 VAX-sourced START
  frames report a single version string, `"VMS V7.3"` on `"VAX "` (§4(g)).
  **So §4(N)'s GROUNDED claims mean "stable across identity, node number, root,
  boot, incarnation and role in this one installation" and nothing wider** —
  three roots of one install agreeing about a byte is nearer one observation
  repeated than three confirmations. Nothing in §4(N) is evidence about a
  second VMS version, a second build, or a second installation of V7.3; the
  sample holds exactly one of each. **This bullet is the reason OVMX ships no
  version policy** (see the connect-data gap above): the field is a version
  claim a peer may reject on, and one build's value is not a basis for deciding
  what to accept from anyone else. Lifting the limit needs a capture from an
  installation OVMX did not build — a second VMS version, or the same version
  installed independently — and `tools/scs_connect_data_measure.py` reds if a
  second version string ever appears without this text being re-derived.

- **What a non-VAX peer puts in the field (`vms-fdd`, §4(N)).** Nothing in the
  census is evidence about implementations other than VAX/VMS V7.3, and — since
  the guard now excludes them — nothing in it is evidence drawn from OVMX's own
  transmissions either. The library holds 466 OVMX-sourced connect frames; they
  ground OVMX's *encoder*, and nothing about VMS. A second real VMS
  implementation on the wire is what would raise the confidence here, and we do
  not have one.

- **Peer-liveness detection (`vms-17f`, §4M).** The two silence *populations* are
  GROUNDED (measured, re-derivable). What is **NOT** grounded is the timer a real
  VMS port driver uses to decide a channel is dead: no capture shows it and ch. 2
  of the book does not publish one. OVMX's 20 s default is an OVMX choice sitting
  in the gap between the measured populations, and is labeled as such in
  `src/vmsscs/include/scs_depart.h`. Also not grounded: what a real node
  *transmits*, if anything, on declaring a peer gone — OVMX transmits nothing at
  that moment, which is an absence of evidence, not evidence of absence.

- **THE BLOCK DATA TRANSFER SERVICE — DELIBERATELY UNIMPLEMENTED, and this is
  its only durable record** (`vms-941`, closed as RECORDED; written down here by
  `vms-096` after an audit found the deferral existed nowhere in the tree).

  SCA offers SYSAPs three services. OVMX implements the datagram and message
  services (§4(h), `scs_dgram.c`, `scs_credit.c`). It implements **none** of the
  third, and the omission is a decision rather than an oversight. What is being
  deferred, from *VAXcluster Principles* ch. 2 sec 2.7 (pp. 2-32..2-41) and
  sec 2.9 (pp. 2-45..2-46):

  | mechanism | pages | what it is |
  |---|---|---|
  | **Named buffers** | 2-32..2-34 | a SYSAP declares a region of its own memory to SCS and receives an opaque *buffer name*, which it may then hand to the remote SYSAP; the name, not the address, is what the far end quotes |
  | **Buffer mapping** | 2-34..2-36 | the port maps a named buffer for direct port-to-port access, so the transfer never passes through the SYSAP's own copy loop |
  | **`SNDDAT` / `REQDAT`** | 2-36..2-38 | the two directions of the transfer: SEND DATA pushes into a remote named buffer, REQUEST DATA pulls from one. These are port *commands*, not connection-control message types, and neither appears in the `[46:48]` namespace §4(h)(1a) grounds |
  | **Buffer Descriptor Table (BDT)** | 2-34..2-36 | the per-port table the buffer names index into |
  | **Class Driver Request Packets (CDRPs)** | 2-38..2-40 | the request context a class driver queues to the port for the duration of a transfer |
  | **RSPID / Request Descriptor Table (RDT)** | 2-40..2-41 | the response identifier a request carries so its (possibly much later, possibly out-of-order) completion can be matched back to its CDRP |
  | **Pool / BDT / RDT SCS Waits** | 2-45..2-46 | the three additional wait states a SYSAP can be suspended in when the resource it needs — non-paged pool, a BDT entry, an RDT entry — is exhausted. Siblings of the Credit Wait `scs_credit.c` does implement |

  **WHY IT IS DEFERRED:** every one of these exists to move bulk data between a
  disk class driver and a disk *server*, and **OVMX serves no real storage to a
  cluster peer.** OVMX's MSCP work (`vms-760`, `scs_mscp.c`) is the disk
  **client** side, and its purpose is admission: a joiner that never presents a
  disk-client connection is not promoted to MEMBER. A client that reads nothing
  issues no `REQDAT` and is asked for no `SNDDAT`. Implementing the service now
  would be a large, entirely untestable surface — there is no traffic on our lab
  wire to ground it against, because nothing in our lab asks OVMX for data.

  **THE UN-DEFER TRIGGER, so this is a decision with an expiry and not a
  permanent silence.** Revisit the moment EITHER holds:
  1. OVMX serves real storage to a cluster peer (an OVMX MSCP **server**, not
     client), or
  2. a peer issues a `SNDDAT`/`REQDAT` — or any port command naming a buffer —
     that OVMX must answer. Today that cannot arise, because OVMX declares no
     named buffers and a peer can only address one it has been given.

  **NOT CLAIMED:** that the seven mechanisms above are decoded. They are *named*
  from the public book, at page granularity, so a future implementer knows what
  the gap contains. No byte layout for any of them appears anywhere in OVMX, and
  none has been observed on our wire.

- HELLO/SOLICIT: the offset-30 "per-frame word" is **now GROUNDED for the
  directed values (b2/b3/b4) in §4(a).1** (`vms-d94`, the NISCA channel-verify
  request/response counter); only the multicast `a0` / SOLICIT `b6` values of
  that word remain inferred-constant. Still unknown: the offset-36 message-class
  byte's exact semantics (label works empirically, not documented), the
  offset-47 17-byte capability span, offset-64 constant byte, offset-94
  trailer word, offset-96 "changing 4-byte value" (candidate: local timer),
  offset-100 12-byte constant tail, offset-130 constant `0x0064`.
- SCS 190-byte class: the offset-30 sequence/type word, the exact CSB-field
  mapping of the three repeated 16-bit counters at offset 32–63 (candidate:
  Next-seq/Last-seq-rcvd/Last-ack-seq from `SHOW CLUSTER`, not confirmed). The
  **132-byte SYSAP body is now partially GROUNDED in §4(j)** (`vms-f85`): the
  SYSAP transaction envelope (send-msg#/ack-msg# at body[0:4], txn/checksum
  correlation token at body[4:8], message-category+response-bit at body[8],
  opcode at body[9]), the connection-manager add-member SEND sequence
  (category-`0x01` opcodes `0x14`/`0x01`/`0x02`/`0x03`/`0x05`), the joiner's
  **VOTES** at body[22:24] (abs 94, four vote configs), and member-CSID routing
  tags at body[30:34]. Still unknown in that body: the per-opcode DLM
  lock-mode/resource-id/status and MSCP command-block sub-fields, `EXPECTED_VOTES`,
  the Member-State-Seq field, the discrete CSID-assignment field, and the
  transaction-checksum derivation (see §4j gaps).
- Non-190-byte SCS envelope classes (58/62/66/70/94/106/110 and the
  206–1500-byte block-transfer classes): **the `0x5b` directory-lookup and
  `0x48` credit-return classes are now GROUNDED in §4(h)** (`vms-560`) — the
  connection-handle pair at [50:58], the inner-length [42:44], the
  `"NOT PRESENT HERE"` result marker, the `0x48` acknowledged-sequence at
  [18:20]/[26:28], and the seq/ack lockstep. The `0x4b` connect classes are
  grounded in §4(g) phase 4. Of the large classes: the **1500-byte padded
  directed HELLO** (channel packet-size verification) is **GROUNDED in §4(k)**
  (`vms-84f`) — a zero-padded §4b HELLO whose reciprocal on the reverse channel
  is the "ack" an established VAX1 waits on before opening the joiner's CSB; the
  remaining `0x4b`/`0x13` **MSCP bulk block-transfer** classes (206–718 bytes,
  nonzero data body) remain unknown beyond the common dst/flag/src preamble.
- The directory-lookup/connect-handshake opcode fields (§4c) — **now grounded
  in §4(h)** (`vms-560`, see the bullet above): the `SCS$DIRECTORY` connect
  handshake, the name-resolution body, and its credit-return acks. Earlier
  partial closes retained: the offset-16 message-type byte and offset-17 `0x13`
  format constant (§4g), the `VMS$VAXcluster` connect Con.ID binding (§4g phase
  4). **The phase-2 START/config body is GROUNDED** (§4g phase 2, `vms-cd0`):
  inner length [42:44], config-round counter [44:46], SCSSYSTEMID [46:48],
  version/hardware/node-name ASCII fields. Remaining unknown in that body: the
  `0x0240`/`0x00d8` pair [54:58] (constants, no tunable match) and the per-boot
  incarnation tokens [66:71]/[98:104]. Remaining unknown in §4h: the `0x48`
  secondary counter [30:32] and the affirmative-lookup result encoding. **The
  [46:48] field is no longer unknown** — §4(h)(1a) (`vms-dd5`) grounds it as the
  SCA connection-control message type (`0`=CONNECT_REQ, `1`=CONNECT_RSP,
  `2`=ACCEPT_REQ, `3`=ACCEPT_RSP over 16 dialogues with an exact accept/reject
  partition), which also REFUTES this document's earlier "per-dialogue message
  counter" reading. What remains open there, **as corrected by `vms-591` and
  then by the MSCP research pass**: `4` (REJECT_REQ) and `6` (DISCONNECT_REQ)
  are each supported by a decisive behavioural partition rather than a decoded
  field name; `5` and `7` **DO occur** — §4(h)(1b) pairs them to `4` and `6`
  over 696 and 262 dialogues, and this document's earlier claim that they appear
  on no capture we hold is **REFUTED**, having rested on a census restricted to
  the length classes {62, 66, 110} while both response messages are 58 bytes.
  That cause is recorded because it is reusable: a class-restricted census is
  how this document came to publish a false absence.
  **`10` is IDENTIFIED** as the SCS "application message" MTYPE (*VAXcluster
  Principles* pp. 4-13..4-15) carrying all SYSAP payloads — which explains its
  2 889-frame majority of the 110-byte class and the fact that OVMX, which
  routes no SYSAP payload, has never emitted one.
  `8`/`9` are a paired, envelope-only control exchange on established
  connections — that is a CHARACTERISATION, not an identification:
  **nothing we hold identifies them**, and they are not
  named here. The candidate and its decisive experiment are in
  `docs/design-mscp-direction.md` §1.3. Do not name or emit them.
  **The vocabulary gap is a COUNTED RUNTIME FIGURE, not only a note**
  (`vms-561`): the five SCS services (`src/vmsscs/scs_svc.c`) ask the port
  driver to emit the packet each transition names, and `scsd.c` answers
  `SCS_SVC_EMIT_NOBUILDER` for every class OVMX cannot build. As of `vms-591`
  that list is `CONNECT_RSP`, `ACCEPT_RSP`, `REJECT_REQ` and `REJECT_RSP` —
  `DISCONNECT_REQ` and `DISCONNECT_RSP` came off it, and are built by
  `src/vmsscs/scs_disc.c`. Each remaining one logs `SCSD-W-CONNNOACT` and
  increments `struct scs_svc_port::unemitted`, so the distance between OVMX's
  frame vocabulary and SCA's is a number in every run log rather than a sentence
  in a header. On a normal join the member-opened `VMS$VAXcluster` connection
  produces exactly one such report per formation (the `CONNECT_RSP` the real VAX
  does send, 16 of 16 dialogues).
- **The DISCONNECT dialogue and its `[60:62]` matching flag** (§4(h)(1b),
  `vms-591`): **GROUNDED-BY-PARTITION.** The `[60:62]` word on a DISCONNECT_REQ
  reads `0x0000` on the first request of a Con.ID-pair dialogue and `0x0001` on
  the matching one, 131/89 with **zero residuals** over all 220 VMS-origin
  frames. What is measured is the partition; what is inferred is that it means
  "matching". **What is still NOT grounded:** the reason code's byte offset
  (unchanged, see the `vms-6b3` entry below — it remains a LABELED OVMX
  placement, and both DISCONNECT templates read zero there), and whether a VMS
  peer keys on the matching flag at all. OVMX emits it because emitting the
  value the wire carries in that position is strictly closer to VMS than
  emitting a constant; a peer that ignores it costs nothing.
- **DISCONNECT_REQ → DISCONNECT_RSP latency** (`vms-591`): 220 of 220 VMS-origin
  requests answered, max 0.006919 s, none over 10 ms. **Explicit non-claim**, in
  the same terms as §4(M): that is the largest latency in our captures, not an
  upper bound. It justifies `scsd.c`'s 500 ms bounded shutdown wait by margin —
  and exceeding the bound costs only a log line, because the daemon exits anyway
  rather than blocking on a peer.
- **~~A VAX DID NOT ANSWER *OVMX's* DISCONNECT_REQ ON THE `vaxlab-4` RUNS~~ —
  REFUTED, 10 answers out of 10 answerable requests** (`vms-abd`, refuted by
  `vms-096` against this branch's own `vaxlab-4` captures).

  <!-- REFUTED-QUOTE-BEGIN -->
  > REFUTED — the entry here used to read: *"When OVMX sends the same frame … no
  > `DISCONNECT_RSP` arrived on any of these four runs, over 20 s of capture past
  > the request."* That claim is false; the measurement below kills it.
  <!-- REFUTED-QUOTE-END -->

  **THE MEASUREMENT THAT KILLS IT.** Over the six `vaxlab-4` captures this branch
  produced (`vms578-{B1,B2,B3}` + `vms70e2-{A0,A1,A3}`, 2026-08-05, now held in
  `/data/training/vax/cluster/captures-lab2/`), pairing every OVMX-sourced
  msgtype-`6` with the first later VMS-origin frame carrying the same Con.ID pair
  with the two handles swapped — the pairing rule `scs_disc_measure.py` uses:

  | | |
  |---|---|
  | OVMX `DISCONNECT_REQ` (msgtype `6`) emitted | **14** |
  | answered by a VMS-origin frame | **10** — msgtype `7`, 58-byte class, 10/10 |
  | request→response latency | 0.073 – 0.807 ms |
  | unanswered *inside the capture window* | 4 — all at daemon shutdown, at the tail of their capture |

  So a real VAX **does** answer OVMX's `DISCONNECT_REQ`, on this very pod, in
  under a millisecond, and the 58-byte `7` class is exactly the response class
  §4(h)(1a) grounds. The four unanswered ones are an artefact of where the
  capture stops, not a refusal.

  **WHAT SURVIVES AND IS STILL OPEN.** VAX1's console logs, exactly once per
  disconnect-sending run and never for the control run that sent none:

  > `%PEA0, Inappropriate SCA Control Message - FLAGS/OPC/STATUS/PORT 00/22/00/DD`

  **`OPC/22` is NOT decoded** — it is a port-level opcode, not the [46:48]
  connection-control message type, and nothing we hold identifies it. Do not
  guess at it and do not build on it. **What this console line means is now
  ITSELF the open question**, and it is a smaller one than before: the entry used
  to read it as "so the peer receives and **refuses** it", which was the only
  available reading while the response was believed absent. It is not available
  any more — the SCA layer answers 10/10 in under a millisecond, so whatever the
  port layer is complaining about, it is not a refusal of the disconnect.
  **One hypothesis is already dead, with a matched control:** the LABELED OVMX
  reason-code placement at [58:60] is not the cause — a run with
  `OVMX_NO_REASON_CODE=1`, verified on the wire to carry `0x0000` there against
  `0x0005` in its bracketing run, was refused identically. This is a REGISTERED
  GAP, not a solved problem, and it is **not** evidence about the `vms-2f3`
  rejoin failure in either direction (sec 4M.24 already killed self-disconnect
  as a fix for that).

  **AND IT IS NOT A GENERAL FACT ABOUT VMS — measured in `vms-591` round 2.**
  Counting VMS-sourced 58-byte type-7 frames whose Ethernet destination is
  OVMX's own HW MAC `b6:16:8a:dc:3a:53`, over all 47 captures:

  | source | frames | | |
  |---|---|---|---|
  | `aa:00:04:00:01:04` (VAX1) | 16 | total | **42** in **16** pcaps |
  | `08:00:2b:78:56:b9` (VAX2) | 15 | destination Con.ID | `0x4F580007` in 42/42 |
  | `08:00:2b:11:22:33` (VAX3) | 11 | | |

  Three distinct real VAX nodes have answered OVMX on OVMX's own SCS$DIRECTORY
  Con.ID, and `ovmx-760-MEMBER-achieved-20260730.pcap` SCA 181/182/183/184 is a
  complete Figure 2-16 teardown with OVMX at one end of it. So the open question
  is **why these four `vaxlab-4` runs differ**, not whether a VAX will answer
  OVMX at all. `tests/vmsscs/test_scsd_wire.c` drives OVMX's receive side with
  SCA 184 unedited.
- **Joining an ESTABLISHED cluster** (§4i, `vms-af2`): **RESOLVED — two distinct
  differences.** (A) The established member's round-0 `0x41` START
  `send_seq[20:22]` = `prior_VC_send_seq+1` (residual VC continuation, e.g.
  11973→11974); the joiner ignores this and is only *receive-tolerant* of it.
  (B) **The join gate** is the joiner-side incarnation counter `[22:24]`: the
  joiner must stamp `0x41 [22:24]` with the node-incarnation number the member
  advertises in its **directed-HELLO flag `[78:80]`** (1 fresh, 2/3/… on
  successive reconnections). GROUNDED as a monotonic counter (VX3/1050 first-join
  =1, then 2, then 3, member `[78:80]` matching 1-for-1) and proven NOT to be the
  cluster generation (a first-timer joins established VAX1 with `[22:24]=1` and
  succeeds). OVMX's `vms-691` stall = it sent `[22:24]=1` while the member
  advertised `[78:80]=0x0002`. Corrective: echo the member's `[78:80]` into
  `[22:24]`. Remaining unknown: the member's CSB-retirement / incarnation-reset
  rule, and the counter's upper bound/wrap (grounded only for N∈{1,2,3}).
- **Vote/quorum membership fields** (§4g): **RESOLVED as a grounded negative**
  (`vms-cd0`, subsumes `vms-41d`) — a vote-varying capture (VOTES 0 vs 2 on the
  same reconfigured joiner) proves votes/quorum is **not** carried in the
  phase-2 0x41 body; it is exchanged later on the established VC (candidate: the
  190-byte `VMS$VAXcluster` SYSAP body, §4d, still undecoded).
- **Join-nonce derivation** (§4g): the nonce `ee05395b` is grounded as
  credential-derived and cross-boot-stable, but the `(group#, password) →
  nonce` hash is not derivable from passive capture — observe/replay only for
  a known cluster; a general implementation needs the documented
  `CLUSTER_AUTHORIZE` hash (not on the wire).
- **`NEW → MEMBER` reciprocal transaction** (§4L(7)): the choreography that
  promotes a `NEW` node to `MEMBER` is **not yet grounded**. The joiner's
  add-member burst is credited but not reciprocated by the live member; VOTES,
  premature-`0x02`, and 3-node coordination are ruled out as the cause. Leading
  hypothesis: the joiner must open its own `SCS$DIRECTORY` connection (dir
  connect/lookup *requests*) and/or hand back its live `VMS$VAXcluster` handle in
  the lookup response. The choreography/timing that IS grounded (active-joiner
  drive, prompt-connect timeout, Con.ID-signature acceptance, `NEW` status,
  display-only software-version) is in §4L.
- **The SPECIAL CREDIT MESSAGE — UNGROUNDED, no candidate class** (`vms-1d2`).
  *VAXcluster Principles* p. 2-44 defines a second flow-control mechanism beside
  the piggybacked credit field: when the local Receive Credit count is
  "dangerously low" **and** the local Pending Receive Credit count is > 0, SCS
  "immediately sends remote SCS a **special credit message** containing the local
  Pending Receive Credit count". **Which wire class carries one is not known.**
  What IS established:
  - It is **NOT** the 41-byte `0x48` short. §4(h) grounds that class as a strict
    1-for-1 sequence ack with **no locatable credit count** (622/622 frames; the
    connection's credit was 10/8 and no `0x0a`/`0x08` byte tracks it), and it is
    41 bytes so it cannot even reach the grounded credit field at SCA `[48:50]`.
    A special credit message must *carry a count*, so this class is excluded.
  - The seven classes that DO carry a credit field at `[48:50]` (58/62/66/86/94/
    110/190, §4(g) WIRE VERDICT) are all ordinary SCS messages; nothing yet
    distinguishes "an SCS message that happens to piggyback credit" from "a
    message sent *because* credit was low". Distinguishing them needs a capture
    with an engineered one-way SYSAP flow, which the lab has not produced.
  - Consequence for OVMX: `src/vmsscs/scs_credit.c` implements the p. 2-44
    **trigger** (when a special credit message is owed, and what count it
    carries) and hands it to a per-CDT hook. **It builds no frame and OVMX emits
    no special credit message.** Nothing installs the hook. (`vmsscs_credit` IS
    linked into `SCSD.EXE` and, since `vms-aa1`, called directly by `scsd.c` on
    both the send and the receive path — the unwired part is this hook alone.)
- **DATAGRAM BUFFER COUNT (the DFREEQ deposit) — UNGROUNDED, and an OVMX
  CHOSEN VALUE** (`vms-b1d`). *VAXcluster Principles* p. 2-42 has a SYSAP
  optionally request a number of datagram buffers for a connection through "an
  optional argument in the CONNECT and ACCEPT services"; the count is stored in
  the CDT and the buffers go into the port's *Datagram Free Queue* (DFREEQ).
  **No captured field is pinned to that count**, and it may well never appear on
  the wire — the book describes it as an argument to a LOCAL service, unlike the
  message-service credit count, which §4(g)'s WIRE VERDICT grounds at SCA
  `[48:50]`. The book publishes **no default** either, and none of the three
  SYSGEN parameters it lists for the message and datagram services (SCSMAXMSG,
  SCSMAXDG, SCSRSPCNT — p. 2-35) is a datagram buffer count. So
  `SCS_DGRAM_DEFAULT_BUFFERS = 8` in `src/vmsscs/include/scs_dgram.h` is an
  **OVMX chosen value, not an inferred VMS one** — nothing observed suggests 8,
  and callers are expected to pass an explicit count. Also recorded here so the
  next agent does not read it as measured:
  - **The datagram account is on the RECEIVE path, not the send path.** p. 2-42
    opens with "SCA does not provide a flow control mechanism for the datagram
    service"; the DFREEQ is debited when "the port RECEIVES a datagram", and the
    send side (p. 2-35) simply allocates a buffer with no queue consulted. A
    send-side datagram quota does not exist and OVMX does not implement one.
  - **Both documented discard classes are SILENT ON THE WIRE**: a datagram
    dropped for want of connection quota (buffer returned to the DFREEQ) and one
    dropped because the DFREEQ was empty (dropped by the port) both emit
    nothing. OVMX counts them locally — per connection and per port respectively
    — and prints them in the `SCSD.EXE` exit summary, because a discard that is
    invisible locally is indistinguishable from a facility that does nothing
    (INV-6). **Do not conflate this discard with message credit**: credit blocks
    a SENDER, this drops a RECEIPT.
  - **The deposit is returned to the port when the connection is released.**
    p. 2-43's bank analogy — "each person is entitled only to the amount of money
    that he or she has on deposit in the bank" — is what forces this: a depositor
    that no longer exists has no deposit. `scs_cdl_release()` subtracts the
    connection's `dgram_buffers` (the share *still sitting in* the DFREEQ, not
    the `dgram_extended` total it ever contributed — buffers the SYSAP is still
    holding left the queue when the port dequeued them) from the port's depth,
    the exact mirror of the MFREEQ return `vms-61b` added to the same function.
    **This is live, not latent**: `vms-17f`'s departure sweep releases every CDT
    on a departing peer's circuit, so without the return a node that leaves and
    rejoins inflates the port's datagram account on every cycle. Measured on the
    unfixed tree, four connect/extend(8)/release cycles on one port gave a DFREEQ
    depth of 8 → 16 → 24 → 32 and never fell. The reclaim is reported per
    departure as `dfreeq_reclaimed` in `struct scs_depart_stats` and logged in the
    `SCSD-I-PEERGONE` line.
  - **Consequence for OVMX**: `src/vmsscs/scs_dgram.c` implements the whole
    p. 2-42 mechanism and is unit tested, and **no production caller routes a
    datagram through it**. `scs_dgram_cdl_deliver()` and
    `scs_cdl_deliver_datagram()` are siblings, not a wrapper and a wrappee: they
    share the p. 2-29 resolution (`scs_cdl_resolve()`) and diverge after it,
    because the p. 2-42 accounting has to sit *between* the resolution and the
    SYSAP callback and `scs_cdl_deliver_datagram()` does both in one step. Both
    are unreachable from `scsd.c` — vms-e1a already recorded that for the
    unaccounted one. The only live daemon call is the exit-summary report, which
    prints zeros.
- **Minimum Send Credits — UNGROUNDED** (`vms-1d2`). p. 2-44 makes the
  dangerously-low threshold `local SCSFLOWCUSH + remote Minimum Send Credits`,
  where Minimum Send Credits is an argument the remote SYSAP passes to CONNECT or
  ACCEPT. **No captured field is pinned to it.** The 110-byte
  `CONNECT_REQ`/`ACCEPT_REQ` credit field at `[48:50]` is grounded as the number
  of Send Credits being *extended* (the tunable match: 10 = `CLUSTER_CREDITS`,
  8 = `MSCP_CREDITS`, …) — a different quantity, and the two must not be
  conflated. OVMX therefore takes Minimum Send Credits as an API argument
  (`scs_credit_extend`, `scs_credit_set_remote_min_send_credits`) with no wire
  parser behind it.
- **The remote node's SCS Node Name and 64-bit software incarnation number are
  NOT PARSED off the wire** (`vms-22e`). The p. 2-21 footnote anti-masquerade
  tests compare three System Block items — SCS System ID, SCS Node Name, and the
  64-bit software incarnation number (p. 2-16) — and OVMX implements all three in
  `src/vmsscs/scs_config.c` (`scs_config_masquerade_check`). Only the first has a
  parser behind it:
  - **SCS System ID** — populated live, from the `src-logical` field of every
    HELLO (`aa:00:04:00:<LE16(SCSSYSTEMID)>`, §4a), via
    `scs_pb_learn_system_addr()`.
  - **SCS Node Name** — the field IS grounded on the wire: the phase-2 `0x41`
    START body carries an 8-byte blank-padded ASCII node name at `[90:98]`
    (§4g phase 2, GROUNDED). But `scs_start_parse()` / `struct scs_start_view`
    do not extract it, so no System Block SCSD builds for a **remote** node
    carries one. (SCSD does name its own local System Block, from `SCSNODE`.)
  - **64-bit software incarnation number** — **UNGROUNDED, no identified wire
    field.** `vms-7be` left `struct scs_sb.incarnation` unset rather than invent
    a value and that is still the case. The candidates are the two per-boot
    incarnation tokens `[66:71]` / `[98:104]` in the `0x41` START body, which
    this section already lists as replayed-not-decoded; neither has been shown to
    BE the p. 2-16 quadword, and the `0x41 [22:24]` field is a different
    quantity (§4i.B, the member-attributed node-incarnation counter). **Do not
    conflate them.** `vms-2f3` §4M.31 did pin an emitted quadword at abs 80..87
    that a real VAX read back as `Incarnation`, but that is OVMX's *emitted*
    value, not a decode of the peer's.

  **Consequence, stated plainly:** all three masquerade comparisons are
  INDETERMINATE for every System Block the live daemon builds, so OVMX has never
  abandoned a virtual circuit for masquerade on the wire. The rule is implemented
  and unit tested (`tests/vmsscs/test_scs_config.c`) against System Blocks whose
  fields are populated by hand, so it is correct the moment a parser supplies
  them — including against a **non-head** System Block (the queue walk, not just
  the head), and with the incarnation comparison exercised in **both**
  directions, so an accidentally-ordered compare cannot pass. The daemon's half
  — the `SCSD-W-VCMASQ` line naming *which* test failed, and the suppression of
  the `SCSD-I-STARTDONE` / `SCSD-I-VCOPEN` lines on a refused circuit — is
  covered by `test_masquerade_open_is_logged_and_suppresses_vcopen` in
  `tests/vmsscs/test_scsd_wire.c`, which drives production `scsd_vc_on_open()`
  and whose first step re-measures the INDETERMINATE claim above rather than
  trusting it. Kill-switch `OVMX_NO_MASQUERADE_TESTS=1`. This rule was tested as the
  cause of the `vms-2f3` rejoin failure and **REFUTED** (§4M.31 of
  `docs/HANDOFF-vms-2f3.md`): it fixes nothing.
- **The REJECT/DISCONNECT 16-bit REASON CODE — the FIELD is real, its OFFSET is
  UNGROUNDED, and OVMX's placement is a LABELED OVMX DESIGN CHOICE** (`vms-6b3`).
  *VAXcluster Principles* p. 2-26: "When a SYSAP rejects a CONNECT_REQ or
  explicitly breaks an open connection, it also has the option of providing the
  other SYSAP a 16-bit 'reason code' explaining why it did so … the reason code
  is included in the REJECT_REQ packet … [and] in a disconnect request packet".
  The chapter grounds three things — the field exists, it is 16 bits, it rides
  REJECT_REQ and DISCONNECT_REQ — and publishes neither a byte offset nor a
  single code value.

  **What the wire says (measured; re-derive with
  `tools/cluster/scs_reason_measure.py`).** Population rule: every SCA frame in
  the connection-control length classes (§4(h)(1a)), restricted to VMS-origin
  source MACs (DEC OUI `08-00-2b` or the LAVC logical `aa-00-04-00-xx-04`) so no
  OVMX-emitted frame is counted as a VMS observation, over the whole lab capture
  set; message type is payload `[46:48]`, the Con.ID pair `[50:58]`.

      CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=47

  The two carrier frames and the two 16-bit words that follow the pair are the
  CENSUS-A table in §4(h)(1a) above — that table is the single copy of those
  figures and is not repeated here.

  **The two facts that do the refuting, pinned.** Both lines are parsed and
  compared against `EXPECTED` by `scs_reason_figures`, in this document *and* in
  `scs_reason.h`. They are what makes each refuted claim below unwritable: a
  sentence contradicting them cannot coexist with them, and deleting one reds
  the gate exactly as loudly as reinstating the claim would.

      REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:131,0x0001:89
      REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=62

  **The SDA oracle agrees.** `SHOW CONNECTIONS` prints a per-CDT field literally
  named `Rej/Disconn Reason`. Counted out of the captured extract rather than
  asserted (`cdts` = how many CDTs it printed, `values` = the histogram):

      CENSUS-D sda_file=sda-scs-extract-vax1.txt cdts=12 values=0:12

  So the field is real and VMS-named, and both oracles say every reason code our
  lab has ever produced was zero.

  **Therefore the offset CANNOT be grounded from the data we hold** — with no
  nonzero value anywhere, there is no varying field to localize.

  **THE FIRST RATIONALE FOR THE PLACEMENT WAS REFUTED, and the refutation is
  itself measured.** <!-- REFUTED-QUOTE-BEGIN --> Revision 1 of this entry
  justified the slot as "the only 16-bit slot in either frame that is zero in
  100% of observed VMS frames". That is false in both halves.
  <!-- REFUTED-QUOTE-END --> Census B applies the SAME population rule to the whole
  connection-control envelope (every message type) and finds `[58:60]` in live
  use by neighbouring types — including `3` = ACCEPT_RSP, which shares the
  **identical 62-byte layout** with `4` and `6`:

  <!-- CENSUS-B: parsed by tests/vmsscs/test_scs_reason_figures.py against the
       EXPECTED table in tools/cluster/scs_reason_measure.py. Do not hand-edit. -->

  | SCA len | type | name | frames | pcaps | nonzero at `[58:60]` |
  |---|---|---|---|---|---|
  | 62 | `3` | ACCEPT_RSP | 258 | 33 | 62 |
  | 62 | `4` | REJECT_REQ | 453 | 19 | 0 |
  | 62 | `6` | DISCONNECT_REQ | 220 | 25 | 0 |
  | 66 | `1` | CONNECT_RSP | 778 | 26 | 0 |
  | 110 | `0` | CONNECT_REQ | 1101 | 35 | 809 |
  | 110 | `2` | ACCEPT_REQ | 324 | 25 | 101 |
  | 110 | `10` | APPLICATION | 2889 | 39 | 2889 |

  And census C shows it is not the only always-zero slot either — these are the
  16-bit-aligned payload slots that are `0x0000` in 100% of the frames of that
  type:

  <!-- CENSUS-C: parsed by tests/vmsscs/test_scs_reason_figures.py. Do not hand-edit. -->

  | msgtype | always-zero 16-bit payload slots |
  |---|---|
  | `4` | 28, 32, 36, 48, 58 |
  | `6` | 28, 32, 36, 48, 58 |
  | both, at or after payload 50 | 58 |

  **What survives is narrower, and is all the placement needs.** (1) By CENSUS-A,
  `[58:60]` is zero in every frame of the two types that *carry* the reason code,
  so an OVMX REJECT_REQ or DISCONNECT_REQ carrying reason 0 there is
  byte-identical to what VMS sends, and only a deliberately-set nonzero code
  makes OVMX differ. The word is evidently per-message-type, and `4` and `6` are
  precisely the types that leave it zero. (2) By the last CENSUS-C row,
  `[58:60]` is the **only always-zero slot at or after payload 50** — the only
  one outside the SCS sequenced-message counter region `[18:50]`, whose low
  halves demonstrably vary (the others are that region's high halves and would
  collide with a counter as soon as one wraps) — and it sits immediately after
  the Con.ID pair, where p. 2-26 puts the reason code relative to the
  identification of the connection. Payload `[60:62]` is explicitly NOT usable:
  constant on REJECT_REQ and *varying* on DISCONNECT_REQ, i.e. a live field with
  an undecoded meaning. **`SCS_REASON_PAYLOAD_OFF = 58`
  (`src/vmsscs/include/scs_reason.h`) is a LABELED OVMX DESIGN CHOICE, not a
  decoded VMS field. If a real node is ever observed setting a nonzero reason
  code, that observation overrides it.** The **code VALUES**
  (`enum scs_reason_code`) are an OVMX namespace for the same reason; only
  `SCS_REASON_NONE = 0` has any external support, and it is the default.

  **Consequence for OVMX, stated exactly.** The RECEIVE half is LIVE: `scsd.c`
  decodes the field out of every REJECT_REQ/DISCONNECT_REQ addressed to one of
  OVMX's own Con.IDs, logs `SCSD-I-CONNREASON` naming the frame, the code and its
  name, and reports the totals in the exit summary — so the peer's SDA
  `Rej/Disconn Reason` finally has a counterpart on our side.

  **THE SEND HALF IS LIVE TOO, AND OVMX PUTS UNGROUNDED VALUES ON A REAL VAX'S
  WIRE. STATED HERE BECAUSE IT MUST NOT BE SILENT** (`vms-096`; this paragraph
  used to say the send half was "a tested codec with **no production caller**",
  which stopped being true at `vms-591`). `scs_disc_build_request()` calls
  `scs_reason_put()` (`src/vmsscs/scs_disc.c:138`), and `scsd.c` drives it with
  `SCS_REASON_SYSAP_SHUTDOWN` (5) on clean shutdown and
  `SCS_REASON_PEER_DISCONNECT` (7) on the symmetric answer to a peer's
  disconnect. Census over the six lab-2 `vaxlab-4` captures (2026-08-05), 62-byte
  connection-control class:

  | source | msgtype `6` reason values |
  |---|---|
  | **OVMX** | `0` ×4, **`5` ×6**, **`7` ×4** |
  | VMS-origin, same captures | `0` ×10 |
  | VMS-origin, lab-1 library (47 pcaps) | `0` ×220 (msgtype `6`), `0` ×453 (msgtype `4`) |

  **RULED (`vms-096`): the values are an OVMX DESIGN CHOICE and are KEPT, with
  the refusal recorded.** Across 683 frames every VMS-origin reason field reads
  0, so reason `5` and reason `7` are OVMX's own vocabulary in OVMX's own labeled
  slot — not decoded VMS values, and this section is where that is written down.
  (These are REASON CODES in the `[58:60]` slot; they are unrelated to
  connection-control *message types* `5`/`7`, which §4(h)(1a) grounds and which
  are on our wire.) They are kept rather than zeroed because zeroing was MEASURED not
  to help: a matched control run with `OVMX_NO_REASON_CODE=1`, verified on the
  wire to carry `0x0000` where its bracketing run carried `0x0005`, drew VAX1's
  `Inappropriate SCA Control Message` console line identically (see the
  DISCONNECT_RSP entry above, where that console line is the surviving open
  question). Zeroing therefore costs OVMX its own diagnostics and buys nothing.
  **If a real node is ever observed setting a nonzero code, that observation
  overrides both the placement and the namespace.**

  `struct scs_svc_args.reason` is what carries a SYSAP's value into the emit
  callback; `scs_svc.c` itself stamps nothing. Kill switch
  `OVMX_NO_REASON_CODE=1` suppresses both halves and says so in the exit summary.
  Tests: `tests/vmsscs/test_scs_reason.c` (the codec, plus two real captured
  frames asserted to read zero at the slot), `tests/vmsscs/test_scs_disc.c` (the
  builder stamps what it is given), and in `tests/vmsscs/test_scsd_wire.c` both
  the four `test_reason_*` cases (the daemon's receive path, driven by an
  unedited real SCS$DIRECTORY dialogue ending in a real DISCONNECT_REQ) and
  `test_peer_disconnect_req_is_answered_and_matched()`, which asserts the
  daemon's own DISCONNECT_REQ carries `SCS_REASON_PEER_DISCONNECT`. **And the figures above are pinned to the measurement by
  construction:** `tools/cluster/scs_reason_measure.py` carries a checked-in
  `EXPECTED` table and re-derives it from the captures on a lab host, while the
  ctest gate `scs_reason_figures` (`tests/vmsscs/test_scs_reason_figures.py`,
  needs no captures) asserts every figure in `EXPECTED` still appears in both
  `scs_reason.h` and this section. Both defects above were figures carried only
  by a comment; that is why the gate exists.

  **And a refuted claim cannot be written back in.** Review round 3 measured the
  first version of that half of the gate and found it did not work: it looked
  for an excuse word ("refuted", "wrongly", "earlier revision") within a few
  hundred characters of the dead sentence, and this document is *about* the
  refutation, so the excuse words are everywhere — re-asserting a dead claim in
  three natural sites left ctest green. The proximity window is gone. A dead
  claim is now legal in exactly one place: a QUARANTINE BLOCK, of which there is
  one in the refutation paragraph above. Anywhere else it reds, in any wording,
  because the check matches the claim FAMILY (subject + constancy assertion)
  rather than a fixed sentence. The block itself is size-capped, must say the
  claim inside it is refuted, and may not swallow a measurement line; the gate
  names the exact markers when it reds. The `REFUTATION-FACT` lines above pin
  the positive measurements the dead claims deny, so *deleting* the
  contradiction reds as loudly as reinstating the claim. What the gate really
  kills is not asserted in a comment either — it is re-derived on every ctest
  run by `scs_reason_mutants` (`tests/vmsscs/test_scs_reason_mutants.py`), which
  applies each mutant to a scratch copy and requires the gate to red.
- **`NEW → MEMBER` reciprocal transaction** (§4L(7)): the missing predicate is
  now **GROUNDED** (`vms-760`, live 2026-07-29) as the **full joiner-CLIENT
  connection choreography** — the joiner opens its own `SCS$DIRECTORY` client
  connection, **looks up each SYSAP** (`MSCP$DISK`, `VMS$VAXcluster`) on the member
  before connecting to it, then opens the `MSCP$DISK` and `VMS$VAXcluster`
  connections and sends the add-member burst, all on **one shared monotonic
  `send_seq`**; the member reciprocates within ~1 ms. Live-proven mechanism: the
  shared sequence deadlocks if any connect the member cannot yet process (e.g. an
  `MSCP$DISK` connect with no prior dir-client lookup) occupies a slot — it froze
  the member's `recv_ack` and dropped OVMX below `NEW`. VOTES, premature-`0x02`,
  and 3-node coordination remain ruled out. **Implementation of the choreography is
  the next deliverable** (the byte-exact `MSCP$DISK` connect builder is done).

---

### 5(z) OPEN — the last gap to `MEMBER` (current frontier)

**Resolved since the previous entry** (which wrongly concluded admission was
gated above the SCS layer — it was a half-open VC, §4m, plus a directed-HELLO
addressing bug, §4a.0). OVMX now:

- opens `SCS$DIRECTORY`, `MSCP$DISK` and `VMS$VAXcluster`, all accepted;
- completes the MSCP unit enumeration identically to a real VAX;
- exchanges config with **all** members and sends the deferred `op 0x02`;
- is answered `cat 0x04`, and driven through `op 0x03` COMMIT, `op 0x05` lock
  rebuilds, the `op 0x06` burst, `op 0x09`, `op 0x12`, and the cat-`0x06` close;
- executes the §4(p) barrier with correct releases, and answers the interleaved
  cat-`0x02` DLM rebuild transactions.

VMS logs `%CNXMAN, received VAXcluster membership request`, `proposed addition of
node OVMX…`, and `completing VAXcluster state transition`, and SDA shows a real
CSB with an assigned CSID.

> **SUPERSEDED 2026-07-30g — the fan-out anomaly below is SOLVED.** A
> non-coordinator peer silently discards `op 0x02` (see §4(p)); fan-out only ever
> worked because it happened to include the coordinator. Aiming a single `op
> 0x02` at the coordinator produces the relay, the commit, and the barrier. The
> live frontier is now **the cat-`0x02` `op 0x0d` DLM response shape**, which
> bugchecks peers with `LOCKMGRERR` — see §4(p) and `docs/HANDOFF-vms-760.md`.
> The historical measurement is kept below because it is what identified the
> recipient, not the message, as the variable.

**The fan-out anomaly (historical).** Measured as a controlled pair on a
**pristine** 3-node cluster (`reset3.sh`; zero ghost CSBs; all three peers
verified `MEMBER` before each run):

| run | `op 0x02` sent to | result |
|---|---|---|
| `d94-e14` | **one** peer — what the reference does | acked cat-`0x04`, then **nothing**. No commit, no transition, CSID `00000000`, zero barrier steps. |
| `d94-e15` | **all** peers (`OVMX_CFG2_ALL=1`) | `Node VAX3 (csid 00010003) proposed addition of node OVMX…`; the barrier starts. |

So on our cluster the **fan-out** gates the transition — which **contradicts**
the reference, where the joiner demonstrably sends `op 0x02` to exactly one peer
and that peer relays (`op 0x12`) and barriers with everyone. Two readings, both
testable and both kept live behind `OVMX_CFG2_ALL`:

- **(a) the peer-*selection* rule matters** and our "first eligible" pick is
  wrong. The candidate rules ("last to complete config", "last MSCP walk",
  "highest SCSSYSTEMID") are mutually confounded in the single 3-node specimen.
- **(b) something in our `op 0x02` or our `0x81`/`op 0x09` response** stops the
  chosen peer from relaying.

> ⚠ **"Barrier step 5 of 12" is NOT the baseline.** The earlier runs that reached
> step 5 were on a cluster that was itself re-forming — their OPCOM carries
> `%CNXMAN, proposing formation of a VAXcluster`. On a pristine cluster the
> single-coordinator form does not open a transition at all. The §4(p) barrier
> implementation is correct and grounded; it is simply not the current blocker.
> Re-establish any baseline on a freshly reset lab.

The destructive failure mode is understood and no longer occurs: the cluster
stays healthy at 3 members across all of these runs.

### 4(s) Where a member's ADVERTISED cluster state comes from (GROUNDED, `vms-584`)

A member advertises `member_count`, `cluster_formed` and `last_transition` in
`cat 0x01 op 0x01`. The question this section answers is where a correct
implementation *gets* those values — and the answer is **not** the place OVMX
originally took them from.

**`op 0x01` is a REPLY, not a broadcast.** It is a point-in-time answer to a
newcomer's query, sent once per VC, and it is sent to a newcomer **before that
newcomer is counted** — VAX1 answered OVMX 6.7 s early and VAX3 4.1 s early, and
in the whole of `by13` **VAX1 never advertises 4 at all**. So a value copied from
one goes stale the moment the next transition happens, and nothing re-teaches it.
Observed live: `by13` frame 2941, OVMX advertising `member_count=2` to VAX3 while
VAX1 (frame 2869) and VAX2 had both said `3` on the same wire seconds earlier.

**The TRANSITION-OPEN is the bundle.** `op 0x09` (class `0x02` add), `op 0x08`
(class `0x03` remove-failed) and `op 0x0d` (class `0x04` depart) all carry, in
one frame that *every* node sees — member or bystander — on *every* transition:

| body | abs | meaning | grounding |
|---|---|---|---|
| `body[40:48]` | 112:120 | the transition-time quadword | matches the `last_transition` a member later advertises, to the millisecond against OPCOM; present **20 ms before** OPCOM logs "completed" |
| `body[55]` | 127 | coordinator's membership bitmap | `popcount == post-transition member count`, 54/54 opens, zero residuals |

Same encoding as `cluster_formed` (VMS quadword, 1858 epoch, 100 ns ticks).
`cluster_formed` itself never changes in any capture and is correctly copy-once.

`last_transition` updates on **every class, including `0x04`**, which runs no
barrier at all (`af2-firsttimer`: count 2→1 and the transition time set to the
departure instant, twice). Neither `op 0x0c` (barrier release — epoch and step
marker only) nor `op 0x12` carries any membership bundle.

**Two limits, deliberately kept.** The bitmap's **bit-to-node mapping is NOT
grounded** — both observed transitions set a contiguous run `bits[1..count]`,
which fits "bit k = CSID slot" and "bit k = join order" equally, and one byte
cannot be the whole field (`body[52:55]` and `body[56:60]` are zero in every
specimen, so the extent is undetermined). Counting bits needs no mapping;
*asserting* the bitmap would, so an implementation that is not the coordinator
must never assert it. And a member should adopt the facts when the transition
becomes **real** — when its own barrier completes — so that a proposal which
never completes is never advertised; class `0x04`, having no barrier, applies at
the open.

**Consequence of getting it wrong: none observable.** Searched for any peer
reaction to a wrong `member_count` across the whole library and found none — no
refusal, no retransmit storm, no reset. `by13` reached four nodes normally with
OVMX advertising a stale `2`. Recorded as an explicit absence, not as permission:
it means this defect class is invisible on the wire and will not announce itself.

### 4(t) Con.ID allocation (GROUNDED, `vms-584`)

A Con.ID identifies a connection **endpoint**. Real nodes allocate from a single
monotonic counter **shared across all service classes** within one boot —
`formation-ci1`, node `08:00:2b:78:56:b9`: SCS$DIRECTORY `0x33590007`,
VMS$VAXcluster `0x33580008`, MSCP$DISK `0x33580009`, continuing upward on later
reopens, with the peer showing the same simultaneous pattern. The **high word
reseeds non-arithmetically at each incarnation** of the same node identity:
`af2-firsttimer` shows `0x8fd20007 → 0xe9950007 → 0x5b050007` for the same class
across three boots. Consistent with a per-boot seed (address or clock), not a
persisted counter. **A real node therefore never repeats a Con.ID across
incarnations**, and `af2-established-rejoin` confirms a rejoin is always a fresh
CONN-REQ with `remote_conid=0`, never a resumed handshake.

**The peer binds whatever is offered and never validates the value**: 30+ CONNECT
sequences with unpredictable values, every one answered ECHO → ACCEPT → CONFIRM,
zero DISC-RSP substituted for an ECHO, and no NAK anywhere tied to a Con.ID
value. That is what makes changing OVMX's allocation safe.

**Not grounded, and untestable from passive capture:** whether a peer would
reject a literal repeat after a full teardown. No reference capture contains one,
because a real allocator cannot produce one. Only the join/exit cycling test can
answer it.

### 4(u) The cat-`0x04` SYSAP ack cadence (GROUNDED, `vms-584`)

The reference ack is **prompt**, **opportunistic**, **cumulative**, and **never
keyed to an opcode**:

- It names whichever frame was genuinely received last — a member's first ack
  targets the newcomer's `op 0x02` (Δ 0.30 ms), the newcomer's first targets the
  member's `op 0x06` burst (Δ 0.53 ms), and on a link carrying neither it targets
  the first `cat 0x02 op 0x0d` DLM response. Reproduced at 0.39/0.45 ms.
- **No timer and no fixed N.** Idle captures carry zero acks; on busy links
  inter-ack gaps run 0 ms–2.3 s with no period.
- **An `op 0x01` is never acked** (0/4 reference link-checks). Ack-of-ack occurs
  once in 4, as an `amsg` coincidence deep in steady state, not deliberately.

OVMX diverges in two inert ways, both measured: an ack naming an `op 0x01`
~7.0 s after it arrived (`by10` idx 255, `by11` idx 2945, `bystander` idx 254 —
7013/6754/7014 ms), and a genuine ack-of-ack ~4 s late (`by11` idx 3006,
`bystander` idx 4922). **No peer reacted to either in any run.** These are
instrumented (`SCSD-W-STRAYACK`) rather than suppressed — see the commit message
for why widening the emission trigger would contradict the grounded claim that
the `op 0x0a`/`op 0x0c` notifications draw no ack.

### 4(v) Member-initiated connect-back timing — a retired finding (`vms-584`)

Previously filed as "OVMX connects back ~11 minutes early". **Refuted.** The
connect-back is the member side's directed HELLO/channel-init fired off the
newcomer's first multicast self-announce or SOLICIT, and it is **sub-2 s in every
reference specimen** — 0.054 s, 0.183 s, 0.195 s, 0.501 s, 0.696 s, 1.77 s
(×2), 1.925 s across 9 events in 6 captures. There is no 660 s interval anywhere
in the library, and three of the reference captures are shorter than 660 s in
total. OVMX measures 0.046–1.150 s across 5 captures — **inside the reference
range**. The frame originally cited as evidence (`by11` 2980) is an `mt=0x4b`
CONN-REQ — routine MSCP disk-class sub-channel renegotiation on a ~10 s cadence,
reproduced byte-identically in `by10` — not the connect-back at all. The
~660 s numbers were capture *lengths*, not protocol intervals. **No code change;
the rule OVMX already follows is the grounded one.**

## 6. Using the dissector

```
tools/cluster/dissect_sca.py <pcap>                 # full field-by-field decode
tools/cluster/dissect_sca.py <pcap> --summary        # message-class histogram
tools/cluster/dissect_sca.py <pcap> --frame N        # decode just frame N
tools/cluster/dissect_sca.py <pcap> --limit N        # stop after N decoded frames
```

Pure Python 3 stdlib (`struct`, `argparse`) — no `tshark`/`scapy` dependency.
Validated to run cleanly (no exceptions, sane class histograms) against
every pcap in `~/vax/cluster/captures/`, including the full
`formation-ci1-joinwindow.pcap` (2992/2992 SCA frames decoded with no
crashes).
