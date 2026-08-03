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

---

## 1. Specimens used

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
| **44** | **2** | **config-round counter** | **GROUNDED**: increments `0 → 0 → 1 → 1 → 2 → 2` across START(round 0), START-retransmit(round 1) and the 46-byte ack(round 2); both nodes carry the same round. |
| **46** | **2** | **SCSSYSTEMID** (LE `uint16`) | **GROUNDED**: `0x0401`=1025 (VAX1), `0x0402`=1026 (VAX2), `0x044b`=**1099** (the reconfigured ZK node) — byte-exact to the SYSGEN/SDA-reported SCSSYSTEMID in **28/28** 106-byte frames across three distinct values. The joiner's logical LAVC src addr [10:16] tracks it (`aa:00:04:00:4b:04`, node `0x4b`=75 = 1099 & 1023). |
| 48 | 4 | zero (SCSSYSTEMIDH region) | constant observed |
| 52 | 2 | constant `0x0001` | 28/28 |
| 54 | 2 | constant `0x0240` = 576 | 28/28; inferred (SCS transport param, no tunable match) |
| 56 | 2 | constant `0x00d8` = 216 | 28/28; inferred |
| **58** | **8** | **software version string** `"VMS V7.3"` (ASCII) | **GROUNDED**: byte-exact `56 4d 53 20 56 37 2e 33` in **28/28** frames. *Correction to the earlier §4g note:* the field is `"VMS V7.3"` for **all** nodes; the previously-reported `"VMS V7.3f"` was a misread — the `f` (`0x66`) is the first byte of the per-boot token at [66:], which happened to be printable in the golden VAX1 frame (it is `0xd8`/`0x5d`/`0xae` in other boots). |
| 66 | 5 | per-boot token (version-side) | inferred: incarnation/timestamp, **not identity** — changes across reboots of the *same* node (see below) |
| 71 | 1 | token/flag (`0x00`/`0x01` observed) | unknown |
| 72 | 2 | constant `0x00bc` = 188 | observed constant |
| **74** | **4** | **hardware-type string** `"VAX "` (ASCII) | **GROUNDED**: 28/28 frames |
| 78 | 2 | constant `0x0006` | 28/28 |
| 80 | 2 | `0x0a` = 10 = SYSGEN `CLUSTER_CREDITS` at [81] | GROUNDED numeric match (as §4g credit) |
| 82 | 6 | zero | constant observed |
| 88 | 2 | constant `0x0077` | 28/28 |
| **90** | **8** | **node name** (ASCII, **fixed 8-byte, blank-padded, left-justified**) | **GROUNDED**: `"VAX1    "`, `"VAX2    "`, and `"ZK      "` — the 2-char `"ZK"` name occupies the same 8-byte field with 6 trailing spaces and **the following bytes do not shift** (28/28), proving a fixed-width blank-filled field, *not* the length-prefixed encoding HELLO uses (§4a). Distinct encoding from §4a. |
| 98 | 6 | per-boot token (name-side) | inferred: incarnation/timestamp, not identity |
| 104 | 2 | constant `0x00bc` = 188 | observed constant |

**The per-boot tokens ([66:71] and [98:104]) are NOT node identity.** They
change across reboots of the *same* node: VAX1's tokens differ between the
days-old golden capture and the fresh `cd0-boot*` captures although VAX1's
name/SCSSYSTEMID are unchanged. Within a single join both nodes share sub-spans
(e.g. `51 7b`, `e8 fb 01`), consistent with a cluster-wide time/incarnation
component with per-node low bytes. Best label: **inferred incarnation/timestamp
token**; not derivable further from passive capture.

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
> `src/vmsscs/include/scs_credit.h`. **OVMX does not yet stamp a live credit on
> the wire** — see that header's reachability note.

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

**(2) SCS$DIR_LOOKUP body — name resolution with a grounded negative marker.**
Past the handle pair the body carries fixed-position, blank-padded ASCII SYSAP
name fields beginning at [62]. Two observed shapes, selected by a
directory-operation field at **[46:48]** (a per-dialogue message counter:
`0,1,2,3` across the connect handshake frames 21–27, then `10` for the
`MSCP$TAPE` lookup 29/31 — its exact role is **inferred**; a companion
flag/status word sits at [48:50]):

- **connect frame** (SCA 21): target SYSAP `"SCS$DIRECTORY   "` (16-byte field
  [62:78]) + operation `"SCS$DIR_LOOKUP"` (blank-padded, [78:]).
- **lookup req/resp** (SCA 29/31): queried name `"MSCP$TAPE       "` (16-byte
  [62:78]) + a 16-byte **result field [78:94]**: all-zero in the request, and
  the literal ASCII **`"NOT PRESENT HERE"`** in the negative response.

**GROUNDED (directly observed ASCII):** the queried SYSAP name and, decisively,
the `"NOT PRESENT HERE"` result string that signals a negative resolution — the
same string §4c reported but now pinned to the [78:94] result field. The exact
byte width of each name field varies by operation (the operation name in the
connect frame runs longer than 16 bytes), so field *widths* are reported
as-observed, not asserted as a fixed schema; the *presence, position, and
negative-marker semantics* are grounded.

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

**RE gaps left in §4h (honest):** (a) the `0x5b` directory-operation field
[46:48] and its companion [48:50] flag — the value↔operation mapping is
inferred, not documented; (b) the `0x48` secondary counter [30:32] and the
early-phase shorts' non-zero residual at [30:40] (SCA 22/24 carry printable
leftover bytes) are not grounded to a field; (c) the affirmative
(non-`"NOT PRESENT HERE"`) lookup *result* encoding — the capture's directory
lookups that resolve carry the resolved SYSAP name back, but no separate
status/handle-return field was isolated; (d) the absolute `SCS$DIRECTORY`
connection Con.IDs are inferred (dynamically-allocated, absent from the
idle-state decoder ring).

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
peer member's ack). The leading remaining hypothesis: the member reciprocates only
once the joiner presents the **full connection-set a real joiner establishes** —
notably actively **opening its own `SCS$DIRECTORY` connection** (dir connect/lookup
**requests**, of which only the *response* side is currently built) and possibly
returning its live `VMS$VAXcluster` handle in the directory-lookup **response** —
so the member can resolve the joiner and reciprocate. This is the next
deliverable.

**Clean-room provenance:** every claim here is from (a) observing the reference
lab wire (`formation-clean-2node.pcap` + live `SHOW CLUSTER`/`SDA` output on the
lab VAX) and (b) public OpenVMS documentation on cluster connection management and
the `NEW`/`MEMBER` SDA states. No VSI/HPE binary was disassembled (CLAUDE.md
Rule 8).

---

## 5. Summary of unknown/inferred fields (RE gaps)

For visibility, every field NOT marked GROUNDED above:

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
  incarnation tokens [66:71]/[98:104]. Remaining unknown in §4h: the `0x5b`
  directory-operation field [46:48], the `0x48` secondary counter [30:32], and
  the affirmative-lookup result encoding.
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

---

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
