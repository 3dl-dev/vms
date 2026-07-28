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
| `formation-ci1-joinwindow.pcap` | 2992 | §4(c) connect/directory-lookup phase, §4(g) membership handshake |
| `ci3-join-membership-live2node-20260728.pcap` | 58 | §4(g) join-nonce cross-boot stability (vms-b6c) |
| `formation-ci1.pcap` | 18541 | §1 message-class census (Table 2), §4(e) MSCP, large block-transfer frames |
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
| 92 | 2 | **directed-HELLO flag** | GROUNDED: `0x0000` multicast, `0x0001` directed — matches wire direction in every sampled frame |
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

The exact opcode/field layout of this lookup exchange (offsets [30:*]) is
**not grounded** — only the presence and content of the ASCII SYSAP names
is. Byte-level request/response framing here is left `unknown` in the
dissector.

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

**Phase 2 — START/config carries the node's identity to the connection
manager** (opcode `0x41`, 106-byte frames 15/16; verified byte-exact both
directions). GROUNDED ASCII fields, directly observed:

- **software version string**: `"VMS V7.3f"` (VAX1) / `"VMS V7.3"` (VAX2),
  length-prefixed at payload offset ~44
- **hardware-type string**: `"VAX "`
- **node name**: `"VAX1  "` / `"VAX2  "` (space-padded, as §4a)

These are the parameters a joining node presents for membership. The
surrounding integer fields (a `0x3e`=62 length-ish word, a `0x0240`/`0x00d8`
pair) are **unknown** — reported as raw hex, not guessed.

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

**Vote / quorum — RE GAP (not grounded).** VAX1 has `VOTES 1`, VAX2 `VOTES 0`,
`EXPECTED_VOTES 1` (SYSGEN, §3 lab config). We could **not** isolate a vote or
quorum field in the connect/config body by passive diffing: with only one
vote configuration on the wire there is no contrast to bind a byte to. The
connection manager unquestionably exchanges votes/quorum at membership time,
but this specimen set cannot locate the field. **Recommended next step:** a
targeted capture that *varies* `VOTES`/`EXPECTED_VOTES` on the joining node
across two boots and diffs the phase-2/phase-4 bodies — a lab reconfiguration
(disk-mutating; snapshot first), filed as a follow-up.

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
- All non-190-byte SCS envelope classes (58/62/66/70/94/106/110 and the
  206–1500-byte block-transfer classes): header layout beyond the common
  dst/flag/src preamble is entirely unknown.
- The directory-lookup/connect-handshake opcode fields (§4c) — **partially
  closed by §4(g)**: the offset-16 message-type byte and offset-17 `0x13`
  format constant are now grounded, and the `VMS$VAXcluster` connect Con.ID
  binding (§4g phase 4) is grounded; the remaining connect/config body
  integers (START/config length-ish words, per-message credit offset) stay
  unknown.
- **Vote/quorum membership fields** (§4g): the connection manager exchanges
  votes/quorum on join, but no byte could be bound with a single vote config
  on the wire — needs a vote-varying capture (§4g).
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
