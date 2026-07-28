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
| `formation-ci1-joinwindow.pcap` | 2992 | §4(c) connect/directory-lookup phase, §4(g) membership handshake, §4(h) SCS$DIRECTORY connect + 0x5b/0x48 bodies (vms-560) |
| `ci3-join-membership-live2node-20260728.pcap` | 58 | §4(g) join-nonce cross-boot stability (vms-b6c) |
| `cd0-bootB-zk1099-join-20260728.pcap` | 18297 | §4(g) phase-2 START/config body grounding — joiner reconfigured to SCSNODE `"ZK"`/SCSSYSTEMID 1099/VOTES 0 (vms-cd0) |
| `cd0-bootC-zk1099-votes2-20260728.pcap` | ~18k | §4(g) vote-varying diff — same node, VOTES 2 (vms-cd0) |
| `af2-established-rejoin-20260728.pcap` | 16340 | §4(i) joining an ESTABLISHED cluster — VAX2 drop-and-rejoin while VAX1 stays up (Member State Seq 2→3→4); rejoin `0x41` START = SCA 2850–2855 (vms-af2) |
| `af2-firsttimer-established-20260728.pcap` | 51072 | §4(i).B — fresh-identity **first-timer** VX3/1050 joins established VAX1 (STARTs SCA 2558–2563), then **2nd** (20170–20175) and **3rd** (33591–33596) incarnations; grounds the `[22:24]` incarnation counter 1→2→3 and its `[78:80]` HELLO advertisement (vms-af2) |
| `formation-ci1.pcap` | 18541 | §1 message-class census (Table 2), §4(e) MSCP, large block-transfer frames, §4(h) 0x5b/0x48 scale re-validation (605/622 frames, vms-560) |
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
| 526, 398, 634, 462, 270, 1500, 82, 369, 302, 590, 718 | 52 combined | **large block-transfer** frames (up to `NISCS_MAX_PKTSZ`=1498, GROUNDED against SYSGEN tunable) | header NOT decoded (see §4(d) caveat) |

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
| 30 | 2 | per-frame word (`a000` mcast / `b300`,`b400` directed VAX1↔VAX2 / `b600` VAX3 SOLICIT) | unknown/inferred — varies by sender+direction, exact semantics not grounded |
| 32 | 4 | constant prefix `08 00 00 80` | unknown/inferred |
| 36 | 1 | **message-class byte**: `0x05` on every HELLO, `0x02` on every SOLICIT | inferred (consistent 100% split across all captures, but not documented anywhere — a working label, not a confirmed opcode enum) |
| 37 | 3 | constant suffix `01 00 00` | unknown/inferred |
| 40 | 1 | node-name length prefix (observed `6`) | GROUNDED (matches the following ASCII name's byte count in every frame) |
| 41 | *namelen* | node name, ASCII, space-padded (`"VAX1  "`, `"VAX2  "`, `"VAX3  "`) | GROUNDED |
| 33+14=47 | 17 | constant capability/version-ish span, differs slightly HELLO vs. SOLICIT | unknown/inferred |
| 64 | 1 | constant `0x03` | unknown |
| 65 | 3 | zero | unknown |
| 68 | 4 | **connect/join nonce** | **GROUNDED**: `0x00000000` on every multicast HELLO, and the identical non-zero shared token (e.g. `ee 05 39 5b`) on every directed HELLO between VAX1/VAX2 *and* on the VAX3 boot SOLICIT — the same cluster-wide token the initial RE-specimens doc flagged. Confirmed frame examples: `scs-idle-baseline.pcap` frame 1 (zero, multicast) vs. frame 2/3 (`ee05395b`, directed); `satellite-niscs-boot-solicit.pcap` frame 1100 (`ee05395b`). |

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
| 72 | 132 | SYSAP-specific message body | **not grounded at the byte/opcode level.** Contains directly-observed ASCII resource/queue names (see §4f for the DLM case) but no confirmed field map. |

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

## 5. Summary of unknown/inferred fields (RE gaps)

For visibility, every field NOT marked GROUNDED above:

- HELLO/SOLICIT: the offset-30 "per-frame word", the offset-36 message-class
  byte's exact semantics (label works empirically, not documented), the
  offset-47 17-byte capability span, offset-64 constant byte, offset-94
  trailer word, offset-96 "changing 4-byte value" (candidate: local timer),
  offset-100 12-byte constant tail, offset-130 constant `0x0064`.
- SCS 190-byte class: the offset-30 sequence/type word, the exact CSB-field
  mapping of the three repeated 16-bit counters at offset 32–63 (candidate:
  Next-seq/Last-seq-rcvd/Last-ack-seq from `SHOW CLUSTER`, not confirmed),
  and the entire 132-byte SYSAP body beyond the Con.ID pair (opcode,
  lock-mode, status fields for DLM; command block for MSCP).
- Non-190-byte SCS envelope classes (58/62/66/70/94/106/110 and the
  206–1500-byte block-transfer classes): **the `0x5b` directory-lookup and
  `0x48` credit-return classes are now GROUNDED in §4(h)** (`vms-560`) — the
  connection-handle pair at [50:58], the inner-length [42:44], the
  `"NOT PRESENT HERE"` result marker, the `0x48` acknowledged-sequence at
  [18:20]/[26:28], and the seq/ack lockstep. The `0x4b` connect classes are
  grounded in §4(g) phase 4. The 206–1500-byte block-transfer classes remain
  unknown beyond the common dst/flag/src preamble.
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
