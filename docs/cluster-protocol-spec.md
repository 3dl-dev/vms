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
| `scs-dlm-lockconflict.pcap` | 100 | §4(f) DLM section (mixed-op; superseded for field-decode by the `ac4-*` single-op captures) |
| `ac4-{MCR,MCW,MPR,MPW2,MEX}.pcap` | ~15 ea | §4(f).1 lock-mode byte — same resource `OVMXAAAA`, mode-only diff CR/CW/PR/PW/EX = 1/2/3/4/5 (`vms-ac4`, lab-2 `vaxlab-10`) |
| `ac4-RBBBB.pcap` / `ac4-LEN.pcap` | ~15 ea | §4(f).1 resource-name (`OVMXBBBB`) and name-length (`OVMXLONGNAME`, len 0x0c) diffs (`vms-ac4`) |
| `ac4-LKID2.pcap` / `ac4-MPW.pcap` / `ac4-CVT.pcap` | ~20–40 ea | §4(f).1 requester + master lock-ids (byte-exact vs SDA) and the CONVERT opcode `0x07` (`vms-ac4`) |
| `ac4-DENY.pcap` | ~20 | §4(f).1 grant-vs-deny status discriminator (NOQUEUE against an EX holder) (`vms-ac4`) |
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
| 72 | 132 | SYSAP-specific message body | **the transaction envelope is now GROUNDED in §4(j)** (`vms-f85`): a SYSAP-level send-msg#/ack-msg# counter pair, a transaction-id/checksum correlation token, a message-category/flags byte (bit `0x80` = response), and an opcode. The `VMS$VAXcluster` connection-manager add-member dialogue rides here, and the joiner's **VOTES** field is grounded at body-offset 22 (abs 94). The per-opcode DLM lock sub-fields (lock mode, resource name/length, requester + master lock-ids, grant/deny status) are now **GROUNDED in §4(f).1** (`vms-ac4`); the MSCP command sub-fields are grounded in §4(n). |

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

**Update (`vms-ac4`, 2026-08-10): the DLM SYSAP body IS now byte-grounded.**
The `scs-dlm-lockconflict.pcap` scenario above mixed many operations and could
not isolate a single field's meaning (the reason `vms-ac4` was spawned). The
field map below was pinned instead from **fresh single-operation captures** on a
lab-2 pod (`vaxlab-10`), driving KNOWN one-at-a-time `$ENQ`/convert/`$DEQ`
operations at controlled resource names and lock modes and byte-diffing captures
that differ in exactly one variable. See the new subsection **§4(f).1**.

#### 4(f).1 The DLM lock-request/response body — GROUNDED field map (`vms-ac4`)

**Specimens & method (new lab captures, single-operation, one-variable diffs).**
On a healthy 2-node cluster (`vaxlab-10`, `CLUSTER_NODES=2`) a NULL-mode holder
was placed on VAX2 for each test resource (so VAX2 masters the tree and every
VAX1 request crosses the wire and is granted), then VAX1 issued one lock op per
capture via a purpose-built MACRO-32 `$ENQ` driver (`DLMENQ`/`DLMCVT`, resource
name + mode taken from the foreign command line). All DLM traffic rides the
190-byte `VMS$VAXcluster` VC (§4f above) as **category `0x02`** (request) /
**`0x82`** (response) frames (§4j envelope). Body offsets are **SYSAP-body-relative**
(`body[0]` = payload[58] = **abs frame offset 72**; add 72 for the absolute
offset). Captures live at
`/data/training/vax/cluster/captures-lab2/vms-ac4/ac4-*.pcap`; the decoder
`tools/cluster/dlm_body.py --verify <dir>` re-asserts every claim below against
them.

| body off | abs | Size | Field | Grounding (the one-variable diff that pins it) |
|---|---|---|---|---|
| 8 | 80 | 1 | **message category** — `0x02` DLM request, `0x82` response (bit `0x80`) | §4j; reconfirmed on every ac4 frame |
| 9 | 81 | 1 | **DLM opcode** — **`0x01` = new-lock ENQ request**, **`0x07` = lock CONVERT** | **GROUNDED**: the `0x01`/`0x81` pair carries every fresh `$ENQ`; the convert driver's mode-change step is the only `0x07` frame in `ac4-CVT.pcap` (SCA f13, VAX1→VAX2), paired with a `0x87` reply. *(This grounds the `cat 0x02 op 0x01` request `vms-2d6` flagged as "a DLM request we have never grounded".)* |
| 20 | 92 | 4 | **requester lock-id / PID** (LE u32) — in an **ENQ request** it holds the requesting **process PID**; in the **grant response** it is the requester's assigned **local lock-id** | **GROUNDED byte-exact vs SDA** in two captures: the `0x81` grant carries the value SDA `SHOW LOCKS` reports as the requester's *process copy* — `ac4-LKID2` → `0x310000AB`, `ac4-MPW` → `0x0D000380`. In the fresh `0x01` request it instead reads the PID form `0x2020021x` (constant `0x2020021c` across all six console-issued captures = the interactive SYSTEM process; a different value under a SPAWNed subprocess). A CONVERT `0x07` request — where the lock already exists — already carries the real local lock-id here (`ac4-CVT` `0x5000038A`). |
| 24 | 96 | 4 | **master lock-id** (LE u32) — the lock's id on the resource-master node | **GROUNDED byte-exact vs SDA** (the *master copy* id): `ac4-LKID2` → `0x520006AF`, `ac4-MPW` → `0x4A0006AC`, `ac4-CVT` → `0x120004B9`. Present in **both** the request and the response of an established lock (the request references the master's RSB). |
| 30 | 102 | 1 | **requested lock mode** — `NL=0 CR=1 CW=2 PR=3 PW=4 EX=5` | **GROUNDED by a clean six-value one-variable diff** on the *same* resource `OVMXAAAA`: `ac4-MCR/MCW/MPR/MPW2/MEX` read `body[30] = 01/02/03/04/05` and the NL acquire in `ac4-CVT` reads `00`, byte-for-byte the documented encoding (VAXcluster Principles Table 6-1 p. 6-2 and the numeric map NL=0…EX=5 p. 6-3; identical to `lckdef.h` `LCK$K_*`). The CONVERT `0x07` request carries the **new** mode here (`ac4-CVT` NL→EX reads `05`). Cross-checks against the resource diff: `body[30]` is `04` in **both** `ac4-MPW2` (OVMXAAAA) and `ac4-RBBBB` (OVMXBBBB), i.e. it tracks *mode*, not resource. |
| 46 | 118 | 1 | constant `0x03` preceding the resource name | observed constant, all ac4 request frames |
| 47 | 119 | 1 | **resource-name length** (bytes) | **GROUNDED by a length diff**: `0x08` for the 8-byte `OVMXAAAA`/`OVMXBBBB`, `0x0c` for the 12-byte `OVMXLONGNAME` (`ac4-LEN`), `0x16`=22 for the XQP `F11B$aSYSDSK1…` locks seen incidentally in `ac4-CVT`. |
| 48 | 120 | *len* | **resource name** (ASCII, the `$ENQ` `resnam` string) | **GROUNDED by a resource diff**: same mode PW, `ac4-MPW2` carries `OVMXAAAA` and `ac4-RBBBB` carries `OVMXBBBB` at `body[48]` — the only bytes that change are the name itself (`41→42`), with `body[30]` (mode) unchanged. |

**Completion status — GROUNDED as a grant/deny message-shape discriminator, not
a literal code.** Byte-diffing a **granted** PW request (`ac4-LKID2`, resource
held NL) against a **NOQUEUE-denied** PW request (`ac4-DENY`, resource held EX by
VAX2 → `$ENQ` returns `%X2124`) shows the master's reply distinguishes the two
outcomes by *shape*, holding the PW request otherwise constant:

- **granted** → the `0x81` reply **assigns** the requester's local lock-id at
  `body[20]` (the PID placeholder is replaced by a real lock-id, SDA-confirmed)
  and does **not** echo the resource name;
- **denied (`SS$_NOTQUEUED`)** → the reply leaves `body[20]` as the request's PID
  placeholder, clears the mode byte `body[30]` to `0x00`, and **echoes the
  resource name** back at `body[48]`.

The literal VMS status longword (`SS$_NORMAL`=1 / `SS$_NOTQUEUED`=`%X2124`) does
**not** appear anywhere in the reply body — `SS$_NORMAL`'s `0x0001` at `body[4:6]`
is a constant present on grant *and* deny, so it is not the status. The requester
infers the outcome from the reply shape. The exact denied-reply status encoding
is left inferred (§5(dlm)).

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
messages, and this paragraph originally read `8`/`9` as a ninth and tenth of the
same kind. **That reading is superseded by (1f) (`vms-a58`): they are not
connection-control messages at all** — they sit inside the p. 2-43 credit
account, which every one of types `0`–`7` sits outside. Their names are still
unknown. See (1f) and §5.

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
- **Types `8`/`9` — type 8 IDENTIFIED as the special credit message by the
  controlled experiment in (1g); type 9 stays unnamed.** The observations below
  are the pre-experiment record and are kept as written. 58-content class, paired
  request/response on established connections (8 from A handles (X,Y), 9 from
  B handles swapped), envelope-only, credit field = 1 in every inspected
  exemplar; observed immediately preceding teardown
  (`work/control-vax3-late.pcap` frames 5297–5302: `8 → 9 → 6 → 7` on one
  handle pair); real-VAX-sourced instances exist. Candidate (NOT grounded, do
  not name): the credit-flow pair around the p. 2-44 "special credit message"
  (vms-1d2). Decisive experiment and full observations:
  `docs/design-mscp-direction.md` §1.3.

**(1h) `4`/`5` COLLIDED WITH `src/vmsscs/scs_dir.c`'S OWN NAMESPACE — RESOLVED
IN FAVOUR OF THIS SECTION (`vms-754`, 2026-08-06).** REFUTED, `vms-754`:
`src/vmsscs/scs_dir.c` had read `[46:48]`==4/5 as an MSCP connect-ACCEPT/
CONFIRM pair (`SCS_DIR_OP_ACCEPT`/`SCS_DIR_OP_MSCP_CONFIRM`, `vms-760`). Both
readings shared one namespace and could not both be right.

`tools/cluster/scs_t45_measure.py` (`ctest -R scs_t45_figures`) is the
decisive test: an ACCEPT that binds a working connection must be followed, on
that SAME Con.ID pair, by application traffic (`MTYPE` `10`); a REJECT cannot
be, because no connection is left to carry any. Over the 47-capture lab-1
library:

| MTYPE | frames | followed by MTYPE 10 | terminal |
|---|---|---|---|
| `2` ACCEPT_REQ (positive control, undisputed) | 394 | 388 | 6 |
| `4` (disputed) | 733 | 0 | 733 |

Zero of 733 `4` dialogues are EVER followed by application traffic, against
388 of 394 for the message nobody disputes is a real accept. (Of the 733, 453
are VMS-origin — the same figure `scs_reason_measure.py`'s independent
REJECT_REQ census reports over the identical 47-pcap library restricted to
VMS sources — and 280 are OVMX-sourced.)

The exact frame `scs_dir.c` cited as its own grounding
(`af2-firsttimer-established-20260728.pcap`, frame 2584, rel~143.758) makes
the case on its own: read back with no filtering, it is VAX2 (real HW MAC
`08:00:2b:78:56:b9`) sending to VAX1 (logical LAVC `aa:00:04:00:01:04`) — a
REAL VAX addressing ANOTHER REAL VAX, and every source MAC in that entire
capture is one of those two, so there is no OVMX participant in it at all.
"Server-first, OVMX accepts a member's MSCP connect" cannot be what a
two-real-VAX capture shows.

The SAME capture also contains a separate, more legible exhibit: a different
Con.ID family runs NINE consecutive `4`/`5` exchanges between the same two
nodes at ~10 s intervals with a STRICTLY INCREASING Con.ID (one carries an
explicit `0x7b` retransmit marker), and a TENTH attempt switches message type
entirely — to `2`/`3` — and succeeds, its Con.ID pair going on to carry the
94-content MSCP command class. That is retry-until-accepted with the connect
REFUSED nine times running, not nine acceptances of the same connection
followed by a tenth, differently-typed acceptance.

**Conclusion: this section's reading stands. `4` = REJECT_REQ, `5` =
REJECT_RSP.** `src/vmsscs/scs_dir.c`'s `SCS_DIR_OP_ACCEPT`/
`SCS_DIR_OP_MSCP_CONFIRM` naming was a misattribution, corrected there and in
`src/vmsscs/include/scs_env.h` and `src/vmsscs/include/scs_dir.h`. **NOT
settled by this decode:** `src/vmsscs/scsd.c`'s server-first MSCP accept path
still builds and consumes these bytes believing they mean ACCEPT/CONFIRM —
i.e. when a real peer answers OVMX's MSCP$DISK connect with what is actually a
REJECT_REQ, `scsd.c` currently marks the connection bound anyway. Whether that
bears on `vms-abd`'s refusal is an open question; fixing the wire behaviour is
a separate item, out of scope here.

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
- **The 8/9 = "special credit message" candidate was WEAKENED here and is now
  CONFIRMED for type 8 by (1g) below — read the two together.** The weakening
  argument stands as written and was answered by moving SCSFLOWCUSH, not by
  explaining the constant away.
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

**(1e) THE 110-CONTENT TYPE-10 CLASS IS THE MSCP `GET UNIT STATUS` END MESSAGE
— DECODED (`vms-4eb`, 2026-08-06).** Re-derive every figure below with
`tools/cluster/scs_type10_measure.py`; `ctest -R scs_type10_figures` pins this
prose to it and, on a host with the captures, pins it to the packets.

*Sources.* Our own 48 lab-1 captures, plus **AA-L619A-TK** *MSCP Basic Disk
Functions Manual* v1.2 (Apr 1982) — the customer-orderable UDA50 Programmer's
Doc Kit manual, plain copyright page, no distribution restriction, therefore
admissible public documentation under CLAUDE.md rule 8. ⛔ The bitsavers
`dec/dsa/mscp` v2.4.0 files ("DEC CONFIDENTIAL AND PROPRIETARY / RESTRICTED
DISTRIBUTION") and the bitsavers *VAXcluster Disk I/O Internals Manual* are
EXCLUDED and were not read.

**THE CENSUS, AND IT IS NOT LENGTH-RESTRICTED.** Every envelope-conformant SCA
frame in all 48 captures, keyed (content length, message type), real-VAX
sources only (`08:00:2b` / `aa:00:04`; `unclassified_sources` = **0**, and an
unplaceable source REDS the tool rather than joining a population):

| len | mt | VAX | | len | mt | VAX | | len | mt | VAX |
|---|---|---|---|---|---|---|---|---|---|---|
| 58 | 5 | 697 | | 62 | 4 | 453 | | 94 | 10 | 3 621 |
| 58 | 7 | 223 | | 62 | 6 | 220 | | 110 | 0 | 1 101 |
| 58 | 8 | 131 | | 66 | 1 | 778 | | 110 | 2 | 324 |
| 58 | 9 | 89 | | 86 | 10 | 202 | | **110** | **10** | **2 889** |
| 62 | 3 | 258 | | | | | | 190 | 10 | 299 224 |

Type 10 is not a class, it is a **carrier**, and it occupies four length
classes: VAX `{86: 202, 94: 3 621, 110: 2 889, 190: 299 224}` against OVMX
`{94: 585, 190: 7 446}` — OVMX emits type 10 in two of them (its replayed MSCP
command and add-member templates) and has **never** emitted a 110-content one.

**WHAT IDENTIFIES IT — the command/end pairing, 2 889 of 2 889.** For every one
of the 2 889 frames there is an earlier 94-content type-10 frame in the same
capture, on the **mirrored Con.ID pair**, carrying the **same 32-bit value at
body offset 0**. That is MSCP's own correlation rule: AA-L619A-TK §5.1 defines
the command reference number as "a 32-bit, unique, non-zero number identifying
a host command… copied to the end message". **Unmatched: 0.** The opcode
relation across every pair is `0x03 → 0x83`, i.e. exactly Table A-1's
`endcode = command | OP.END` with `OP.GUS = 0x03` and `OP.END = 0x80`. Sources:
`08:00:2b:78:56:b9` 1 126, `aa:00:04:00:01:04` 1 194, `08:00:2b:11:22:33` 569
— the last is vax3's SIMH adapter (`vax3/local.ini`, `set xq
mac=08-00-2b-11-22-33`), so the OUI verdict is cross-checked against the lab's
own configuration and not merely assumed from the prefix.

**THE SYSAP FILTER, because `body[8]` is an opcode only on an MSCP
connection.** Binding Con.IDs to SYSAP names through the connect frames in the
same capture: 1 723 of the 2 889 have both ends bound and **every one** binds
to the `VMS$DISK_CL_DRVR ↔ MSCP$DISK` pair — no other pair, zero residuals; the
remaining 1 166 have no connect frame in-capture (the connection predates it).
The category error, measured on the 94-content class: on that MSCP connection
`body[8]` is `{0x03: 1 715, 0x04: 202}`, **every value a Table A-1 opcode**; on
the `SCS$DIRECTORY ↔ SCS$DIR_LOOKUP` connections the same offset is
`{0x24: 942, 0x56: 178}`, **not one of which is a Table A-1 opcode**. An
unfiltered `body[8]` census over the 94-content class is therefore a category
error, and this is the number that says so.

The bind depends on which 16-byte name field is the sender's, so that is
re-derived here over the whole corpus rather than assumed: on CONNECT_REQ the
pair `([62:78], [78:94])` is (`MSCP$DISK`, `VMS$DISK_CL_DRVR`) 809× and
(`SCS$DIRECTORY`, `SCS$DIR_LOOKUP`) 201×, and on the ACCEPT_REQ answering those
same connections it is those strings **swapped** (101× and 134×). The listeners
are `MSCP$DISK` and `SCS$DIRECTORY` (SDA `SHOW CONNECTIONS`, §3), so `[62:78]`
on a CONNECT_REQ is the **destination** — which is what §4(h)(2) already
grounds by the swap test on one dialogue, now re-derived across all of them.
MSCP confirms it independently: bound the other way, all 2 889 END messages
would be travelling client→controller, and an endcode is by definition what a
controller sends.

**THE FIELDS, against Table A-7 (GET UNIT STATUS end message) and §6.12.**
Body offsets are content-relative to `[58]`; absolute frame offset is body + 72.

| body | field (Table A-7 symbol) | measured over 2 889 VAX frames |
|---|---|---|
| `[0:4]` | `P.CRF` command reference number | pairs 2 889/2 889 to its command |
| `[4:6]` | `P.UNIT` unit number | `0x0000` ×2 485, `0x4000…0x4003` ×101 each |
| `[6:8]` | reserved | zero on every valid-unit frame |
| `[8]` | `P.OPCD` endcode | `0x83` — **2 889/2 889** |
| `[9]` | `P.FLGS` end flags (Table A-3) | `0x00` — 2 889/2 889, no bad block, no error log |
| `[10:12]` | `P.STS` status | `0x03` ×2 485, `0x04` ×402, `0x23` ×2 |
| `[12:14]` | `P.MLUN` multi-unit code | `0x0000/0x0100/0x0200/0x0300` (§6.12: high byte = spindle id) |
| `[14:16]` | `P.UNFL` unit flags | `0x8000` on all 404 valid-unit frames — **see the gap below** |
| `[16:20]` | reserved | zero on every valid-unit frame |
| `[20:28]` | `P.UNTI` unit identifier | `02 00 00 00 NN 00 a1 12`, `NN` = the unit index |
| `[28:32]` | `P.MEDI` media type identifier | `0x2564105c` ×202, `0x25652228` ×202 |
| `[32:34]` | `P.SHUN` shadow unit | `0x0000` — 2 889/2 889 (nothing shadowed) |
| `[36:48]` | track/group/cylinder/RCT geometry | see below |

Every status value is a documented Table B-1/B-2 code: `0x03` = Unit-Offline
sub-code 0 ("unit unknown or online to another controller"), `0x04` =
Unit-Available, `0x23` = Unit-Offline **sub-code 1** ("no volume mounted or
drive disabled"). And §6.12's validity rule holds as an exact **partition with
zero residuals**: the unit identifier is zero on all 2 485 plain Unit-Offline
frames and non-zero on all 404 others — "if unit identifier = 0, virtually no
characteristics are valid", read off the wire rather than quoted at it.

**THE UNITS ARE THE LAB'S OWN DISKS, and that is the independent check.** The
media type identifier decodes by AA-L619A-TK §4.17 + Appendix C (D0/D1 = the
two device-type characters, A0–A2 = one to three media characters, N = two
decimal digits, "A" = 1), calibrated on Appendix C Table C-3's published worked
value **RA80 = `0x25641050`**:

| unit | unit identifier | media id | decodes to | multi-unit | frames |
|---|---|---|---|---|---|
| `0x4000` | `02 00 00 00 00 00 a1 12` | `0x2564105c` | **DU RA92** | `0x0000` | 101 |
| `0x4001` | `02 00 00 00 01 00 a1 12` | `0x2564105c` | **DU RA92** | `0x0100` | 101 |
| `0x4002` | `02 00 00 00 02 00 a1 12` | `0x25652228` | **DU RRD40** | `0x0200` | 101 |
| `0x4003` | `02 00 00 00 03 00 a1 12` | `0x25652228` | **DU RRD40** | `0x0300` | 101 |

The lab's own SIMH configuration, written years before this decode and never
consulted while making it, is `set rq0 ra92 / set rq1 ra92 / set rq2 cdrom /
set rq3 cdrom` (`/data/training/vax/cluster/vax.ini`). Four units, two RA92s
then two CD-ROMs, in that order. The wire and the hardware config agree without
a free parameter.

Geometry follows the media, as §6.12 says it must ("track size … 0 if
inapplicable"): the RA92 rows carry `track 73 / group 13 / cylinder 1 / RCT
size 949 / RBNs 1 / copies 1` (188 frames; 73 × 13 = 949, the RCT being one
cylinder) and all-zero on 14; every RRD40 row is all-zero, 202/202 — a CD-ROM
has no replacement table.

**THE ENUMERATION IS §6.12's `Next Unit` MODIFIER, read off the answered
commands.** Command modifier word `[10:12]` against the unit the end message
returned: modifier `0x0000` → unit `0x0000` ×2 384 (a plain controller-level
probe), and modifier `0x0001` — Table A-2 `MD.NXU`, "returns the next known
unit ≥ the specified unit number, in ascending order" — walking
`0x4000 → 0x4001 → 0x4002 → 0x4003` at 101 frames each and then `0x0000` ×101,
the walk running off the end. The class driver enumerating the served units is
the *behaviour* this class carries, not just the *format*.

**WHAT IS NOT DECODED, and it is exactly four bytes.** Table A-7's last GET
UNIT STATUS field ends at body `[48]`; the SCS payload of this class is 52
bytes. `body[48:50]` is a **constant `0x006e`** across all 2 889 frames.
`0x6e` is 110, this class's own SCA content length, so a length echo and a
plain constant are **indistinguishable on a single-length population** — that
is the honest state of it, not a preference between the two readings.
`body[50:52]` takes 32 distinct values (`0000` ×792, `3142` ×585, `2b20` ×519,
`4924` ×419, …), several of them printable ASCII fragments. **Nothing here
identifies either field. Do not name them and do not emit a guess in them.**
Also undecoded: the unit-flags word reads `0x8000` on all 404 valid-unit
frames, and **Table A-5 defines no bit 15** — it is constant across both media
types, so it is not the removable-media flag either.

**RULING — must OVMX ever EMIT a 110-content type-10 frame? Not until it
serves a disk; and it must PARSE one today.** The frame is an MSCP end message,
and an end message is by definition what a *controller* sends. OVMX is the
client on this connection: it already emits the 94-content `GET UNIT STATUS`
command (195 of the 2 889 VAX end messages in this corpus are answers to an
OVMX-sourced command) and it already parses the reply (`scs_mscp_parse()`,
`tests/vmsscs/test_scs_mscp.c`). So:

- **Emitting one is gated behind, and only behind, `docs/design-mscp-direction.md`
  Phase D** — the disk-server posture question already raised as a gate on
  `vms-54f`. Even the minimal "serves no disk" responder option in that gate
  requires this frame, because *answering* `GET UNIT STATUS` **is** this frame.
  Until Phase D is ruled, OVMX must not emit it: a node that emits an end
  message reporting units it does not serve is the dishonest-success shape
  INV-6 exists to kill.
- **Parsing it is not optional and is not deferred.** It is the answer to a
  command OVMX already sends; dropping it silently would leave OVMX unable to
  tell "no units" from "no answer". The fields above are what a parser is
  entitled to read — and only those; `body[48:52]` must be ignored, not
  replayed with intent.

**(1f) TYPES `8` AND `9`: THE CONSTANT-1 LEAD IS EXHAUSTED, AND THEY ARE NOT
CONNECTION-CONTROL MESSAGES AT ALL (`vms-a58`, 2026-08-06).** Every figure in
this subsection comes out of `tools/cluster/scs_t89_measure.py` over the
47-capture lab-1 library, split by the OUI rule (real VAX = `08:00:2b` or
`aa:00:04`; OVMX's tap MAC reported apart, never mixed in), with the length
census unrestricted and `tools/cluster/census_guard.py` called on it so that a
later edit which narrows the population reds instead of silently under-sampling.
`ctest -R scs_t89_figures` re-derives all of it from the packets on a lab host.

**The unrestricted census by length class.** No length filter — this is the
whole envelope-conformant population, which is what (1b)/`vms-c11` demand and
what no earlier census in this document printed in full:

<!-- CENSUS-T89-A: parsed by tests/vmsscs/test_scs_t89_figures.py. Re-run
     tools/cluster/scs_t89_measure.py; do not hand-edit. -->

| SCA content | msgtype | VMS-origin | OVMX-origin |
|---|---|---|---|
| 58 | `5` | 697 | 1 |
| 58 | `7` | 223 | 42 |
| 58 | `8` | 131 | 0 |
| 58 | `9` | 89 | 42 |
| 62 | `3` | 258 | 123 |
| 62 | `4` | 453 | 280 |
| 62 | `6` | 220 | 42 |
| 66 | `1` | 778 | 338 |
| 86 | `10` | 202 | 0 |
| 94 | `10` | 3621 | 585 |
| 110 | `0` | 1101 | 396 |
| 110 | `2` | 324 | 70 |
| 110 | `10` | 2889 | 0 |
| 190 | `10` | 299224 | 7446 |

A further **93232** SCA-ethertype frames across **39** length classes fail the
envelope-conformance test and are counted but never read at `[46:48]` — that is
(1d)'s rule executed rather than remembered.

**The census by owning SYSAP — and the negative it returns.** Each dialogue is
keyed by its Con.ID handle pair and attributed to the SYSAP its `CONNECT_REQ`
named at `[62:78]` (§4(h)(2)):

<!-- CENSUS-T89-B: parsed by tests/vmsscs/test_scs_t89_figures.py. -->

| SYSAP | dialogues | accepted | torn down | carrying a type `8` |
|---|---|---|---|---|
| `MSCP$DISK` | 1645 | 101 | 0 | 0 |
| `SCA$TRANSPORT` | 32 | 16 | 0 | 0 |
| `SCS$DIRECTORY` | 353 | 188 | 131 | 131 |
| `VMS$VAXcluster` | 154 | 76 | 0 | 0 |
| `?` | 150 | 0 | 0 | 0 |

**Read the third and fourth columns together before drawing anything from this
table.** All 131 type-`8` dialogues are `SCS$DIRECTORY` — and so is every
dialogue in the library that is torn down at all. `MSCP$DISK`, `VMS$VAXcluster`
and `SCA$TRANSPORT` open, accept and then simply outlive the capture. **So the
SYSAP split cannot discriminate an SCA-level exchange from an
`SCS$DIRECTORY`-level one**: the entire teardown population is one SYSAP, and a
"131/131 SCS$DIRECTORY" result is what BOTH hypotheses predict. Recorded as a
measured limit of the corpus, not as a finding.

**The structural invariants, each N-of-N over the library:**

<!-- CENSUS-T89-C: parsed by tests/vmsscs/test_scs_t89_figures.py. -->

| invariant | count |
|---|---|
| type `8` frames / type `9` frames | 131 / 131 |
| dialogues carrying a type `8` | 131 |
| …of those, dialogues that also disconnect | 131 |
| dialogues that disconnect WITHOUT a type `8` | 0 |
| type `8` sender is the connection's opener | 131 |
| type `8` sender is the first `DISCONNECT_REQ` sender | 131 |
| type `9` answers it with the handle pair swapped | 131 |
| frames between the `8` and the `DISCONNECT_REQ`: exactly the `9` | 131 |
| type `8` emitted before the last application message | 0 |
| type `8` frames sourced by OVMX | 0 |
| OVMX-sourced `DISCONNECT_REQ` that INITIATED a teardown (rank 0) | 0 |
| OVMX-sourced `DISCONNECT_REQ` that MATCHED one (rank 1) | 42 |

So the `8`→`9` exchange is **biconditional with teardown** in both directions,
happens **exactly once** per connection, is always opened by the end that opened
the connection and will initiate the disconnect, and is always the last thing on
the connection before `DISCONNECT_REQ`. It is machine-speed, not a timer: 131 of
131 type `8` frames answered, worst case **0.003122 s**, and worst case
**0.002112 s** from the `9` to the `DISCONNECT_REQ`.

**THE CREDIT LEDGER — the measurement that answers the lead.** *VAXcluster
Principles* pp. 2-43..2-44 states a falsifiable rule for the credit field
(quoted with page cites in `src/vmsscs/include/scs_credit.h`): local SCS copies
its Pending Receive Credit count into the field and resets that count to zero;
the receiver adds it to its Send Credit count. That is a per-connection ledger,
so it can be replayed frame by frame and predicted. Doing so over the 131
dialogues, with message types `8`, `9` and `10` counted as consuming a receive
buffer and every other type outside the account:

<!-- CENSUS-T89-D: parsed by tests/vmsscs/test_scs_t89_figures.py. -->

| frames whose `[48:50]` the ledger predicts | msgtype `8` | msgtype `9` | msgtype `10` |
|---|---|---|---|
| agreeing | 131 | 131 | 676 |
| residual | 0 | 0 | 0 |

938 of 938, zero residuals. Two consequences, and they are the deliverable of
this item:

1. **The constant `1` is a COUNT OF ONE, and the lead is exhausted.** It is not
   a version and not a flag. The same field, at the same offset, in the same
   131 dialogues, reads `0` on the first application message of every connection
   (131 frames — nothing has been received yet) and `1` on all 545 later ones;
   across the library it also reads `2`, `3` and `4` on type `10`, and `3`, `6`,
   `8` and `10` on the `CONNECT_REQ`/`ACCEPT_REQ` initial grant. It is `1` on
   types `8` and `9` because every dialogue here is the strictly alternating
   `SCS$DIRECTORY` lookup, which never leaves more than one buffer outstanding
   in either direction. **"Constant" was a property of the workload, not of the
   field** — which is why a value histogram could not settle this and a ledger
   could.
2. **Types `8` and `9` are not connection-control messages.** Message types
   `1`, `3`, `4`, `5`, `6` and `7` carry credit `0` in 100% of their frames in
   this library: connection-control frames neither consume a receive buffer nor
   return one. Types `8` and `9` sit *inside* the credit account and behave
   exactly like the type-`10` application messages beside them. So (1b)'s
   framing — "a ninth and tenth" where ch. 2 draws eight connection-control
   messages — is **refuted by their own credit behaviour**. Whatever they are,
   they are in the same taxon as sequenced messages, not in the taxon `0`–`7`
   occupy.

**One residual, kept because it is evidence.** The initiating `DISCONNECT_REQ`
carries credit `0` while the ledger says `1` is owed, in **131 of 131**
dialogues. That is not a model failure; it is the same rule seen from the other
side — the last credit on a connection is never returned, because the connection
is being destroyed.

**WHAT IS STILL NOT KNOWN, stated as narrowly as the evidence allows.** The
*names* of message types `8` and `9`, and what a SYSAP asks SCS to do that puts
them on the wire. The p. 2-44 **special credit message** is **not** restored as
the candidate, and the reason is no longer the constant: (i) that exchange is
*unconditional* here — 131 of 131 teardowns, never once mid-dialogue on a
low-credit trigger — where p. 2-44 describes a message sent only when the local
Receive Credit count is dangerously low; and (ii) it is *answered* 131 of 131
times with the handle pair swapped, where p. 2-44 describes no reply at all.
Either observation alone is a mismatch; both together are why this section still
**does not name them**. See §5 for the register entry, the eliminated
hypotheses, and the three experiments that would discriminate next.

**(1g) TYPE 8 IS THE SCA SPECIAL CREDIT MESSAGE — GROUNDED BY A CONTROLLED
EXPERIMENT (`vms-f03`, 2026-08-06).** (1c) left the credit reading *weakened*:
types 8/9 carried a **constant 1**, and p. 2-44 requires a special credit
message to carry the *Pending Receive Credit count*, which varies. That
weakening is now explained and the reading is confirmed — by moving the trigger
rather than by finding more frames.

*The knob, and why it is a legitimate one.* p. 2-44 states the VMS trigger
exactly: "The VMS implementation of SCS considers the local Receive Credit count
to be dangerously low if it is less than the sum of the local SYSGEN parameter
**SCSFLOWCUSH** and the remote value for Minimum Send Credits." SCSFLOWCUSH is
a documented, **dynamic** parameter (VAX/VMS V7.3 SYSGEN reports default 1, min
0, max 16, Dynamic=D), so the threshold can be raised on one node with
`WRITE ACTIVE` and no reboot, under a load held otherwise constant. Nothing is
disassembled and nothing is patched — this is observation of a documented
control, Rule 8 clean.

*Method.* One lab-2 replica (`vaxlab-5`, `CLUSTER_NODES`=2 verified on both
nodes before the run). A shared-file lock-contention loop on both nodes held the
SCS message rate at ≈3 500–3 700 messages/s throughout. SCSFLOWCUSH was varied
on **VAX1 only**; VAX2 stayed at 1 for every run and is the matched same-wire
control. Node identity was proven **on the node**: VAX1's own
`NCP SHOW EXECUTOR STATUS` printed `Physical address = AA-00-04-00-01-04`, and
VAX2 answered `%SYSTEM-W-NOSUCHDEV` (no DECnet) so it keeps its SIMH hardware
MAC `08:00:2b:62:02:09`. Each run is a 120 s capture on the pod's `br0`; the
cushion was read back from `SYSGEN SHOW SCSFLOWCUSH` before every capture.

| SCSFLOWCUSH on VAX1 | type-8 frames from VAX1 | rate | type-10 msgs/s |
|---|---|---|---|
| 1 (pre-bracket) | 0 | — | 3 674.7 |
| 0 | 0 | — | 3 639.7 |
| 8 | 26 719 | 222.8/s | 3 556.1 |
| 16 | 64 305 | 536.3/s | 3 229.7 |
| 1 (post-bracket) | 0 | — | 3 570.4 |

**Zero type-8 frames at the default cushion — in 440 367 and 427 958 envelope
frames respectively, bracketing the condition on both sides** — then a count
strictly monotonic in the cushion. The application-message rate barely moves
across the sweep, so this is not a traffic-volume artifact. VAX2, cushion fixed
at 1, emitted **no type 8 in any run**.

*The decode of `[48:50]` — an accounting identity, not a correlation.* Credit
returned by VAX1 rides on two carriers: the ordinary piggyback in the type-10
credit field, and the type-8 message. Summed, they must equal the number of
messages VAX1 received, because a credit is returned per released buffer. They
do, in every run:

| run | credit on type-10 | + credit on type-8 | == msgs VAX2→VAX1 | error |
|---|---|---|---|---|
| cushion 1 (pre) | 224 020 | 0 | 223 975 | 0.020% |
| cushion 0 | 221 439 | 0 | 221 413 | 0.012% |
| cushion 8 | 179 192 | 36 707 | 215 873 | 0.012% |
| cushion 16 | 139 429 | 57 012 | 196 411 | 0.015% |
| cushion 1 (post) | 217 849 | 0 | 217 799 | 0.023% |

Raising the cushion moves credit *between the two carriers* — 100% piggybacked
at the default, 29% carried by type 8 at cushion 16 — while the total stays an
exact count of messages received. `[48:50]` on a type 8 is therefore denominated
in the same units as the piggybacked credit field and is the **Pending Receive
Credit count**, which is p. 2-44's definition of the special credit message.

*A second, independent dose-response — on the value, not the rate.* A higher
cushion fires the message sooner, so it carries a smaller accumulated pending
count: mean credit per type-8 is **1.3738** at cushion 8 and **0.8866** at
cushion 16. The (1c) "constant 1" is thus explained rather than contradicted:
at the default cushion the message fires only at genuine near-exhaustion, where
the pending count is essentially always 1.

*What a type-8 costs, and what type 9 is.* Reverse accounting (VAX2's piggyback
against VAX1's message count) closes on VAX1's **type-10 messages alone** in
every run, cushion-8 and cushion-16 included, to within 25–33 units. Two
consequences: a type-8 message **does not consume a send credit**, and the
credit value on the type 9 **is not a credit return** — adding it overshoots by
exactly its own total (36 707 and 57 012), which would inflate VAX1's send
credit without bound. Type 9 is an echo: over 26 719 and 64 305 pairs, every
type 8 was answered by exactly one type 9 (zero unmatched, zero unanswered,
p50 latency 0.13 ms) carrying the **identical** credit value.

> **Type 9 is deliberately NOT named here.** p. 2-44 describes the special
> credit message and **names no acknowledgment or response to it**. The wire
> shows a strict one-for-one response half that the book does not document, so
> naming it would be exactly the vms-c11 pattern this spec has rejected before.
> Recorded as: *the response half of the special-credit exchange; carries an
> echo of the credited count; book-unnamed.* It is a real book-vs-wire
> divergence and it stays labelled as one.

*Limits of this result, stated plainly.*
- **The handle-swap claim in (1b)/(1c) was NOT tested by this run.** On this lab
  both handle fields carry the **identical** 4-byte value (`0800fd82`), so a
  reversal is undetectable. The pairing proven here is by direction and by
  time-adjacency, not by handle swap.
- All five runs are a **single** lab-2 replica and a single SYSAP (the lock
  traffic of one connection — 8/9 appeared on exactly one connection).
- The result does **not contradict** any lab-1 measurement — it explains lab-1's
  constant-1 population as the default-cushion regime — so `tests/lab/README.md`'s
  reproduce-on-lab-1 rule is not triggered. A lab-1 confirmation would still
  strengthen it.
- SCSFLOWCUSH=0 produced zero type-8 frames, same as 1; the threshold at 0 is
  never reached under this load. Minimum Send Credits stays UNGROUNDED
  (`vms-1d2`) — this experiment moves the *other* term of the sum.
- 7 388 of the cushion-16 type-8 frames carry credit **0**, which p. 2-44's
  "and if the local Pending Receive Credit count is greater than 0" does not
  predict. Recorded as an observed implementation divergence, not smoothed over.

Re-derive: `tools/scs_flowcush_measure.py` (15 checks) against
`/data/training/vax/cluster/captures-lab2/vms-f03/`; gate
`tests/vmsscs/test_scs_flowcush_figures.py`.

**Consequence for `vms-abd`.** §1.4 noted an 8/9 exchange immediately preceding
an accepted DISCONNECT_REQ and asked whether OVMX must reproduce it. It must
not be read as a teardown handshake: type 8 is credit flushing, emitted whenever
receive credit is low, and at teardown a draining connection is exactly when
that happens. There is no evidence of a disconnect **precondition** here, which
**confirms** the reframed hypothesis of `docs/design-mscp-direction.md` §4 —
"Inappropriate SCA Control Message" is a **CSB state mismatch, not a missing
message**. The 8/9-present vs 8/9-absent teardown comparison is no longer the
next move for vms-abd; the peer's CSB state is.

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
- **Category-`0x02` DLM lock sub-fields are now GROUNDED in §4(f).1** (`vms-ac4`):
  opcode `0x01` = ENQ, `0x07` = CONVERT, with the lock-mode / resource-name+length
  / requester + master lock-id / grant-vs-deny fields pinned by single-op
  captures. The remaining undecoded items are the **category-`0x01` opcode
  `0x03`/`0x05`/`0x06`** membership/rebuild sub-field semantics beyond the
  envelope, and the category-`0x04` ack opcode meanings (`0x49`/`0x00`).

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
`[94:110]` over those 1 425 VAX-sourced frames, keyed on the destination SYSAP
name `[62:78]` — §4(h)(2) already grounds `[62:78]` as the CONNECT_REQ's
**TARGET** SYSAP name, not the sender's own. The last column records what
OVMX contributed and the guard removed:

| destination SYSAP | VAX frames | distinct values | value(s) | OVMX frames excluded |
|---|---|---|---|---|
| `MSCP$DISK` | 809 | 1 | ASCII **`"V5.0          + "`** | 243 |
| `SCS$DIRECTORY` | 201 | 1 | 16 ASCII spaces | 113 |
| `SCS$DIR_LOOKUP` | 134 | 1 | 16 ASCII spaces | 55 |
| `SCA$TRANSPORT` | 32 | 2 | `02 02 01 03 …` | 0 |
| `VMS$DISK_CL_DRVR` | 101 | 5 | `00 00 04 a0 00 00 00 00 NN 04 00 00 00 00 04 01` | 0 |
| `VMS$VAXcluster` | 148 | 5 | `01 1b 01 03 …` | 55 |

**`MSCP$DISK` is the decisive row**: a printable ASCII **version string** in the
`CONNECT_REQ` connect data **destined for** the disk-server SYSAP — i.e. it is
the **disk class driver's** version claim, carried **to** `MSCP$DISK`, not
`MSCP$DISK`'s own version — p. 2-25's "which version" read straight off the
wire, at these offsets, with **809** VAX-sourced frames from **5 node
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
window or the join suffers" is not a claim this lab supports. **CORRECTED
(`vms-5c7e`, fixed via the item of that number — the earlier "Filed, not
fixed" note here was itself wrong: the `rd create` that was supposed to file
it ran from a worktree and was ephemeral, so nothing was ever actually filed
until the post-merge veracity audit caught it and vms-5c7e was created for
real).** `OVMX_DISKRUN_GATE_MS`'s default (2000 ms, still inside the
1.4–4.4 s gap) is kept for the reason that survives: it replays where a real
joiner's own disk-client run falls, which is an authenticity match to §4c.8,
not a join-success dependency. `src/vmsscs/scsd.c`'s `scsd_diskrun_gate_ms()`
header and `tests/vmsscs/test_scsd_wire.c`'s
`test_the_diskrun_gate_default_and_override()` carry the corrected framing.

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

#### 4(O.5) `vms-7c0`'s stricter CDL receive path does not regress the `vms-449` rejoin (GROUNDED live, `vms-8e4`)

`vms-449` (§4(O.2)) and `vms-7c0` (routing received application messages through
the CDL, p. 2-29) merged to main independently and had never been exercised
together. `vms-7c0` flagged three of its own tightened behaviors as an
unmeasured risk against the rejoin finding: a 190-content frame to a Con.ID
with no live CDT is now dropped-and-counted (`rx_deliver_no_cdt`) rather than
processed; a frame whose source Con.ID mismatches the bound peer handle is now
refused (`rx_deliver_src_mismatch`); and a previously-accepted defensive-OR
shape is now unreachable. This re-runs the question on current `main`
(`3809b14`, both `vms-449` and `vms-7c0` ancestors).

**Method.** A first-join + rejoin pair on lab-2 `vaxlab-6` — the same pod
`vms-449`'s own bracket used — binary built from this tree, identity `OVMXW1` /
`SCSSYSTEMID` 1610, default environment.

| run | role | verdict | RX-CDL: no-cdt | RX-CDL: src-mismatch | CM 190-class messages |
|---|---|---|---|---|---|
| `A2` | first join | **JOINED, t+26s** | 0 | 0 | 342 (cm-messages) |
| `B1` | rejoin, same identity | **REFUSED** (`CLUSTER_NODES` never advanced) | 0 | 0 | 596 (cm-messages, peer-driven — see caveat) |

**The rejoin-refused finding still holds** on current main, and **neither of
`vms-7c0`'s two countable tightened paths fired on either run** — `no-cdt` and
`src-mismatch` both read 0 in the exit summary for the join and for the
rejoin, so the stricter receive path is not what is refusing the rejoin.

**Caveat, recorded rather than smoothed over.** `vaxlab-6` had already carried
`vms-449`'s own 9-run bracket plus other sessions' traffic earlier the same
day; mid-bracket here, `VAX2` showed `SHOW CLUSTER` status `BRK_NON` (broken,
non-member) rather than the clean 2-node baseline, and `OVMXW1` sat at `NEW`
rather than being refused outright the way `vms-449`'s bracket recorded it.
This is pod wear, not a code path: a companion first-join run on a virgin pod
(`vaxlab-8`, scaled up fresh for this check) never joined at all across two
identities in 156s each — a pod-level SIMH/br0 problem on that specific
replica, distinct from both the `vms-449` and this bracket's `vaxlab-6`
results, and not chased further here (out of this item's scope; a fresh
lab-2 replica should not need to be treated as suspect after one bad boot,
but this one was). Neither confound changes the two RX-CDL counters, which
are read directly off the exit summary and are unaffected by `VAX2`'s state.

**Not claimed:** a byte-clean replication of `vms-449`'s exact bracket shape
(guardrail 20's control-between-runs discipline) — the degraded `VAX2` breaks
that. What is grounded is the counter reading, which is what this item asked
for. Re-derive: `docs/HANDOFF-vms-2f3.md` §4(O.2) for the original bracket;
this section's numbers come from `tests/lab/tools/lab2run.sh` runs
`8e4A2`/`8e4B1` on `vaxlab-6`, 2026-08-06.

#### 4(O.6) The VMS$VAXcluster SYSAP connection DOES reach `0002 open` — `0007 con_sent` "at teardown" was a sampling artifact (GROUNDED live, `vms-694`)

**The premise this item was dispatched on is REFUTED.** The frontier was stated
as: OVMX is admitted at CNXMAN but *the peer's `VMS$VAXcluster ← OVMX` CDT sits
`0007 con_sent`, never `0002 open`, at teardown* — the cluster-management SYSAP
connection supposedly never completes. Measured against the peer's own SDA
**while OVMX is still running**, it completes and stays open; the `con_sent`
reading was an artifact of sampling the peer **after** OVMX exited.

**Method.** Three arms, three **virgin CN_2** lab-2 pods, three **fresh**
`SCSSYSTEMID`s minted through `mk_sysgen.py` (no `%PEA0 … Conflicts`, no
`SCSD-W-VCNOACK` on any arm — §4(w) hygiene satisfied). Unlike the prior rounds
that recorded `con_sent`, the peer's CDT state is sampled **every ~5 s during
the run** via `SDA> SHOW CONNECTIONS` (the `/NODE=` filter resolves only once
the identity is an admitted member; the unfiltered form does not), not only in
one end-of-run snapshot. Runner: a `connwatch.sh` variant sampling all
connections per cycle; identity proven on the wire (`strings … | grep OVMX..`),
guardrail 18.

| arm | binary | pod | identity / SYSSYSTEMID | `VMS$VAXcluster→OVMX` mid-run | at T-END (post-`pkill`) | join |
|---|---|---|---|---|---|---|
| `CRhead` | `main` reverted (no switch) | `vaxlab-0` | `OVMXF0` / 1817 | **29/29 `0002 open`** | `0007 con_sent` (new Con.ID) | `XITDONE=1` |
| `CRctl` | +switch, `OVMX_CONNRSP_FIRST_ONLY=1` (= HEAD's first-only echo path, wire-identical) | `vaxlab-8` | `OVMXD0` / 1813 | **29/29 `0002 open`** | `0007 con_sent` (`36A8000D`) | `XITDONE=1` |
| `CRfix2` | +switch default (echo re-answered every CONNECT_REQ) | `vaxlab-9` | `OVMXE0` / 1815 | **35/35 `0002 open`** | `0007 con_sent` (new Con.ID) | `XITDONE=1` |

**What `con_sent` at teardown actually is.** In every arm the `0007 con_sent`
CDT in the end-of-run `SHOW CONNECTIONS` carries a **different, higher Con.ID**
than the `0002 open` CDT that was present all through the run (e.g. `CRctl`: open
throughout, then at T-END a *new* `36A8000D` in `con_sent` to `OVMXD0`). That is
the peer noticing OVMX vanished (`pkill SCSD.EXE` at T+`DUR`) and **immediately
re-opening** a VMS$VAXcluster connection to the now-dead node — a `CONNECT_REQ`
that can never be answered, hence `con_sent`. It is the peer reacting to OVMX's
death, not a formation failure. Davis Figure 2-14 / p. 2-23 describe the source
leaving CONNECT SENT only on a received `CONNECT_RSP`; with OVMX gone there is no
responder, exactly as the figure predicts.

**A sub-hypothesis was tested and is also refuted: the op=1 CONNECT-ECHO
`first`-gate is not the cause.** `scsd.c`'s member-connect answer emits its op=1
`CONNECT-ECHO` (the `CONNECT_RSP`) only on the first bind (`first =
!ps->connected`), re-answering retransmits with the op=2 `ACCEPT_REQ` alone —
the asymmetry §4(O.1) measured and declined to bless. An `OVMX_CONNRSP_FIRST_ONLY`
kill switch was added to bracket it (`CRctl` = first-only, `CRfix2` =
re-answer-every) and **made no difference to the open outcome**: both reach
`0002 open` and hold it. The switch was reverted; it changed nothing on the wire
that mattered. (`CRfix2` also joins cleanly, `XITDONE=1` — an earlier
`echo-every` arm on `vaxlab-7` stalled at START, `start_acked=0`, before ever
reaching the member-connect path; that was a pod-level SIMH/br0 fault, the same
class §4(O.5)'s caveat records, not the code path.)

**The real open question this relocates the frontier to — recorded, not fixed.**
The peer holds the connection **open**, but OVMX's *own* exit summary for the
same peer reads `connected=no conn[member=untracked joiner=CLOSED]`, `connect_sent=0`,
and OVMX receives **zero 110-byte CONNECT-class frames** (envelope census on
`CRctl`: 190×547, plus 41/58/62/66 — no 110). OVMX's VMS$VAXcluster connection
is bound through the **190-byte add-member CM dialogue** (`SCSD-I-JOINBOUND,
member accepted OUR VMS$VAXcluster connect`), which the peer treats as an open
SCS connection, while OVMX's *SCS connection state machine* never tracks it as
such. So the gap is not on the wire and not `con_sent`: it is that **OVMX's CDT
bookkeeping does not model the VMS$VAXcluster connection the peer already
considers open**. Making OVMX's state machine track that connection (so it can
carry and account for cluster-management SYSAP data, and tear it down cleanly) is
the next increment — not driving a `con_sent` that does not persist.

Captures/logs on the tank volume: `/data/training/vax/k8s-labs/{vaxlab-0,vaxlab-8,vaxlab-9}/logs/d94-CR{head,ctl,fix2}.pcap`
and `scsd-CR*.log`; per-cycle CDT samples in `…/cluster/work/CR{head,ctl,fix2}.csb`. 2026-08-09.

#### 4(O.7) OVMX's OWN state machine DOES model the VMS$VAXcluster connection — `joiner=CLOSED` in the exit summary was a teardown-sampling artifact, and OVMX now latches the membership fact (GROUNDED live, `vms-694`)

**The premise §4(O.6) relocated the frontier to is itself over-read, in the same
teardown-sampling way as `con_sent`.** §4(O.6) recorded that OVMX's exit summary
reads `connected=no ... conn[member=untracked joiner=CLOSED]` while the peer holds
the `VMS$VAXcluster → OVMX` connection `0002 open`, and inferred that OVMX "never
tracks it as an open SCS connection". The daemon's OWN connection-FSM history
(`SCSD-I-CONNFSM` lines, emitted by `scs_conn_fsm_step`) refutes the inference:
on all three CR runs the joiner CDT is driven, BY THE DAEMON off the member's
real frames, through the full Figure 2-14 source column and holds OPEN for the
run —

```
conid=0xC1960002 VMS$VAXcluster->VMS$VAXcluster: CLOSED --SVC_CONNECT--> CONNECT SENT
conid=0xC1960002 VMS$VAXcluster->VMS$VAXcluster: CONNECT SENT --RCV_CONNECT_RSP--> CONNECT ACK
conid=0xC1960002 VMS$VAXcluster->VMS$VAXcluster: CONNECT ACK --RCV_ACCEPT_REQ--> OPEN   (SCSD-I-JOINBOUND)
   … run: 204 cm_responses, sysap_send=210 sysap_recv=217 carried on this CDT …
conid=0xC1960002 VMS$VAXcluster->VMS$VAXcluster: OPEN --SVC_DISCONNECT--> DISC SENT      (graceful teardown at SIGTERM)
conid=0xC1960002 VMS$VAXcluster->VMS$VAXcluster: DISC SENT --RCV_DISCONNECT_RSP--> DISC ACK
conid=0xC1960002 VMS$VAXcluster->VMS$VAXcluster: DISC ACK --RCV_DISCONNECT_REQ--> CLOSED
```

So the connection **is** modelled, across its full lifecycle, and it is the
mirror-image (p. 2-28) of the peer's `0002 open` CDT. The exit summary read
`joiner=CLOSED` only because it samples the CDT AFTER the graceful DISCONNECT the
daemon runs at shutdown — the identical teardown-sampling error §4(O.6) caught in
`con_sent`. The other two fields are not the connection either: `connected=` and
`conn[member=]` track the **member-opened** VMS$VAXcluster connection (`cdt_member`),
which this wire never creates — the member reciprocates the add-member dialogue on
OVMX's OWN joiner VC (envelope census: zero inbound VMS$VAXcluster `CONNECT_REQ`;
`RX-CONTROL` `CONNECT_REQ` are the MSCP/dir reject-storm), so there is one
VMS$VAXcluster connection per peer (`cdt_joiner`), not two.

**What was genuinely missing, and the fix (`vms-694`).** OVMX kept no DURABLE
record that the connection reached OPEN, so every reader — including the item that
dispatched this increment — took the post-teardown/`connected=no` sample as
"not modelled". OVMX now latches `peer_state.vaxcluster_open_reached` the moment
the connection reaches OPEN (`ps_note_vaxcluster_open`, called off the JOINBOUND
`conn_step`), logs `SCSD-I-VAXCLMEMBER` once, and reports `vaxcluster_member=CONNECTED(reached-OPEN)`
in the exit summary. It is read from the CDT the FSM actually drove (no fabricated
connection — anti-LARP), and it SURVIVES the teardown sample; it is cleared only
on a genuine re-START session reset. Fail-pre/pass-post: `test_scsd_wire.c`
`test_joinbound_models_vaxcluster_membership_and_survives_teardown` drives the two
REAL captured frames (`cap_ovmx_joiner_connect_rsp`, `cap_ovmx_joiner_accept_req`)
through the daemon to OPEN, asserts the latch set, then runs the production
`scs_disconnect` and asserts the fact survives — FAILS with the latch removed,
PASSES with it. Wire-invisible (local bookkeeping + one log line; no frame
emitted or changed), so no kill-switch is required.

**Live bracket (CLEAN pod, FRESH identity, `vms-694`, 2026-08-09).** `vaxlab-4`
(virgin `CN_2`), identity `OVMXV0` minted through `mk_sysgen.py` — no `%PEA0`,
no `SCSD-W-VCNOACK` (0/0; §4(w) hygiene satisfied), `OVMXV0` proven on the wire
(guardrail 18):

| what | before this fix | with the fix (this run) |
|---|---|---|
| join | `XITDONE=1`, `CLUSTER_NODES` 2→3 (real F$GETSYI) | unchanged: `XITDONE=1`, `CN_3` at t+13 s |
| latch on the wire | — (no such log line) | `SCSD-I-VAXCLMEMBER … reached OPEN` fires once per peer (joiner conids `0x93EA0002`, `0x93EA0012`) |
| exit-summary membership | `connected=no … conn[member=untracked joiner=CLOSED]` — read as "not modelled" | `vaxcluster_member=CONNECTED(reached-OPEN)` on BOTH peers, *beside* the unchanged `connected=no … joiner=CLOSED` — the modelled fact now survives the teardown sample |

The two fields sitting side by side in the same line (`vaxcluster_member=CONNECTED(reached-OPEN)`
with `conn[joiner=CLOSED]`) is the artifact made visible: the connection reached
OPEN and OVMX models it, while the raw CDT read is the post-teardown CLOSED that
misled §4(O.6)'s reader. Logs: `/data/training/vax/k8s-labs/vaxlab-4/logs/scsd-vms694O7.log`,
`d94-vms694O7.pcap`. 2026-08-09.

#### 4(O.8) The joiner VC's `SCSD-W-CONNNOACT` for `ACCEPT_RSP` was a FALSE ALARM — OVMX builds and sends that frame (the op=3 CONNECT-CONFIRM), and the FSM step was merely blind to it (GROUNDED live, `vms-694`)

**The premise §4(O.7) left as remainder (a) is REFUTED-AS-STATED.** §4(O.7)
flagged: *"`SCSD-W-CONNNOACT` — OVMX still has no `ACCEPT_RSP` builder at OPEN
(connection works without it)"*, reading the warning as a genuine faithfulness
gap — a load-bearing frame OVMX omits. It is not. OVMX **has** a builder for that
frame and **puts it on the wire twice**, once per peer. The warning was fired by a
`conn_step` FSM-bookkeeping call that was passed `emitted=NULL` while the very same
frame went out 41 source lines — and, in the run log, the *same millisecond* —
later.

**The frame the FSM names `send ACCEPT_RSP` IS §4(m)'s op=3 CONNECT-CONFIRM, and
it is LOAD-BEARING.** The Figure 2-14 source-column transition
`CONNECT ACK --RCV_ACCEPT_REQ--> OPEN` carries the action the FSM labels
"send ACCEPT_RSP" (`scs_conn.c`). On the wire that is the 62-byte op=3
CONNECT-CONFIRM the *initiator* sends to acknowledge the peer's op=2
CONNECT-RESPONSE (§4(m)), and on the `VMS$VAXcluster` VC specifically it is what
makes the peer run the add-member dialogue at all (§4(m), "Omit frame 139 and
everything from 145 onward disappears"). It is not optional and OVMX must send it —
and does.

**GROUNDED three ways that OVMX already emits it.**
1. *The real oracle.* In `vax3-2to3-established-join-20260730.pcap` a real VAX
   joiner opens its own `VMS$VAXcluster` VC (initiator Con.ID `0x18E30009`) and
   drives it `op=0 CONNECT-REQ (#132) → op=1 CONNECT-ECHO (#135, M→J) → op=2
   CONNECT-RESPONSE (#136, M→J, binds `0x3552000E`) → op=3 CONNECT-CONFIRM (#139,
   J→M) →` the 190-byte add-member config (#142+). The op=3 confirm precedes the
   config burst, exactly as §4(m) states.
2. *OVMX matches it.* In `d94-vms694O7.pcap` (§4(O.7)) OVMX drives its own VC
   `O→M op=0 → M→O op=1 → M→O op=2 → O→M op=3 CONNECT-CONFIRM`, byte-for-byte the
   same shape, and the OVMX-sourced connection-control histogram carries `op=3 ×2`
   (one per peer) — these are the `ACCEPT_RSP ×2` §4(O)/§4(O.1)/§4(O.2) counted on
   every joining arm. The builder is `scs_dir_build_connect_confirm`, sent at
   `scsd.c`'s JOINBOUND site as `SCSD-I-JOINCONFIRM`.
3. *The log catches the contradiction in the act.* `scsd-vms694O7.log` line 70:
   `SCSD-W-CONNNOACT, conid=0x93EA0012: … 'send ACCEPT_RSP' … OVMX has no builder
   for it -- nothing was sent`; line 73, **same Con.ID, same 17:13:15.248**:
   `SCSD-I-JOINCONFIRM, confirmed OUR VMS$VAXcluster VC (op=3 seq=5)`. The frame
   the warning said was never built was sent three lines later.

**Root cause.** `scsd.c` called `conn_step(ps->cdt_joiner, RCV_ACCEPT_REQ, NULL)`
to record the transition to OPEN, then built and sent the op=3 confirm in a
separate block below. `conn_step`'s `emitted` argument is "the frame the daemon
actually put on the wire for this step"; passing `NULL` made the FSM log the
action as unemitted (`SCSD-W-CONNNOACT`, `actions-required-but-not-emitted++`) for
a frame the daemon does emit. The FSM step and the emit were decoupled.

**Fix (`vms-694`, wire-INVISIBLE).** Build and send the op=3 CONNECT-CONFIRM
first, capture its identity, and pass it to `conn_step` so the FSM records the
emit instead of a false unemitted action. No frame is added, removed, reordered on
the wire, or changed — the confirm was already the next frame out and still is;
only the FSM's internal accounting and one spurious log line change. Because it is
wire-invisible (§4(O.7) precedent), no kill switch is required. Fail-pre/pass-post:
`test_scsd_wire.c` `test_joiner_vc_op3_confirm_is_recorded_not_reported_unbuilt`
drives the two REAL captured frames (`cap_ovmx_joiner_connect_rsp`,
`cap_ovmx_joiner_accept_req`) to OPEN, asserts the op=3 confirm went out
(`SCSD-I-JOINCONFIRM`, frame count rose) AND that the FSM scored **no** unemitted
action / logged **no** `SCSD-W-CONNNOACT` for it — FAILS with the pre-fix `NULL`
(scores 1 unemitted, logs CONNNOACT), PASSES with the emit passed. `vmsscs|scs`
ctest 50/50 + the new case.

**Live bracket (CLEAN pod, FRESH identity, `vms-694`, 2026-08-09).** `vaxlab-10`
(virgin `CN_2`, scaled fresh for this run), identity `OVMXR0`/`SCSSYSTEMID 1852`
minted through `mk_sysgen.py`, `OVMXR0` proven on the wire (guardrail 18); no
`%PEA0`, no `SCSD-W-VCNOACK` (§4(w) hygiene satisfied):

| what | §4(O.7) (pre-fix, `d94-vms694O7`) | with the fix (this run, `d94-vms694O8`) |
|---|---|---|
| `SCSD-W-CONNNOACT` on the joiner VC | 2 (one per peer) | **0** |
| `CONN-FSM actions-required-but-not-emitted` | 2 | **0** |
| `SCSD-I-JOINCONFIRM` (op=3 confirm sent) | 2 | **2** (unchanged) |
| op=3 CONNECT-CONFIRM on the wire | ×2 | **×2** (from OVMX hw MAC `3e:c7:c3:43:69:a7`, `l=0x5F1A0012`, `l=0x5F1A0002` — the two JOINBOUND joiner Con.IDs) |
| join | `XITDONE=1`, `CN` 2→3 | unchanged: `XITDONE=1`, `CN_3` at t+13 s (real VAX `F$GETSYI`) |

The op=3 confirm is on the wire both runs (wire-invisible), the false CONNNOACT is
gone, and admission is unaffected. This is the fourth teardown/artifact-class
frontier this session resolved without a wire change (after bidirectional-accept
§4(y)/#232, con_sent §4(O.6)/#235, joiner=CLOSED §4(O.7)/#237): the op=3 confirm
was never missing — the FSM bookkeeping was blind to it. Logs:
`/data/training/vax/k8s-labs/vaxlab-10/logs/scsd-vms694O8.log`, `d94-vms694O8.pcap`.
2026-08-09.

---

#### 4(O.9) The rejoin refusal is the CM READMISSION transaction, NOT the VC-START identity conflict — an independent HEAD re-bracket, and the two timing-dependent surface modes (GROUNDED live, `vms-694`, 2026-08-09)

**Frame.** After five refutation-class resolutions this session (§4(O.6)–§4(z))
cleared the *fresh-join* accounting artifacts, the single genuine open admission
bug was: **a just-departed identity's rejoin does not complete (`XITDONE=0`).**
This subsection re-brackets it clean at HEAD, ELIMINATES the standing hypothesis
that it is the §4(w)/#230 `SCSSYSTEMID` identity conflict, and locates the stall
precisely. It CONFIRMS and refines §4(O.2)/§4(O.3) (`vms-449`) on current `main`.

**Method.** One virgin lab-2 pod (`vaxlab-10`, scaled up fresh, verified `CN_2`),
`SCSD.EXE` built from `main` at `a283f6a` (all of #221 per-peer Con.ID +
the §4i.B incarnation echo + the `cm_apply_rejoin_form` REJOIN form present),
`OVMX_JOIN_SEQ=1`, `connwatch.sh` sampling VAX1's CDT for our identity *while
OVMX runs* (guardrail 22). Identity proven from capture bytes each run
(guardrail 18). Four arms:

| arm | identity | scenario | verdict |
|---|---|---|---|
| a | `OVMXA4`/1812 | first join, no prior-admission sidecar | **`XITDONE=1`** (peer CSB `member`, `CN` 2→3) |
| c | `OVMXA4`/1812 | rejoin ~45 s later (sidecar present) | **`XITDONE=0`** |
| E | `OVMXA4`/1812 | rejoin ~7 min later (peer CSB in `long_break`) | **`XITDONE=0`** |
| d | `OVMXB0`/1813 | fresh id, same pod, after c/E were refused | **`XITDONE=1`** |

Arm d admits a fresh identity on the very pod that just refused the reused one —
so the stall is **id-reuse, not a broken pod** (guardrail 20, the control sits
after the refusals).

**⛔ The §4(w)/#230 identity-conflict hypothesis is ELIMINATED as the rejoin
cause.** Across all three rejoin/join runs the VC-START completes cleanly: **0
`%PEA0`, 0 `SCSD-W-VCNOACK`, VC reaches `OPEN` on every peer, `start_acked=1`**,
and the member correctly advertises the returning node's **node-incarnation N=2
(arm c) / N=3 (arm E)** in its directed HELLO `[78:80]` (§4i.B), which OVMX reads
off the wire and echoes into its `0x41` START `[22:24]` (`SCSD-I-INCARN` +
`SCSD-I-STARTTX … incarnation=2`). The peer never withholds its round-2 ACK.
Whatever refuses the rejoin lives **downstream of the VC**, at the connection-
manager readmission — exactly where §4(O.2) put it, and NOT at the START gate.
(The `%PEA0, Port has Closed Virtual Circuit` on VAX1's console is the *normal*
teardown when OVMX exits — "lost connection", not "Remote System Conflicts".)

**Two timing-dependent SURFACE modes of the one refusal** (this is the new datum
over §4(O.2), which only sampled the slow mode):

- **Fast rejoin (arm c, residual connection still live).** OVMX returns ~45 s
  after the kill, while the peer still holds arm a's `VMS$VAXcluster ← OVMXA4`
  CDT at `0002 open`. The peer then puts OVMX's *new* joiner-initiated
  `VMS$VAXcluster` CONNECT-REQ into **`000B rej_sent` / blocked `0003 rej_pend`,
  `Rej/Disconn Reason` non-zero** (SDA column-overflows it to `****`) — stable
  T+5 s→T+150 s. This is the first **explicit SCS-connection-layer REJECTION**
  captured on a rejoin (the "reject" the reject-storm thread is named for): the
  member refuses a second `VMS$VAXcluster` connection to a node it still has an
  open one to. No `JOINBOUND`, no CM add-member burst, no barrier. OVMX's joiner
  connection parks at `CONNECT ACK`, `remote_conid=0`.

- **Slow rejoin (arm E, connections torn down, CSB `long_break`).** By ~7 min the
  peer has torn its connections down (`No Connections Found`) and holds OVMXA4's
  CSB in **`wait / long_break`, CSID `00000000`, Nodes back to 2** — never
  freeing it. Now the member **accepts** OVMX's joiner `VMS$VAXcluster` connect
  (`SCSD-I-JOINBOUND local=0x33FE0002 remote=0xD155000E`), OVMX sends the
  add-member burst **and** the REJOIN form of op 0x02 (`SCSD-I-CMREJOIN`,
  `cm_apply_rejoin_form`, `rejoin=1` + founding time/sysid). The member then
  **ABANDONS the CM readmission transaction** — the joiner VC goes `DISC SENT`,
  no state-transition barrier runs, `XITDONE=0`. This is §4(O.2)'s "neither
  completes nor aborts" observation, now located: OVMX's op 0x02 REJOIN form is
  *exercised but incomplete* — it does not carry whatever the member needs to
  re-admit a node whose CSB it holds in `long_break` and re-allocate the fresh
  CSB a real crash-rejoin earns (`csbcycle.sh` §4j).

**Verdict: a real OVMX incompleteness, NOT inherent record-aging.** A real VAX
that is `kill -9`'d, crash-removed and rebooted rejoins under an unchanged
`SCSNODE`/`SCSSYSTEMID` (`vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`), and
the fresh-id controls join every time. Waiting for the peer to reach `long_break`
does **not** rescue OVMX (arm E). So the rejoin is not gated by an aging timeout
one can wait out; it is gated by a CM readmission transaction OVMX does not yet
complete. `cm_apply_rejoin_form` (added by `vms-2f3`) is the right lever and the
right place — it is simply not yet sufficient for the member to finish the
transition.

**Correct operational response TODAY:** mint a **fresh** `SCSNODE`/`SCSSYSTEMID`
for each OVMX (re)start — never reuse a just-departed identity — proven by arm d.
The VMS-faithful behaviour (same identity across restarts, as a real VAX does) is
the next increment: complete the op 0x02 REJOIN-form CM transaction against
`vax3-class03-crash-REJOIN-SUCCESS`. Filed as `vms-694`'s rejoin-completion child.

**Explicit non-claims.** (1) This does not isolate *which* byte of the REJOIN-form
op 0x02 the member requires to finish the transition — that is the follow-up RE.
**(Answered in §4(O.10), and the answer is negative: the op 0x02 body carries exactly
one field OVMX omits (`body[36:40]`), and filling it does NOT finish the transition —
the readmission blocker is not in the op 0x02 body at all.)**
(2) The `Rej/Disconn Reason` value is recorded as *non-zero* (SDA rendered `****`),
not decoded to a specific SCS reject code. (3) The fast/slow split is a function of
how long the peer's own connection teardown takes after our departure; the exact
threshold is not measured (two timepoints, 45 s and ~7 min).

**Evidence** (host, tank volume):
`/data/training/vax/k8s-labs/vaxlab-10/logs/scsd-{a4join1,a4rejoin,a4rejoin2,b0fresh}.log`,
`/data/training/vax/cluster/work/{a4join1,a4rejoin,a4rejoin2,b0fresh}.{csb,status}`,
`d94-{a4join1,a4rejoin,a4rejoin2,b0fresh}.pcap`.

#### 4(O.10) The op 0x02 REJOIN body is not where the readmission stall lives — `body[36:40]` is the one field OVMX omits, and filling it does NOT re-admit (GROUNDED live, `vms-e15`, 2026-08-09)

**Frame.** §4(O.9)'s non-claim (1) — "this does not isolate *which* byte of the
REJOIN-form op 0x02 the member requires" — is the RE this subsection does, against
the SUCCESS oracle `vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`. The answer is
sharper than expected and **negative**: the op 0x02 body carries exactly one field
OVMX omits, and completing it does **not** finish the transition. The rejoin
incompleteness is therefore **not in the op 0x02 body**.

**The op 0x02 body diff (GROUNDED, byte-for-byte).** A full 190-byte diff of OVMX's
rejoin op 0x02 (`d94-a4rejoin2` frame 356, slow mode, HEAD+`cm_apply_rejoin_form`)
against the SUCCESS oracle's rejoin op 0x02 (frame 1297, a real VAX3 crash-rejoining
under an unchanged identity) leaves — after the legitimate identity/session bytes
(MACs, Con.IDs, sequence counters, the per-cluster founding time) — **exactly three
structural differences**, all in the SYSAP body:

| body off | SUCCESS oracle #1297 | OVMX #356 | verdict |
|---|---|---|---|
| `[10:12]` | `41 50` ("AP") | `00 00` | **ruled out** — `e81` rejoin #2969 carries `00 00` here and is re-admitted; vms-760's stale-buffer reading holds |
| `[36:40]` | `09 00 00 00` | `00 00 00 00` | **the sole rejoin discriminator OVMX omits** (see below) |
| `[40:52]` | twelve `0x20` | twelve `0x00` | **ruled out** — spaces on crash-rejoin, zeros on `af2`/`e81`; varies within successes |

`body[36:40]` is **NON-ZERO on 4/4 real rejoin specimens** (`#1297`=9, `af2 #2923`=2,
`af2-firsttimer #2636`=2, `e81 #2969`=3) and **ZERO on 3/3 real first joins**
(`vax3-2to3 #285`, `formation-ci1 #67`, `e81 #255`). Its value is **not
member-validated** — the four rejoiners carry four different values and every one is
re-admitted — so it is read as a **membership generation ordinal** (the af2 capture's
own "2nd/3rd incarnation" language) and OVMX supplies its OWN admission count (spec
sec 5, an OVMX ordinal, not a VMS-authentic byte). Only its **non-zero presence on a
rejoin** is grounded.

**The choreography the member drives for a real rejoiner (GROUNDED, #1297+).** After
VAX3's op 0x02 (frame 1297) the member (VAX2) **immediately** answers **on VAX3's
joiner VC**: `cat 0x04 op 0x04` (1299), then `cat 0x01 op 0x03` **commit** (1303,
VAX3 `0x81`-responds 1304), `cat 0x01 op 0x05` **lock-rebuild** (1306+, VAX3
`0x81`-responds 1313–1316), `cat 0x01 op 0x06` **barrier** (1320+). That is the
readmission a rejoiner earns.

**⛔ Filling `body[36:40]` does NOT trigger it — PROVEN LIVE.** A slow-mode bracket on
lab-2 `vaxlab-10` (virgin pod, fresh identity `OVMXG0`/1888, `SCSD.EXE` built with
the `body[36:40]` change):

| arm | env | scenario | on the wire | verdict |
|---|---|---|---|---|
| A | default | first join | op 0x02 first-join form (`[36:40]=0`) | **`XITDONE=1`**, sidecar `generation=1` persisted |
| B | `OVMX_REJOIN_GEN=0` | rejoin, peer still holds member | fast-refuse: peer rejects the joiner connect, op 0x02 never sent | **`XITDONE=0`** |
| D | default | rejoin from `long_break` (slow mode) | op 0x02 REJOIN form, **`[36:40]=02`** on the wire (frame 323), member accepted the connect (`JOINBOUND`) | **`XITDONE=0`** |

In arm D the member **accepted** OVMX's connect and OVMX's op 0x02 with `[36:40]=2`
**reached it on the JOINBOUND VC** — and the member then sent **ZERO sequenced frames
back on OVMX's joiner VC** (only HELLOs), byte-for-byte the same silence as the
`[36:40]=0` pre-fix run (`d94-a4rejoin2` frame 356: the member likewise answered the
joiner VC with HELLOs only, 0 sequenced frames). **The stall is identical with and
without `body[36:40]`.**

**Verdict.** `body[36:40]` is a **grounded, regression-free wire correction** — a real
rejoiner carries it, OVMX now does too, and it is a prerequisite a completed
readmission needs — but it is **not** the readmission fix. The blocker is the member
**declining to reciprocate config on the joiner VC** — the same `NEW → MEMBER`
non-reciprocation point §4(L)(7) hit for the *fresh* join, now on a *rejoin* — for a
reason that is **not in the op 0x02 body** (ruled out live). What makes the member
drive VAX3's joiner VC (frame 1299) but not OVMX's is the open frontier. Candidate,
observed but not isolated: on some timing arms the member opens its OWN
`VMS$VAXcluster` CONNECT-REQ *to* the returning node (re-establishing the residual
CDT it still holds) and OVMX has **no `CONNECT_RSP` builder** for it (`SCSD-W-CONNNOACT`
in `scsd-e15C.log`) — a rejoin-specific exchange a first join never sees. **Filed as
`vms-694`'s next rejoin-completion increment.**

**What shipped (`vms-e15`).** OVMX emits `body[36:40]` = its persisted membership
generation on a rejoin (`scs_member_build_config`; `cm_apply_rejoin_form` supplies
the value from the prior-admission sidecar, which now also persists the count).
Kill-switch `OVMX_REJOIN_GEN=0` forces the field back to zero. Fail-pre/pass-post
wire-shape test in `tests/vmsscs/test_scs_member.c::test_op02_rejoin_form`.

**Explicit non-claims.** (1) This does **not** re-admit a reused identity —
`XITDONE=1` on a rejoin is NOT achieved and stays open on `vms-694`. (2) `body[36:40]`
is grounded only as "non-zero on a rejoin"; the ordinal *reading* and OVMX's value are
an OVMX design choice (spec sec 5). (3) The member's joiner-VC non-reciprocation is
located but its trigger is not isolated.

**Evidence** (host, tank volume):
`/data/training/vax/k8s-labs/vaxlab-10/logs/scsd-e15{A,B,C,D}.log`,
`/data/training/vax/cluster/work/e15{A,B,C,D}.{status,csb}`,
`d94-e15{A,B,C,D}.pcap`; op 0x02 diff tool `/tmp/framediff.py` (session-local).

#### 4(O.11) On a rejoin the CM readmission runs on the MEMBER-INITIATED VMS$VAXcluster connection (rejoiner = TARGET), not on the joiner's own outbound VC — and the `CONNECT_RSP`-builder candidate is REFUTED (GROUNDED, SUCCESS oracle + live first-join/rejoin bracket, `vms-2248`, 2026-08-10)

**Frame.** §4(O.10) located the rejoin blocker at "the member declines to
reciprocate config on OVMX's joiner VC" and left two things open: (1) *why* the
member drives the real rejoiner's joiner VC (SUCCESS #1299) but not OVMX's, and
(2) the candidate that OVMX's missing `CONNECT_RSP` builder (`SCSD-W-CONNNOACT`)
for the member's inbound `VMS$VAXcluster` connect is the gap. This subsection
answers (1) against the SUCCESS oracle and **refutes** (2).

**The connect census of the SUCCESS oracle (GROUNDED, `vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`, every `CONNECT_REQ` — `rcid==0`, SYSAP name in body — over the readmission window f1170–f1400).** The
crash-rejoiner VAX3 opens only two connections outbound, and **neither is
`VMS$VAXcluster`**:

| frame | dir | SYSAP | who opens |
|---|---|---|---|
| f1184 | VAX1→VAX3 | `SCS$DIRECTORY` | member → rejoiner |
| f1202 | VAX1→VAX3 | **`VMS$VAXcluster`** | **member → rejoiner** |
| f1230 | VAX2→VAX3 | `SCS$DIRECTORY` | member → rejoiner |
| f1248 | VAX2→VAX3 | **`VMS$VAXcluster`** | **member → rejoiner** |
| f1264 | VAX3→VAX1 | `SCS$DIRECTORY` | rejoiner → member |
| f1277 | VAX3→VAX1 | `MSCP$DISK` | rejoiner → member |

Both established members open the `VMS$VAXcluster` connection **to** the
returning node; the rejoiner **never opens its own outbound `VMS$VAXcluster`
connect**. The rejoiner answers each member's connect as the Figure-2-14
TARGET (NODE_2): for VAX2's f1248 (`lcid=0cc6000d`, `rcid=0`), VAX3 sends the
`CONNECT_RSP` echo f1249 (`rcid=0cc6000d`, `lcid=0`) then the `ACCEPT_REQ`
f1250 supplying its own conid (`lcid=45b80009`); VAX2 `ACCEPT_RSP`s f1252 and
the connection opens. **The entire readmission choreography then runs on that
member-initiated connection, on VAX3's TARGET-side conid `45b80009`**: op 0x02
f1297 (`lcid=45b80009`) → member reciprocation `cat 0x04 op 0x04` f1299 → op
0x03 commit f1303 → op 0x05 lock-rebuild f1306 → op 0x06 barrier f1320. This is
the "Rule of Total Connectivity" (§4(y), Davis p.7-39) made byte-concrete for a
rejoin: the coordinator re-opens its `VMS$VAXcluster` connection to the
returning node and drives CM readmission over it.

**The OVMX bug (GROUNDED, live first-join vs rejoin bracket, same pod
`vaxlab-10`, same build — arms A/C/D).** OVMX's join sequencer *always* opens
its OWN outbound `VMS$VAXcluster` connect at join step 7/8 and drives op 0x02 /
CM on that outbound joiner VC. That is correct for a FIRST join and WRONG for a
rejoin:

| arm | scenario | OVMX outbound VMS$VAXcluster | member-initiated inbound | op 0x02 | result |
|---|---|---|---|---|---|
| A | first join | `0x28E40002` reaches **OPEN** (`CONNECT ACK --RCV_ACCEPT_REQ--> OPEN`, line 82 — member ACCEPTs it) | — | sent on outbound VC (line 317), member acks 0x04 | **`XITDONE=1`** |
| C | rejoin, fast | `0x9CEF0002` **parks in CONNECT ACK forever** (member sends bare `CONNECT_RSP`, never `ACCEPT_REQ`; line 1133) | `0x9CEF0001` reaches **OPEN** (line 143) — but the sequencer never touches it | **NEVER SENT** (sequencer waits on its own outbound to OPEN) | **`XITDONE=0`** |
| D | rejoin, slow (`long_break`) | `0xE0860002` accepted (`JOINBOUND`, line 110) | none opened this arm | sent on outbound VC (line 292) — member sends **zero** sequenced frames back | **`XITDONE=0`** |

The discriminator is the acceptance of OVMX's *outbound* `VMS$VAXcluster`
connect: a first-join member ACCEPTs it and reciprocates on it; a rejoin member
either refuses it and opens its own inbound instead (arm C — the SUCCESS
oracle's topology, which OVMX then ignores for CM) or accepts it but still will
not reciprocate config on it (arm D). Either way the member's CM readmission is
bound to the **member-initiated** connection, and OVMX drives CM only on its
**own outbound** VC.

**The `CONNECT_RSP`-builder candidate is REFUTED (6th refutation-class finding
of this thread).** The `SCSD-W-CONNNOACT` for the member's inbound
`VMS$VAXcluster` `CONNECT_REQ` is a FALSE ALARM of the same class as §4(O.8):
OVMX ALREADY answers that inbound connect and it reaches OPEN. e15C, conid
`0x9CEF0001` remote `0x302B000F`: `CLOSED --RCV_CONNECT_REQ--> CONNECT REC`
(CONNNOACT fires here — no separate echo frame) `--SVC_ACCEPT--> ACCEPT SENT`
(`SCSD-I-CONNRESP, answered peer VMS$VAXcluster CONNECT-REQUEST`)
`--RCV_ACCEPT_RSP--> OPEN` (line 143). The connection the member opened is
bidirectionally OPEN on OVMX's side. Building a standalone `CONNECT_RSP` echo
would not change readmission: the gap is not that OVMX fails to *answer* the
member's connection, it is that OVMX never *drives CM readmission on* it.

**Verdict / the fix (next increment, `vms-2248`'s child).** On a rejoin
(`SCSD-I-PRIORCLU`/`CMREJOIN` detected), OVMX must run the op 0x02 CM
readmission — and the op 0x04-ack / op 0x03-commit / op 0x05-lockrb / op 0x06-
barrier dialogue that follows — on the **member-initiated `VMS$VAXcluster`
connection** (the TARGET-side CDT that reaches OPEN), NOT on its own outbound
joiner VC. Concretely the sequencer must stop gating step 8 on its own outbound
connect reaching OPEN for a rejoin, and instead bind `cm_apply_rejoin_form` +
the deferred op 0x02 send to the member-initiated CDT once that reaches OPEN.
This is **wire-visible** (op 0x02 moves to a different conid, and OVMX's
redundant outbound connect should be suppressed to match the oracle, which
never sends one) → it ships an env kill-switch, a fail-pre/pass-post
frame-replay test, and a live rejoin bracket flipping `XITDONE` 0→1 vs the
current stall. This is more than one increment beyond this RE task and is filed
as such.

**Explicit non-claims.** (1) The op 0x02 body is not re-opened — §4(O.10) ruled
it out; this is a *connection-selection* bug, not a payload bug. (2) The slow
arm (D) shows the member accepting OVMX's outbound yet not reciprocating; the
oracle-grounded fix targets the member-initiated connection (arm C's topology,
which the oracle confirms), and whether arm D's member would reciprocate once
OVMX stops competing with its own outbound is a live question for the fix
increment's bracket, not asserted here. (3) The readmission opcode-level
choreography (0x02/0x04/0x03/0x05/0x06) is grounded as the SUCCESS oracle's
frame *sequence and conid*; the SYSAP-body opcode fields inside each remain as
§4(L)/§5 have them.

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(connect census f1170–1400; choreography f1248–1320); live bracket
`/data/training/vax/k8s-labs/vaxlab-10/logs/scsd-e15{A,C,D}.log`
(arm A first-join OPEN+`XITDONE=1`; arm C outbound-parked-CONNECT-ACK + inbound
OPEN ignored; arm D outbound JOINBOUND + no reciprocation); OVMX code sites
`src/vmsscs/scsd.c` join sequencer step 7/8 (`SCSD-I-JOINSEQ ... VC connect`)
and `conn_step`/`scsd_svc_no_builder` (the CONNNOACT that is the false alarm),
`src/vmsscs/scs_conn.c` Figure-2-14 target column (rows at lines 65–82).

#### 4(O.12) The rejoin fix: bind CM readmission to the member-initiated connection on a rejoin (`vms-4838`)

**Frame.** §4(O.11) located the bug (OVMX drives op 0x02 on its own outbound
joiner VC; a rejoin member reciprocates only on the connection *it* opened) and
specified the fix. This subsection records the implementation and its brackets.

**The change (`src/vmsscs/scsd.c`, wire-visible, kill-switched).** A rejoin is
detected at runtime by the PRIORCLU fact `ovmx_prior.valid` — this identity holds
a persisted prior-admission sidecar, i.e. a member legitimately still holds a
residual CDT for it. `cm_rejoin_target_mode()` gates the change on that fact and
on the `OVMX_REJOIN_TARGET` kill-switch (`=0` forces the historical first-join
topology). On a rejoin OVMX now:

1. **suppresses its own outbound `VMS$VAXcluster` connect** at join step 7/8
   (the `OVMX_NO_OWN_VC` branch now also fires automatically for a rejoin), and
   awaits the member's inbound connect;
2. **drives the add-member burst — and the deferred op 0x02 that follows it — on
   the member-initiated CDT's Con.ID pair** (`PS_LOCAL_CONID(ps)` /
   `ps->remote_conid`), not on its own joiner handle. Two triggers cover it: the
   server-first reactive trigger (if the member sends config first) and, because
   the SUCCESS oracle shows the TARGET sends op 0x02 *first* (f1297, before the
   member reciprocates at f1299), a **proactive** trigger (`SCSD-I-CMREADMIT`)
   that fires the burst on the HELLO clock the moment the member-opened
   connection reaches OPEN.

The frame **contents** are unchanged from what OVMX already sends — this is a
connection-**selection** change, not a payload change (§4(O.11) non-claim 1).

**Unit bracket (fail-pre/pass-post + matched control).**
`tests/vmsscs/test_scsd_wire.c:test_rejoin_drives_readmission_on_the_member_-`
`initiated_connection` drives the daemon off real captured frames: OVMX answers
the member's `VMS$VAXcluster` CONNECT-REQUEST (binding `cdt_member` as the
TARGET), the member's ACCEPT_RSP drives that connection to OPEN, and scsd.c's own
tail path runs `cm_send_config_burst()` on the member Con.ID pair. Arm A (fix
active) → the burst rides the member connection (`cfg_local_conid` =
`PS_LOCAL_CONID`, `cfg_remote_conid` = the member's handle). Arm B
(`OVMX_REJOIN_TARGET=0`) → no burst rides the member connection (the pre-fix
stall). `scs`/`vmsscs` ctest 50/50.

**Live bracket (lab-2 `vaxlab-9`, fresh CN_2 pod, identity `OVMXN0/1993`, one
pod = one isolated 2-node VAXcluster; `OVMX_JOIN_SEQ=1`, 2026-08-10).** The
departed→rejoin triple, same identity throughout:

| arm | scenario | topology observed | XITDONE |
|---|---|---|---|
| 1 | first join (no sidecar; fix inactive — `cm_rejoin_target_mode()`=0) | OVMX opens its own outbound VC (step 7/8), JOINBOUND, op 0x02 on OUR joiner VC | **1** (unchanged) |
| 2 | rejoin same-id, `OVMX_REJOIN_TARGET=0` (kill-switch, pre-fix) | `JOINSEQ VC connect local=0xA8D30002 step 7/8`; op 0x02 on OVMX's OWN outbound joiner VC | **0** |
| 3 | rejoin same-id, fix active | `NOT opening our own VC (REJOIN, member-initiated topology)`; add-member burst + op 0x02 driven on the **member connection** (`CMCONFIG ... local=0x99D10001 remote=0x0B660010`) | **0** |

**Verdict — the connection-selection fix is proven but NECESSARY-NOT-SUFFICIENT.**
Arm 3 vs arm 2 is a clean, wire-visible bracket: the fix does exactly what
§4(O.11) specified — OVMX stops opening its redundant outbound `VMS$VAXcluster`
connect and drives the op 0x02 readmission on the member-initiated connection, as
the SUCCESS oracle does. But **`XITDONE` did not flip to 1**: with op 0x02 now on
the correct (member-initiated) connection, the member STILL does not reciprocate
(`op 0x04` ack = 0, no op 0x03 commit, no barrier). So §4(O.11)'s connection-
selection is confirmed necessary (the oracle's topology) but is **not sufficient**
to complete readmission — the member's non-reciprocation persists on the correct
connection. That non-reciprocation is the relocated frontier (filed on `vms-694`).
(Note: in this lab run the member sent its config first, so the reactive
`SCSD-I-CMCONFIG` trigger fired and the proactive `SCSD-I-CMREADMIT` path — for
the oracle's target-sends-op-0x02-first ordering — was not exercised live; it is
covered by the unit bracket.)

**Kill-switch (guardrail 23).** `OVMX_REJOIN_TARGET=0` reproduces the pre-fix
behaviour (arm 2: op 0x02 on OVMX's own outbound VC); with it unset a rejoin
drives readmission on the member-initiated connection (arm 3). RUN and confirmed
suppressed before recording the above.

**Evidence** (host, tank volume): live bracket
`/data/training/vax/k8s-labs/vaxlab-9/logs/scsd-n0{join1,rejoinoff,rejoinfix}.log`
and `/data/training/vax/cluster/work/n0{join1,rejoinoff,rejoinfix}.status`;
unit `tests/vmsscs/test_scsd_wire.c`; code
`src/vmsscs/scsd.c` (`cm_rejoin_target_mode`, step 7/8 branch, `SCSD-I-CMREADMIT`
proactive burst, `SCSD-I-CMCONFIG2`).

#### 4(O.13) On the member connection the rejoiner's add-member advertisement is model→params→config as THREE CONTIGUOUS SYSAP messages with NO standalone cat-0x04 ahead of the op 0x02 — OVMX inserted one, shifting the config to sms=4; removing it makes the op 0x02 byte-match the oracle but STILL does not re-admit (GROUNDED, SUCCESS oracle + live vaxlab-11 bracket, `vms-71d`, 2026-08-10)

**Frame.** §4(O.12) put OVMX's op 0x02 on the member-initiated connection (the
oracle's topology) but `XITDONE` stayed 0: the member sends no `op 0x04`
reciprocation. This subsection isolates, against the SUCCESS oracle, exactly what
the rejoiner's op 0x02 must look like on that connection — and finds a grounded
SYSAP-sequence divergence, fixes it, and records that the fix is (again)
necessary-not-sufficient.

**The SUCCESS-oracle SYSAP sequence on the member connection (GROUNDED,
`vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`, connection VAX3 `lcid=45B80009`
/ VAX2 `lcid=0CC6000D`).** The crash-rejoiner VAX3 sends its add-member
advertisement as THREE CONTIGUOUS SYSAP messages, then the member reciprocates
~1 ms later:

| frame | dir | cat/op | SYSAP `sms` | SYSAP `ams` | SCS `send_seq` |
|---|---|---|---|---|---|
| f1257 | VAX3→VAX2 | 0x01 / 0x14 (model) | 1 | 0 | 10 |
| f1258 | VAX3→VAX2 | 0x01 / 0x01 (params) | 2 | 0 | 11 |
| f1298 | VAX3→VAX2 | 0x01 / 0x02 (config) | **3** | 2 | 14 |
| f1299 | VAX2→VAX3 | 0x04 / 0x04 (reciprocation) | 3 | **3** | 14 |

VAX3's op 0x02 is SYSAP `sms=3`, immediately following model(1)+params(2), and
its `ams=2` acknowledges the member's own model+params inline. VAX3 emits **no
standalone cat-0x04 ack in this window**; its FIRST is f1330 (`sms=9`), AFTER the
member's op 0x04. The member's reciprocation f1299 acks VAX3's op 0x02 both at the
SCS level (`recv_ack=14` = the config's `send_seq`) and at the SYSAP level
(`ams=3` = the config's `sms`). The op 0x02, arriving in-sequence as the expected
next SYSAP message, IS the trigger.

**The OVMX divergence (GROUNDED, live vaxlab-11 fresh CN_2 pod, identity
`OVMXR1/1971`, `OVMX_JOIN_SEQ=1`, HEAD before the fix, 2026-08-10).** On the
member connection (`lcid=0xBF240001`) OVMX's SYSAP stream was: op 0x14 (`sms=1`),
op 0x01 (`sms=2`), **cat 0x04 op 0x00 (`sms=3`)**, op 0x02 (`sms=4`, `ams=2`). The
poll-loop flush (`cm_send_ack`, scsd.c) fired between params and the deferred op
0x02 — the very ack the daemon's own `SCSD-W-STRAYACK` flags as "the reference
acks within ~1 ms and never acks an op 0x01" — consuming `sms=3` and pushing the
config to `sms=4`. The coordinator (`08:00:2b:e0:68:e3`) received it and sent
nothing back; `XITDONE=0`. So OVMX inserts a SYSAP message the oracle never sends
in that window, and its op 0x02 does not ride the contiguous `sms=3` slot the
oracle's does.

**The fix (`src/vmsscs/scsd.c`, `rejoin_hold_standalone_ack`, wire-visible,
kill-switched).** On a rejoin (`cm_rejoin_target_mode()`), HOLD the standalone
cat-0x04 CM ack — at both emission sites, the poll-loop flush and the receive-path
batch ack — until the deferred op 0x02 has ridden the member connection
(`!joiner_cfg2_sent`). The op 0x02's own `ams` field (`=sysap_recv=2`) carries the
acknowledgment inline, exactly as the oracle carries it (f1298 `ams=2`); after the
op 0x02 the hold releases and normal flushing resumes for the member-driven
commit / lock-rebuild / barrier round (the reference's post-reciprocation acks,
first at f1330). Kill-switch `OVMX_REJOIN_ACKHOLD=0` restores the pre-fix
behaviour. Scoped to the rejoin readmission so the working first-join path is
byte-unchanged (guard 8).

**Unit bracket (fail-pre/pass-post + matched control).**
`tests/vmsscs/test_scsd_wire.c:test_rejoin_op02_is_contiguous_sysap_msg3_-`
`not_shifted_by_a_stray_ack` drives the production receive path + `cm_send_config_-`
`burst()` + the production `rejoin_hold_standalone_ack()`-gated `cm_send_ack()`,
then reads the SYSAP number the deferred op 0x02 would consume. ARM A (fix) → 3;
ARM B (`OVMX_REJOIN_ACKHOLD=0`) → 4 (the pre-fix stray-ack shift); first-join
(no PRIORCLU) → the gate never holds. Neutering the gate reds ARM A. `scs`/`vmsscs`
ctest green.

**Live bracket (lab-2 `vaxlab-11`, fresh CN_2 pod, identity `OVMXR1/1971`,
`OVMX_JOIN_SEQ=1`, departed→rejoin triple, 2026-08-10).**

| arm | scenario | op 0x02 on member conn | XITDONE |
|---|---|---|---|
| A1 | first join (no sidecar) | — (fix inactive) | **1** |
| B1 | rejoin same-id, fix active | `sms=3`, contiguous after model(1)+params(2); stray cat-0x04 HELD to `sms=4` AFTER the config (f228 then f242) | **0** |
| C1 | rejoin same-id, `OVMX_REJOIN_ACKHOLD=0` (kill-switch) | cat-0x04 `sms=3` THEN op 0x02 `sms=4` (f203/f204) — pre-fix | **0** |

**Verdict — the SYSAP-contiguity fix is proven wire-visible but
NECESSARY-NOT-SUFFICIENT.** Arm B1 vs C1 is a clean, kill-switched, wire-visible
bracket: the fix makes OVMX's op 0x02 byte-match the oracle's SYSAP framing
(`sms=3`, `ams=2`, no standalone cat-0x04 ahead of it, contiguous after model +
params). But `XITDONE` did not flip — the member still does not reciprocate.

**The relocated frontier (GROUNDED, live).** With the fix, on the member
connection (`lcid=0xF08A0011` / member `lcid=0xE5520010`) the member's LAST frame
before going silent is its own op 0x01 params (`send_seq=21`, `recv_ack=18`); after
OVMX sends model (`ss=21`), params (`ss=22`) and op 0x02 (`ss=24`), the member's
`recv_ack` never advances past 18 — it never SCS-acknowledges OVMX's readmission
frames, and sends no reciprocation and no further sequenced frame on that
connection. In the SUCCESS oracle the member's reciprocation f1299 carries
`recv_ack=14`, exactly the config's `send_seq`. So the residual is at the SCS
sequenced-**delivery** layer or the member's CM readmission-state gate: even a
byte-faithful op 0x02 is not being accepted/delivered on OVMX's member connection.
A live candidate to run next: OVMX draws `send_seq` from one per-peer VC counter
across all of its connections to that peer, so the member connection sees a sparse
subsequence (21, 22, 24 — 19/20/23 consumed on other conids); whether the member's
per-connection expected-next rejects that gap is the open question. Filed on
`vms-694`. **[ANSWERED — see §4(O.14): NO. `send_seq` is per-VC (Davis pp.
2-30/2-31), the oracle's rejoiner→member stream is CONTIGUOUS, and the "sparse
subsequence" here was a measurement artifact of tabulating only the 190-byte
frames. The per-connection-counter candidate is REFUTED; the residual is the
member freezing its per-VC `recv_seq` mid-stream, not a per-connection gap.]**

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(SYSAP sequence f1257/f1258/f1298/f1299; first standalone cat-0x04 f1330); live
bracket `/data/training/vax/k8s-labs/vaxlab-11/logs/scsd-71dA2rejoinfix.log`
(pre-fix op 0x02 `send_msg=4`), `scsd-71dB1fix.log` + `d94-71dB1fix.pcap` (fix: op
0x02 `sms=3`, cat-0x04 held to `sms=4`; member `recv_ack` stuck at 18),
`scsd-71dC1killsw.log` + `d94-71dC1killsw.pcap` (kill-switch: cat-0x04 `sms=3`
then op 0x02 `sms=4`); driver `tests/lab/tools/rejoin_arm_lab2.sh`; code
`src/vmsscs/scsd.c` (`rejoin_hold_standalone_ack`, the two `cm_send_ack` call
sites); unit `tests/vmsscs/test_scsd_wire.c`.

#### 4(O.14) SCS `send_seq` is PER-VIRTUAL-CIRCUIT, not per-connection — the §4(O.13) "sparse subsequence" reading was a MEASUREMENT ARTIFACT, and the per-connection-counter candidate is REFUTED; the real residual is the member freezing its per-VC `recv_seq` mid-stream so the op 0x02 (behind the frozen point) is never delivered (GROUNDED, transcript pp. 2-30/2-31 + SUCCESS oracle + fresh live lab-2 bracket, `vms-bc2`, 2026-08-10)

**Frame.** §4(O.13) closed with a candidate: OVMX "draws `send_seq` from one
per-peer VC counter across all of its connections to that peer, so the member
connection sees a sparse subsequence (21, 22, 24 — 19/20/23 consumed on other
conids); whether the member's per-connection expected-next rejects that gap is
the open question." This subsection answers that question against the public
docs, the SUCCESS oracle, and a fresh live rejoin, and the answer is
**negative on every axis**: `send_seq` is a per-VC counter (so a per-connection
subsequence is expected and correct, not a bug), OVMX already implements it that
way, and the "sparse subsequence" table in §4(O.13) was an artifact of tabulating
only the 190-byte Con.ID-bearing frames and omitting the small (58/62/94-byte)
frames that carry the intervening sequence numbers.

**`send_seq` is per-VC — public-doc grounding (Davis, *VAXcluster Principles*,
ch. 2 §2.5–2.6, pp. 2-30/2-31; HOST-ONLY, cited by page).** p. 2-30 defines the
message service: "SCS also guarantees that a set of messages sent to a **common
destination** will arrive in the order in which they were sent" — the ordered
unit is the destination *node/port*, not the connection. p. 2-31 makes the scope
explicit: "if either the guarantee of message delivery or the guarantee of
message sequentiality cannot be satisfied, the **virtual circuit** between the
ports involved will be explicitly broken … then **every connection supported by
this virtual circuit is also broken**." Sequentiality is a property of the VC
(the port pair) and its failure tears down *all* connections on it. The
deliberate contrast is p. 2-38 (transcript part4): "flow control for the SCS
message service is done on a **per connection basis**" — credit is per-CDT,
sequencing is per-VC. So one `send_seq`/`recv_seq` pair spans all connections
multiplexed on a VC.

**`send_seq` is per-VC — oracle grounding (`vax3-class03-crash-REJOIN-SUCCESS-`
`20260801.pcap`).** The crash-rejoiner VAX3 keeps INDEPENDENT counters per peer:
its `send_seq` to VAX2 and its `send_seq` to VAX1 both pass through 14 in the same
window (f1297 VAX3→VAX2 `ss=14`; f1264 VAX3→VAX1 `ss=14`) — two separate VCs, two
separate counters. Within the VAX3→VAX2 VC the counter is **fully contiguous
across all connections and frame classes**: `4,5,6,…,22` with NO gaps (measured
over f1243–f1335, `tools/cluster/dissect_sca.py`). The op 0x02 config rides
`ss=14`, immediately after `ss=13` (f1261, a 62-byte frame) and `ss=12` (f1260, a
58-byte frame) — the two frames §4(O.13)'s 190-byte-only table did not see. There
is **no sparse subsequence** in the success case; the member's `recv_ack` walks
the contiguous VC stream and reciprocates at f1299 (`recv_ack=14` = the config's
`send_seq`).

**OVMX already implements per-VC `send_seq`, and its member-VC stream is
contiguous on fresh live wire.** `src/vmsscs/scsd.c:864` holds ONE
`struct scs_vc vc` per `peer_state`; :1021 "All sends ride the shared per-channel
`ps->vc.seq`"; every emitter calls `scs_seq_advance(&ps->vc.seq)`. This is
architecturally identical to the oracle. A fresh lab-2 rejoin (vaxlab-7, identity
`OVMXB0/1973`, `OVMX_JOIN_SEQ=1`, HEAD, 2026-08-10) shows OVMX's OVMX→member VC
`send_seq` = `3,4,5,…,27` **contiguous** (`d94-bc2B1rejoin.pcap`, filtered to real
SCS classes 0x41/0x4b/0x5b, all destined to the member MAC). OVMX's own VC engine
reports `VC-GUARANTEES: seq-gaps=0 vc-breaks=0`. **Building per-connection
counters would DIVERGE from the oracle (whose rejoiner uses one per-VC counter)
and is the wrong fix — do not build it.**

**The relocated residual (GROUNDED, fresh live bracket, reproduced identically on
two independent captures).** With a contiguous per-VC stream, the rejoin still
stalls. On the member-initiated VC the member acks OVMX's readmission stream only
up to a fixed point and then goes silent — in BOTH the prior `vaxlab-11`
(`recv_ack` last = 19, was reported as "18" in §4(O.13) from the 190-byte frames
only) and the fresh `vaxlab-7` run the member's last sequenced frame carries
`recv_ack=19`, while OVMX's op 0x02 rides `ss=24` (model `ss=21`, params `ss=22`
on the member VMS$VAXcluster Con.ID pair). The op 0x02 is therefore several
messages BEHIND the point where the member's per-VC `recv_seq` stops advancing, so
it is never delivered to the member's CM SYSAP; the member sends `cm_responses=0`,
no op 0x04 reciprocation, then abandons the sequenced VC and reverts to
HELLO-only (`mt=0xb3`), and the connection ends `conn[member=DISC SENT]`,
`vaxcluster_member=no`. **XITDONE=0** (fresh bracket: arm 1 first-join
**XITDONE=1** + sidecar `generation=1`; arm 2 same-id rejoin **XITDONE=0**). So
the blocker is NOT the op 0x02 payload (§4(O.10)), NOT the connection selection
(§4(O.11/12)), NOT the SYSAP-contiguity (§4(O.13)), and NOT a per-connection
sequence gap (this subsection): it is that the member's per-VC `recv_seq` freezes
mid-stream, stranding the op 0x02 behind it.

**Next isolation target (open, on `vms-694`).** WHY does the member stop
advancing `recv_seq` at that point? The first frame the member never acks is a
58-byte `0x4b` sequenced message (fresh: f167 `ss=20`; prior: f162 `ss=20`), and
OVMX is spraying the readmission across several distinct member Con.IDs
(`6F18000E`, `6F16000D`, `6F1B0010`, … — Con.ID reads for the non-190 classes are
NOT independently grounded, spec §4(d) docstring). OVMX's OWN receive side mirrors
a connection-identity mismatch: `RX-CDL … no-cdt=14` (14 of 26 sequenced
app-messages match no open CDT). The leading (not-yet-isolated) hypothesis is a
rejoin-specific CDT/Con.ID mismatch — OVMX emitting sequenced messages on member
connections that are not open on the member — which stalls per-VC delivery (p.
2-31: an undeliverable message can break the VC). Isolating it needs an
experiment that varies the connection SET OVMX drives the readmission over (drive
op 0x02 and only the model/params on the single member-initiated VMS$VAXcluster
Con.ID, suppress the other sequenced sends), not a `send_seq` change. **[REFINED —
see §4(O.15): the frame the member freezes on (`f167`, `ss=20`) rides a connection
that DID reach OPEN on the member, and it is byte-identical to the 58-byte `0x4b`
frames the SUCCESS oracle sends and the member ACKS — so the "sequenced message on a
connection not open on the member" reading does not hold for the freeze frame
itself. The grounded root is that OVMX opens its OWN SCS$DIRECTORY and MSCP$DISK
CLIENT connections to the COORDINATOR member and front-loads that member's VC with
~19 messages of directory + disk-discovery traffic BEFORE op 0x02 (`ss=21`), where
the oracle rejoiner keeps the coordinator's VC lean and reaches op 0x02 at `ss=14`.]**

**Kill-switch / test note.** This subsection ships **no wire change** (it refutes
a proposed change and records where the frontier actually is), so it carries no
new kill-switch or fail-pre/pass-post frame test; the evidence is the oracle
`send_seq` contiguity, the code sites, and the live XITDONE 1-vs-0 bracket. Full
`scs`/`vmsscs` ctest stays green.

**Evidence** (host, tank volume): public doc Davis pp. 2-30/2-31/2-38
(`/home/baron/cluster/transcript/part3.md` §2.5–2.6, `part4.md` §2.8, HOST-ONLY);
SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(VAX3→VAX2 `send_seq` 4..22 contiguous, f1243–f1335; independent VAX3→VAX1
counter); fresh live bracket
`/data/training/vax/k8s-labs/vaxlab-7/logs/scsd-bc2A1join.log` (first-join
XITDONE=1 + sidecar), `scsd-bc2B1rejoin.log` + `d94-bc2B1rejoin.pcap` (rejoin
XITDONE=0, `send_seq` 3..27 contiguous, member `recv_ack` frozen at 19,
`cm_responses=0`, `conn[member=DISC SENT]`, `RX-CDL no-cdt=14`); driver
`tests/lab/tools/rejoin_arm_lab2.sh`; code `src/vmsscs/scsd.c:864/1021` (one
`ps->vc.seq` per peer) and `src/vmsscs/scs_vc.c` (the VC seq engine);
measurement `tools/cluster/dissect_sca.py`.

#### 4(O.15) The rejoin readmission stalls because OVMX FRONT-LOADS the coordinator member's virtual circuit with its OWN SCS$DIRECTORY + MSCP$DISK client discovery, so op 0x02 lands at `ss=21` behind the point (`ss=19→20`) where the member stops advancing `recv_seq` — the freeze frame is a VALID op-5 directory CONFIRM5, byte-identical to one the SUCCESS oracle sends and the member ACKS, so it is NEITHER the frame's content NOR an unopened Con.ID; the oracle rejoiner keeps that VC lean and reaches op 0x02 at `ss=14` (GROUNDED, SUCCESS oracle + same-day live lab-2 bracket, freeze-frame forensic, `vms-e15`, 2026-08-10)

**Frame.** §4(O.14) refuted the per-connection-`send_seq` candidate and left the
open question: WHY does the member stop advancing its per-VC `recv_seq` at
`ss=19→20`, stranding op 0x02 behind it? Its leading (not-yet-isolated) hypothesis
was "OVMX emitting sequenced messages on member connections that are not open on the
member." This subsection pins the exact freeze frame, REFUTES that hypothesis for
the freeze frame itself, and relocates the root to the SHAPE of OVMX's readmission
stream on the coordinator's VC.

**The freeze frame, pinned (GROUNDED, `d94-bc2B1rejoin.pcap`, OVMX
`66:fb:26:46:a3:49` → coordinator VAX2 `08:00:2b:f2:30:ed`; `send_seq` at abs 34 and
`recv_ack` at abs 32 are the §4(O.14)/`scs_seqgap_measure.py` grounded offsets, read
for every sequenced envelope regardless of length class).** On the OVMX→VAX2 VC the
member's `recv_ack` ceiling over the WHOLE capture is **19** (last at `f165`) and
never reaches 20. OVMX's `ss=20` frame is **`f167`: a 58-byte `0x4b` SCS$DIRECTORY
op-5 CONFIRM5** (`SCS_DIR_CONFIRM5`, `src/vmsscs/scs_dir.c`; vms-754 flags op-5's
identity as the shared-namespace REJECT_RSP shape) riding the **member-opened**
SCS$DIRECTORY connection (OVMX `local=0x22770017` / VAX2 `remote=0x6F16000D` — the
connection VAX2 opened and OVMX answered, `SCSD-I-DIRCONN ... remote=0x6F16000D
local=0x22770017 with peer 08:00:2b:f2:30:ed`, which reached `--RCV_ACCEPT_RSP-->
OPEN`). OVMX's op 0x02 rides `ss=21/22/24` (`f170/f171/f207`, the 190-byte class on
the VMS$VAXcluster VC `local=0x22770011` / `remote=0x6F1B0010`) — several messages
BEHIND the frozen point, so it is never delivered; VAX2 sends `cm_responses=0`, no
op 0x04 reciprocation (its only two 190-byte frames to OVMX are its OWN op 0x01
advertisement, not a reciprocation), then breaks the VC (`conn[member=DISC SENT]`),
`XITDONE=0`.

**REFUTATION — the freeze frame is neither malformed nor on an unopened Con.ID.**
Two grounded facts kill §4(O.14)'s leading hypothesis for `f167`:
1. **It rides an OPEN connection.** The member-opened SCS$DIRECTORY connection
   `0x22770017`/`0x6F16000D` reached the Figure-2-14 OPEN state on both sides (the
   DIRCONN bind above), so this is not "a sequenced message on a connection not open
   on the member."
2. **It is byte-identical to a frame the oracle sends AND the member ACKS.** In the
   SUCCESS oracle the crash-rejoiner VAX3 (`08:00:2b:11:22:33`) sends 58-byte `0x4b`
   op-5 frames to VAX2 (`08:00:2b:78:56:b9`) at `f1251 ss=8` and `f1262 ss=13`, with
   the identical trailing body shape (`… 01 00 00 02 0e 00 04 00 …` then the Con.ID
   pair), and VAX2 walks its `recv_seq` straight through them to op 0x02 at `ss=14`
   (`f1298`), reciprocated at `f1299`. So the 58-byte op-5 CONFIRM5 frame class is a
   normal, ackable part of a SUCCESSFUL rejoin — the freeze is not in this frame.

**ROOT (GROUNDED) — OVMX bloats the COORDINATOR's VC; the oracle keeps it lean.**
The difference is which connections ride the one VC that must carry the readmission.
`send_seq` is per-VC (§4(O.14)), so every connection OVMX multiplexes onto the
coordinator VAX2 shares VAX2's single sequence. OVMX opens, to VAX2:

| OVMX conid | VAX2 conid | connection | `send_seq` it consumes on VAX2's VC |
|---|---|---|---|
| `0x22770018` | `0xAA8C000C` | **OVMX's OWN** SCS$DIRECTORY client (`SCSD-I-OWNDIRBOUND`) | 1, 2, 3, 6, 8, 13 |
| `0x22770017` | `0x6F16000D` | member-opened SCS$DIRECTORY (OVMX answers) | 5, 7, 10, 14, 15, **20 = freeze** |
| `0x2277001A` | `0x6F18000E` | **OVMX's OWN** MSCP$DISK client (`SCSD-I-MSCPBOUND`) | 9, 11, 12, 16, 19, 23 |
| `0x22770011` | `0x6F1B0010` | member-opened VMS$VAXcluster (op 0x02 rides here) | 18, **21, 22, 24 = op 0x02** |

OVMX opens its OWN SCS$DIRECTORY and OWN MSCP$DISK CLIENT connections to VAX2 and
runs directory lookups + MSCP disk discovery (SCC + GET UNIT STATUS walk) on them —
so ~19 of OVMX's sequenced messages to VAX2 are its own discovery traffic, and op
0x02 does not ride until `ss=21`. The SUCCESS oracle's rejoiner keeps VAX2's VC
LEAN: §4(O.11)'s connect census shows VAX3 opens its OWN SCS$DIRECTORY (`f1264`) and
MSCP$DISK (`f1277`) client connections to **VAX1** (the OTHER member) and merely
ANSWERS VAX2's inbound connects — so on the VAX3→VAX2 VC the whole stream is
contiguous `4..22` with op 0x02 at `ss=14`. A rejoiner runs its disk discovery
against a NON-coordinator member and keeps the coordinator's VC carrying essentially
only the readmission; OVMX runs it against BOTH, including the coordinator, and buries
op 0x02 under it.

**The blocker relocates AGAIN, and is now NONE of the prior candidates NOR the
freeze frame's content.** Not the op 0x02 body (§4(O.10)); not the connection
selection (§4(O.11/12) — op 0x02 IS on the member-initiated VMS$VAXcluster VC); not
SYSAP-contiguity (§4(O.13)); not a per-connection `send_seq` gap (§4(O.14)); and not
the `ss=20` frame's payload (this subsection — identical to an acked oracle frame).
It is that OVMX front-loads the coordinator's one readmission VC with its own
directory + disk-discovery client traffic, and the member stops advancing `recv_seq`
at `ss=19→20` — before op 0x02 at `ss=21` is ever reached. Whatever makes VAX2 stop
at the `ss=20` CONFIRM5 (a frame it accepts in the oracle) does so only because OVMX
placed ~19 messages of its own discovery ahead of the readmission on the single VC
that must carry it, where the oracle placed none.

**Two live candidates for the fix increment (open, `vms-694`).**
1. **COORDINATOR-LEAN readmission (leading).** On a rejoin, do NOT open OVMX's own
   SCS$DIRECTORY / MSCP$DISK CLIENT connections to the COORDINATOR member (the one
   whose VMS$VAXcluster VC drives op 0x02); keep that VC carrying only
   model/params/op 0x02, and run disk discovery against a NON-coordinator member, as
   the oracle does (§4(O.11) census). Oracle-grounded and wire-visible (op 0x02 moves
   to `ss≈14`; the own-dir/own-MSCP frames leave the coordinator's VC) → it ships an
   env kill-switch, a fail-pre/pass-post frame-replay test on the coordinator VC's
   pre-op-0x02 stream length, and a live rejoin bracket flipping `XITDONE` 0→1. This
   is the isolation experiment §4(O.14) asked for, now with its target named, and is
   the next fix increment — more than this RE subsection.
2. **WHY VAX2 will not deliver the `ss=20` CONFIRM5 specifically** (subordinate).
   Whether OVMX's op-5 (vms-754: REJECT_RSP-shaped) directory frame is the wrong
   message for the member-opened directory connection's state needs the op-4/op-5
   directory-handshake identities pinned against the oracle's directory exchange,
   which the non-190 Con.ID/offset ungroundedness (§4(d)) currently blocks. Candidate
   1 subsumes it if removing the bloat means the member never has to deliver that
   frame ahead of op 0x02.

**Kill-switch / test note.** This subsection ships **no wire change** (it pins the
freeze frame, refutes the frame-content and Con.ID-not-open readings, and relocates
the frontier to the coordinator-VC bloat), so it carries no new kill-switch or
fail-pre/pass-post frame test — exactly as §4(O.14) and #270. The evidence is the
freeze-point forensic on a same-day live bracket, the oracle contrast, and the
connect census. Full `scs`/`vmsscs` ctest stays green.

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(VAX3→VAX2 contiguous `send_seq` `4..22`, op 0x02 `f1298 ss=14`, reciprocation
`f1299`; identical-class 58-byte op-5 frames `f1251 ss=8`, `f1262 ss=13`, acked;
§4(O.11) connect census f1170–1400 — VAX3's own dir/MSCP go to VAX1); live bracket
`/data/training/vax/k8s-labs/vaxlab-7/logs/d94-bc2B1rejoin.pcap` (member `recv_ack`
ceiling 19 at `f165`; freeze frame `f167 ss=20`, 58-byte `0x4b` dir op-5 on
`0x22770017`/`0x6F16000D`; op 0x02 `f170/f171/f207 ss=21/22/24` on
`0x22770011`/`0x6F1B0010`) and `scsd-bc2B1rejoin.log` (`OWNDIRBOUND local=0x22770018`,
`MSCPBOUND local=0x2277001A`, `CMCONFIG2 ... local=0x22770011 remote=0x6F1B0010`,
`RX-CDL no-cdt=14`, `cm_responses=0`, `conn[member=DISC SENT]`, `XITDONE=0`); code
`src/vmsscs/scsd.c` (`send_own_dir_connect_request`, `send_mscp_connect_request`,
`ps_mscp_disc`, the join-step client half) and `src/vmsscs/scs_dir.c`
(`dir_confirm5_tmpl`, `SCS_DIR_CONFIRM5`); measurement `tools/cluster/dissect_sca.py`
+ the freeze-point forensic (`send_seq`/`recv_ack` at abs 34/32).

#### 4(O.16) The coordinator-lean fix WORKS ON THE WIRE — op 0x02 moves from `ss=24` to `ss=12` when OVMX's own dir/MSCP client half is suppressed to the coordinator — but it is NECESSARY-NOT-SUFFICIENT: with op 0x02 delivered lean AND in-order AND well under any ceiling, the coordinator STILL returns `cm_responses=0` and tears the VC down, so the freeze is NOT the send_seq position alone (GROUNDED, live lab-2 matched kill-switch bracket, `vms-9af`, 2026-08-10)

**Frame.** §4(O.15) located the rejoin stall at coordinator-VC BLOAT (OVMX front-loads
its own `SCS$DIRECTORY` + `MSCP$DISK` client discovery onto the one VC that must carry
the readmission, so op 0x02 lands at `ss=21`, behind the member's `recv_ack` ceiling
of 19) and named candidate 1 — keep the coordinator's VC LEAN — as the leading fix.
This subsection SHIPS that fix and BRACKETS it live. The bloat is real and removable
and op 0x02 does move under the ceiling; **`XITDONE` still does not flip**, so the
`ss=20→21` freeze is necessary-not-sufficiently explained by the bloat, exactly as
§4(O.12) found for the connection-selection fix.

**The change (`src/vmsscs/scsd.c`, wire-visible, kill-switched).** On a detected
rejoin (`cm_rejoin_target_mode()` → PRIORCLU sidecar) OVMX now SUPPRESSES its own
`SCS$DIRECTORY` + `MSCP$DISK` CLIENT connects to the COORDINATOR member — the peer
whose `VMS$VAXcluster` VC carries op 0x02 — and runs that discovery against a
NON-coordinator member instead, matching the §4(O.11) oracle census. The coordinator
is `cm_peer_is_coordinator()` (the op-0x02 recipient: strict-maximum node number with
a lower peer present, or `OVMX_CFG2_PEER` named). Suppressing the single own-dir
CONNECT (join step 1/8) prevents the whole client-half cascade (dir lookups → MSCP
connect → MSCP discovery) on that VC. Gate: `cm_rejoin_lean_vc()`; kill-switch
`OVMX_REJOIN_LEAN_VC=0` restores the pre-fix bloat.

**The matched bracket (GROUNDED, live lab-2 `vaxlab-0`, one identity `OVMX90`/1973
throughout, `OVMX_JOIN_SEQ=1 OVMX_CFG2_PEER=2` so the coordinator is deterministically
node 2 = SCSSYSTEMID 1026 ↔ the oracle's VAX2; first-join arm established membership
and wrote the PRIORCLU sidecar, 2026-08-10).** op 0x02's per-VC `send_seq` read at abs
`[34:36]` on OVMX's own 190-byte config frame to the coordinator:

| arm | `OVMX_REJOIN_LEAN_VC` | `SCSD-I-LEANVC` | own-dir binds to members (`OWNDIRBOUND`) | op 0x02 `send_seq` to coordinator | `XITDONE` |
|---|---|---|---|---|---|
| RfixCFG2 (fix) | on (default) | fires once (node 2) | **1** (non-coordinator only) | **`ss=12`** (LEAN, ≈ oracle `ss=14`) | **0** |
| RoffCFG2 (kill-switch) | `=0` | none | **2** (both members, the bloat) | **`ss=24`** (BLOATED, above the §4(O.15) `ss=21` point) | **0** |

The fix does EXACTLY what §4(O.15) predicted and is wire-visible in three independent
readings: `LEANVC` fires for the coordinator only, `OWNDIRBOUND` drops from 2 to 1
(OVMX's own client half leaves the coordinator's VC and stays on the non-coordinator's,
per the oracle), and **op 0x02's `send_seq` drops from 24 to 12** — comfortably under
the member's ceiling and below even the oracle's `ss=14`. The op 0x02 also rides as the
contiguous SYSAP `send_msg=3` (§4(O.13)/vms-71d), so it is lean in BOTH the per-VC
sequence and the SYSAP sequence.

**And it is STILL not enough (GROUNDED, same bracket).** In the fix arm, with op 0x02
delivered to the coordinator at `ss=12` — lean, in-order, well under the ceiling — the
coordinator (`08:00:2b:8e:b7:85`, node 2) returns **`cm_responses=0`**: no `cat-0x04`
op 0x04 reciprocation, no op 0x03 commit, no barrier. After ~10 s both members drive
every connection `OPEN --SVC_DISCONNECT--> DISC SENT` and OVMX's exit summary shows
`vaxcluster_member=no`, `conn[dir=DISC SENT member=DISC SENT]`, `XITDONE=0` — the same
teardown as the pre-fix arm. So removing the coordinator-VC bloat, though real and
oracle-aligned, does NOT by itself complete readmission: **the member's refusal to
reciprocate op 0x04 survives op 0x02 arriving lean**, which refutes "op 0x02 was
stranded only because it was behind the `recv_ack` ceiling" as the WHOLE story.

**The frontier relocates to §4(O.15)'s candidate 2, now isolated.** With the bloat
removed as a confound, the live blocker is: *why does the coordinator decline op 0x04
reciprocation when op 0x02 reaches it lean and in-order?* Two grounded threads for the
next increment (`vms-694`): (1) a **late STRAY cat-0x04 ack** — in the fix arm OVMX
emits a `cat-0x04` ack naming SYSAP `msg#2` (the op 0x01 params) ~9.8 s after that
frame, where the reference acks within ~1 ms and NEVER acks an op 0x01
(`SCSD-W-STRAYACK`); this is a distinct stray-ack from the one vms-71d's
`rejoin_hold_standalone_ack()` holds, and it fires on the member-initiated VC after
op 0x02. (2) the §4(O.15) candidate-2 question of whether OVMX's op-5 directory frame
is the wrong message for the member-opened directory connection's state, which the
non-190 Con.ID/offset ungroundedness (§4(d)) still blocks. Neither is the send_seq
position, which this bracket has now taken off the table.

**What ships (`vms-9af`).** The coordinator-lean change (kill-switch
`OVMX_REJOIN_LEAN_VC`, coordinator id `cm_peer_is_coordinator` / `OVMX_CFG2_PEER`), a
fail-pre/pass-post unit test
(`tests/vmsscs/test_scsd_wire.c:test_rejoin_keeps_coordinator_vc_lean_so_op02_rides_low`
— ARM A suppressed → op 0x02 at the lean baseline, ARM B `OVMX_REJOIN_LEAN_VC=0` →
op 0x02 bloated by the client-half frame count; non-coordinator never suppressed;
first-join untouched), and this live bracket. `scs`/`vmsscs` ctest stays green. It is a
real wire improvement (op 0x02 now rides lean, matching the oracle) that is a strict
PREREQUISITE for readmission without being its completion — banked, not a dead end.

**Non-claims.** (1) This does not claim which member is "the" coordinator is grounded —
`OVMX_CFG2_PEER=2` NAMES it (node 2 ↔ the oracle's VAX2 by SCSSYSTEMID); the default
heuristic (strict-max node number with a lower peer present) is START-order dependent
and best-effort, never presented as VMS-authentic. (2) It does not claim the stray-ack
or the op-5 directory frame IS the residual blocker — only that the send_seq position
is NOT, and those two are the isolated candidates. (3) The op 0x02 body is unchanged
(§4(O.10)); this is a connection-population change, not a payload change.

**Evidence** (host, tank volume): live bracket
`/data/training/vax/k8s-labs/vaxlab-0/logs/` — `d94-RfixCFG2.pcap` (op 0x02 frame 178,
`cat 0x01 op 0x02`, OVMX `6a:85:cb:48:e0:62` → coordinator, `send_seq=12`) vs
`d94-RoffCFG2.pcap` (op 0x02 frame 217, `send_seq=24`); `scsd-RfixCFG2.log`
(`LEANVC ... node 2`, one `OWNDIRBOUND`, `CMCONFIG2 ... to node 2 ... send_msg=3`,
`SCSD-W-STRAYACK ... msg#2 ... 9795 ms`, `cm_responses=0`, `conn[member=DISC SENT]`,
`XITDONE=0`) vs `scsd-RoffCFG2.log` (no `LEANVC`, two `OWNDIRBOUND`, op 0x02
`send_seq=24`); first-join `scsd-9afF2.log` (`XITDONE=1`, sidecar
`formed=00bc0c7124275ac0 founding_sysid=1025 generation=1`); code
`src/vmsscs/scsd.c` (`cm_rejoin_lean_vc`, `cm_peer_is_coordinator`,
`cm_lean_vc_suppress_peer`, the two own-dir initiation gates); measurement
`tools/cluster/dissect_sca.py` + `send_seq` at abs `[34:36]`.

#### 4(O.17) The coordinator returns `cm_responses=0` because it is SEND-CREDIT-STARVED, not because op 0x02 is malformed or mis-sequenced: OVMX never returns Pending Receive Credit to the coordinator — the SUCCESS-oracle rejoiner INITIATES its own op-6 special-credit request on the `SCS$DIRECTORY` connection BEFORE op 0x02, and OVMX defers its op-6 until AFTER op 0x02 (too late) — so the coordinator's Send Credit to OVMX is never replenished, it cannot transmit its op 0x04 reciprocation (SCS Credit Wait, Davis pp. 2-43/2-45), and the readmission stalls. The late STRAY cat-0x04 ack (§4(O.16) lead 1) is REFUTED as causal; the op-5 directory-frame (lead 2) is REFINED to the op-6/op-8 special-credit exchange on that connection (GROUNDED, SUCCESS oracle + live lab-2 `d94-RfixCFG2` freeze-point forensic + *VAXcluster Principles* pp. 2-43/2-44/2-45 & 7-37/7-38, `vms-96f`, 2026-08-10)

**Frame.** §4(O.16) delivered op 0x02 to the coordinator LEAN (`ss=12`) and IN-ORDER
and found readmission still incomplete (`cm_responses=0`, no op 0x04), naming two
isolated leads independent of `send_seq`: (1) a late STRAY `cat-0x04` ack of op 0x01
`msg#2` at ~9.8 s; (2) the op-5 `SCS$DIRECTORY` directory-frame question. This
subsection reads the fix-arm capture frame-by-frame against the SUCCESS oracle,
REFUTES lead 1, REFINES lead 2, and — following the operator's structural steer to
map OVMX against the documented protocol rather than byte-diff in isolation —
identifies the FUNDAMENTALLY MISSING protocol step as SCS **Send Credit** flow
control (Davis ch. 2), one layer BENEATH the connection-manager JOIN protocol.

**The coordinator's virtual circuit freezes at OVMX `send_seq=8`; op 0x02 (`ss=12`)
is NEVER delivered (GROUNDED, `d94-RfixCFG2.pcap`, OVMX `6a:85:cb:48:e0:62` →
coordinator VAX2 `08:00:2b:8e:b7:85`; VC `recv_ack` at abs `[32:34]`).** The
coordinator's cumulative `recv_ack` toward OVMX advances `…6,7,8` and then FREEZES at
**8** for the whole run (last-advanced `f61`; still 8 at the teardown retransmits
`f146`/`f174`, which are `0x7b` retransmits of the coordinator's own `ss=11` params).
OVMX's op 0x02 rides `f178 ss=12` (190-byte `cat 0x01 op 0x02` on the member-initiated
`VMS$VAXcluster` VC `local=0x0C640001`/`remote=0x8D1B000C`) — four messages behind the
frozen point — so the coordinator's connection manager never receives the readmission
request, never proposes addition, sends no op 0x04, and tears the VCs down
(`cm_responses=0`, `XITDONE=0`). The coordinator's own console confirms it: across ALL
FOUR rejoin runs on this pod VAX2 logs only `lost connection to node OVMX90`, and NEVER
`received VAXcluster membership request` / `proposed addition of node OVMX90` — the two
lines it DID log for the first-join at `19:19:55` (`vaxlab-0/logs/vax2.log`). The
readmission request is dropped BELOW the CM layer.

**Lead 1 (the STRAY `cat-0x04` ack) is REFUTED as causal.** `SCSD-W-STRAYACK` fires at
`20:20:14.489`, naming `msg#2` 9795 ms stale — ~3 s AFTER op 0x02 went out
(`SCSD-I-CMCONFIG2` `20:20:11.492`) and it rides `ss≥14` (`f834`/`f835`), DOWNSTREAM of
the `ss=8` freeze. A frame emitted after, and behind, the freeze cannot be the cause of
a freeze that already happened. The stray ack is a symptom of the same silence (OVMX
still holds `sysap_recv=2` because the coordinator went quiet), not its cause.

**The freeze frame is OVMX's `ss=9` = the op-9 special-credit RESPONSE, on the
coordinator-opened `SCS$DIRECTORY` connection — not an op-5 CONFIRM5 (GROUNDED, op field
at abs `[60:62]`).** On that connection (OVMX `local=0x0C640007`/coordinator
`remote=0x8DE4000D`) the coordinator sends `f59 op=8` (the p. 2-44 special credit
message, `ss=8`) and OVMX answers `f60 op=9` (`ss=9`, `scs_reflect_credit`). A byte diff
of OVMX's op-9 (`f60`) against the oracle's ACCEPTED op-9 (`f1255`) shows the frame is
byte-faithful: every difference is either a correctly-patched sequence counter or a
field echoed from the respective op-8 request (the credit byte at `[48:50]` is 2 vs 1
because OVMX's inbound op-8 carried 2 where the oracle's carried 1). The stream SHAPE is
identical to the oracle through `ss=9` (`op 1,2,10,10,10,10,1,2,9`). So the freeze is
NEITHER the op-9 payload NOR the op-5 frame content (§4(O.15) already showed the op-5
CONFIRM5 is byte-identical to an acked oracle frame).

**ROOT (GROUNDED, oracle contrast + Davis pp. 2-43/2-44/2-45) — OVMX never returns
Pending Receive Credit, so the coordinator runs out of Send Credit and cannot
reciprocate.** SCA gives the message service a debit/credit flow control (p. 2-43): the
receiving SYSAP extends N *Send Credits* (its receive-buffer count) to the sender; the
sender must hold ≥ 1 Send Credit to transmit an application (MTYPE-10) message, and with
none it enters **Credit Wait** — the operation is SUSPENDED, the frame is NOT sent
(p. 2-45). Credit is replenished by the header piggyback and, when one direction is
quiet, by a **special credit message** carrying the *Pending Receive Credit* count
(p. 2-44). The SUCCESS oracle runs this bidirectionally on the `SCS$DIRECTORY` VC before
op 0x02, and the coordinator's `recv_ack` advances in lock-step with it:

| oracle frame | dir | op | `ss` | VAX2 `recv_ack` |
|---|---|---|---|---|
| f1251 | VAX2→VAX3 | 8 (special-credit msg) | 8 | 8 |
| f1255 | VAX3→VAX2 | 9 (response) | 9 | — |
| **f1261** | **VAX3→VAX2** | **6 (special-credit REQUEST)** | **13** | — |
| f1262 | VAX2→VAX3 | 7 (response) | 13 | **13** |
| f1297 | VAX3→VAX2 | op 0x02 config | 14 | — |
| f1299 | VAX2→VAX3 | op 0x04 reciprocation | 15 | **14** |

The rejoiner VAX3 **INITIATES its own op-6 special-credit request (`f1261`, `ss=13`)
BEFORE op 0x02 (`f1297`, `ss=14`)**, returning Pending Receive Credit to the coordinator;
only then does VAX2's `recv_ack` climb `8→11→13→14`, deliver op 0x02, and reciprocate
op 0x04 (`f1299`, itself a 190-byte MTYPE-10 that piggybacks 3 credits at `[48:50]`).
OVMX does the OPPOSITE ORDER: it sends op 0x02 at `ss=12` and defers its op-6 to
`ss=14/15` (`f834`/`f835`) — AFTER op 0x02 and after the freeze. OVMX's exit summary
proves the starvation directly: `CREDIT: … starved=10 … grants=0 grant-units=0` — it
transmitted 10 application frames with no Send Credit (p. 2-45 violated) and banked ZERO
credit grants. The coordinator, its Send Credit to OVMX consumed by its own model+params
and never replenished, cannot transmit its op 0x04 reciprocation (an MTYPE-10 message
needs a Send Credit, p. 2-43) — it is in Credit Wait, can only retransmit its params, and
finally tears the VC down. This is why `cm_responses=0`: not a rejected op 0x02, a
credit-starved coordinator that cannot answer one.

**Why this was invisible until now.** `scs_credit.c` DELIBERATELY leaves the special
credit message alone — its census called the op-8/op-9 credit byte "an unnamed constant"
and declined to bank it (guard against a wire claim with no observation); and `scsd.c`
(the `scs_reflect_credit` block, ~line 5870) records that OVMX ANSWERS op-6/op-8 but
that INITIATING one "is REQUIRED, is NOT implemented here." Davis p. 2-44 now GROUNDS the
identification (the special credit message and its Pending-Receive-Credit payload are
documented), and the oracle GROUNDS the requirement (a completing rejoin sends op-6
before op 0x02). The "unnamed constant" caveat is thereby upgraded: MTYPE 8/6 is the
p. 2-44 special credit message, and OVMX's non-initiation of it is the rejoin blocker.

**What the fix increment must do (`vms-694` child).** On a rejoin, OVMX must run the
rejoiner's half of SCS Send Credit flow control on the coordinator's `SCS$DIRECTORY`
connection BEFORE it sends op 0x02: INITIATE an op-6 special-credit request returning its
Pending Receive Credit (as the oracle does at `f1261 ss=13`), and honor Credit Wait
rather than transmitting the CM readmission starved. Concretely the deferred op 0x02 send
must be ORDERED after the op-6/op-7 credit round on that connection, mirroring the
oracle's `…op9(9), model(10), params(11), op7(12), op6(13), op02(14)`. This is
wire-visible (a new op-6 appears before op 0x02) → it ships an env kill-switch, a
fail-pre/pass-post frame test asserting op-6 precedes op 0x02 on the rejoin stream, and a
live rejoin bracket flipping `XITDONE` 0→1. Whether the credit return ALONE completes
readmission, or the coordinator then also needs the JOIN Phase-1 connectivity handshake
(pp. 7-38/7-39, the Rule of Total Connectivity dialogue) it currently never reaches, is a
question the bracket answers once the credit starvation is removed as a confound — the
same necessary-vs-sufficient discipline §4(O.12)/§4(O.16) applied to their fixes.

**Non-claims.** (1) The op-9 frame content is byte-faithful and is NOT the blocker
(byte diff `f60` vs `f1255`). (2) The op 0x02 body is unchanged (§4(O.10)); this is a
credit/ordering gap, not a payload gap. (3) This subsection ships NO wire change — it is
a freeze-point forensic + oracle contrast + book grounding, so (like §4(O.14)/§4(O.15))
it carries no kill-switch or frame test; those belong to the fix increment. (4) The
special-credit op-numbering (6 request / 7 response / 8 message / 9 response) is read from
the shared MTYPE namespace `{0..10}` (`scs_env.h`, §4(h)(1a)); p. 2-44 names the
mechanism, not the on-wire opcodes, which remain observation-grounded.

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(VAX3→VAX2 credit rounds `f1251`/`f1255`/`f1259`-`f1262`, rejoiner op-6 `f1261 ss=13`
BEFORE op 0x02 `f1297 ss=14`, reciprocation `f1299`; VAX2 `recv_ack` `8→11→13→14`);
live fix-arm `/data/training/vax/k8s-labs/vaxlab-0/logs/d94-RfixCFG2.pcap` (coordinator
`recv_ack` frozen at 8; freeze frame OVMX `f60 op=9 ss=9` on `SCS$DIRECTORY`
`0x0C640007`/`0x8DE4000D`; op 0x02 `f178 ss=12`; OVMX op-6 not until `f834`/`f835`
`ss=14/15`) and `scsd-RfixCFG2.log` (`CREDIT: … starved=10 … grants=0`, `STRAYACK …
9795 ms`, `cm_responses=0`, `XITDONE=0`) and `vax2.log` (no `received … membership
request` on any rejoin); *VAXcluster Principles* (Davis 1993, HOST-ONLY, cited by page,
never committed) pp. 2-43 (Send Credit), 2-44 (special credit message / Pending Receive
Credit), 2-45 (Credit Wait), 7-37/7-38 (JOIN request + coordinator quorum tests); code
`src/vmsscs/scs_credit.c` (the MTYPE-8/9 "left alone" census; `scs_credit_grant_from_peer`)
and `src/vmsscs/scsd.c` (`scs_reflect_credit` "initiate … NOT implemented", the credit
accounting counters, `SCSD-W-STRAYACK`); measurement `tools/cluster/dissect_sca.py`
(op field abs `[60:62]`, `recv_ack` abs `[32:34]`, `send_seq` abs `[34:36]`).

#### 4(O.18) The op-6 credit-first fix (#291) is CONFIRMED LIVE — op-6 provably precedes op 0x02 on the rejoin wire and SCS Send Credit now FLOWS to the coordinator — but it is NECESSARY-NOT-SUFFICIENT: rejoin still does NOT re-admit (`XITDONE=0`), because the coordinator's special-credit handshake never reaches a satisfied state (it loops RETRANSMITTING op-8 and NEVER sends op-7 to OVMX's op-6), so it never proceeds to the CM readmission — no op 0x04, no `membership request` on VAX2. The frontier relocates from "OVMX never initiates op-6" (§4(O.17), now DONE) to "the op-6/op-8 credit handshake does not COMPLETE" (GROUNDED, clean same-pod fix-vs-control bracket on the just-wiped `vaxlab-0` + freeze-point + credit-op forensic + *VAXcluster Principles* pp. 2-44/2-45 & 7-37/7-39, `vms-5bc`, 2026-08-11)

**Frame.** §4(O.17) identified the rejoin blocker as SCS Send Credit starvation (OVMX
never returns Pending Receive Credit, so the coordinator cannot transmit its op 0x04),
and shipped the fix increment (#291, `vms-46f`): on a rejoin OVMX INITIATES its own op-6
special-credit request on the coordinator's `SCS$DIRECTORY` connection BEFORE op 0x02,
behind kill-switch `OVMX_REJOIN_CREDIT_FIRST` (default ON). §4(O.17) deferred the LIVE
`XITDONE` bracket to this subsection. It is now run — three arms, one clean pod, the
static (`-DOVMX_STATIC=ON`) daemon `md5=e909cbda…` byte-identical across all three
(staged+post-run verified in-pod).

**The bracket (GROUNDED live, `vaxlab-0`, re-verified `CLUSTER_NODES=2` before each arm;
identity `OVX460`, `SCSSYSTEMID` founding 1025, sidecar `generation=1`;
`OVMX_CFG2_PEER=2` deterministic coordinator on every arm).**

| arm | `OVMX_REJOIN_CREDIT_FIRST` | wire order | `CREDIT` (grants / starved) | `cm_responses` | op 0x04? | VAX2 console | `XITDONE` |
|---|---|---|---|---|---|---|---|
| **1 first-join** | ON (default) | — | — | — | — | `membership request` + `proposed addition` (admitted) | **1** (`CN_3`, sidecar written) |
| **2 REJOIN FIX** | ON (default) | op-6 `ss=12` **BEFORE** op02 `ss=13` | `grants=2 grant-units=20 starved=4 banked=12` | **0** | **no** | only `lost connection` / `timed-out` | **0** |
| **3 REJOIN CONTROL** | OFF (`=0`) | op02 `ss=8` **BEFORE** op-6 `ss=20` | `grants=2 grant-units=20 starved=4 banked=19` | **0** | **no** | only `lost connection` / `timed-out` | **0** |

**HEADLINE — rejoin is NOT closed. `XITDONE=0` on the FIX arm (and the control).** The
reused identity does NOT re-admit. The first-join arm (arm 1) admits cleanly on the same
pod and identity (`CN_3`, `XITDONE=1`, VAX2 logs `received VAXcluster membership request`
+ `proposed addition of node OVX460`), so the defect is specifically the REJOIN, exactly
where §4(O.2)/§4(O.9) placed it — not a broken lab or a bad identity.

**The fix FIRES CORRECTLY (GROUNDED, `scsd-v46f-Rfix.log` + `d94-v46f-Rfix.pcap`, OVMX
`d6:1c:18:40:c0:ff`).** `SCSD-I-CREDITFIRST` logs *"REJOIN: initiated op-6 special-credit
request on the coordinator's `SCS$DIRECTORY` connection (local=0x2B2C0017
remote=0x3097000D) BEFORE op 0x02"*, and the capture confirms it on the wire: on the
member-initiated CM VC the op-6 (76-byte, op field abs `[60]=6`) rides `ss=12`, one ahead
of op 0x02 (`cat 0x01 op 0x02`) at `ss=13`. The control arm inverts it (op 0x02 at `ss=8`,
op-6 not until `ss=20/21`), so the kill-switch's isolated effect — the op-6/op-02
ORDER — is exactly as designed.

**Send Credit now FLOWS — the §4(O.17) hard freeze-at-8 is GONE.** In §4(O.17) the
coordinator's cumulative `recv_ack` toward OVMX FROZE at 8 and op 0x02 was never
delivered. On this build it advances PAST 8 (fix arm → 10; control arm → 12;
`v46f_freeze.py`, `recv_ack` abs `[32:34]`), and the exit summary banks real grants:
`grants=2 grant-units=20`, `starved` down to 4. So credit starvation as §4(O.17) framed
it is RELIEVED. **Caveat (honest, or the reader over-reads the win):** on current `main`
(with #291 merged) the CONTROL arm ALSO banks `grants=2` and advances past 8 — because
#291's `SCSD-I-CREDITRETX` handling (answering the coordinator's RETRANSMITTED `0x7b`
op-8) is UNCONDITIONAL, not gated behind `OVMX_REJOIN_CREDIT_FIRST`. So the §4(O.17)
prediction "`grants` 0→>0 discriminates the fix" is CONFOUNDED: `grants>0` is a property
of the #291 build, not specifically of the op-6-first ordering. The pre-fix
`starved=10 grants=0` baseline (§4(O.17)) is the thing that no longer reproduces; the flag
itself moves only the op-6 position.

**Why rejoin STILL stalls — the credit handshake never COMPLETES (GROUNDED, credit-op
census + `SCSD-I-CREDITRETX` loop).** On the coordinator's `SCS$DIRECTORY` connection
(conid `0x2B2C0007`/`0x2B2C0017`) the credit exchange never terminates:

| direction | op | meaning | fix arm | control arm |
|---|---|---|---|---|
| coord→OVMX | op-8 | special-credit / ready handshake (p. 2-44) | **34** | 32 |
| OVMX→coord | op-9 | OVMX's RESPONSE to op-8 | 34 | 32 |
| OVMX→coord | op-6 | OVMX's OWN special-credit REQUEST (the fix) | 6 | 4 |
| coord→OVMX | **op-7** | coordinator's RESPONSE to OVMX's op-6 | **0** | **0** |

The coordinator RETRANSMITS its op-8 credit/ready handshake ~every 3 s for the whole run
(`SCSD-I-CREDITRETX, answered a RETRANSMITTED (0x7b) op8 … conid=0x2B2C0007` repeating
`00:53:15`→teardown), OVMX answers each with op-9, but the coordinator NEVER answers
OVMX's op-6 with an op-7 and never stops retransmitting op-8 — i.e. the special-credit
handshake never reaches a satisfied/terminal state on either half. The coordinator
therefore never advances to the connection-manager readmission: `cm_responses=0`, no op
0x04, and VAX2 logs only `timed-out lost connection to node OVX460`, never `received
VAXcluster membership request`. So op-6 INITIATION (the #291 fix) was necessary and is now
proven live, but it is NOT SUFFICIENT — the same necessary-vs-sufficient result
§4(O.12)/§4(O.16) reached for their fixes.

**The relocated frontier (grounded, NOT yet resolved — two candidates, do not design
against one).** The coordinator's op-8 retransmit loop + its silence to OVMX's op-6 says
the credit handshake does not COMPLETE. Two documented mechanisms remain, and this
bracket does not separate them:
1. **The op-8/op-9 exchange is not being ACCEPTED as complete (Davis ch. 2, pp.
   2-44/2-45).** OVMX emits an op-9 for every op-8, but the coordinator keeps
   retransmitting the same op-8 — consistent with OVMX's op-9 not satisfying the
   coordinator's Pending-Receive-Credit accounting (a field/count in the op-9 the
   coordinator does not accept), leaving it in Credit Wait and unable to advance. (§4(O.17)
   byte-diffed OVMX's op-9 as *content*-faithful to an oracle op-9, but did not verify the
   credit COUNT the coordinator needs to see to CLOSE the loop.)
2. **The CM JOIN Phase-1 connectivity dialogue is never entered (Davis ch. 7, pp.
   7-37/7-39, the Rule of Total Connectivity).** Even were credit fully satisfied, the
   coordinator's readmission may gate on the JOIN request + quorum/connectivity tests that
   the wire never reaches here.

The next increment must instrument WHICH: read the op-8 the coordinator retransmits and
the op-9 OVMX returns for the exact Pending-Receive-Credit count the coordinator is
waiting on (candidate 1), and confirm whether any Phase-1 JOIN frame (pp. 7-38/7-39) ever
appears (candidate 2). Contrast against the SUCCESS-oracle rejoin, whose op-6 DID draw an
op-7 and whose op-8 loop terminated.

**Non-claims.** (1) This subsection ships NO wire change — it is the deferred live bracket
for #291 plus a freeze/credit forensic, so (like §4(O.14)–§4(O.17)) it carries no
kill-switch or frame test. (2) `XITDONE=0` is the FIX arm's verdict — stated flatly; the
op-6-first fix does NOT close rejoin. (3) The `recv_ack` high-water marks (fix 10, control
12) are read across VC incarnations that RESTART mid-run (the control VC cycled
incarnations, `vms-694` note) — they establish "the §4(O.17) freeze-at-8 no longer holds",
NOT a new single fixed freeze frame; do not design against "the freeze is now at 10".
(4) The special-credit op-numbering (6 req / 7 resp / 8 msg / 9 resp) is the §4(O.17)
observation-grounded reading of the shared MTYPE namespace, unchanged here.

**Evidence** (host, tank volume, `vaxlab-0`): fix arm
`/data/training/vax/k8s-labs/vaxlab-0/logs/{scsd,d94}-v46f-Rfix.{log,pcap}` (`XITDONE=0`,
`SCSD-I-CREDITFIRST` op-6 before op 0x02, `CREDIT: … grants=2 grant-units=20 starved=4`,
op-8×34/op-9×34/op-6×6/op-7×0, coordinator `recv_ack` 8→10); control arm
`…-v46f-Rctl.{log,pcap}` (`XITDONE=0`, op 0x02 `ss=8` before op-6 `ss=20`, `grants=2`,
op-7×0); first-join `…-v46f-A1.{log,pcap}` (`XITDONE=1`, sidecar written); VAX2 console
`/data/training/vax/k8s-labs/vaxlab-0/logs/vax2.log` (`membership request`+`proposed
addition` on the first-join, only `timed-out lost connection` on both rejoins); prior
admission `/data/training/vax/cluster/work/vms46f.sysgen.cluster`
(`formed=… founding_sysid=1025 generation=1`); *VAXcluster Principles* (Davis 1993,
HOST-ONLY, cited by page) pp. 2-44/2-45 (special credit message, Credit Wait), 7-37/7-39
(JOIN request, Rule of Total Connectivity); code `src/vmsscs/scsd.c`
(`rejoin_credit_first_enabled`, `SCSD-I-CREDITFIRST`, `SCSD-I-CREDITRETX`,
`scs_reflect_credit`) and `src/vmsscs/scs_credit.c`; runner `tests/lab/tools/lab2run.sh`;
measurement `tools/cluster/dissect_sca.py`.

> **Lab-harness note (`vms-694`, `vms-5bc`).** `lab2run.sh`'s prior-admission-sidecar
> PULL-BACK reads `/lab/$TAG.sysgen.cluster`, but the daemon writes it under the per-run
> dir `/lab/run-$TAG/$TAG.sysgen.cluster` (the `vms-1da` per-run staging change moved the
> daemon's cwd but not this pull-back path). So a first-join's sidecar is silently not
> carried back to the host store, and a scripted bracket sees "NO SIDECAR" and aborts. This
> bracket pulled the sidecar by hand from the per-run dir. Fixing the pull-back path is
> tracked separately; it does not affect any datum above (the sidecar WAS carried IN to
> both rejoin arms, verified `carried prior-admission sidecar (57 bytes)`).

#### 4(O.19) There is NO "special-credit handshake to complete": the op-6/op-7/op-8/op-9 exchange §4(O.17)/§4(O.18) read as SCS Send-Credit flow control is the SCS$DIRECTORY connection's DISCONNECT + teardown-prologue, and BOTH §4(O.18) candidates are REFUTED — the coordinator withholds op-7 for the same reason it withholds op 0x04: its per-VC `recv_seq` FREEZES at OVMX `send_seq=10`, so OVMX's op-9 (`ss=11`), its op-6 DISCONNECT_REQ (`ss=12`) and the deferred op 0x02 (`ss=13`) all ride behind the frozen point and are never delivered — this is the §4(O.14)/§4(O.15) freeze, now pinned to the frame at `ss=11`, and #291's op-6 is a mislabeled disconnect that CANNOT help (GROUNDED, SUCCESS oracle frame-by-frame + the same `vaxlab-0` fix-arm capture §4(O.18) ran + `scs_env.h` MTYPE namespace + `scs_t89_measure.py` + *VAXcluster Principles* pp. 2-43/2-44/2-45, `vms-b1db`, 2026-08-11)

**Frame.** §4(O.18) left two candidates for why the coordinator retransmits op-8 ×34 and
never sends op-7 to OVMX's op-6: (1) OVMX's op-9 sends a credit COUNT the coordinator
rejects (Davis pp. 2-44/2-45), or (2) the CM JOIN Phase-1 dialogue is never entered
(pp. 7-37/7-39). This subsection reads the fix-arm capture frame-by-frame against the
SUCCESS oracle and against OVMX's own MTYPE namespace, REFUTES BOTH, and shows the
op-6/7/8/9 "credit handshake" was a mis-layering: those opcodes are the SCS$DIRECTORY
connection being torn down, riding the same virtual circuit whose `recv_seq` freeze
§4(O.14)/§4(O.15) already identified as the rejoin blocker. It ships NO wire change.

**op-6/op-7 are DISCONNECT_REQ / DISCONNECT_RSP, not a "special-credit request/response"
(GROUNDED, `scs_env.h` + `scs_t89_measure.py` + oracle wire).** In OVMX's shared MTYPE
namespace at content `[46]` (abs `[60]`), `6 = SCS_ENV_MTYPE_DISCONNECT_REQ` and
`7 = DISCONNECT_RSP` (`src/vmsscs/include/scs_env.h`); `scsd.c`'s own deleted-function
comment states it flatly — *"op 6 IS the DISCONNECT_REQUEST of spec sec 4(h)(1a)"*. On the
oracle wire op-6/op-7 is a **bidirectional connection close**: for every op-6 (62-byte,
credit 0) the peer immediately returns an op-7 (58-byte, credit 0) with the Con.ID pair
swapped, then reverses roles (oracle `f1214` op-6 VAX1→rejoiner → `f1215` op-7 →
`f1216` op-6 rejoiner→VAX1 → `f1218` op-7). op-8/op-9 is the teardown PROLOGUE that
immediately precedes it (`scs_t89_measure.py`: 131/131 type-8 dialogues are SCS$DIRECTORY
and biconditional with teardown; op-8 → op-9 → op-6 → op-7). So "op-6 = OVMX's own
special-credit request" (§4(O.17)/§4(O.18)) mislabels a DISCONNECT_REQ, and
`scs_send_rejoin_credit_op6()` confirms it in code: it builds the frame from
`scs_dir_disc_tmpl` (a 76-byte DISCONNECT_REQ template, op field `[60]=6`, credit `[62]=0`)
— it returns NO credit at all.

**The coordinator's per-VC `recv_seq` freezes at OVMX `send_seq=10` and never advances —
op-9, op-6 and op 0x02 all ride behind it (GROUNDED, `d94-v46f-Rfix.pcap`, OVMX
`d6:1c:18:40:c0:ff`, `recv_ack` abs `[32:34]`, `send_seq` abs `[34:36]`, op abs `[60]`).**
On BOTH coordinator VCs the cumulative `recv_ack` toward OVMX climbs to **10** and then
FREEZES for the whole run (max `recv_ack` = 10 from VAX1 `aa:00:04:00:01:04` AND from VAX2
`08:00:2b:8e:b7:85`; op-7 count toward OVMX = **0**). OVMX's directory + member connections
to each peer share ONE per-VC `send_seq` (§4(O.14)): to VAX2 the SCS$DIRECTORY conid
`0x2B2C0017` and the member conid `0x2B2C0012` interleave a single counter `1..16`. What
sits at each position after the freeze:

| OVMX frame | `send_seq` | conid | what it is |
|---|---|---|---|
| `f63` op-10 (94 B) | 10 | `0x2B2C0017` dir | last directory-lookup msg the coordinator ACCEPTS (`recv_ack`→10) |
| `f65` **op-9** (58 B) | **11** | `0x2B2C0017` dir | teardown-prologue response — **coordinator drops it, `recv_ack` stays 10** |
| `f142` **op-6** (62 B) | **12** | `0x2B2C0017` dir | #291's "credit-first" frame = a DISCONNECT_REQ — behind the freeze, never processed → no op-7 |
| `f143` **op 0x02** (190 B) | **13** | `0x2B2C0012` member | the deferred CM add-member config (`SCSD-I-CMCONFIG2`) — behind the freeze → `cm_responses=0` |

The coordinator keeps RETRANSMITTING op-8 (mt16 `0x4b`→`0x7b`, `SCSD-I-CREDITRETX` every
~3 s) because from its side the VC is stuck at 10; it never reaches the op-6→op-7 close,
never opens the config connection, never delivers op 0x02, and finally tears down
(`XITDONE=0`). So "why won't the coordinator send op-7?" has one answer: **op-7 is a
DISCONNECT_RSP, and OVMX's op-6 (`ss=12`) is behind the same frozen point that already
dropped its op-9 (`ss=11`)** — there is no separate credit transaction being withheld.

**Candidate 1 (wrong op-9 credit COUNT) — REFUTED.** OVMX's op-9 credit `[62:64]=2`
correctly ECHOES the coordinator's op-8 credit `2`, exactly as the oracle echoes `1→1`
(`f1206` op-8 cr 1 → `f1211` op-9 cr 1). The frame is dropped at the VC-sequence layer
(`recv_seq` frozen at 10) BEFORE any credit accounting runs, so the credit value cannot be
the cause. A byte diff of OVMX's op-9 vs the oracle's ACCEPTED op-9 leaves exactly three
deltas — `send_seq` (VC position), the echoed credit, and content `[22:24]` (abs `[36:38]`:
OVMX inherits `1` from the op-8; the oracle rejoiner sends `2`) — and none is a
credit-count field. (See the residual lead below for `[22:24]`.)

**Candidate 2 (CM JOIN Phase-1 never entered) — REFUTED as the proximate cause.** The
coordinator never reaches ANY connection-manager layer: it is stuck retransmitting a
transport-layer directory-connection teardown message, three `send_seq` positions below
the op 0x02 that would even begin JOIN. Phase-1 (pp. 7-37/7-39) is downstream of a freeze
that happens first; it cannot be gating what the wire never gets to.

**#291 (`OVMX_REJOIN_CREDIT_FIRST`) cannot close rejoin, by construction.** Its op-6 is a
DISCONNECT_REQ emitted at `ss=12` — already behind the freeze — so it moves nothing; and by
inserting a frame ahead of op 0x02 it pushes the config from `ss=12` to `ss=13`, deeper
behind the frozen point. This is why the fix arm and the (unconditional-CREDITRETX) control
arm are indistinguishable at `XITDONE=0` (§4(O.18) confound). The op-6-first ordering is
not wrong so much as IRRELEVANT: it operates entirely inside the un-delivered region.

**What the fix increment must do (`vms-694` child).** Abandon the Send-Credit framing — the
blocker is NOT credit starvation and NOT JOIN Phase-1. It is the §4(O.14)/§4(O.15)
per-VC `recv_seq` freeze, now pinned to a single frame: **OVMX's `send_seq=11` (the op-9
directory-teardown-prologue response on the coordinator's SCS$DIRECTORY VC) is the frame
the coordinator refuses to advance past.** The next increment must determine WHY that one
frame is dropped while OVMX's `send_seq≤10` are accepted, then make it acceptable so
`recv_seq` climbs past 10 and the op-6 close, the config connection and op 0x02 all deliver.
The lead on the record is content `[22:24]` (abs `[36:38]`): the oracle rejoiner's op-9
carries `2` there and OVMX carries `1` (inherited unchanged from the op-8 by
`scs_reflect_credit()`, which never rewrites abs `[36:38]`); across the oracle the field is
`1` on coordinator→member frames and `2` on member→coordinator frames on these directory
dialogues, so its semantics must be GROUNDED (a path/channel or per-direction selector, not
yet named) before OVMX changes it — a wrong guess here is exactly the "over-declared runt"
class `scs_reflect_credit()`'s own `vms-760` comment records as having frozen `recv_seq`
before. This is a `recv_seq`-delivery question, continuous with §4(O.14), NOT the
pp. 2-44/7-39 questions §4(O.17)/§4(O.18) posed.

**Non-claims.** (1) NO wire change ships here — a freeze-point + opcode-identity forensic,
so (like §4(O.14)–§4(O.18)) it carries no kill-switch or frame test. (2) This does NOT
resolve the §4(O.14) ROOT — WHY the coordinator drops `send_seq=11` — it PINS the freeze to
that frame and refutes the two §4(O.18) candidates; `[22:24]` is a lead, not a grounded
cause. (3) The opcode re-identification (6/7 = DISCONNECT, 8/9 = teardown prologue) is the
`scs_env.h`/`scs_t89_measure.py`/oracle reading and does not touch the still-open RE gap of
what a "special credit message" looks like on this wire (§4(h)(1f), `vms-7e7`). (4) `recv_ack`
high-water = 10 is read across possible VC-incarnation restarts (§4(O.18) note); it
establishes "the ceiling is 10 and op-7 never appears", not a byte-fixed freeze frame.

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap`
(op-8/op-9 teardown prologue `f1206`/`f1211`, bidirectional op-6/op-7 close
`f1214`/`f1215`/`f1216`/`f1218`, coordinator opens the config connection `f1203` op-0 and
sends 190-byte config `f1208`/`f1209`; rejoiner op-9 `[22:24]=2`); live fix arm
`/data/training/vax/k8s-labs/vaxlab-0/logs/d94-v46f-Rfix.pcap` (coordinator `recv_ack`
frozen at 10 on both VCs, op-7×0; OVMX op-9 `ss=11` `f65`/`f102`, op-6 DISCONNECT `ss=12`
`f142`/`f150`, deferred op 0x02 `ss=13` `f143`; shared per-VC `send_seq` `1..16` across dir
conid `0x2B2C0017` + member conid `0x2B2C0012`) and `scsd-v46f-Rfix.log`
(`SCSD-I-CREDITFIRST`, `SCSD-I-CREDITRETX ×34`, `SCSD-I-CMCONFIG2 … local=0x2B2C0012`,
`cm_responses=0`, `XITDONE=0`); code `src/vmsscs/scsd.c` (`scs_send_rejoin_credit_op6`
builds from `scs_dir_disc_tmpl`; `scs_reflect_credit` never rewrites abs `[36:38]`;
the deleted-`scs_send_disconnect` comment "op 6 IS the DISCONNECT_REQUEST") and
`src/vmsscs/include/scs_env.h` (MTYPE 6/7 = DISCONNECT_REQ/RSP, 8/9 = special-credit
msg/paired response); measurement `tools/cluster/scs_t89_measure.py` (op-8/op-9 biconditional
with teardown) and `tools/cluster/dissect_sca.py` (op abs `[60]`, `recv_ack` abs `[32:34]`,
`send_seq` abs `[34:36]`); *VAXcluster Principles* (Davis 1993, HOST-ONLY, cited by page,
never committed) pp. 2-43 (Send Credit), 2-44 (special credit message / Pending Receive
Credit), 2-45 (Credit Wait), 7-37/7-39 (JOIN / Rule of Total Connectivity — the layer the
wire never reaches).

#### 4(O.20) The `[22:24]` freeze frame IS a node-incarnation mismatch: OVMX's op-9/op-6 hand-built control frames inherited the COORDINATOR's incarnation instead of stamping OVMX's own, and stamping the grounded value UNFREEZES the coordinator's per-VC `recv_seq` (it climbs to 14, past the §4(O.19) freeze) — but rejoin still does NOT complete (`XITDONE=0`): with the freeze gone and the op 0x02 delivered in REJOIN form in-order to the coordinator, the member STILL declines to reciprocate (`cm_responses=0`, no op 0x04), so §4(O.19)'s freeze was NECESSARY-NOT-SUFFICIENT and the frontier relocates to the member non-reciprocation (§4(O.10)) (GROUNDED, SUCCESS-oracle per-source census + *VAXcluster Principles* §4(i).B + live fix/kill-switch bracket on `vaxlab-0` and a clean-identity rejoin on a virgin `vaxlab-1`, `vms-a63`, 2026-08-11)

**Frame.** §4(O.19) pinned the rejoin blocker to OVMX's op-9 at `send_seq=11` and left the content byte `[22:24]` (abs `[36:38]`) as the lead — OVMX sends `1`, the SUCCESS-oracle rejoiner sends `2` — with its semantics unnamed and the standing instruction to GROUND them before flipping (the `vms-760` runt precedent). This subsection grounds the field, ships the fix behind a kill-switch, and brackets it live. The freeze is broken; rejoin is not closed.

**`[22:24]` IS the node-incarnation counter (GROUNDED, census + §4(i).B).** Two independent groundings converge:
- **Census over every capture we hold** (`tools/cluster/dissect_sca.py` reader; content `[22:24]` = abs `[36:38]`, LE u16): the field is a **per-SOURCE-NODE constant**, invariant across peer, op, and `send_seq`. In the SUCCESS rejoin oracle `vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` the rejoiner stamps **2 on 10 495/10 495** of its frames — op-6/7/8/9 and application MTYPE-10 alike — and the two established members stamp **1 on 8 216/8 216** of theirs. In `formation-ci1.pcap` (steady 2-node) both nodes stamp 1 purely; in `af2-firsttimer-established` / `cd0-bootB-zk1099-join` a node stamps 1→2→3 as it re-forms.
- **§4(i).B already names it** the node-incarnation counter (byte-exact 1→2→3 across the `af2` first-timer specimens; the value the member advertises in its directed-HELLO `[78:80]` and the joiner echoes into `[22:24]`). Every OVMX envelope builder already stamps it (`ps_fill_mscp`/`scs_member`/`scs_connect`/`scs_dir` all write `put_le16(out+14+22, ps->incarnation)`).

**The bug (GROUNDED, `d94-v46f-Rfix` re-read + code).** `scs_reflect_credit()` builds OVMX's op-7/op-9 by `memcpy`-ing the received op-8 and rewriting seq/op/Con.IDs/length but NEVER rewriting `[36:38]` — so an op-8 FROM the coordinator (incarnation 1) makes OVMX's op-9 carry 1, while OVMX advertises its own incarnation (2 on a rejoin) on every envelope frame. Live on `vaxlab-0`, OVMX stamped 2 on op-0/1/2/3/10 but **1** on op-9 (the frozen frame). The two op-6 disconnect builders (`scs_send_disconnect_self`, `scs_send_rejoin_credit_op6`) had the same defect via `scs_dir_disc_tmpl`'s hard-coded `[36:38]=0x0100`.

**The fix UNFREEZES `recv_seq` (GROUNDED, live kill-switch bracket).** `src/vmsscs/scsd.c` now stamps `ps->incarnation` at `[36:38]` in all three hand-built control paths, behind `OVMX_CREDIT_NO_INCARN_ECHO` (default ON; `=1` restores the inherited value). Matched fix-vs-control bracket, one binary, same pod/store:
| arm | op-9 `[22:24]` | coordinator `recv_ack` high-water | op-9 disposition |
|---|---|---|---|
| kill-switch (`=1`, = old behaviour) | **1** (34/34) | **frozen at 12** | op-9 RETRANSMITTED 34× at `ss=13` |
| fix (default) | own incarnation | **climbs to 14** | op-9 delivered, `recv_seq` advances |

On a clean-identity rejoin on the virgin pod `vaxlab-1` (`OVMA60`/1973, one fresh join → removed → rejoin, incarnation 2) the fix stamps the **coordinator a consistent incarnation 2 (27/27 frames)**, its op-9/op-6 at `ss=11–15` all carry 2, and the coordinator's `recv_seq` reaches 14 with the op 0x02 delivered in REJOIN form (`membership generation 2`, `SCSD-I-CMREJOIN`). The unit test `tests/vmsscs/test_scsd_wire.c::test_rejoin_op9_stamps_our_own_incarnation` is the fail-pre/pass-post frame gate (op-9 carries 2 with the fix, 1 under the kill-switch).

**But rejoin is NOT closed — NECESSARY-NOT-SUFFICIENT, and the residual is the SAME §4(O.10) non-reciprocation reached cleanly.** With the freeze gone and op 0x02 delivered in-order to the coordinator in REJOIN form — `body[36:40]` = membership generation **2**, the exact field §4(O.10) named as the one OVMX must fill (`SCSD-I-CMREJOIN … membership generation 2 (body[36:40])`) — the member STILL returns `cm_responses=0`, never opens the reciprocal connection (`connect_sent=0`, `remote_conid=0`), and never sends its op 0x04 — `XITDONE=0`, `CLUSTER_NODES` stays 2. This is NOT a new blocker: §4(O.10)/`vms-e15` already GROUNDED that a correct op 0x02 REJOIN body (generation filled) does **not** re-admit — "the rejoin incompleteness is … not in the op 0x02 body." What §4(O.19)'s freeze did was prevent that op 0x02 from being DELIVERED at all (it rode behind `ss=11`); removing the freeze lets it deliver in-order, and the member's response is exactly the §4(O.10) negative — it declines to reciprocate on the joiner VC. So `vms-a63` removes the §4(O.19)/§4(O.14) `recv_seq` freeze and re-exposes the §4(O.10) member-non-reciprocation frontier underneath it, now with op 0x02 provably delivered.

**Non-claims / RE gaps.** (1) The fix is grounded and proven to unfreeze `recv_seq`, but does NOT close rejoin — it relocates the frontier. (2) `[22:24]` is stamped PER-PEER (`ps->incarnation` is per-peer, echoing each member's advertisement per §4(i).B); on a heavily-rejoined identity the two members DISAGREE (coordinator advertised 2, VAX1 advertised 3 on `vaxlab-1`; 4 vs 6/7 on the saturated `vaxlab-0`). The coordinator got the correct consistent value and `recv_seq` still advanced, so the per-peer disagreement is not the proximate blocker — but whether a real rejoiner must present a SINGLE per-node incarnation to all members when they disagree is an RE gap (the oracle never shows members disagreeing). (3) **Lab methodology finding:** the members track incarnation by OVMX's NISCA channel MAC, which is constant within a pod's lifetime, so repeated OVMX runs SATURATE a pod's incarnation state (`vaxlab-0` reached 6/10 and the nodes diverged); a clean incarnation test needs a virgin replica.

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` (per-source `[22:24]` census: rejoiner 2×10 495, members 1×8 216); live fix/control
`/data/training/vax/k8s-labs/vaxlab-0/logs/d94-a63Rfix.pcap` (`recv_ack`→14/16) and `d94-a63Rctl.pcap` (`OVMX_CREDIT_NO_INCARN_ECHO=1`: op-9 `[22:24]=1`×34, `recv_ack` frozen at 12); clean-identity rejoin `/data/training/vax/k8s-labs/vaxlab-1/logs/d94-a63V2rejoin.pcap` + `scsd-a63V2rejoin.log` (`SCSD-I-CMREJOIN` generation 2, coordinator incarnation 2, `cm_responses=0`, `XITDONE=0`); code `src/vmsscs/scsd.c` (`scs_reflect_credit`/`scs_send_disconnect_self`/`scs_send_rejoin_credit_op6` now stamp `ps->incarnation` at abs `[36:38]`), test `tests/vmsscs/test_scsd_wire.c`; *VAXcluster Principles* (Davis 1993, HOST-ONLY, cited by page, never committed) §4(i).B basis pp. 2-30/2-31.

#### 4(O.21) The rejoin readmission is a THREE-PARTY, MEMBER-DRIVEN JOIN and there is NO separate rejoin protocol at the CM layer — the frontier is that a RETURNING identity elicits ZERO of the per-member op02/op03 JOIN handshakes even with the wire delivery clean and a fresh incarnation presented; quorum-suspend, `remote_conid=0`, and "only-coordinator" are REFUTED as causes this session (GROUNDED, SUCCESS-oracle 3-party flow analysis + first-join-vs-rejoin daemon-summary bracket on `vaxlab-1` + *VAXcluster Principles* pp. 7-23/7-24/7-29/7-37/7-39/7-41/7-42, `vms-f61`, 2026-08-11)

**Frame.** §4(O.20) removed the `recv_seq` freeze and re-exposed the member non-reciprocation
(§4(O.10)) with op 0x02 provably delivered. Under the operator's ruling to STOP wire
byte-diffing and MAP OVMX against the documented connection-manager state machine, this
subsection reads the SUCCESS oracle's flow *shape* (not its bytes) against the book's JOIN
CLUSTER protocol, re-brackets first-join-vs-rejoin from the daemon's own per-peer summary,
and REFUTES three candidate causes. It ships **no wire change** — a log-only readmission map
(`SCSD-I-READMITMAP`) plus a design record; the fix is deferred as an isolation experiment,
not a guess. Full map: `docs/design-rejoin-cm-state-map.md`.

**The book gives a rejoin no distinct protocol (Davis pp. 7-23/7-24/7-25, HOST-ONLY, cited by
page).** The CSB **NEW** state explicitly covers "a remote Connection Manager in a system
that left the cluster and is returning" — identical to a newly discovered one. When a node
leaves and rejoins, its old CSB is deallocated and "a new CSB is created for it just as if it
were joining the cluster for the first time," with a **new CSID**; the old incarnation's CSB
goes to **DEAD** ("a new incarnation of a VAX system has been seen"). So a returning node runs
the **JOIN CLUSTER** transition (pp. 7-37/7-42) identically to a first-timer. OVMX's
rejoin-specific apparatus (`cm_rejoin_target_mode`, `cm_apply_rejoin_form`, the REJOIN-form op
0x02 generation ordinal, the op-6 "credit-first" already shown to be a mislabeled
DISCONNECT_REQ in §4(O.19)) models a fork the book does not have at this layer.

**The JOIN completion gate is three-party and MEMBER-DRIVEN — GROUNDED on the oracle's flow
shape.** `vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` runs **two complete, independent,
symmetric per-member bring-ups sequentially**: VAX1↔VAX3 (f1176–1218) then VAX2↔VAX3
(f1224–1263). On each, **the member OPENS the `VMS$VAXcluster` connection toward VAX3** (VAX1
f1200, VAX2 f1246), VAX3 answers as TARGET, the member sends op00, **VAX3 sends op02** (f1204,
f1250), and **the member sends op03 COMMIT** (f1206, f1252) — then op06/op07 close each
handshake. Finally **the two established members reconcile the new membership with each other
via `op0c`** (f1308/f1312). VAX3 originates NO outbound `VMS$VAXcluster` connect. This is the
book's Rule of Total Connectivity (p. 7-39) and FORM/JOIN connectivity dialogue
(pp. 7-34/7-37/7-39) made concrete: admission requires the returning node to be **independently
admitted by every member** and then reconciled between them. **Corrected op-map for this
window:** there is **no distinct `op04` at abs `[60]`** — the member's "reciprocation" the
daemon awaits is **op03, the member's COMMIT** (Phase 2, pp. 7-41/7-42); `op0c` is the
member↔member reconciliation.

**The rejoin, re-bracketed from the daemon's own per-peer summary (GROUNDED, same identity/pod,
`vaxlab-1`, `OVMA60`).** First-join `scsd-a63V1.log` vs rejoin `scsd-a63V2rejoin.log`:

| per-peer counter | first-join (`XITDONE=1`) | rejoin (`XITDONE=0`) |
|---|---|---|
| `cm_responses` (M1026 / VAX1) | **76 / 151** | **0 / 0** |
| `cm_config` | YES | no |
| `vaxcluster_member` | CONNECTED(reached-OPEN) | no |
| VC / channel / dir | OPEN / UP / YES | OPEN / UP / YES |

Both members run the CM JOIN handshake on the first join and **neither** does on the rejoin.
OVMX *does* reach both members at VC/channel/directory level and *does* drive op 0x02 REJOIN
toward both, so the members will not engage the JOIN for a **returning** identity — the same
frontier §4(O.10)/§4(O.20) named, now grounded from the per-member handshake count.

**Three candidate causes REFUTED this session (recorded so they are not re-chased).**
1. **Quorum self-suspend — NOT causal.** The rejoin logs `present_votes 1→0 → quorum LOST
   (suspend + wait)`, but the **identical** benign flip appears on the SUCCESSFUL first-join
   (`scsd-a63V1.log` 02:32:34.028→.102), which reaches `XITDONE` 170 ms later. A transient.
2. **`remote_conid=0x00000000` / `connect_sent=0` — NOT anomalies.** Both appear on the
   successful first-join's per-peer summary; OVMX answering member-driven connects
   (`CONNECT_REQ` received, `connect_sent=0`) is the oracle's topology, not a defect.
3. **Stale `incarnation_time` — NOT the live cause.** `ovmx_incarnation_time()` reads
   `CLOCK_REALTIME` (ns) per process start, so every rejoin presents a **fresh** [66:74]
   incarnation; the vms-2f3 live-incarnation fix is present and is necessary-but-not-sufficient.

Also refuted: "OVMX ignores the non-coordinator" — OVMX reaches and drives BOTH members.

**Where the frontier is, and the responsible next step.** With delivery clean (recv_seq
unfrozen, op 0x02 in-order + REJOIN form, incarnation fresh, both members reached), the
members still run ZERO per-member JOIN handshakes for a returning identity and reconcile only
*among themselves* (`op0c`), leaving OVMX out. Every remaining lever is a wire behaviour toward
a live VAX, and an ungrounded control frame in this region has crashed real VAX nodes and
frozen `recv_seq` before (§4(O.19)/§4(O.20); the `vms-760` runt precedent; `cm_response_shape`).
So the next increment must be an **isolation experiment**, not a guess: instrument, per member,
whether that member opens a fresh `VMS$VAXcluster` connect to OVMX after OVMX's fresh-incarnation
START and drives op00→(OVMX op02)→**op03 commit** — i.e. whether the member is running a JOIN
transition for OVMX at all, or a member↔member transition that excludes it.

**What ships (`vms-f61`).** (1) The design record `docs/design-rejoin-cm-state-map.md`. (2) A
**log-only, kill-switched** readmission map: at exit, per member, the grounded verdict
`readmit_verdict_of()` (`ADMITTED` / `ENGAGED-NOT-LATCHED` / `NO-ENGAGE(returning-identity
non-admission)` / `NO-CHANNEL`) plus a `READMITMAP-SUMMARY` naming the frontier when every
reached member reads `NO-ENGAGE`, and the incarnation OVMX presented. Silence with
`OVMX_NO_READMITMAP=1`; the working first-join path is byte-unchanged (guard 8). (3)
Fail-pre/pass-post unit test
`tests/vmsscs/test_scsd_wire.c::test_readmit_verdict_classifies_the_rejoin_frontier`. It exists
so the next isolation is one log line, not another pcap dig.

**Non-claims.** (1) This ships NO wire change and does NOT close rejoin — it reframes the
thread against the book and refutes three candidates. (2) The op-map correction (op03 = member
COMMIT; no distinct op04; op0c = member↔member reconciliation) is grounded on the oracle's flow
*shape*, not a new byte-level field claim. (3) The "member↔member transition excludes OVMX"
reading is the leading hypothesis for the next experiment, not asserted as the cause.

**Evidence** (host, tank volume): SUCCESS oracle
`/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` (per-member
bring-ups f1176–1218 / f1224–1263: member-opened `VMS$VAXcluster` f1200/f1246, VAX3 op02
f1204/f1250, member op03 f1206/f1252, op06/op07 f1213–1217/f1259–1262; member↔member `op0c`
f1308/f1312); first-join-vs-rejoin bracket
`/data/training/vax/k8s-labs/vaxlab-1/logs/scsd-a63V1.log` (`XITDONE=1`, `cm_responses=151/76`)
vs `scsd-a63V2rejoin.log` (`XITDONE=0`, `cm_responses=0/0`, benign quorum 1→0 flip present on
BOTH); code `src/vmsscs/scsd.c` (`readmit_verdict_of`, `READMITMAP`, `ovmx_incarnation_time`,
`scs_start.c` incarnation [66:74]) and `src/vmsscs/scs_recnx.c` (the CSB 10-state model, DEAD
defined); design record `docs/design-rejoin-cm-state-map.md`; unit
`tests/vmsscs/test_scsd_wire.c`; *VAXcluster Principles* (Davis 1993, HOST-ONLY, cited by page,
never committed) pp. 7-23/7-24/7-25 (CSB states, DEAD, rejoin=fresh CSB), 7-29 (JOIN
precondition), 7-37/7-38/7-39 (JOIN + Rule of Total Connectivity), 7-41/7-42 (Phase 1/Phase 2
commit).

#### 4(O.22) The member-side suppressing state is ISOLATED: it is the member's RESIDUAL per-identity CSB for a node it saw before — a returning identity collides with a retained `BRK_NON`/`BRK_NEW` CSB the member never reclaims, whereas a FRESH identity has no such CSB and is admitted; proven by a single-factor lab flip (same pod/daemon/op02-form, only the identity's residual-CSB status varies) and directly visible in real VMS `SHOW CLUSTER` (GROUNDED, `vms-4dd`, live `vaxlab-1` A/B′/C bracket + *VAXcluster Principles* pp. 7-24/7-25/7-33/7-39/7-40/7-41, 2026-08-11)

**Frame.** §4(O.21) named the frontier — a member ADMITS a fresh identity but NO-ENGAGEs a
returning one — and left the *cause* open, mandating an isolation experiment (no speculative
wire change; the `vms-760` runt precedent). `vms-4dd` runs it.

**The single-factor bracket (GROUNDED, freshly-booted `vaxlab-1`, clean member state).** Three
consecutive arms on ONE pod, **all in first-join op02 form** (op02 shape held CONSTANT),
`OVMX_JOIN_SEQ=1`, `vms-4dd` READMITMAP daemon (`md5=e824f3c1…`, verified in-pod), hard
`timeout` per op, two brand-new identities (`OVX40`/1973, `OVX50`/1974, never used on any pod):

| arm | identity | member holds a residual CSB for it? | `cm_responses` (VAX1/VAX2) | `XITDONE` |
|---|---|---|---|---|
| A `4dAf` | `OVX40` FRESH | no | **196 / 39** | **1** (CN_3) |
| B′ `4dBr` | `OVX40` RETURN | yes (created by arm A) | **0 / 0** | **0** |
| C `4dCf` | `OVX50` FRESH | no | **196 / 40** | **1** (CN_3) |

Same pod, same daemon, same op02 form, back-to-back. The ONLY factor differing between B′
(NO-ENGAGE) and C (ADMITTED) is **whether the member already holds a CSB for the presented
SCSSYSTEMID** — flipping just that flips the member from NO-ENGAGE to ADMITTED. The suppressing
state is the **residual per-identity CSB**, not the op02 body, not incarnation freshness (OVMX
presents a distinct live incarnation each arm), not quorum, not "only-coordinator" (§4(O.21)
refutations preserved).

**Directly observable in the oracle's own `SHOW CLUSTER`.** Before arm A: VAX1/VAX2 only. After
arm A (`OVX40` joined to CN_3, daemon exited): `%PEA0, Port has Closed Virtual Circuit - REMOTE
NODE OVX40` and `OVX40 | VMX V0.1 | BRK_NON` — a retained broken non-member CSB the member does
NOT free. After the B′ return: the entry advances to `BRK_NEW` and the VC closes again — the
member re-`NEW`s the *residual* CSB rather than building a fresh one and running JOIN. On a
hammered pod (`vaxlab-0`), prior identities `OVXF61`/`OVX460` sat stuck in `BRK_NEW` for hours
and even a FRESH identity drew 0/0 there — un-reclaimed residual CSBs accumulate and degrade
admission for everyone, so the clean bracket required a freshly-booted pod. `BRK_NON`/`BRK_NEW`
are real VMS `SHOW CLUSTER` connection-state strings — the oracle naming its own retained state.

**Book grounding (Davis, HOST-ONLY, page cites only — clean-room Rule 8).** A member keeps a CSB
for every node it has had connectivity with; on departure the CSB is RETAINED, and only on
rejoin is the old CSB deallocated and a fresh one built "as if joining for the first time" with
a new CSID (pp. 7-24/7-25). The trigger to move the old CSB to DEAD is the member seeing a NEW
INCARNATION of that SCSSYSTEMID, matched on **both SCSSYSTEMID and incarnation time** (pp.
7-33/7-39). A join is abandoned if the prior membership's removal/rebuild is still pending (pp.
7-40/7-41). `BRK_NON`/`BRK_NEW` is precisely the retained-but-not-reclaimed CSB: a FRESH identity
has no such CSB (NEW-state CSB built → JOIN proceeds); a RETURNING one collides with it.

**The OVMX gap and fix design (relocated frontier).** OVMX already presents a fresh incarnation
each run, yet the member never reclaims `OVX40`'s CSB (holds it `BRK_NON`, re-`NEW`s to
`BRK_NEW` on return) instead of DEAD → deallocate → fresh JOIN. Two candidate causes, separable
by the next single-factor isolation (both are wire-region changes; neither ships speculatively):
(F1) OVMX's departure is a bare VC break, not a CM leave, so the member never runs the removal
reconfiguration (evict → REMOVED → break connectivity, p. 7-46) and retains the CSB — *test:* a
clean CM leave drops `OVX40` from `SHOW CLUSTER`, then a return admits; (F2) the member does not
read OVMX's fresh incarnation in the config/START field it matches against the stale CSB (pp.
7-33/7-39), so it never sees "new incarnation" — *test:* presenting it there moves the CSB DEAD
→ deallocated → JOIN (`XITDONE` 0→1). The next increment brackets F1 vs F2.

**What ships (`vms-4dd`, no wire change).** (1) The isolation record `docs/design-rejoin-cm-state-map.md` §6.
(2) A verdict refinement to the READMITMAP diagnostic: `ADMITTED` now requires the membership
latch **AND** `cm_responses > 0`. Arm B′ exposed a per-member false positive — VAX1's CDT
reached OPEN off a residual/partial reconnect (`vaxcluster_open_reached` latched) while it ran
ZERO CM handshakes (`cm_responses=0`, `XITDONE=0`), and the latch-only classifier reported that
member `ADMITTED`, masking the non-admission. A member that sent 0 CM responses did not run the
per-member JOIN handshake; it now reads `NO-ENGAGE` (INV-6: never report success from residual
state). Log-only, kill-switch `OVMX_NO_READMITMAP` unchanged, byte-unchanged on the wire (guard
8), with a fail-pre/pass-post case in `tests/vmsscs/test_scsd_wire.c`.

**Non-claims.** (1) NO wire change; rejoin is NOT closed — this ISOLATES the cause and designs
the fix. (2) F1 vs F2 is the next experiment, not asserted. (3) The verdict refinement is a
diagnostic-honesty fix, not the rejoin fix.

**Evidence** (host, tank volume): `/data/training/vax/k8s-labs/vaxlab-1/logs/scsd-4dAf.log`
(`XITDONE=1`, admitted=2), `scsd-4dBr.log` (`XITDONE=0`, returning-identity non-admission),
`scsd-4dCf.log` (`XITDONE=1`, admitted=2), matching `d94-4dAf/4dBr/4dCf.pcap` (`OVX40`/`OVX50`
on the wire), and the `SHOW CLUSTER` `BRK_NON`→`BRK_NEW` transitions in `vax1.log`; code
`src/vmsscs/scsd.c` (`readmit_verdict_of`); unit `tests/vmsscs/test_scsd_wire.c`; design record
`docs/design-rejoin-cm-state-map.md` §6; *VAXcluster Principles* pp. 7-24/7-25/7-33/7-39/7-40/7-41/7-46.

#### 4(O.23) F1-vs-F2 bracketed — BOTH the unclean-leave (F1) and the incarnation-not-read (F2) hypotheses are REFUTED: the member DOES run removal reconfiguration for a returning OVMX identity, and DOES read OVMX's fresh incarnation and DEALLOCATE+REBUILD the CSB (a genuine reclaim), yet the CM JOIN transition still never runs — so the frontier relocates from "residual CSB retained" to "reclaim succeeds, CM JOIN does not run" (GROUNDED, `vms-944`, live SDA bracket on freshly-booted `vaxlab-1` A/B1/B2 + *VAXcluster Principles* pp. 7-23/7-24/7-25/7-29/7-30/7-37/7-39, 2026-08-11)

**Frame.** §4(O.22) isolated the suppressing state to the member's residual per-identity CSB and
left TWO candidate causes for the next single-factor bracket: **(F1)** OVMX's departure is a bare
VC break, not a CM leave, so the member never runs removal reconfiguration and the CSB is retained;
**(F2)** the member does not read OVMX's fresh incarnation in the field it matches against the
stale CSB, so it never sees "a new incarnation" (pp. 7-33/7-39). `vms-944` runs the bracket, and
the **SDA oracle** (VAX1 parked in `ANALYZE/SYSTEM`, sampling the CSB per arm — far finer than the
`SHOW CLUSTER` `BRK_NON`/`BRK_NEW` strings §4(O.22) had) refutes **both**.

**The bracket (GROUNDED, freshly-booted `vaxlab-1`, clean CN_2, one identity `OVX940`/1973).**
Three consecutive arms, first-join op02 form held constant, `OVMX_JOIN_SEQ=1`, hard `timeout` per op:

| arm | what | member's CSB for `OVX940` (SDA) | `XITDONE` |
|---|---|---|---|
| A/B1 `944B1f` | FRESH join, live incarnation | CSB **built at `879EBA40`**, Incarnation **11-AUG-2026 05:18:23**, Flags `member,selected`; `received VAXcluster membership request` | **1** |
| (departure) | daemon exits (bare VC break) | CSB `879EBA40` → State `09 wait`, Flags `06040005 long_break,**removed**`, Incarnation still 11-AUG | — |
| B2 `944B2r` | RETURN, `OVMX_INCARNATION_TIME=52975296000000000` (**1-OCT-2026**, blatantly later) | old CSB `879EBA40` **DEALLOCATED**; **NEW CSB `879EBE00`**, State `01 open`, Incarnation **updated 11-AUG-2026 → 1-OCT-2026**, Flags `00000000` (**non-member**); NO membership request | **0** |

**Why the OCT-2026 pin isolates F2.** F2 says the member cannot see a new incarnation. Presenting
an incarnation ~7 weeks in the future — impossible to confuse with the AUG join — makes any member
read of it unmissable in SDA. The oracle answered directly: the CSB's stored **Incarnation changed
11-AUG → 1-OCT** and the CSB itself was **deallocated and rebuilt at a new address** (`879EBA40` →
`879EBE00`). That is precisely Davis's rejoin path — p. 7-24 (**DEAD**: "a new incarnation of a VAX
system has been seen") → p. 7-25 ("the old CSB is deallocated, and a new CSB is created for it just
as if it were joining for the first time"). **The member reads our incarnation and reclaims the
CSB. F2 is refuted — you can watch the read happen.**

**Why F1 is refuted.** The B2 **T-PRE** sample (before the return daemon starts) shows the residual
CSB already carrying the **`removed`** flag — the MEMBER flag is cleared and REMOVED set only by the
removal reconfiguration (p. 7-30: after a non-last-gasp VC loss the CM reconnects for RECNXINTERVAL,
then reconfigures regardless). So the member **DID run removal** after OVMX's ordinary departure,
with no CM leave from OVMX. Independently, p. 7-25 makes CSB retention the NORMAL post-departure
state for **any** leave (clean shutdown included — only the CSV entry is released), so "unclean leave
→ CSB retained" never distinguished success from failure. **F1 is refuted.**

**The relocated frontier.** With the CSB reclaimed (new address), OVMX's fresh incarnation read into
it, and the SCS connection re-`open`, the rebuilt CSB **stayed `Flags 00000000` (non-member) for the
whole 90 s arm and no `received VAXcluster membership request` was logged** — the **CM JOIN state
transition never ran** (`XITDONE=0`). The gate is neither the CSB nor the incarnation (both demonstrably
work); it is the JOIN transition itself — the three-party Rule of Total Connectivity (pp. 7-37/7-39),
exactly where §2.2 of `docs/design-rejoin-cm-state-map.md` already placed the completion gate. The
`vms-944` daemon refinement names this: `RECLAIMED-NOJOIN` (SYSAP re-opened + `cm_responses==0`) is
split out of `NO-ENGAGE` so the reclaim-succeeded-JOIN-did-not case is visible, not masked.

**Non-claims / open sub-question.** (1) NO wire change ships; rejoin is NOT closed — this REFUTES F1
and F2 and relocates the frontier. (2) B2 pinned a FUTURE incarnation to make the read visible; whether
a REALISTIC (near-now, slightly-later) incarnation ALSO triggers the reclaim — i.e. whether §4(O.22)'s
"never reclaims" was a `SHOW CLUSTER`-string artifact (`BRK_NEW` being the freshly-allocated NEW-state
CSB, not a re-NEW of the old one) or whether the reclaim needs a coarse incarnation delta — is the ONE
open sub-question, resolved by a realistic-incarnation return under the SAME SDA observation. Either way
the CM JOIN transition is the frontier: §4(O.22)'s arm `4dBr` (realistic live incarnation) already gave
`XITDONE=0`. (3) The `RECLAIMED-NOJOIN` split is a diagnostic-honesty refinement, not the rejoin fix.

**Evidence** (host, tank volume): `/data/training/vax/cluster/work/944B1f.csb` (fresh join, CSB
`879EBA40` member, Incarnation 11-AUG-2026 05:18:23) and `944B2r.csb` (return: T-PRE `879EBA40`
`long_break,removed`; T+5s new CSB `879EBE00` open, Incarnation 1-OCT-2026, non-member), with
`944B1f.status` (`XITDONE=1`) / `944B2r.status` (`XITDONE=0`) and `/lab/k8s-labs/vaxlab-1/logs/
scsd-944B1f.log`/`scsd-944B2r.log` + `d94-944B1f/944B2r.pcap`; the wire incarnation `[66:74]`
(`docs/clean-room/tools/af2start.py` region) distinct per run — arm A `0x00bc0cd311bc6c62`
(11-AUG 04:33:49), arm B′ `0x00bc0cd380815b38` (11-AUG 04:36:55); code `src/vmsscs/scsd.c`
(`readmit_verdict_of`, `READMIT_RECLAIMED_NOJOIN`); unit `tests/vmsscs/test_scsd_wire.c`; design
record `docs/design-rejoin-cm-state-map.md` §7; *VAXcluster Principles* pp. 7-23/7-24/7-25/7-29/7-30/7-37/7-39.

#### 4(O.24) The §4(O.23) sub-question is SETTLED and the CM JOIN gate is BISECTED on the wire: a REALISTIC near-now incarnation reclaims the CSB IDENTICALLY to the future pin (so the frontier stays at the CM JOIN transition, NOT at "make the member see a new incarnation"); and a same-pod first-join-vs-return op02 diff proves OVMX DOES drive op 0x02 to the coordinator on the return — the coordinator receives it and starts NO ADD transition (no op 0x12 relay, no op 0x03 commit), running a member↔member reconcile + ABORT instead, so the gate is the COORDINATOR ignoring an op02-DRIVEN join for a returning identity (Davis p. 7-38), NOT an OVMX op02 omission (GROUNDED, `vms-0425`, same-boot single-factor SDA bracket + Rf/Rr CM-frame wire diff on `vaxlab-1` + *VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39, 2026-08-11)

**Frame.** §4(O.23) relocated the frontier to "reclaim succeeds, CM JOIN does not run" and left ONE
open sub-question: its reclaim was shown with a **blatantly future** incarnation pin (1-OCT-2026,
for SDA day-resolution visibility). Does a **realistic near-now** incarnation — what OVMX presents
normally — ALSO reclaim, or is the reclaim an artifact of a coarse delta (which would move the
frontier to "make the member see a new incarnation")? `vms-0425` settles it, then bisects the JOIN
gate on the wire.

**Sub-question SETTLED (same boot, single factor).** Two first-join→return pairs on ONE
freshly-booted `vaxlab-1`, two brand-new identities, first-join op02 form constant, `OVMX_JOIN_SEQ=1`,
VAX1 in SDA per arm — the pairs differ in **exactly one factor**, the return's incarnation:

| arm | identity | return incarnation | member's CSB for it (SDA) | `XITDONE` |
|---|---|---|---|---|
| `0425Rf` | `OVXR40`/1975 | — (first join) | built `8796AE80` → `member,selected` | **1** |
| `0425Rr` | `OVXR40` | default live (near-now) | `8796AE80` DEALLOCATED → NEW `879DF540`, `02040000 status_rcvd` (non-member) | **0** |
| `0425Ff` | `OVXF40`/1976 | — (first join) | built `879DEF00` → `member,selected` | **1** |
| `0425Fr` | `OVXF40` | `1-OCT-2026` | `879DEF00` DEALLOCATED → NEW `879DF2C0`, Incarnation `11-AUG → 1-OCT`, `02040000 status_rcvd` (non-member) | **0** |

The realistic return (`Rr`) and the future-pinned return (`Fr`) produce **byte-identical CSB
outcomes**: both DEALLOCATE the residual CSB and REBUILD a fresh one at a new address (Davis
pp. 7-24/7-25), both reach `02040000 status_rcvd` — the exact intermediate state a SUCCESSFUL first
join passes through — and both **stall there, never reaching `member,selected`** (`XITDONE=0`). Only
the SDA-displayed Incarnation *date* differs. So a realistic incarnation reclaims exactly as the
future pin does; §4(O.22)'s "never reclaims" was a `SHOW CLUSTER`-string artifact. **The frontier
does NOT relocate — it stays at the CM JOIN transition.**

**CM JOIN gate BISECTED (Rf-vs-Rr CM-frame wire diff, same identity/pod).** Decoding the
`VMS$VAXcluster` CM frames (category `body[8]`, opcode `body[9]`; OVMX `b2df5fa41233`, coordinator
VAX2 `08002b45161a`): on the FIRST join OVMX sends op 0x02 → the coordinator RELAYS it (op 0x12) to
the other member and COMMITS (op 0x03) to OVMX → op 0x05/op 0x06 → joined. On the RETURN **OVMX
still sends op 0x02** (its own log: `SCSD-I-CMCONFIG2 … sent DEFERRED op 0x02 … to node 2 … expect
its op 0x03 COMMIT`), correctly addressed — but the coordinator sends **no op 0x12 relay and no op
0x03 commit**; it runs a member↔member cat-0x06 MEMBERSHIP reconcile that EXCLUDES OVMX, then a
transition ABORT, and OVMX's `VMS$VAXcluster` VCs end CONNSTUCK (`still-open-at-exit=2`). This
**bisects the either/or**: it is NOT that OVMX omits op 0x02 (it drives it), and NOT that the member
waits silently (it actively runs a different transition). Per Davis p. 7-38 the coordinator, on a
join request, runs admission/quorum tests and **"ignores the request … and no state transition
occurs"** when they fail; a FIRST join passes them, a RETURN (against a member that just reclaimed
OVMX's CSB) does not. **The gate is the coordinator ignoring an op02-driven ADD for a returning
identity** — one step in from §4(O.23)'s "CM JOIN does not run".

**Relocated frontier.** The next isolation is **SDA on the COORDINATOR** (the member OVMX sends op
0x02 to), across departure+return, to see why it declines an op02-driven ADD for a reclaimed
identity it admits when fresh — i.e. whether its CLUB nodemap / quorum-vote accounting still carries
residual state for the departed OVMX so its p. 7-38 tests fail. A separate OVMX-side defect surfaced
(not the gate): on the return OVMX re-drives its add-member burst with `remote_conid=0x00000000` and
leaves its `VMS$VAXcluster` VCs CONNSTUCK.

**Non-claims / what ships (no wire change, guard 8).** (1) rejoin is NOT closed; this SETTLES the
sub-question and BISECTS the gate. (2) The READMITMAP verdict gains `JOIN-ABANDONED`
(`joiner_cfg2_sent && cm_responses==0`), split out of `RECLAIMED-NOJOIN`/`NO-ENGAGE`, naming the
member OVMX actually DROVE the join request to (the coordinator) distinctly from a member it only
reached — checked before the open latch because OVMX's own VC ends CONNSTUCK on the return (so the
latch under-reports; the live `0425Rr` read `NO-ENGAGE` under the old latch-first classifier, masking
that op 0x02 had been driven). Log-only, kill-switch `OVMX_NO_READMITMAP` unchanged, fail-pre/pass-post
case in `tests/vmsscs/test_scsd_wire.c`, byte-unchanged on the wire.

**Evidence** (host, tank volume): `/data/training/vax/cluster/work/0425{Rf,Rr,Ff,Fr}.csb` (SDA CSB
timelines) with `.status` (`Rf`/`Ff` `XITDONE=1`; `Rr`/`Fr` `XITDONE=0`);
`/lab/k8s-labs/vaxlab-1/logs/scsd-0425{Rf,Rr,Ff,Fr}.log` + `d94-0425{Rf,Rr,Ff,Fr}.pcap` (identities
`OVXR40`/1975, `OVXF40`/1976 on the wire); CM-frame decoder `docs/clean-room/tools/cmdiff.py`; code `src/vmsscs/scsd.c`
(`readmit_verdict_of`, `READMIT_JOIN_ABANDONED`); unit `tests/vmsscs/test_scsd_wire.c`; design record
`docs/design-rejoin-cm-state-map.md` §8. A live re-run with the refined daemon (`0425Rr2`,
`scsd-0425Rr2.log`) reads the new verdict on the wire: coordinator `08:00:2b:45:16:1a`
`verdict=JOIN-ABANDONED`, the non-coordinator `NO-ENGAGE`, SUMMARY `join_abandoned=1 no_engage=1`
(the old latch-first classifier read both `NO-ENGAGE`, masking the op02-driven fact).
*VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39.

#### 4(O.25) SDA ON THE COORDINATOR settles WHY the op02-driven ADD does not run, and it is NOT the coordinator "ignoring" a received request (Davis p. 7-38): on the return the coordinator's connection manager **never PROPOSES the addition** — its console (`ANALYZE/SYSTEM` on the coordinator) prints `%CNXMAN, received VAXcluster membership request` → `proposing addition` → `completing … state transition` for the FRESH join, but for the op02-driven RETURN it prints ONLY `%CNXMAN, lost connection` / `timed-out lost connection`, never `received … request`, never `proposing addition`; the coordinator's per-node CSB for the returning identity reaches `02040000 status_rcvd` but stalls in connectivity State `03 reconnect`, **never `selected`, never `member`**, while the coordinator sits the whole return in `con_sent`/`con_pend` (`Remote Con. ID 00000000`) reconnect retries to the returning node — so the §4(O.24) `remote_conid=0x00000000` is the FINGERPRINT of a member-initiated readmission connection that never reaches a stable OPEN, NOT a separate defect; the gate RELOCATES one layer below §4(O.24) from "coordinator ignores an op02-driven ADD (CM-admission)" to "the coordinator's `VMS$VAXcluster` reconnect to the returning identity never settles OPEN, so CNXMAN never receives a membership request to propose" (GROUNDED, `vms-9a7`, coordinator-side SDA — `SHOW CLUSTER`/`SHOW CLUSTER/NODE`/`SHOW CONNECTIONS/NODE` parked on VAX2 across a same-boot fresh→crash→op02-driven-return single-factor bracket on virgin `vaxlab-2` + Rf/Rr CM-frame wire diff (`cmdiff.py`) + *VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39/7-40/7-46, 2026-08-11)

**Frame.** §4(O.24) relocated the frontier to **SDA on the coordinator** — the VAX OVMX drives op
0x02 to (VAX2 in this lab) — and left the open question: does the coordinator carry residual CLUB /
quorum state for the departed OVMX so its p. 7-38 admission tests fail? `vms-9a7` puts SDA on the
coordinator across the return and answers it: the p. 7-38 admission tests are **never reached**,
because the coordinator's connection manager (CNXMAN) never gets far enough to treat the return as a
join at all.

**Method (coordinator SDA + wire, single factor, same boot).** One virgin `vaxlab-2` (CN_2), one
minted identity (`OVX4`/1980), `ANALYZE/SYSTEM` parked on the COORDINATOR VAX2 the whole run,
sampling `SHOW CLUSTER` / `SHOW CLUSTER/NODE=OVX4` / `SHOW CONNECTIONS/NODE=OVX4`. The daemon runs
FRESH (no sidecar → admitted, `XITDONE=1`), is SIGKILLed (crash), and returns after a ~22 s gap
(past the member's reconnect detection but with the identity's prior-admission sidecar present, so op
0x02 takes the REJOIN form) — the ONLY factor that differs between the two op02 drives the coordinator
sees is fresh-vs-return. Current post-0425 daemon (`build-9a7`); `build-d94` predates seven rejoin
fixes and must not be used for a return arm.

**What the coordinator's CSB shows (single factor).**

| moment | coordinator CSB for the identity (SDA) | `XITDONE` |
|---|---|---|
| FRESH join | `879C05C0` → `02040000 status_rcvd` → `02060002 member,selected,status_rcvd`, State `01 open` | **1** |
| after crash | `879C05C0` State `09 wait`, still `member,selected,status_rcvd` (reconnect-wait) |
| after removal | `879C05C0` → `06040005 long_break,removed,status_rcvd` (departure reconfiguration removes the old incarnation) |
| op02-driven RETURN | **NEW** CSB `879CEA40` → `02040000 status_rcvd`, State `03 reconnect` — **never `selected`, never `member`** | **0** |

The fresh CSB reaches `member,selected`/`open`; the return's fresh-built CSB reaches `status_rcvd`
(op 0x02 was received) but **stalls in State `reconnect` and is never `selected`** — the coordinator
never runs the JOIN CLUSTER SELECT (p. 7-38) that would start the ADD.

**What CNXMAN prints (the decisive datum).** On the FRESH join the coordinator console prints, in
order, `%CNXMAN, received VAXcluster membership request from system OVX4` → `proposing addition of
system OVX4` → `completing VAXcluster state transition`. On the RETURN — with op 0x02 provably on the
wire (`OVMX>VAX2 op02-CONFIG`, `cmdiff.py`) — the coordinator console prints, for the entire return,
ONLY `%CNXMAN, lost connection to system OVX4` and finally `%CNXMAN, timed-out lost connection to
system OVX4`. It **never** prints `received … request` or `proposing addition`. The coordinator does
not *ignore* a received join request; its CNXMAN never *receives* one.

**Why: the readmission connection never settles OPEN (`SHOW CONNECTIONS`).** Across the return the
coordinator's `SHOW CONNECTIONS/NODE=OVX4` shows its `VMS$VAXcluster` (and `SCS$DIRECTORY`,
`VMS$DISK_CL_DRVR`) connections to the returning node parked in `State: 0007 con_sent` /
`Blocked State: 0001 con_pend` with `Remote Con. ID 00000000` — the coordinator is re-opening its
member-initiated connections to the returner (the Figure-2-14 pattern, Davis pp. 7-37/7-40/7-46) but
they **never reach OPEN**. The coordinator sent five `VMS$VAXcluster CONNECT_REQ`s over the return;
OVMX accepted exactly two to OPEN (one per member) and drove op 0x02 on the coordinator's, but the
coordinator's CNXMAN keys membership on a reconnect connection that stays `con_sent` and repeatedly
`timed-out lost connection`. Because that connection never settles OPEN, CNXMAN never sees a
membership request to propose, the CSB stays `reconnect`/`status_rcvd`, and no relay/commit/MEMB
follows the driven op 0x02 (`cmdiff.py`: after the return `OVMX>VAX2 op02-CONFIG` there is **no**
`op12-RELAY`, **no** `op03-COMMIT`, **no** `op06-MEMB` — only the pre-op02 member↔member `op0d-DEPART`
that removed the old incarnation). The `remote_conid=0x00000000` §4(O.24) flagged as "a separate
OVMX-side defect" is the SAME connection observed from OVMX's side — the readmission connection whose
non-settling IS the gate — so it is **reframed from "separate defect" to "fingerprint of the gate."**

**Relocated frontier.** The gate is NOT the coordinator's p. 7-38 quorum/connectivity ADMISSION
tests (never reached) and NOT residual CLUB/quorum accounting for the departed node (the coordinator
DEALLOCATEs the old CSB and builds a new one, exactly as §4(O.23)/§4(O.24) found on the member). It
is one layer below: **the coordinator's member-initiated `VMS$VAXcluster` reconnect to the returning
identity never reaches a stable OPEN (`con_sent`/`reconnect`, `Remote Con. ID 0`, `timed-out lost
connection`), so CNXMAN never proposes the addition.** The next isolation is WHY that reconnect never
completes: the coordinator opens (at least) two `VMS$VAXcluster` connects to the returner — one OVMX
accepts and drives op 0x02 on, and one that stays `con_sent` — and CNXMAN waits on the one that never
opens; determine whether OVMX must complete/accept the coordinator's expected reconnect (an
incarnation/connection-identity handling gap on the return) rather than presenting a separate fresh
VC. This is a CONNECTION-layer gate, upstream of CM admission.

**Non-claims / what ships (no wire change, guard 8/23).** (1) rejoin is NOT closed; this ISOLATES the
gate on the coordinator and RELOCATES the frontier to the SCS connection layer. (2) `vms-9a7`
confirms §4(O.24)'s wire fact (no relay/commit for the return op 0x02) and CORRECTS its explanation:
the coordinator does not "ignore" a received request (its CNXMAN never receives one). (3) The
READMITMAP `JOIN-ABANDONED` verdict TEXT is corrected to the grounded coordinator-SDA finding
(CNXMAN never proposes; the `VMS$VAXcluster` reconnect stays `con_sent`/`reconnect` and times out) —
log-only, classifier UNCHANGED (`joiner_cfg2_sent && cm_responses==0`), kill-switch
`OVMX_NO_READMITMAP` unchanged, fail-pre/pass-post case in `tests/vmsscs/test_scsd_wire.c`,
byte-unchanged on the wire.

**Evidence** (host, tank volume): coordinator CSB/CONN timelines
`/data/training/vax/cluster/work/9a7v2g22.csb` (fresh→crash→return, one boot) with `.status`
(`XITDONE`: fresh #1 =1, return #2 =0; op 0x02 to node 2 driven on the return); daemon logs
`/lab/k8s-labs/vaxlab-2/logs/scsd-9a7v2g22-{1,2}.log` (`#2` shows both `VMS$VAXcluster` VCs reach
OPEN then the SHUTDOWN-only `DISC SENT`, and `READMITMAP … verdict=JOIN-ABANDONED`); pcap
`d94-9a7v2g22.pcap` decoded with `docs/clean-room/tools/cmdiff.py` (fresh op02→relay→commit→MEMB at
t≈24 s; return op02 at t≈87 s with no relay/commit); harness
`/data/training/vax/cluster/tools/coordfast.sh` (coordinator SDA parked across a same-boot
fresh→crash→return). Fresh baseline cross-check on virgin `vaxlab-1`
(`9a7v-f.csb`/`d94-9a7v-f.pcap`). *VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39/7-40/7-46.

#### 4(O.26) THE MEMBER-INITIATED RECONNECT DOES REACH A STABLE OPEN — §4(O.25)'s "never settles OPEN" was a SAMPLING ARTIFACT, and the gate is one layer back UP: coordinator SDA across a fresh→crash→return single-factor bracket (virgin `vaxlab-1`, CURRENT post-9a7 build re-proven to drive op 0x02 on the return, GAP=22) shows the coordinator's member-initiated `VMS$VAXcluster` connection to the returning identity reach **`State 0002 open`, `Remote Con. ID = OUR handle`** at R+4 s (together with its `SCS$DIRECTORY` and `MSCP$DISK` connections), and OVMX's op 0x02 join IS driven on that OPEN connection — so it is NOT "never reaches a stable OPEN" (§4(O.25) sampled only the post-break `con_sent`/`03 reconnect` phase and mistook it for never-opened). The connection is then **LOST ~10 ms after OVMX drives its op 0x02 readmission config on it** (coordinator console `%CNXMAN, lost connection to system <id>` at the SAME 10 ms as OVMX's `CMCONFIG2` op 0x02): OVMX's OWN `SCS$DIRECTORY`+`MSCP$DISK` client discovery to the coordinator bloats the shared per-VC `send_seq`, so op 0x01 (params) and op 0x02 (config) ride at **`send_seq` 21/22 — PAST the coordinator's `recv_ack` ceiling of 18** on that VC; the coordinator never delivers them, `CNXMAN` never proposes the addition (no second `received VAXcluster membership request`), the VC reverts to `03 reconnect`/`Remote Con. ID 0` and times out. The `vms-9af` lean-VC suppression (§4(O.15)) DID fire but ~1.3 s TOO LATE (after the op 0x02 and the break), and the post-break reconnect's op 0x05/op 0x06 are then addressed to `Remote Con. ID 0x00000000`. So the gate **relocates from the SCS connection layer BACK to op 0x02 readmission DELIVERY on an already-OPEN member connection** — the `send_seq`-ceiling / lean-VC-timing family (§4(O.15) `vms-9af`, §4(O.17) `vms-46f`, §4(O.10) `vms-e15`), now PROVEN open-then-break with coordinator SDA rather than inferred. The next increment is to make lean-VC / credit-first engage BEFORE op 0x02 is driven on the member-initiated connection (deterministically via `OVMX_CFG2_PEER`, or by deferring op 0x02 until the coordinator's VC is lean) so op 0x02 rides UNDER the `recv_ack` ceiling; this is NOT a byte-unchanged log fix and is deferred to `vms-694`'s SCA-state build (GROUNDED, `vms-c40`, coordinator-side SDA `SHOW CLUSTER`/`SHOW CLUSTER/NODE`/`SHOW CONNECTIONS/NODE` parked on VAX2 across a same-boot fresh→crash→return on virgin `vaxlab-1` + `d94-c40repB.pcap` per-`send_seq` decode + *VAXcluster Principles* pp. 2-43/2-44/2-45/7-37/7-40/7-46, 2026-08-11). Evidence (host, tank volume): coordinator CSB/CONN timeline `/data/training/vax/cluster/work/c40repB.csb` (fresh admits `CNXMAN proposing addition`; return connection `open` at R+4 s then `lost` at the op 0x02); daemon log `/data/training/vax/k8s-labs/vaxlab-1/logs/scsd-c40repB-2.log` (`CONNRESP` answers the member VMS$VAXcluster CONNECT-REQUEST, `CMCONFIG2` drives op 0x02, `LEANVC` fires afterwards); pcap `d94-c40repB.pcap`. This section ships only the isolation, the corrected frontier and the corrected `READMITMAP` verdict text (log-only, classifier UNCHANGED, kill-switch `OVMX_NO_READMITMAP` unchanged, byte-unchanged on the wire); it is a non-admission (never `ADMITTED`; INV-6).

#### 4(O.27) THE §4(O.26) send_seq-CEILING GATE IS CLOSED — lean-VC + credit-first now engage BEFORE op 0x02, so on a live return op 0x02 rides `send_seq` 2/8/17 (≤18) and the coordinator's `recv_ack` advances to 13 PAST it (op 0x02 DELIVERED, credit op6/op7/op8/op9 exchange completes) instead of the §4(O.26) 21/22 stranding — but rejoin STILL does not re-admit (`XITDONE=0`): with op 0x02 delivered in-order under the ceiling and credit returned, `cm_responses=0` and CNXMAN prints only `lost connection` (never `received membership request`/`proposing addition`) for the RETURNING identity, so the ceiling was NECESSARY-NOT-SUFFICIENT and the frontier RELOCATES back to the §4(O.20)/§4(O.24)/§4(O.25) returning-identity non-admission — the coordinator receives a clean, delivered, credited op 0x02 and proposes NOTHING for an identity it has held before (GROUNDED, live first-join→return bracket on a freshly-booted virgin `vaxlab-0` + coordinator console + `send_seq` decode, `vms-3aba`, 2026-08-11)

**Frame.** §4(O.26) (`vms-c40`) proved the return's member-initiated `VMS$VAXcluster`
connection reaches OPEN and is then LOST ~10 ms after OVMX drives op 0x02, because
OVMX's own `SCS$DIRECTORY`+`MSCP$DISK` client discovery bloats the shared per-VC
`send_seq` so op 0x01/op 0x02 ride at `send_seq` 21/22 — PAST the coordinator's
`recv_ack` ceiling of 18 — and the `vms-9af` lean-VC suppression (§4(O.15)) engaged
~1.3 s too LATE. It named the next increment: make lean-VC / credit-first engage BEFORE
op 0x02. `vms-3aba` ships that increment and BRACKETS it live.

**Root cause of the late firing (GROUNDED, code).** `cm_peer_is_coordinator()` requires a
strictly-lower peer to already be known (its `lower_peer_seen` guard, §4(O.16) header). The
two members complete their 0x41 START in a non-deterministic order; when the coordinator
(higher node number) completes START FIRST, the predicate returns 0 — no lower peer yet — so
`cm_lean_vc_suppress_peer()` does not fire and OVMX's own-dir/MSCP client half rides the
coordinator's VC before the non-coordinator ever appears, exactly the §4(O.26) bloat.

**The fix (`src/vmsscs/scsd.c`, wire-visible, kill-switched).** On a rejoin, HOLD OVMX's
own-dir/MSCP client half to a coordinator CANDIDATE (`cm_peer_coord_pending()` — the top node
known with no lower peer yet) for a bounded settle window (`LEAN_COORD_SETTLE_MS`,
liveness-capped) via `cm_rejoin_lean_early_hold()`, added at both own-dir initiation sites
(greet timer + join_seq). The non-coordinator then appears, `cm_lean_vc_suppress_peer()`
suppresses the coordinator BEFORE op 0x02 rides, and op 0x02 lands under the ceiling. This is
the "defer until the coordinator's VC is lean" path §4(O.26)/§10.3 named, and it needs NO
`OVMX_CFG2_PEER`. Kill-switch `OVMX_REJOIN_LEAN_EARLY=0` restores the pre-fix late firing.

**The live bracket (GROUNDED, `vaxlab-0` deleted+re-cloned to a virgin CN_2 — `SHOW CLUSTER` =
VAX1/VAX2 only; one identity `OVX3A0`/1973; current-source daemon `md5=60e44b04…`,
staged+md5-verified in-pod; `OVMX_JOIN_SEQ=1`, NO `OVMX_CFG2_PEER`; first join → ~25 s
departure gap → return, hard `timeout` per op, 2026-08-11).**

| arm | scenario | LEANHOLD/CREDITFIRST fire? | op 0x02 `send_seq` (coordinator VC) | coordinator `recv_ack` reached | CNXMAN on coordinator (VAX2) | `XITDONE` |
|---|---|---|---|---|---|---|
| A | first join (no sidecar) | n/a (first join) | op 0x02 on OUR joiner VC | — | **`received … membership request` → `proposing addition` → `completing … transition`** | **1** (CN_3) |
| B | return, same id | **YES** — `LEANHOLD` (node 2) then `CREDITFIRST` op-6 then `CMCONFIG2` op 0x02 `send_msg=3`, then `LEANVC` | **ss=2, ss=8, ss=17 — all ≤18** (§4(O.26): 21/22) | **13 — PAST op 0x02 → DELIVERED** | **`lost connection` only** — never `received … request`, never `proposing addition` | **0** |

Three independent readings confirm the wire mechanism is fixed: (1) the daemon log emits
`SCSD-I-LEANHOLD` → `SCSD-I-CREDITFIRST` → `SCSD-I-CMCONFIG2 … MEMBER-initiated (rejoin) VC …
send_msg=3` → `SCSD-I-LEANVC` IN THAT ORDER (lean/credit engage before op 0x02); (2) the
`send_seq` decode of `d94-3abaB.pcap` puts op 0x02 at ss=2/8/17 and the coordinator's
cumulative `recv_ack` at 13 — so op 0x02 is DELIVERED, and the op6/op7/op8/op9 special-credit
exchange (Davis pp. 2-43/2-44/2-45) completes on the wire; (3) NONE of this happened in
§4(O.26), where op 0x02 rode 21/22 and was never delivered.

**And it is STILL not enough (GROUNDED, same bracket).** With op 0x02 delivered in-order under
the ceiling and credit returned, the coordinator returns `cm_responses=0` (both members), the
member VC ends `DISC SENT`, `vaxcluster_member=no`, and — the decisive datum — the
coordinator's own console prints, for the RETURN, ONLY `%CNXMAN, lost connection to system
OVX3A0` / `timed-out lost connection`, and NEVER `received VAXcluster membership request` or
`proposing addition of system OVX3A0` — the two lines it DID print for the FIRST join of the
SAME identity ~3 minutes earlier. So delivering op 0x02 lean, in-order, credited, and under
the ceiling does NOT by itself make the coordinator register a membership request for a
returning identity.

**The frontier relocates BACK to returning-identity non-admission.** §4(O.26)'s send_seq-ceiling
was a real intervening gate that had crept on top of the deeper one; closing it returns the
frontier to exactly where §4(O.20) (`vms-a63`) left it — "with the freeze gone and op 0x02
delivered in REJOIN form in-order to the coordinator, the member STILL declines to reciprocate
(`cm_responses=0`, no op 0x04)" — and where §4(O.24)/§4(O.25) mapped it: **the coordinator
receives a clean, delivered, credited op 0x02 readmission for an identity it has held before and
runs ZERO CM JOIN handshakes / never proposes the addition, though it admits a FRESH identity
through the identical path.** The next isolation is that CM-layer returning-identity gate, now
with the wire delivery PROVABLY clean (op 0x02 delivered under the ceiling, `recv_ack` advancing
past it, credit flowing) — a confound §4(O.20)–§4(O.26) could never fully remove.

**What `vms-3aba` ships.** (1) The `cm_rejoin_lean_early_hold` change (kill-switch
`OVMX_REJOIN_LEAN_EARLY`, helper `cm_peer_coord_pending`, `LEAN_COORD_SETTLE_MS`), at both
own-dir initiation sites, with `lean_hold_start_ms`/`lean_vc_suppressed` reset on re-START. (2) A
fail-pre/pass-post unit test (`tests/vmsscs/test_scsd_wire.c:`
`test_rejoin_lean_engages_before_op02_when_coordinator_appears_first`): in the appears-first
state (only the coordinator peer known, no `OVMX_CFG2_PEER`) op 0x02 rides `send_seq` 16 (≤18)
with the fix and 22 (>18, the grounded §4(O.26) bloated value) with `OVMX_REJOIN_LEAN_EARLY=0`;
plus liveness-release, settle→suppress handoff, and first-join-untouched cases. (3) This §4(O.27)
and the corrected `READMITMAP` `JOIN-ABANDONED` verdict text (log-only, classifier UNCHANGED —
`joiner_cfg2_sent && cm_responses==0`; kill-switch `OVMX_NO_READMITMAP` unchanged). Still a
non-admission (never `ADMITTED`; INV-6). `scs`/`vmsscs` ctest stays green (55/55).

**Non-claims.** (1) This does not claim WHY the coordinator declines to propose for a returning
identity — only that the send_seq-ceiling is no longer the reason (op 0x02 is delivered). (2) The
op-6 special-credit exchange completing on the wire is banked as progress over §4(O.26)/§4(O.18),
but whether the credit return is itself sufficient is refuted here (it completes and `XITDONE`
stays 0). (3) The op 0x02 body is unchanged (§4(O.10)); this is a connection-population / timing
change, not a payload change.

**Evidence** (host, tank volume): live bracket
`/data/training/vax/k8s-labs/vaxlab-0/logs/scsd-3abaA.log` (first join `XITDONE=1`),
`scsd-3abaB.log` (return `XITDONE=0`, `SCSD-I-LEANHOLD` node 2, `SCSD-I-CREDITFIRST` op-6 BEFORE
op 0x02, `SCSD-I-CMCONFIG2 … MEMBER-initiated (rejoin) … send_msg=3`, `cm_responses=0`,
`member_vc=DISC SENT`, `READMITMAP … JOIN-ABANDONED`), `d94-3abaA.pcap`/`d94-3abaB.pcap`
(identity `OVX3A0` on the wire; return op 0x02 at `send_seq` 2/8/17, coordinator `recv_ack`→13),
coordinator console `vax2.log` (FRESH: `received VAXcluster membership request` + `proposing
addition`; RETURN: `lost connection` only); code `src/vmsscs/scsd.c` (`cm_peer_coord_pending`,
`cm_rejoin_lean_early_hold`, `LEAN_COORD_SETTLE_MS`, `OVMX_REJOIN_LEAN_EARLY`); unit
`tests/vmsscs/test_scsd_wire.c`; decoder `tools/cluster` + `send_seq`/`recv_ack` at abs
`[34:36]`/`[32:34]`. *VAXcluster Principles* pp. 2-43/2-44/2-45/7-37/7-38/7-39/7-40.

#### 4(O.28) THE COORDINATOR HAS NO PERSISTENT "HELD-BEFORE" BLOCK — the retained state that refuses the return is the departed node's OWN per-SCSSYSTEMID CSB stuck in State `03 reconnect`→`09 wait` (`long_break`), which OVMX's rejoin-form return KEEPS ALIVE (never removed, so never re-proposed); a node with no such live CSB is admitted, and the `qf_failed_node` quorum flag is a RED HERRING (a fresh id is admitted with it set) (GROUNDED, three same-boot single-factor coordinator-SDA brackets on freshly-booted virgin `vaxlab-0`, current-source daemon `md5=2518a52a`, `vms-358`, 2026-08-11)

**Frame.** §4(O.27) left op 0x02 delivered in-order under the ceiling, credited, and STILL not re-admitting (`cm_responses=0`, CNXMAN proposes nothing) for a RETURNING identity, and posed the question this section answers: WHAT per-identity state does the coordinator retain that distinguishes RETURNING from FRESH? Three coordinator-SDA brackets (`ANALYZE/SYSTEM` parked on the coordinator VAX2, sampling `SHOW CLUSTER` CLUB flags/votes, `SHOW CLUSTER/NODE` CSB, `SHOW CONNECTIONS/NODE` CDT) settle it, and the answer CORRECTS the "returning-identity non-admission" framing the series carried since §4(O.20).

**Bracket 1 — the SCSSYSTEMID-freshness flip does NOT flip on identity (`358flip`).** One boot, coordinator SDA, three arms with the presented SCSSYSTEMID as the single varied factor: `A` FRESH join `OVX3S0`/1974 → **ADMITTED** (CNXMAN `received … request`→`proposing addition`→CSB `02060002 member,selected` CSID `00010003`); `B` FAST return SAME `OVX3S0` (GAP 8 s < RECNXINTERVAL 20) → **REFUSED**; `C` FAST return presenting a FRESH `OVX3F0`/1975 the coordinator has never held → **REFUSED** (authoritative per-run daemon `READMITMAP-SUMMARY admitted=0 reclaimed_nojoin=1 join_abandoned=1`; no `proposing … OVX3F0` on the coordinator console). **A fresh SCSSYSTEMID presented during the return window is refused too**, so the block is NOT keyed on the exact held SCSSYSTEMID. (The tag's `358flip.status` SUMMARY line reads `C=1` — a FALSE POSITIVE from `grep -ac XITDONE` counting the literal string "XITDONE STILL 0" inside the verbose READMITMAP verdict text; the daemon's own `admitted=0` is authoritative and shows `C` REFUSED. Any downstream reader must use the per-run `scsd-*.log` `READMITMAP-SUMMARY admitted=` count, never the SUMMARY's XITDONE grep.) At arm C the CLUB carried `Flags 11082001 …,qf_failed_node`, `Figure of Merit 00000102` (set when `OVX3S0` crashed in arm B) — which first looked like the gate.

**Bracket 2 — `qf_failed_node` is NOT the gate; a settled fresh join admits with it set (`cq358`).** One pristine boot, four fresh first-joins, the varied factor being how the PRIOR daemon departed: `A` join→GRACEFUL exit; `B` join (after A's clean departure) → **ADMITTED** (`admitted=2`); `C` join→SIGKILL crash; `D` join (after C's unclean crash + a 26 s settle) → **ADMITTED** (`admitted=2`). The CLUB `qf_failed_node` flag LATCHES after the first departure processes and PERSISTS set through B, C and D — yet B, C and D ALL admit. **`qf_failed_node` does not gate admission; a fresh join admits once the prior node's teardown has SETTLED, even after an unclean crash.** This refutes bracket 1's apparent `qf_failed_node` correlation, and re-reads `358flip` arm C: it was refused NOT because it was fresh-during-`qf_failed_node`, but because it arrived ~3 s after arm B's 70 s SAME-id (`OVX3S0`) reconnect-HAMMER was killed — i.e. while the coordinator's teardown/reconnect of the wedged prior node was still in flight. The SAME fresh id `OVX3F0` is REFUSED mid-churn (`358flip` C) and ADMITTED after a clean settle (`cq358` D). Davis grounds the mid-churn refusal: a join is ABANDONED if a more extensive rebuild (the prior membership's removal) is still pending (pp. 7-40/7-41).

**Bracket 3 — the returning SAME id is self-perpetuating (`sameid358`).** One id `OVX3X0`/1980, single factor = how the return is presented: `F1` first-join → **ADMITTED** → SIGKILL crash (cleanly `removed from VAXcluster` at T+3 s); `R` FAST return, sidecar present (REJOIN form) → **REFUSED**, and R's op 0x02 drive re-creates a coordinator CSB for `OVX3X0` that lands in State `03 reconnect`→`09 wait`, `Flags 02040001 long_break,status_rcvd`, CSID `0`, and STAYS there (a 35 s no-daemon settle did NOT clear it — the wedge outlives the hammer); `J` return `OVX3X0` as a FRESH FIRST-JOIN (sidecar deleted) onto that residual → **REFUSED** (`verdict=JOIN-ABANDONED`, `admitted=0`; the coordinator `received VAXcluster membership request` on VAX1 but NEVER proposed; the residual `long_break` CSB reopened to State `01 open Flags 00000000` non-member and never advanced). **No arm achieved `XITDONE 0→1` for a same-id return.**

**The retained coordinator-side record, named from SDA.** It is NOT a cluster-DB "known-system" marker and NOT the `qf_failed_node` quorum flag. It is the departed node's **own per-SCSSYSTEMID Cluster System Block held in a reconnect/wait connectivity state** — `SHOW CLUSTER/NODE=<id>` State `03 reconnect` then `09 wait`, `Flags …0001 long_break,status_rcvd`, CSID `0`, never `selected`/`member`; `SHOW CONNECTIONS/NODE=<id>` the coordinator's member-initiated `VMS$VAXcluster`/`SCS$DIRECTORY`/`MSCP$DISK` reconnects to it parked `0007 con_sent`/`0001 con_pend`, `Remote Con. ID 00000000`. The coordinator is holding this CSB open because it believes the node is *reconnecting* (RECNXINTERVAL/`long_break`, Davis pp. 7-29/7-30), and it will not PROPOSE a fresh JOIN for a SCSSYSTEMID whose CSB it is still reconnecting — and while that reconfiguration is pending it also declines other joins (bracket 1 arm C). OVMX's rejoin apparatus (`cm_rejoin_target_mode`, deferred op 0x02, the reconnect/readmit path) is what KEEPS this CSB alive: by fast-returning as the same id and driving op 0x02/readmit onto the coordinator's still-open reconnect — which can never complete, since OVMX returns as a fresh incarnation/Con.ID — OVMX perpetually re-arms the `long_break` wait so the CSB is never removed. A node the coordinator has cleanly `removed` (`F1`'s crash; `cq358`'s settled crashes) leaves NO such CSB, and a subsequent join — fresh id proven (`cq358` D), same id after a clean unhammered removal NOT yet proven (the one untested arm) — is admitted.

**Relocated frontier — OVMX-side, and it is the reconnect/readmit apparatus itself.** Davis is categorical that a rejoin has no distinct transaction: the old CSB is deallocated and a new one built "just as if it were joining the cluster for the first time" with a new CSID (pp. 7-24/7-25). OVMX instead models a rejoin FORK — it returns fast and drives a reconnect/readmit onto the coordinator's still-live CSB, which wedges it (`long_break`) rather than letting it die; and on a long-gap return OVMX's `rejoin-target-mode` waits forever for a reconnect that will not come (§9 `vms-9a7`). **Fix design:** abandon the reconnect/readmit rejoin path; make OVMX's departure DRIVE the coordinator to fully REMOVE its prior incarnation's CSB (a clean CM self-departure/last-gasp — which OVMX does NOT currently emit; it does only a per-connection SCS DISCONNECT — forces immediate removal, Davis p. 7-29), THEN drive a plain FRESH first-join (NEW→JOIN, new incarnation/Con.ID) exactly as `cq358` arm D does. The single arm that proves this end-to-end for the SAME id — clean unhammered removal, then a same-id first-join — is the next increment; it was scoped out here to avoid a speculative wire change in the crash-prone op02/START region (guard 8) before the last-gasp emit exists.

**What `vms-358` ships (no wire change).** (1) This §4(O.28) and `docs/design-rejoin-cm-state-map.md` §12. (2) A `READMITMAP` verdict-TEXT correction: `JOIN-ABANDONED` no longer says the coordinator "proposes NOTHING for an identity it has held before" (a persistent held-before block, which the brackets refute); it states the grounded §4(O.28) finding — the return collides with the departed node's own per-SCSSYSTEMID CSB held in reconnect/`wait` (`long_break`) that OVMX's rejoin-form return keeps alive, `qf_failed_node` is not the gate, and a settled fresh join admits. Classifier logic UNCHANGED (`joiner_cfg2_sent && cm_responses==0`), kill-switch `OVMX_NO_READMITMAP` unchanged, byte-unchanged on the wire (guard 8), with a fail-pre/pass-post text assertion in `tests/vmsscs/test_scsd_wire.c`. Still a non-admission (never `ADMITTED`; INV-6). The three runners (`coord358.sh`, `cq358.sh`, `sameid358.sh`) are snapshotted under `tests/lab/tools/`.

**Evidence** (host, tank volume). `358flip`: `/data/training/vax/k8s-labs/vaxlab-0/logs/scsd-358flip-{A,B,C}.log` (arm C `READMITMAP-SUMMARY admitted=0`), coordinator SDA timeline `/data/training/vax/cluster/work/358flip.csb` (arm A `member,selected` CSID `00010003`; arm B CSB `05 connect`/`con_sent Remote Con.ID 0`; CLUB `10082001`→`11082001 qf_failed_node`, FoM `0`→`00000102`), `d94-358flip.pcap`. `cq358`: `scsd-cq{A,B,C,D}.log` (B,D `admitted=2`), `cq358.csb` (`qf_failed_node` set at B/C/D SETTLE while all admit), `d94-cq358.pcap`. `sameid358`: `scsd-sid{F1,R,J}.log` (J `verdict=JOIN-ABANDONED admitted=0`), `sameid358.csb` (R→J residual CSB `03 reconnect`→`09 wait long_break`, never removed), `d94-sameid.pcap`. Daemon `md5=2518a52a` built from `b5e75f35` (=§4(O.27)); `scsd.c` byte-identical to `origin/main` HEAD at build. Code `src/vmsscs/scsd.c` (`readmit_verdict_name` JOIN-ABANDONED text); unit `tests/vmsscs/test_scsd_wire.c`. *VAXcluster Principles* pp. 7-24/7-25/7-29/7-30/7-37/7-38/7-39/7-40/7-41.

#### 4(O.29) THE FIX SHIPPED — the fresh-first-join RETURN (safe, default) advances the coordinator from proposing NOTHING (§4(O.28)) to PROPOSE+SELECT with the prior incarnation confirmed REMOVED on SDA; and the CM-layer op-0x0d "clean last-gasp" CRASHES the real VAX and is defaulted OFF (GROUNDED, live brackets on `vaxlab-0` current-source `md5=8593cf0f` and a fresh `vaxlab-1` `md5=73c18ef6`, `vms-ab1`, 2026-08-11)

**Frame.** §4(O.28) proved the rejoin refusal was OVMX's OWN rejoin-form return driving op 0x02/readmit onto the coordinator's still-open reconnect, wedging the departed node's per-SCSSYSTEMID CSB in `long_break` forever, and named the two-part fix. `vms-ab1` implements both, kill-switched, and the LIVE lab arbitrates each.

**Part 2 (SAFE, DEFAULT ON) — return as a plain FRESH FIRST-JOIN.** `ovmx_rejoin_cleanleave()` (default on; `OVMX_REJOIN_CLEANLEAVE=0` restores the legacy apparatus) neutralises the reconnect/readmit apparatus at one point each: `cm_rejoin_target_mode()` → 0 (own-outbound first-join topology) and `cm_apply_rejoin_form()` → no-op (plain first-timer op 0x02). Every rejoin sub-lever chains through those two. **Live result (GROUNDED, fresh `vaxlab-2`, current-source daemon `md5=d5a8946f`, 45 s settle):** F1 joins (`admitted=2`, coordinator CSB `02060002 member,selected`, `SCSD-I-XITDONE`), departs cleanly (`--duration` expiry → `scsd_shutdown_teardown` → SCS DISCONNECT), and coordinator SDA shows the F1 CSB move `member,selected` → `06040005 long_break,removed` across the settle — **the prior incarnation IS REMOVED** by the coordinator's own RECNXINTERVAL, with no rejoin-form return keeping it alive. This is the load-bearing win over §4(O.28): the wedge that outlived every settle is gone. **BUT the return is STILL not admitted:** rejoining as a plain fresh first-join, the return builds a NEW CSB (`879D7880`) that reaches only `01 open` non-member and the daemon verdict is `admitted=0` (`READMITMAP` `JOIN-ABANDONED` on the peer OVMX drove op 0x02 to, `NO-ENGAGE` on the other) — the members run ZERO CM JOIN for the returning identity. (A single earlier arm on the *contaminated* `vaxlab-0` did once reach `02060000 selected` with a `proposing addition` on the coordinator console; it did not reproduce on a clean pod and is NOT relied on.) So fresh-first-join removes the harmful apparatus and the prior CSB, but a further gate on the RETURN path — the §4(O.26)-class op 0x02 delivery / member engagement — persists.

**Part 1 (op-0x0d CM last-gasp) CRASHES THE VAX — DEFAULT OFF.** OVMX also gained `scsd_emit_clean_departure` / `scs_member_build_depart`: the class-0x04 op-0x0d SELF-DEPARTURE open (header-only body, no node-parameter template) on the OPEN VC at shutdown, to force IMMEDIATE removal. The live bracket on a **fresh** `vaxlab-1` proved it **CRASHES the real VAX coordinator** — VAX2 bugchecked and rebooted (`%SYSBOOT` → `OpenVMS VAX V7.3` → `%SYSINIT, waiting to form or join`) right after F1's depart-with-last-gasp, so the return then saw `admitted=0` (coordinator reforming); the Part-2-only clean shutdown NEVER crashed any VAX. This is the clean-room-warned crash class (`scs_member.h`): a class-0x04 transition-OPEN emitted OUT of its grounded `0x12→0x03→0x0d→0x0a` departure choreography drives CNXMAN into an inconsistent-state bugcheck. **`scsd_emit_clean_departure` is DEFAULT OFF (opt-in `OVMX_LASTGASP=1`), kept only as an RE probe.** The AUTHENTIC immediate-removal signal is the PORT-LEVEL last-gasp datagram (p. 7-29), whose byte form is NOT grounded in OVMX — deferred to a wire-RE increment.

**Verdict — XITDONE=0 for the returning node (full `member` not yet reached); necessary-not-sufficient, frontier RELOCATED.** Fresh-first-join (safe, default) moves the return from TOTAL non-admission (§4(O.28): coordinator proposed nothing) to PROPOSED+SELECTED, with the prior incarnation confirmed REMOVED on coordinator SDA. What still blocks full `member` commit: the second member NO-ENGAGEs the returning identity's JOIN barrier (its residual is not cleared as promptly as the coordinator's), and the only SAFE way to force immediate per-member removal — the port-level last-gasp datagram — is not yet RE'd (the CM-layer op-0x0d substitute crashes the VAX). Next increment: ground the port-level last-gasp datagram, OR a longer settle so BOTH members remove via their own RECNXINTERVAL before the return.

**What `vms-ab1` ships.** (1) This §4(O.29) and `docs/design-rejoin-cm-state-map.md` §13. (2) Part 2: `ovmx_rejoin_cleanleave()` default-on kill-switched fresh-first-join return, with a fail-pre/pass-post unit test (`test_rejoin_cleanleave_default_is_a_fresh_first_join` in `tests/vmsscs/test_scsd_wire.c`). (3) Part 1: `scsd_emit_clean_departure` / `scs_member_build_depart` **default OFF** (opt-in `OVMX_LASTGASP`), with a builder unit test (`test_build_depart_self_departure_open` in `tests/vmsscs/test_scs_member.c`) and the new send site registered in the census (`test_scsd_send_sites.py`). Guard 8/23 respected: every wire-visible change is kill-switched and bisectable.

**Evidence** (host, tank volume). Grounded clean run (`vaxlab-2`, daemon `md5=d5a8946f`, last-gasp default-off): `/data/training/vax/k8s-labs/vaxlab-2/logs/scsd-ab-J.log` (J `READMITMAP-SUMMARY admitted=0`, `JOIN-ABANDONED`/`NO-ENGAGE`; F1 `admitted=2 SCSD-I-XITDONE`), coordinator SDA `/data/training/vax/cluster/work/abrun-p2.csb` (F1 `02060002 member,selected` → settle `06040005 long_break,removed` → return fresh CSB `879D7880 01 open` non-member). op-0x0d last-gasp CRASH (daemon `md5=73c18ef6`, fresh `vaxlab-1`): `/data/training/vax/k8s-labs/vaxlab-1/logs/vax2.log` (`%SYSBOOT`/`OpenVMS VAX V7.3`/`%SYSINIT` reboot) + `abrun-clean.csb`. Code `src/vmsscs/scsd.c`, `src/vmsscs/scs_member.c`; runner `tests/lab/tools/abrun.sh`. *VAXcluster Principles* pp. 7-24/7-25/7-29.

#### 4(O.30) THE AUTHENTIC PORT-LEVEL CLEAN-LEAVE "LAST GASP" DATAGRAM — RE'd VAX-vs-VAX, byte-grounded on TWO real-VAX clean leaves; it is a MULTICAST HELLO with abs-30 `0xa0→0xb1` + the cluster nonce; OVMX now emits it (clean-room, default-on, kill-switched) REPLACING the crashing op-0x0d — the emit is safe (no VAX bugcheck) but XITDONE stays 0: the returning-identity CM-JOIN NO-ENGAGE gate is independent of the removal signal (`vms-708`, 2026-08-11)

**Frame.** Every prior increment (§4(O.20)–§4(O.29)) observed only OVMX-vs-VAX; NONE observed VAX-vs-VAX, so the AUTHENTIC clean-leave datagram — the port-level "last gasp" a real VAX emits so peers remove it without the RECNXINTERVAL reconnect wait (*VAXcluster Principles* p. 7-29) — was **un-RE'd**, and OVMX's GUESSED substitute for it (the CM-layer op-0x0d, §4(O.29)) CRASHED the real coordinator. `vms-708` recycled `vaxlab-0` to a **virgin 2-node cluster of TWO REAL VAXes** (both `MEMBER`, `CLUSTER_NODES=2`) and captured what a real VAX (VAX1) actually puts on the wire when it leaves.

**Method.** `tcpdump -i br0 'ether proto 0x6007'` on the pod's private bridge across VAX1 doing an orderly `@SYS$SYSTEM:SHUTDOWN` with the `REMOVE_NODE` option (a CLEAN leave, NOT a crash). Captured TWICE: `d94-708-leave` (VAX1 leaves, VAX2 quorum-blocks after removing it) and `d94-708-leave2` (with `SET CLUSTER/EXPECTED_VOTES=1` so VAX2 survives). Decoded with `tools/cluster/dissect_sca.py` + a byte-diff against a normal periodic multicast HELLO from the SAME node in the SAME capture.

**THE DATAGRAM (GROUNDED, 2 specimens).** VAX1's **true final frame** — zero frames follow it, VAX1 then goes silent — is a **120-byte multicast HELLO** (message-class `0x05`, dst = cluster group multicast `AB-00-04-01-01-01`, ethertype `0x6007`). It is byte-for-byte a normal multicast HELLO EXCEPT at exactly **two semantic fields**:

| abs | normal multicast HELLO | LAST GASP | meaning |
|---|---|---|---|
| **30–31** | `a0 00` (`0x00a0`) | **`b1 00` (`0x00b1`)** | the per-frame word / departure marker. `b1` sits one below the `b2/b3/b4` channel-verify family; OVMX does not claim the bit semantics, only the observed word. |
| **68–71** | `00 00 00 00` | **`ee 05 39 5b`** | connect/join nonce. Zero on every periodic multicast HELLO; the last gasp carries the shared **cluster token** (the same `ee053 95b` seen on directed HELLOs, §4(a) offset-68) — the departing node authenticates its last gasp. |

(The abs-96 4-byte field also differs — that is the live 100 ns channel timer, §4(b), expected to differ every frame; it is NOT a semantic marker.) Within each capture the `0x00b1`+nonce combination appears **EXACTLY ONCE** (`d94-708-leave` frame 4899 among 2441 VAX1 frames / 11 normal `a0` multicast HELLOs; `d94-708-leave2` frame 5015 among 2513 / 23), and the byte-diff vs a normal HELLO is **identical** across the two specimens. GROUNDED to the standard of every other HELLO field.

**Immediate removal, corroborated on the wire.** In `d94-708-leave2` the last gasp is at t=0; VAX1 emits nothing after it; VAX2 addresses VAX1 **exactly once** more (t=+3.26 s, a single in-flight `b3` probe) and then **never again** — only periodic multicast `a0` HELLOs to the group. A non-clean VC loss would instead have VAX2 sending directed reconnect HELLOs at VAX1 once/second for the full RECNXINTERVAL (~16–20 s, §4(aa)). The absence of that reconnect storm corroborates prompt removal (VAX2's console: `%CNXMAN, removed from VAXcluster system VAX1` then `completing VAXcluster state transition`). The join engagement a live VAX accepts was re-captured VAX-vs-VAX in `d94-708-formation` (`%CNXMAN, proposing addition of system VAX2` → both `now a VAXcluster member`), corroborating the documented op-02 handshake (§4(O.21)–(O.24)).

**What OVMX ships (clean-room, `vms-708`).** `scs_hello_build_lastgasp_frame()` (`src/vmsscs/scs_hello.c`) builds the datagram: a plain multicast HELLO with abs-30 set to `SCS_HELLO_PFW_LASTGASP` (`0xb1`) and the cluster nonce written at abs-68. `scsd_emit_port_lastgasp()` (`src/vmsscs/scsd.c`) emits ONE such frame to the cluster multicast as OVMX's **true final frame** at shutdown (after the SCS DISCONNECT dialogue), **DEFAULT ON**, kill switches `OVMX_NO_PORT_LASTGASP=1` (isolate this frame) / `OVMX_REJOIN_CLEANLEAVE=0` (whole pre-fix path). It **REPLACES** the crashing CM-layer op-0x0d (`scsd_emit_clean_departure`, now RE-probe-only, opt-in `OVMX_LASTGASP`). A HELLO variant is the SAFE class: best-effort CHANNEL traffic below the VC (p. 2-33), no SCS transition state — unlike the class-0x04 op-0x0d open that drove CNXMAN into an inconsistent-state bugcheck. Fail-pre/pass-post builder test in `tests/vmsscs/test_scs_hello.c` (byte-diff must be EXACTLY abs-30 + abs-68..71); send site registered in the census (`test_scsd_send_sites.py` `CHANNEL_CALLERS`). `scs`/`vmsscs` ctest 54/54.

**LIVE RESULT — the emit is SAFE, but XITDONE stays 0 (frontier RELOCATED, GROUNDED).** `abrun.sh` on `vaxlab-0` with the current-source daemon (`md5=2e75a29e`): F1 fresh first-join `admitted=2 SCSD-I-XITDONE`, then graceful depart emits `SCSD-I-LASTGASP` and — the decisive safety datum — **NO VAX bugchecks** (both members answer the return with verdicts; no `%SYSBOOT`/`INCONSTATE`/`INVEXCEPTN`), the exact contrast with §4(O.29)'s op-0x0d crash. BUT the return J is **still not admitted**: `READMITMAP-SUMMARY admitted=0 no_engage=2`, both members `NO-ENGAGE (member sent 0 CM responses, SYSAP never re-opened)`, coordinator `proposing_delta=0`, peer `incarnation=2`. Holds under a longer 95 s settle too (`admitted=0`). **So the authentic removal signal, correctly RE'd and safely emitted, is NECESSARY-NOT-SUFFICIENT.** The relocated frontier is the **returning-identity CM-JOIN NO-ENGAGE gate**: after a clean departure the members still do not run the per-member op-02/op-03 JOIN handshake for the returning SCSSYSTEMID (they carry `incarnation=2`, treating the return as a re-form rather than a fresh joiner), and this is **independent of the removal signal and of settle length**. It is deferred to a dedicated CM-JOIN-engagement increment — a member-side engagement question, not a wire-format gap, and NOT the op-0x0d crash region.

**Evidence** (host, tank volume). RE captures (virgin `vaxlab-0`, 2 real VAXes): `/data/training/vax/k8s-labs/vaxlab-0/logs/d94-708-leave.pcap` (frame 4899), `d94-708-leave2.pcap` (frame 5015), `d94-708-formation.pcap` (join engagement). Live OVMX proof (daemon `md5=2e75a29e`): `/data/training/vax/k8s-labs/vaxlab-0/logs/scsd-ab-F1.log` (`SCSD-I-LASTGASP`, F1 `admitted=2 XITDONE`), `scsd-ab-J.log` (`admitted=0 no_engage=2`), coordinator SDA `/data/training/vax/cluster/work/abrun-fix.csb`; no bugcheck in `vax1.log`/`vax2.log`. Code `src/vmsscs/scs_hello.c` (`scs_hello_build_lastgasp_frame`), `src/vmsscs/include/scs_hello.h` (`SCS_HELLO_PFW_LASTGASP`), `src/vmsscs/scsd.c` (`scsd_emit_port_lastgasp`); unit `tests/vmsscs/test_scs_hello.c`; census `tests/vmsscs/test_scsd_send_sites.py`. *VAXcluster Principles* p. 7-29.

#### 4(O.31) THE AUTHENTIC MEMBER-SIDE CM-JOIN ENGAGEMENT FOR A RETURNING NODE — RE'd from a REAL-VAX return; the member OPENS a config connection to the returner, the returner drives op 0x02, the member drives **op 0x12 RELAY (three-party connectivity, Davis p.7-39)** then op 0x03 COMMIT. OVMX's return diverges at step one: the member that admits the identical first-join NEVER re-opens its member-driven connection to the returner, so op 0x02 lands on a NON-coordinating member and the op 0x12 RELAY — present first-join, **ABSENT return** — never fires (`vms-942`, 2026-08-11)

**Frame.** §4(O.30) shipped the authentic clean-leave removal (safe) and left the frontier at "after a clean departure the members still run ZERO CM responses for a returning SCSSYSTEMID" — a member-side ENGAGEMENT question. Every prior increment observed the return only from OVMX's side; NONE had decoded, at the CM-op level, how a REAL VAX member ENGAGES a returning REAL node. `vms-942` RE's that authentic reference and diffs OVMX's return against it on a fresh virgin pod with the current-HEAD daemon.

**THE AUTHENTIC REFERENCE (GROUNDED, real-VAX capture).** `cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` — VAX3 (`08:00:2b:11:22:33`) returns to a cluster of VAX1 (`aa:00:04:00:01:04`) + VAX2 (`08:00:2b:78:56:b9`); VAX3 IS re-admitted. Decoded per-member (`docs/clean-room/tools/cmdiff.py`), the readmission is **member-DRIVEN and three-party**, run INDEPENDENTLY by each member and then reconciled between them:

| t (s) | frame | meaning |
|---|---|---|
| 111.182 | **VAX2m → VAX3r** MODEL+PARAMS (mt 0x5b) | the MEMBER **opens** its own `VMS$VAXcluster` config connection TO the returner |
| 111.183 | VAX3r → VAX2m MODEL+PARAMS | the returner answers as TARGET (it originates no outbound connect) |
| 111.595 | **VAX3r → VAX2m op 0x02-CONFIG** | the returner sends its join/add-member request ON the member-opened connection |
| 111.596 | **VAX2m → VAX1 op 0x12-RELAY** | the member RELAYS the join to the OTHER member — the three-party Rule of Total Connectivity (p.7-39) |
| 111.597 | VAX1 → VAX2m op 0x12-RELAY.r | the other member confirms connectivity with the returner |
| 111.598 | **VAX2m → VAX3r op 0x03-COMMIT** | ONLY NOW does the member COMMIT the returner (Phase 2, p.7-41/7-42) |
| 111.599 | VAX2m → VAX3r op 0x05-LOCKRB burst, then op 0x06-MEMB burst | lock/resource-DB rebuild + membership burst → VAX3 is `member` |

The op 0x12 RELAY sits **between op 0x02 and op 0x03** and is the commit gate: the member does NOT commit the returner until it has relayed the join to the other member and heard back. VAX1 runs the symmetric sequence toward VAX3 in the same window (§4.1 of `docs/design-rejoin-cm-state-map.md`, f1200–1263); the two established members then reconcile the new membership between themselves (op 0x0c) **including** VAX3. Per Davis pp.7-24/7-25 a rejoin has NO distinct CM transaction — the old CSB is deallocated and a fresh one built "just as if it were joining for the first time," so this is the FIRST-JOIN engagement, and it is the authentic reference regardless of whether the prior incarnation left by crash or by clean last-gasp (§4(O.30)).

**OVMX'S RETURN DIVERGES — same-capture F1-vs-J bracket (GROUNDED, fresh virgin `vaxlab-0`, current-HEAD daemon `md5=d782c6943123f3a34d2d0270090c10d3`).** `abrun.sh` (identity `OVXAB0`): F1 fresh first-join → clean last-gasp depart → 32 s settle → J return the SAME identity, tcpdump on `br0` + coordinator SDA on VAX2. The two arms differ only in first-join-vs-return:

| step | F1 first-join (`XITDONE=1`, `admitted=2`) | J return (`XITDONE=0`, `admitted=0`) |
|---|---|---|
| OVMX op 0x02 target (daemon + wire) | **node 2** (`08:00:2b:8e:b7:85`), t=26.005 | **node 1** (`aa:00:04:00:01:04`), t=117.861 |
| receiving member → other member op 0x12-RELAY | **present** (node2 → VAX1, t=26.005) | **ABSENT** |
| receiving member → OVMX op 0x03-COMMIT | **present** (node2 → OVMX, t=26.006) | **ABSENT** |
| that member's own READMITMAP verdict | admitted | node1 = `JOIN-ABANDONED` (op 0x02 driven, 0 CM responses) |
| the OTHER member (the F1 admitter) on the return | — | node2 = `NO-ENGAGE`, **`SYSAP never re-opened`**, `member_vc=DISC ACK` |
| coordinator console | `%CNXMAN, proposing addition of system OVXAB0` | `proposing_delta=0` (no new proposal for the return) |

**The isolated gap.** OVMX's op 0x02 body is byte-identical across F1 and J (all-zeros after the `01 02 … 00 02` header), so the op 0x02 CONTENT is NOT the discriminator — the member accepts OVMX's lean op 0x02 on the first join. The discriminator is **which member receives op 0x02, and whether that member re-opened its member-driven connection to the returner.** On F1, op 0x02 reaches node 2, which OPENED a `VMS$VAXcluster` config connection to OVMX, so it relays (op 0x12) and commits (op 0x03) — exactly the authentic sequence. On the return, the member that admitted the identical first-join minutes earlier (node 2, the op 0x12-RELAY driver) does **NOT** re-open its member-driven connection to the returning OVMX (`SYSAP never re-opened`), so `cm_pick_coordinator` sees only node 1, OVMX's op 0x02 lands on node 1 — a member not driving the transition — and no op 0x12 RELAY / op 0x03 COMMIT follows. **The wire discriminator is the op 0x12 RELAY: present between op 0x02 and op 0x03 on the first-join, absent on the return.**

**Why no wire change ships here (discipline).** The gate is a MEMBER decision — the coordinating member choosing to open its `VMS$VAXcluster` connection to the returner — which OVMX cannot force by a routing or op 0x02-body change: `cm_pick_coordinator`'s own grounding (vms-2f3 run s3C) already forced op 0x02 to the SDA-named coordinator on a refused identity and it was **refused identically**, and forcing op 0x02 to a peer whose config never completed sends nothing. A speculative control frame toward a live VAX in this region is the RE'd crash class (§4(O.29) op-0x0d bugcheck; `cm_response_shape`'s two-VAX-crash precedent). This increment ships the RE'd reference, the isolated gap, and the fix design; the wire change awaits the next isolation.

**Relocated frontier + fix design.** The frontier is now pinned to a SPECIFIC missing step with a SPECIFIC wire discriminator: **the coordinating member does not (re)open its member-driven `VMS$VAXcluster` connection to the returner, so the returner's op 0x02 never reaches a member that will RELAY it.** Per the authentic reference the member OPENS this connection (member-driven, TARGET on the returner), and per Davis pp.7-24/7-25 the member should treat the returner as a fresh joiner (deallocate old CSB → build new → open connection → JOIN). §4(O.30) proved the *coordinator* removes the prior CSB; this shows a member does not then *re-open toward* the returner. The next isolation instruments, PER MEMBER on the return: (a) whether that member issues a `VMS$VAXcluster` CONNECT-REQ to the returner at all, and if so what OVMX answers that fails to reach OPEN (vs the first-join answer that succeeds); (b) whether a longer/quieter settle lets the member fully deallocate+rebuild the returner's CSB before the return; (c) whether OVMX must present, on the SURVIVING member connection, the signal that makes the coordinating member re-discover the returner as a fresh node. One of those names the member-side trigger; it is a member-side connection question, not an op 0x02-payload gap.

**Evidence** (host, tank volume). Authentic reference: `/data/training/vax/cluster/captures/vax3-class03-crash-REJOIN-SUCCESS-20260801.pcap` (VAX3 readmission t≈110–112 s), decoder `docs/clean-room/tools/cmdiff.py`. OVMX bracket (fresh virgin `vaxlab-0`, daemon `md5=d782c6943123f3a34d2d0270090c10d3` built from HEAD): `/data/training/vax/cluster/work/942-return-vs-firstjoin.pcap` (F1 op 0x02→node2→op 0x12-RELAY→op 0x03-COMMIT at t≈26 s; J op 0x02→node1, no relay/commit at t≈118 s), `942-F1.log` (`admitted=2 XITDONE`), `942-J.log` (`admitted=0`, node1 `JOIN-ABANDONED`, node2 `NO-ENGAGE SYSAP-never-re-opened`), `942-abrun.status`. Design record `docs/design-rejoin-cm-state-map.md` §15. Code: verdict text corrected in `src/vmsscs/scsd.c` (`readmit_verdict_name`, `READMIT_JOIN_ABANDONED`); unit `tests/vmsscs/test_scsd_wire.c`. *VAXcluster Principles* pp.7-24/7-25/7-29/7-33/7-34/7-37/7-38/7-39/7-41/7-42.

#### 4(O.32) THE MEMBER DOES ISSUE ITS `VMS$VAXcluster` CONNECT AND OVMX ANSWERS IT TO OPEN ON A RETURN — §4(O.31)'s "SYSAP never re-opened" and §4(O.25)'s "never proposes" are BOTH REFUTED as OVMX-daemon-classifier / sampling artifacts: real-VAX member SDA `SHOW CONNECTIONS/NODE` shows the returner's `VMS$VAXcluster` CDT reach **State 0002 open** on BOTH members with a valid Remote Con.ID and hundreds of messages exchanged, and BOTH members' CNXMAN prints `received VAXcluster membership request` → `proposing addition` — so the connection layer is NOT the blocker and rejoin is OVMX-FORCEABLE (verdict A); the gate relocates one layer forward to the CM state transition, which the coordinator **ABORTS for a returning identity** (cat 0x01 op 0x04, role 0x50 — OVMX's own `SCSD-E-XITABORT` reads it "a DECISION, not a timeout … we are being refused") where the first-join sends cat 0x04 op 0x00 → op 0x06 MEMB → completes (GROUNDED, `vms-694`/run `o32`, single-factor first-join-vs-return bracket with member-side SDA `SHOW CONNECTIONS/NODE=OVXAB0` on BOTH real VAX members + OVMX daemon connection-FSM + tcpdump on virgin `vaxlab-2`, current-HEAD daemon `md5=d782c6943123f3a34d2d0270090c10d3`, 2026-08-11)

**Frame.** §4(O.31) isolated OVMX's return-divergence to a member-side connection question and left the decisive next isolation explicit (its non-claim (a)): PER MEMBER on the return, *does that member issue a `VMS$VAXcluster` CONNECT-REQ to the returner at all, and if so what does OVMX answer that fails to reach OPEN?* — with the A/B fork that answers whether rejoin is OVMX-forceable. §4(O.31) read the answer from OVMX's OWN daemon classifier (`READMITMAP`: node2 `NO-ENGAGE / SYSAP-never-re-opened`). Nobody had yet read the **member's** connection-manager state — the CDT/SYSAP layer, `SHOW CONNECTIONS/NODE=` — through a return; §4(O.25)/§4(O.26) sampled only `SHOW CLUSTER/NODE=` (the CSB, which cannot separate "member opened its own config connection" from "the VC is up because OVMX opened it"). This runs that read, single-factor.

**THE SINGLE-FACTOR BRACKET (GROUNDED, virgin `vaxlab-2`, current-HEAD daemon).** Identity `OVXAB0`, fresh sidecar-less store. F1 = fresh first-join (30 s) → clean-leave depart → 35 s settle (prior CSB fully removed, confirmed on SDA) → J = return the SAME identity. The ONLY difference between arms is first-join-vs-return. Both real VAX members (VAX1 `aa:00:04:00:01:04`, VAX2 `08:00:2b:8e:b7:85`) were parked in `ANALYZE/SYSTEM` firing `SHOW CONNECTIONS/NODE=OVXAB0` throughout; tcpdump on `br0`.

| what the MEMBER's SDA/console shows for `OVXAB0` | F1 first-join (`XITDONE=1`, `admitted=2`) | J return (`XITDONE=0`) |
|---|---|---|
| `VMS$VAXcluster` CDT toward the returner | **State 0002 open**, Rej/Disconn 0 | **State 0002 open**, Rej/Disconn 0 |
| that CDT's Remote Con. ID (peer conid assigned ⇒ established) | non-zero | **non-zero** (VAX2 `04FF0002`, VAX1 `04FF0012`) |
| messages on that CDT (Sent / Rcvd) | active | **active** (VAX2 306→309 / 270→280) |
| CNXMAN on the member | `received … request` → `proposing addition` → **`completing`/`completed` state transition** | `received … request` → **`proposing addition`** … then **NO completed ADD** |
| CM transition outcome (OVMX daemon `CMIN`) | member sends cat 0x04 op 0x00 → **op 0x06 MEMB** burst → committed | member sends **cat 0x01 op 0x04 ABORT** (role 0x50) → `SCSD-E-XITABORT` |
| ~50 s after propose | member (`OVXAB0`) is a cluster member | `lost connection` → `removed from VAXcluster system OVXAB0` → reconfig completes WITHOUT it |

**The verdict is A, and it is stronger than the A/B fork anticipated.** Hypothesis B (the member never issues the connect / the SYSAP never re-opens) is **REFUTED on the authoritative real-VAX oracle**: on the return each member holds a `VMS$VAXcluster` CDT toward the returner in **State 0002 open** with the peer's Con.ID assigned and hundreds of messages flowing, and each member's CNXMAN **proposes the addition**. OVMX's connection-FSM confirms it drives that connection to OPEN (`conid=0x04FF0002 remote=0x2987000C VMS$VAXcluster: CONNECT ACK --RCV_ACCEPT_REQ--> OPEN action=send ACCEPT_RSP notify=SYSAP-accepted` — OVMX's local `0x04FF0002` = the member's Remote Con. ID, OVMX's remote `0x2987000C` = the member's Local Con. ID, so the two oracles are the same connection). **§4(O.31)'s `SYSAP-never-re-opened` and §4(O.25)'s `never proposes` were artifacts of the OVMX daemon's own classifier and of `SHOW CLUSTER/NODE` sampling — the member DOES engage the returner and DOES propose.** So the connection layer is not the gate, and rejoin is OVMX-forceable.

**The gate relocates one layer forward — to the CM state transition, which the coordinator ABORTS for a returning identity.** After the `VMS$VAXcluster` connection is open, the member drives its ADD transition (op 0x03 COMMIT txn 0x0009, op 0x05 LOCKRB burst) exactly as on the first-join, then sends **cat 0x01 op 0x04, role 0x50**, which OVMX classifies precisely: *"node 2 ABORTED the class-0x02 state transition … a DECISION, not a timeout — the coordinator sends it instead of the op 0x09 transition-open, on state it already holds. We are being refused."* On F1 the same point is cat 0x04 op 0x00 → op 0x06 MEMB → `completed VAXcluster state transition`. Because the arms differ only in first-join-vs-return, the abort is a property of the RETURN, on state the coordinator holds about the returning identity — this is the returning-identity non-admission the chain has circled since §4(O.20)/§4(O.28), now proven to sit ABOVE (not at) an OPEN member connection and an issued proposal.

**Two concrete OVMX-side anomalies are visible on the return (candidates for the fix), but neither is yet the proven cause of the abort.** (1) OVMX **defers its op 0x02 config far too long**: `SCSD-W-STRAYACK` records the member acking a frame that arrived **6892 ms** earlier, and OVMX drives its DEFERRED op 0x02 at t+7 s — *after* the abort — so the §4(O.27) lean-VC/credit-first deferral now lands past the member's transition window; **but the abort PRECEDES the deferred op 0x02 in this run, so late op 0x02 does not cause this abort** (it is a separate defect to fix, not the gate). (2) The member's **`VMS$DISK_CL_DRVR` → `MSCP$DISK`** connect stays in **State 0007 con_sent, Remote Con. ID 0** on both members, while OVMX's FSM loops `MSCP$DISK->VMS$DISK_CL_DRVR: CLOSED --RCV_REJECT_RSP--> CLOSED ILLEGAL-EVENT-FOR-THIS-STATE` every ~3 s — but MSCP is a layered SYSAP and F1 committed membership ~1 s in, *before* any MSCP connect, so this is a symptom of the pending (never-committed) transition, not the CM gate. The next isolation (§4(O.33)) targets the coordinator's op-0x04 ABORT decision directly: what state it holds for a returning `SCSSYSTEMID` at `proposing addition` that makes it send op 0x04 (refuse) instead of op 0x09 (transition-open), and whether OVMX can change that state on the SURVIVING member connection (the §4(O.28)/§4(O.29) clean-removal → fresh-first-join lineage) — it is a coordinator CM-decision question, not a connection or op 0x02-payload gap.

**No wire change ships here (discipline).** This increment ships only the isolation, the corrected frontier, and the corrected reading of §4(O.31)/§4(O.25); it is pure observation (member SDA + OVMX daemon FSM + tcpdump), no speculative frame toward a live VAX (the RE'd crash class, §4(O.29)). The observation aid used — the SCS connection-FSM trace already in `scsd.c` — is unchanged and default-on log-only.

**Evidence** (host, tank volume). Single-factor bracket on virgin `vaxlab-2`, daemon `md5=d782c6943123f3a34d2d0270090c10d3` (== `origin/main` `src/vmsscs` HEAD, commit `2d82d3f8`/§4(O.31)): member-side SDA timelines `/data/training/vax/cluster/work/o32.vax2.conn` and `o32.vax1.conn` (`VMS$VAXcluster` State 0002 open + `VMS$DISK_CL_DRVR`→`MSCP$DISK` con_sent, per-arm), run log `o32.status` (F1 `admitted=2 XITDONE`; J `XITDONE=0`), OVMX daemon `/data/training/vax/k8s-labs/vaxlab-2/logs/scsd-o32-F1.log` (op 0x06 MEMB → committed) and `scsd-o32-J.log` (`SCSD-E-XITABORT` cat 0x01 op 0x04 role 0x50; `SCSD-W-STRAYACK` 6892 ms; `MSCP$DISK … CLOSED --RCV_REJECT_RSP--> CLOSED ILLEGAL-EVENT`), pcap `o32.pcap`; harness `o32run.sh`. *VAXcluster Principles* pp. 7-23/7-24/7-25/7-37/7-38/7-39/7-40/7-41/7-42/7-46.

#### 4(O.33) THE op-0x04 ABORT IS NOT THE DISCRIMINATOR AND THERE IS NO STABLE RETURNING-IDENTITY "REFUSAL DECISION" TO ISOLATE — read against the AUTHORITATIVE membership oracle (each member's `F$GETSYI("CLUSTER_NODES")`, not OVMX's derived verdicts), a returning `SCSSYSTEMID` reaches full membership (`CLUSTER_NODES` 2→3, coordinator "completed VAXcluster state transition", FRESH CSID `00010004`) on some attempts and stalls at `CLUSTER_NODES=2` on others; the `cat 0x01 op 0x04 role 0x50` §4(O.32) read as "a DECISION … we are being refused" is present in the SUCCESSFUL fresh first-join too (`CM-XITABORT-RECEIVED`≥1 with the VAX at `CLUSTER_NODES=3`) and ABSENT (`=0`) from the reproducible Debug-daemon FAILURE, so it is a normal per-member barrier message, not a returning-identity refusal (same classifier-artifact class §4(O.32) flagged for §4(O.31)/§4(O.25), one layer deeper); the real governing fact is a TIMING-sensitive race against the departed incarnation's residual per-`SCSSYSTEMID` CSB, which — §4(O.30)'s "fully removes" REFUTED — survives the clean-leave (DCL `SHOW CLUSTER` STATUS `BRK_NON` on BOTH members, SDA `long_break,removed` with retained CSID, for >100 s ≫ `RECNXINTERVAL=20 s`), forcing the coordinating member to deallocate+rebuild that CSB (Davis pp.7-24/7-25) before it will run the CM JOIN, a reclaim that races OVMX's re-engagement: reliably LOST with a slower (Debug) daemon (`CLUSTER_NODES` stays 2, coordinating member `NO-ENGAGE`) and sometimes WON with a faster (Release) one (`CLUSTER_NODES` 2→3, `admitted=2`) — so XITDONE 0→1 is ACHIEVABLE on current HEAD but NOT reliable, and no clean non-crash-prone OVMX-side fix is available (removal-completion via the CM op-0x0d self-departure crashes the VAX, §4(O.29)) (GROUNDED, `vms-694`/run `o33`, first-join-vs-return single-factor bracket with the VAX membership oracle on virgin `vaxlab-2`, current-HEAD daemons `md5=f059ce0c`(Release) & `md5=17c25b26`(Debug), both built from `origin/main` `src/vmsscs` HEAD `2d82d3f8`, 2026-08-11)

**Frame.** §4(O.32) closed by naming the next isolation (its own words): read the coordinator's CM decision path THROUGH the op-0x04 abort, and it flagged that OVMX's own classifier/`SHOW CLUSTER` sampling MISLEAD. §4(O.33) takes that seriously and reads the one oracle OVMX cannot colour: each real VAX member's connection-manager member count, `F$GETSYI("CLUSTER_NODES")` — which counts local CM members and does NOT query the joining node — polled at ~3 s cadence across the whole connected window, on a virgin single-factor bracket (first-join of a FRESH `SCSSYSTEMID` vs return of one previously admitted, same pod/daemon/sysgen). It adds one axis §4(O.32) did not: the SAME source (`2d82d3f8`) built Release (`-O2`, `md5=f059ce0c`, 388 KB) vs Debug (`-O0 -g`, `md5=17c25b26`, 732 KB), to test whether the outcome is a stable CM decision or a timing artifact.

| observation (VAX oracle) | fresh first-join (`OVXAD1`/1991, no residual) | return (`OVXAC1`/1986, residual `BRK_NON`) |
|---|---|---|
| `CLUSTER_NODES`, Debug daemon | 2→3, held (8 samples) | **stays 2** (11 samples) |
| `CLUSTER_NODES`, Release daemon | 2→3, held (16 samples) | 2→3 held (16 samples) OR stays 2 — run-to-run |
| coordinating member (VAX2) READMITMAP | `ADMITTED` (`cm_responses=11`, JOIN OPEN) | `NO-ENGAGE` (0 CM responses) on the lost-race runs |
| `CM-XITABORT-RECEIVED` | **≥1 even when `CLUSTER_NODES` reaches 3** | `0` on the Debug non-engage; `≥1` on some Release runs |
| membership outcome | admitted (deterministic) | **nondeterministic** |

**Finding 1 — §4(O.30)'s "coordinator fully REMOVES the prior incarnation" is REFUTED by direct observation.** After a clean-leave with the port-level last-gasp emitted (`SCSD-I-LASTGASP`, `PEER-DEPARTURES=0 CONNECTIONS-LOST=0`), the departed `SCSSYSTEMID` persists in DCL `SHOW CLUSTER` as STATUS `BRK_NON` on BOTH members, and in SDA `SHOW CLUSTER` (CSB dump) as `long_break,removed` with its CSID retained, for >100 s — far beyond `RECNXINTERVAL=20 s`. The §4(O.30) multicast-HELLO last gasp is SAFE (no bugcheck, confirmed) but it does NOT deallocate the members' CSB; the "removal now works" claim was read from a narrower SDA slice, not the members' own membership view.

**Finding 2 — the op-0x04 abort is NOT the admit/non-admit discriminator.** `CM-XITABORT-RECEIVED`≥1 appears in the SUCCESSFUL fresh first-join baseline (Release: `admitted=2`, VAX at `CLUSTER_NODES=3`) and in successful returns; the cleanly REPRODUCIBLE failure (Debug return) carries `CM-XITABORT-RECEIVED=0` and fails anyway (pure member `NO-ENGAGE`, no abort at all). So `cat 0x01 op 0x04 role 0x50` does not separate success from failure — §4(O.32)'s `SCSD-E-XITABORT` "we are being refused" was OVMX's daemon classifier over-reading one sampled barrier message, exactly the artifact class §4(O.32) itself caught in §4(O.31)/§4(O.25).

**Finding 3 — the discriminator is the residual CSB → member-reclaim RACE.** The coordinating member (VAX2, `08:00:2b:1e:85:61`) that runs the CM JOIN for a FRESH identity (`ADMITTED`, `cm_responses=11`, membership OPEN → `CLUSTER_NODES=3`) does NOT engage a RETURNING identity on the lost-race runs (`NO-ENGAGE`, 0 CM responses, SYSAP not re-opened). Per Davis pp.7-24/7-25 a rejoin has no distinct transaction: the member must deallocate the old CSB and build a fresh one "just as if joining for the first time." That reclaim of the still-present `BRK_NON` CSB races OVMX's re-engagement — reliably LOST under the slower Debug daemon (`CLUSTER_NODES` never leaves 2) and sometimes WON under the faster Release daemon (`CLUSTER_NODES` 2→3, fresh CSID assigned). A fresh `SCSSYSTEMID` has no CSB to reclaim and is admitted every time. This is the §4(O.20)/§4(O.28) returning-identity non-admission, now grounded to the SPECIFIC surviving structure (the `BRK_NON` CSB) and shown to be a race, not a fixed refusal.

**Finding 4 — XITDONE 0→1 IS achieved (oracle-confirmed), just not reliably.** On faster-daemon return runs the VAX drives `CLUSTER_NODES` 2→3 and holds it (16 consecutive samples), the coordinator's console prints `proposing addition of system OVXAC1` → `completed VAXcluster state transition`, and SDA shows the returner assigned a FRESH CSID `00010004`. The thread's objective — a returning identity admitted, membership committed — is reachable on current `main`; the residual is RELIABILITY.

**Verdict.** There is no stable "coordinator refuses a returning identity" decision to isolate; the frontier relocates from "why does the coordinator abort" to "win or remove the residual-CSB reclaim race." Two fix candidates, NEITHER cleanly shippable now: (a) make the members FULLY deallocate the departed CSB on clean-leave so every return is an unambiguous fresh first-join — but §4(O.30)'s port-level last gasp does not achieve deallocation and the CM op-0x0d self-departure that would crashes the VAX (§4(O.29), default OFF) — CRASH-PRONE; (b) OVMX-side, re-drive the plain first-join JOIN (bounded retry) until the coordinating member engages / `CLUSTER_NODES` increments, converting a lost race into an eventual win — reversible, no wire guess, but NOT validatable to a deterministic pass given the nondeterminism (the failure reproduces only under the slow build). Per discipline (no speculative wire change; ship only a cleanly-isolated, validatable fix) this increment ships the isolation + this proposal ONLY; **no `src/vmsscs` change**.

**Explicit non-claims.** (1) This does NOT re-open the op 0x02 body — under the §4(O.29) clean-leave default F1 and J present the identical first-join op 0x02 (verified: same wire form), so the flip is not payload. (2) It does NOT claim the op-0x04 abort never occurs — §4(O.32)'s abort is a real sampled variant; the claim is narrower: the op-0x04 is not the discriminator between admit and non-admit. (3) The Release-vs-Debug axis is a PROBE of timing-sensitivity, not a claim that one build is "correct" — both are the same source `2d82d3f8`; the axis demonstrates the outcome sits on a timing knife-edge, which is why §4(O.32) sampled an abort and §4(O.33)'s reproducible slow-build run samples a pure non-engagement.

**Evidence** (host, tank volume). Single-factor bracket on virgin `vaxlab-2` (disks re-cloned from `.3node-golden.bak`, `CLUSTER_NODES=2` confirmed pre-run), daemons built from `origin/main` `src/vmsscs` HEAD `2d82d3f8`: Release `md5=f059ce0c776361266ddf62652ecc6846`, Debug `md5=17c25b261f22d94237705bd3f078ae4e`. VAX membership-oracle poll logs in `/data/training/vax/k8s-labs/vaxlab-2/logs/vax1.log` (`CNP_*`=fresh baseline 2→3; `CNR_*`=return-success 2→3; `FCN_*`=return-fail stays 2; Debug `ZCN_*`: `DBGbase` 2→3, `DBGret` stays 2). OVMX daemon logs `scsd-baseline.log`/`scsd-return.log` (`admitted=2`), `scsd-final.log` & `scsd-DBGret.log` (`admitted=0`, coordinating member `NO-ENGAGE`, other `JOIN-ABANDONED`, `CM-XITABORT-RECEIVED=0`). `BRK_NON` persistence (>100 s, both members) and coordinator `completed VAXcluster state transition` + fresh CSID `00010004` captured in `vax1.log`/`vax2.log` (OPCOM `REPLY/ENABLE=(CLUSTER,CONFIG)`). Harness `tests/lab/tools/o33run.sh`. *VAXcluster Principles* pp. 7-24/7-25/7-37/7-38/7-39.


#### 4(O.34) THE CLEAN-LEAVE "FULL CSB DEALLOCATION" TRIGGER DOES NOT EXIST — a REAL VAX leaving as cleanly as possible (`@SYS$SYSTEM:SHUTDOWN` with `REMOVE_NODE`, quorum adjusted) leaves the surviving member's CSB for it in the IDENTICAL lingering `long_break,removed` state OVMX produces (same CSB address, same retained CSID `00010002`, State `09 wait`), unchanged for the whole 220 s watch window (≫ `RECNXINTERVAL=20 s`); the departed CSB is NOT deallocated on the leave — it is RETAINED (Davis p.7-25: a departed system's CSB is held on a queue headed in the CLUB, only its CSV entry released) and is deallocated + rebuilt with a FRESH CSID ONLY at the RETURN (directly observed: on VAX2's reboot the survivor dropped CSID `00010002` and readmitted VAX2 as CSID `00010003`, State `01 open`, member). So §4(O.33)'s residual-CSB race is INHERENT to the documented VMS design (deallocate-at-rejoin) on BOTH the real VAX and OVMX; the operator's deterministic-fix premise ("make members fully deallocate on clean leave so a return is a fresh first-join with no stale block") corresponds to NO real-VAX behaviour and has NO wire trigger to reproduce — the authentic deallocation trigger is the rejoin itself, so the deterministic path is a return-reclaim-engagement fix (§4(O.31)/(O.32)/(O.33)), NOT a leave-time deallocation. No `src/vmsscs` change (GROUNDED, `vms-694`/runs `o34`+`o34ret`, VAX-vs-VAX single-factor bracket on virgin `vaxlab-2`, NO OVMX daemon in the loop, 2026-08-11).

**Frame.** §4(O.33) relocated the frontier to "win or remove the residual-CSB reclaim race" and the operator ruled the gate must be closed DETERMINISTICALLY, via one specific mechanism: make the surviving members FULLY DEALLOCATE a departed node's CSB after a clean leave, so a return is a plain fresh first-join with no stale block to race. That mechanism rests on an un-measured premise — that a REAL VAX fully deallocates the departed CSB on a clean leave and OVMX fails to. Every prior increment measured OVMX DEPARTING (§4(O.33): OVMX's CSB lingers `long_break,removed` >100 s on the real members). NONE measured a REAL VAX departing cleanly with a long survivor-side CSB poll, and NONE measured what actually deallocates a departed CSB. §4(O.34) measures both, VAX-vs-VAX, with no OVMX daemon in the loop.

**Method.** Virgin `vaxlab-2` (disks re-cloned from `.3node-golden.bak`, both nodes `CLUSTER_NODES=2`, golden preserved). Departing node = VAX2 (`VOTES=0`, `EXPECTED_VOTES=1` on the survivor, ground-truthed live: `EV_1_V_0` on VAX2, `EV_1_V_1` on VAX1 — so a VAX2 departure never moves quorum and VAX1 survives without any `SET CLUSTER` change). Survivor VAX1 parked in `ANALYZE/SYSTEM`, sampling the CSB oracle `SHOW CLUSTER/NODE=VAX2` (narrow, per csbcycle.sh, to survive the OPCOM flood) every ~6 s across the leave and for 220 s after. **Run `o34`** (leave): VAX2 does `@SYS$SYSTEM:SHUTDOWN` with the `REMOVE_NODE` option (the fullest clean leave — the authentic port last-gasp of §4(O.30) is emitted and quorum is adjusted), NOT a kill. **Run `o34ret`** (return): VAX2 is rebooted to the SAME root/identity and the survivor's CSB is watched for the deallocation + readmission. `tcpdump -i br0 'ether proto 0x6007'` throughout.

**Finding 1 — a clean leave does NOT deallocate the survivor's CSB; it lingers, identical to OVMX (`o34`).** Single narrow-sample timeline for VAX2 on VAX1:

| when | VAX2 CSB on the survivor (SDA `SHOW CLUSTER/NODE=VAX2`) |
|---|---|
| PRE (VAX2 up) | CSB `87935140`, CSID `00010002`, **State `01 open`**, `member,selected,status_rcvd,vcc` |
| SD+21 s (mid-shutdown) | CSB `87935140`, CSID `00010002`, State `01 open`, `member` |
| W+0 s (leave complete, VAX2 halted) | CSB `87935140`, CSID `00010002`, **State `09 wait`**, **`long_break,removed,gone`**, CDT `00000000` |
| W+30 s … W+220 s (END) | **UNCHANGED** — CSB `87935140`, CSID `00010002`, State `09 wait`, `long_break,removed`; full CSB list still carries VAX2; cluster `Nodes 1`; **`Index of next CSID` still `0003`** (no CSV entry recycled) |

The departed CSB stays ALLOCATED at the SAME address with the SAME CSID for the whole 220 s window (≫ `RECNXINTERVAL=20 s`). This is byte-for-byte the `long_break,removed`-with-retained-CSID state §4(O.33) reported for OVMX's clean departure. **The real VAX does not deallocate on the leave any more than OVMX does.**

**Finding 2 — the deallocation is REJOIN-triggered, and it is what assigns a fresh CSID (`o34ret`).** On VAX2's reboot, the survivor's CSB for VAX2 flipped in one sample: `long_break,removed` CSID `00010002` (State `09 wait`) → **`member` CSID `00010003` (State `01 open`)**. The old CSB (the retained incarnation) was deallocated and a NEW CSB built with the sequence-incremented CSID — exactly the documented rejoin path. The returning node's OWN console corroborates the fresh CSID (`Node VAX2 (csid 00010003) is now a VAXcluster member`). There is no leave-time timer, follow-up datagram, or last-gasp variant that does this; the RETURN does it.

**Grounding (Davis p.7-25, cited by page only).** The documented VMS mechanism matches the observation exactly: when a system leaves the cluster its CSB is RETAINED in a queue (headed in the CLUB) and only its CSV entry is released; **if the system later rejoins, its old CSB is deallocated and a new CSB is created for it "just as if it were joining the cluster for the first time,"** taking a new CSV entry and therefore a new CSID. Deallocation is a property of the REJOIN, by design — the CSB is deliberately retained across the absence so the CSID sequence number can be advanced on return. The ten CSB connectivity states (pp. 7-23/7-24) include no deallocate-on-departure transition; `wait`/`long_break` is the retained-with-lost-connectivity state, and the `NEW` state is documented to cover a returning system's freshly re-allocated CSB.

**Verdict — the premise is REFUTED; STOP at isolation (operator discipline).** There is no clean-leave full-deallocation trigger to isolate because a real VAX does not fully deallocate on a clean leave — it retains the CSB (`long_break,removed`, same address/CSID) exactly as OVMX does, and deallocates only at the return. OVMX's lingering post-leave CSB is therefore AUTHENTIC, not a defect. The §4(O.33) residual-CSB reclaim race is inherent to the deallocate-at-rejoin design on both sides. Consequently the operator's specific deterministic mechanism ("force full CSB deallocation on the clean leave") cannot be produced OVMX-side: there is no VMS behaviour to reproduce, and the only OVMX-emittable signal that would drive a member to complete removal earlier is the CM-layer op-0x0d self-departure — the RE'd crash class that bugchecks the real VAX (§4(O.29)). Per the operator's ruling (no speculative/crash-prone wire change; ship only a cleanly-isolated, validatable fix) this increment ships the isolation ONLY — **no `src/vmsscs` change**. The deterministic goal STANDS but its mechanism relocates: the authentic deallocation trigger is the rejoin, so the deterministic path is to make the RETURN reliably drive the surviving member's deallocate-and-rebuild (the member-side return engagement of §4(O.31)/(O.32)/(O.33)), not a nonexistent leave-time deallocation.

**Explicit non-claims.** (1) This does NOT claim a real VAX never removes a departed node — it removes it from MEMBERSHIP promptly on a clean leave (cluster `Nodes` drops 2→1, CSB flagged `removed`); the narrow claim is that the CSB structure is not DEALLOCATED on the leave, it is retained. (2) `o34ret` is n=1 for the return-trigger observation and is not a determinism proof for any OVMX fix — it positively identifies the trigger (the rejoin) and shows the real-VAX reclaim assigning a fresh CSID; the documented mechanism (Davis p.7-25) is the general ground. (3) No claim about OVMX's return reliability is added or removed here — that remains §4(O.33)'s reachable-not-reliable state; §4(O.34) only removes the "deallocate on leave" fix candidate from the table as corresponding to no real behaviour.

**Evidence** (host, tank volume). VAX-vs-VAX single-factor bracket on virgin `vaxlab-2`, no OVMX daemon. Leave run `o34`: per-sample CSB timeline `/data/training/vax/cluster/work/o34.csb` (PRE `01 open member` → W+0..W+220 `09 wait long_break,removed`, CSID `00010002`, address `87935140`, `Index of next CSID 0003`), status `o34.status`, shutdown dialogue `o34.trace`, pcap `/data/training/vax/k8s-labs/vaxlab-2/logs/o34.pcap`. Return run `o34ret`: `o34ret.csb`/`o34ret.status` (`>>> RECLAIM: old CSID 00010002 GONE; VAX2 now CSID=00010003 (fresh)`, `>>> READMITTED: VAX2 back to member`), pcap `o34ret.pcap`. Harnesses `tests/lab/tools/o34run.sh`, `tests/lab/tools/o34ret.sh`. *VAXcluster Principles* pp. 7-23/7-24/7-25.

## 5. Summary of unknown/inferred fields (RE gaps)

For visibility, every field NOT marked GROUNDED above:

- **§5(dlm) — DLM body: what §4(f).1 does NOT ground (`vms-ac4`).** The
  category-`0x02` lock request/response fields (mode, resource name/length,
  requester + master lock-ids, opcodes `0x01`/`0x07`) ARE grounded. Still open:
  (1) the **literal denied-reply status encoding** — the grant/deny outcome is
  grounded as a message-*shape* discriminator, but the byte(s) (if any) that
  encode `SS$_NOTQUEUED` vs a queued-then-blocked vs a deadlock (`SS$_DEADLOCK`)
  reply were not isolated; only NL-holder grants and one EX-holder NOQUEUE denial
  were captured. (2) The **lock value block** (16-byte `LKSB` VALBLK) round-trip —
  not exercised (driver requested no VALBLK). (3) The **`$DEQ` release** frame —
  present in the enqueue+deq captures but not separately byte-diffed for its own
  field map. (4) The **category-`0x01` opcode `0x05` lock/resource-database
  rebuild** body (fires only on a state transition; the envelope is grounded in
  §4j/§4o but the per-lock rebuild payload needs a join/leave event and a
  whole-database diff, not a single-op diff). (5) `body[10:20]` and `body[28:30]`
  between the envelope and the lock-mode byte carry small per-request values
  (e.g. `body[28:30]` reads `0x0100` in requests) not individually pinned. The
  requester + master lock-ids are grounded against the SDA decoder ring, the
  strongest available oracle; the checksum derivation (`body[6:8]`) remains
  one-way (§4j).
- **QUORUM/CEVOTES DYNAMICS ARE DOCUMENTED, NOT WIRE-GROUNDED (`vms-7a9`,
  `vms-41d`, `vms-2d6`).** The per-node **VOTES** field IS grounded on the wire
  (§4j, VC `body[22:24]`, LE u16, across four vote configurations) and OVMX both
  advertises it (`scs_member_build_params`) and consumes it (`scs_member_parse`
  → `v.votes`/`v.has_votes`). The **quorum arithmetic**, however, has NO wire
  contrast: every reference-lab capture ran one vote configuration (VOTES 1/0/0,
  `EXPECTED_VOTES` held at 1 in every config, `F$GETSYI` QUORUM=1), so no
  captured byte binds the quorum value. OVMX therefore implements the quorum
  recomputation from the **documented algorithm** — VMScluster Systems for
  OpenVMS §2.3.5 "Calculating Cluster Votes" (`New CEVOTES = max{max
  EXPECTED_VOTES; ΣVOTES; Old CEVOTES}`, `QUORUM = (New CEVOTES + 2)/2` rounded
  down, and the connection manager "never decreases" the value), cross-checked
  against the mined CM transcript ch7-part01 pp. 7-5…7-7 (same rule + a 5-node
  worked example; HOST-ONLY, cited by page, never committed). The code is
  `src/vmsscs/scs_quorum.c`; the proof is the known-answer unit test
  `tests/vmsscs/test_scs_quorum.c` against that worked example — **not** a
  fabricated wire assertion. The quorum-loss BEHAVIOUR (survivors block-and-wait,
  no reconfiguration) is grounded by `vms-2d6` and modelled by
  `scs_quorum_gate()` → `SCS_QUORUM_BLOCK`.
- **`EXPECTED_VOTES` — an unresolved wire RE gap (`vms-41d`).** It is almost
  certainly a sibling field of VOTES in the same op-0x01 body, but it was held at
  1 in every capture, so no offset is grounded. The quorum model seeds each
  peer's `EXPECTED_VOTES` from its advertised VOTES until a controlled-reconfig
  capture (`SET EXPECTED_VOTES` varied, byte-diff the op-0x01 body — same runbook
  as `vms-cd0`) grounds it. Enforcing the gate against a live peer is
  admission-gated / lab-bracketed at 0.4.

- **THE 110-CONTENT TYPE-10 RESIDUE — `body[48:52]`, four bytes past the end of
  the documented message (`vms-4eb`).** §4(h)(1e) decodes that class as the MSCP
  `GET UNIT STATUS` end message and every field AA-L619A-TK Table A-7 defines
  re-derives from the wire. Table A-7's last field ends at body `[48]`; the SCS
  payload is 52 bytes. `body[48:50]` is a constant `0x006e` on 2 889/2 889
  frames — and because `0x6e` is 110, which is this class's own SCA content
  length, and because **every** frame of the class is that length, a
  length-echo reading and a plain-constant reading are
  **INDISTINGUISHABLE ON THIS POPULATION**. Neither is asserted.
  `body[50:52]` takes 32 distinct values, some
  of them printable ASCII fragments; nothing we hold identifies it. **Do not
  name either field, and do not put a meaningful value in them** — OVMX has no
  occasion to build this frame at all (see the ruling in §4(h)(1e)), and if
  Phase D ever gives it one, these four bytes are the part that must be copied
  from an observation rather than composed. Closing this needs a capture of a
  `GET UNIT STATUS` end message at some OTHER content length, which the lab
  cannot produce on demand: the length is fixed by the message, not by the
  configuration.

- **THE UNIT-FLAGS BIT 15 (`vms-4eb`).** `body[14:16]` (Table A-7 `P.UNFL`)
  reads `0x8000` on all 404 valid-unit frames. **Table A-5 defines no bit 15**,
  and the bit is set identically for the two RA92 fixed disks and the two RRD40
  CD-ROMs, so it is not the `UF.RMV` removable-media flag under a different
  numbering either. Recorded as a live field with an undecoded meaning. It is
  not evidence of anything and must not be cited as one.

- **§4(N) KEYS ITS CENSUS ON THE *DESTINATION* SYSAP, WHICH ITS PROSE CALLS
  "local" (found by `vms-4eb`, filed as `vms-5da`; NOT fixed here).** §4(h)(2)
  grounds `[62:78]`/`[78:94]` as (destination, source) by the request/response
  swap; `tools/cluster/scs_type10_measure.py` re-derives that over all 48
  captures (CONNECT_REQ (`MSCP$DISK`, `VMS$DISK_CL_DRVR`) 809× and
  (`SCS$DIRECTORY`, `SCS$DIR_LOOKUP`) 201×, swapped on the answering
  ACCEPT_REQ), and MSCP confirms it independently — bound the other way, all
  2 889 END messages would run client→controller, which an endcode cannot.
  §4(N) nevertheless calls `[62:78]` "the local SYSAP name" while keying its
  connect-data table on it. **No count moves** — `scs_type10_measure.py`
  re-derives 1 101/324/2 889 and the 809 identically to
  `scs_connect_data_measure.py` — and the table stays per-SYSAP either way. What
  moves is *whose claim it is*: the `MSCP$DISK` row's `"V5.0          + "` sits
  in frames addressed **to** the disk server, so it is the disk CLASS DRIVER's
  version claim, not the server's. §4(N), `tools/scs_connect_data_measure.py`
  and `src/vmsscs/include/scs_connect.h` are left untouched by `vms-4eb`
  deliberately: they carry their own gated EXPECTED table, and a label fix that
  drags a figures gate with it belongs to the item that owns them.

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

- **THE BLOCK DATA TRANSFER SERVICE.** ~~DELIBERATELY UNIMPLEMENTED~~
  **UN-DEFERRED (`vms-4e31`) and BUILT; command/end path LIVE-WIRED
  (`vms-34b`); the block-transfer wire itself still unexercised live.**
  Originally recorded here as closed-deliberately-deferred (`vms-941`,
  `vms-096`'s audit note) when OVMX served no real storage to a cluster peer.
  That premise changed twice since, and this entry is corrected rather than
  reissued so the history stays visible:

  1. **`vms-4e31` un-deferred it.** A real VAX serving a disk to another real
     VAX was captured for the first time (lab-2 `vaxlab-9`, 2026-08-06 —
     `docs/design-mscp-direction.md`, "Phase D part 1's lab capture"),
     decoding the 28-byte block-transfer header (dest connection ID; two
     UNGROUNDED per-connection/per-transfer constants at `+4`/`+6`; a
     down-counting bytes-remaining field; source/destination buffer names and
     offsets) and two framing traps no manual gives away: READ's final chunk
     piggybacks into the SAME frame as the end message, and WRITE's
     request/response headers are byte-identical (only data presence
     distinguishes them). `src/vmsscs/scs_mscp_srv.c` now **builds and
     parses** this wire shape end to end against a real backing store
     (`scs_mscp_srv_blk_sink`, `scs_mscp_srv_read_blocks`/`_write_blocks`),
     proven by `tests/vmsscs/test_scs_mscp_srv.c` /
     `test_scs_mscp_srv_mutants.py`. v1's WRITE REFUSAL (design decision (2),
     below) is unchanged — the mechanism exists, the policy still says no.
  2. **`vms-34b` wired the RESPONDER into the live daemon.** Before this, no
     CDT was ever allocated at `OVMX_MSCP_SERVER_CONID` (`scs_mscp.h`'s own
     comment named this gap), so `scs_cdl_deliver_message()` resolved
     `SCS_DELIVER_NO_CDT` for every command a real class driver sent after the
     accept, and it vanished in silence — accept-then-black-hole, worse than a
     refusal. `scsd_mscp_srv_msg_input()` (`scsd.c`) now decodes the inbound
     command and calls `scs_mscp_srv_handle()`, so SET CONTROLLER
     CHARACTERISTICS / GET UNIT STATUS / ONLINE / READ / WRITE all get a real
     end message on the wire. `MSCP$DISK` is LISTENed by default now that this
     is true (§4(o)/scsd.c's `ovmx_mscp_server_enabled()`). **No backing store
     is attached by `scsd.c` itself** — a shipped node answers honestly with
     zero served units (Unit-Offline / Invalid Command / Write Protected)
     until an operator wires `scs_mscp_srv_attach_fd()`/`_set_xfer()` to a real
     one, which is unstarted follow-up work, not implied by this entry.

  What is STILL not proven, and is the live successor to the old un-defer
  trigger: **a real VAX has not yet MOUNTed a unit OVMX serves over this wired
  path.** vms-291's own headline ("a real VAX in a lab-2/lab-Alpha cluster
  runs MOUNT against the OVMX-served unit") remains open — everything above is
  measured at the module-test and synthetic-live-frame level
  (`tests/vmsscs/test_scsd_wire.c`'s
  `test_mscp_srv_answers_a_command_on_a_live_connection`), not yet against a
  real class driver on a real wire with a real backing store attached. That is
  a lab bracket (guardrail 23), not a code change, and is the next gate.

  SCA offers SYSAPs three services. OVMX now implements all three at the
  module level — datagram and message (§4(h), `scs_dgram.c`, `scs_credit.c`)
  from before, block data transfer (this entry) as of `vms-4e31`/`vms-34b`.
  What is being described, from *VAXcluster Principles* ch. 2 sec 2.7
  (pp. 2-32..2-41) and sec 2.9 (pp. 2-45..2-46):

  | mechanism | pages | what it is | OVMX status |
  |---|---|---|---|
  | **Named buffers** | 2-32..2-34 | a SYSAP declares a region of its own memory to SCS and receives an opaque *buffer name*, which it may then hand to the remote SYSAP; the name, not the address, is what the far end quotes | buffer NAME correlation implemented (`SCS_MSCP_BLK_SRC_NAME`/`_DST_NAME`); no SYSAP-side buffer table — OVMX only ever plays the served-block side |
  | **Buffer mapping** | 2-34..2-36 | the port maps a named buffer for direct port-to-port access, so the transfer never passes through the SYSAP's own copy loop | not modeled; OVMX moves bytes through `scs_mscp_srv_blk_sink`/the backing-store pread/pwrite path instead, which is wire-compatible but not an implementation of this VMS-internal mechanism |
  | **`SNDDAT` / `REQDAT`** | 2-36..2-38 | the two directions of the transfer: SEND DATA pushes into a remote named buffer, REQUEST DATA pulls from one. These are port *commands*, not connection-control message types, and neither appears in the `[46:48]` namespace §4(h)(1a) grounds | the WIRE SHAPE both ride (the 28-byte block header) is built/parsed; the command NAMES `SNDDAT`/`REQDAT` are not decoded fields anywhere in OVMX, per the book's own silence on their wire encoding |
  | **Buffer Descriptor Table (BDT)** | 2-34..2-36 | the per-port table the buffer names index into | not modeled (OVMX-internal equivalent is the caller-supplied buffer name/offset carried straight from the command) |
  | **Class Driver Request Packets (CDRPs)** | 2-38..2-40 | the request context a class driver queues to the port for the duration of a transfer | not modeled; OVMX's request lifetime is the synchronous `scs_mscp_srv_handle()` call |
  | **RSPID / Request Descriptor Table (RDT)** | 2-40..2-41 | the response identifier a request carries so its (possibly much later, possibly out-of-order) completion can be matched back to its CDRP | not needed yet — OVMX answers synchronously, so nothing is ever outstanding across calls |
  | **Pool / BDT / RDT SCS Waits** | 2-45..2-46 | the three additional wait states a SYSAP can be suspended in when the resource it needs — non-paged pool, a BDT entry, an RDT entry — is exhausted. Siblings of the Credit Wait `scs_credit.c` does implement | not implemented; no resource pool exists to exhaust |

  **NOT CLAIMED:** that every VMS-internal mechanism above (buffer mapping,
  BDT, CDRPs, RSPID/RDT, the three SCS Waits) is decoded or implemented — only
  the WIRE SHAPE a class driver and a server exchange is. They stay *named*
  from the public book, at page granularity, so a future implementer knows
  what OVMX's synchronous, pool-free design deliberately does not reproduce.
  The two per-connection/per-transfer header constants (`+4`, `+6`) also stay
  UNGROUNDED — carried through opaquely, never interpreted.

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
  **And in the 110-content class it is now DECODED, not merely classified —
  §4(h)(1e) (`vms-4eb`): the MSCP `GET UNIT STATUS` end message**, every field
  named against AA-L619A-TK Table A-7, paired 2 889/2 889 to its `0x03` command
  by MSCP's own command-reference rule, on the `VMS$DISK_CL_DRVR ↔ MSCP$DISK`
  connection and no other. A taxon is not a decode, and the gap between the two
  was the largest unknown on our wire until that section. What is left of it is
  four bytes and one flag bit, both registered above.
  `8`/`9` are a paired, envelope-only control exchange on established
  connections — that is a CHARACTERISATION, not an identification:
  **nothing we hold identifies them**, and they are not
  named here. The candidate and its decisive experiment are in
  `docs/design-mscp-direction.md` §1.3. Do not name them. **The "do not emit
  them" half of this line is SUPERSEDED by the `vms-a58` entry below** — see it
  before writing an emitter, and see §4(h)(1f) before writing a name.
- **Message types `8` and `9`: WHAT THEY ARE NOT, and the ruling on emitting
  them** (§4(h)(1f), `vms-a58`, 2026-08-06). The measurement is
  `tools/cluster/scs_t89_measure.py`; the gate is `ctest -R scs_t89_figures`.

  **ELIMINATED, with what did it.**
  1. *"The constant `1` is a version or a flag."* — **DEAD.** The p. 2-43
     credit ledger predicts `[48:50]` on 938 of 938 frames with zero residuals,
     so the field is a count; the same field reads `0`, `1`, `2`, `3`, `4` on
     type `10` and `3`, `6`, `8`, `10` on the initial grant. `1` on types `8`
     and `9` is a count-of-one produced by a strictly alternating workload that
     never leaves two buffers outstanding.
  2. *"They are the ninth and tenth connection-control messages."* — **DEAD.**
     Types `1`, `3`, `4`, `5`, `6`, `7` carry credit `0` in 100% of frames;
     `8` and `9` are inside the credit account. Different taxon.
  3. *"The pairing may be incidental / the response optional."* — **DEAD.**
     131 of 131 answered, handle pair swapped, worst case 3.1 ms.
  4. *"They are the p. 2-44 special credit message."* — **NOT RESTORED**, and
     no longer for the constant-`1` reason, which (1) dissolves. Two
     independent mismatches now carry it: the exchange is unconditional
     (131/131 teardowns, never mid-dialogue on a low-credit trigger) and it is
     answered (p. 2-44 describes no reply). Do not re-propose it without
     evidence that addresses BOTH.

  **NOT ELIMINATED, and honestly it cannot be from this corpus.** Whether the
  exchange is an SCA-level property of every connection or an `SCS$DIRECTORY`
  protocol element: 131 of 131 type-`8` dialogues are `SCS$DIRECTORY`, but so
  are 131 of 131 dialogues that tear down at all, so both hypotheses predict
  the observation exactly (§4(h)(1f)).

  **WHAT WOULD DISCRIMINATE NEXT, cheapest first.** (a) Capture a teardown on a
  `MSCP$DISK` or `VMS$VAXcluster` connection — the §4(h)(1f) table shows both
  are accepted in quantity and neither ever closes, so this needs a lab run that
  dismounts a served disk or drops a member cleanly. If an `8`/`9` precedes it, the exchange is SCA-level;
  if not, it is `SCS$DIRECTORY`'s. (b) Drive a dialogue that leaves more than
  one message outstanding in a direction and read `[48:50]` on the resulting
  `8`: a value other than `1` confirms the count reading against a second
  workload; a stubborn `1` would falsify it. (c) The `docs/design-mscp-direction.md`
  §1.3 engineered one-way-flow experiment, which is also `vms-1d2`'s missing
  capture.

  **RULING — must OVMX emit them? YES, but only on the half OVMX actually
  performs, and NOT as a replayed template.**
  - **Answering a received type `8` with a type `9`: KEEP.** OVMX already does
    it (`scs_reflect_credit()` in `src/vmsscs/scsd.c`), and the library records
    42 OVMX-sourced type-`9` frames against 0 OVMX-sourced type-`8`. Removing
    it would break a 131-of-131 invariant on a peer that is waiting for it.
  - **Emitting a type `8` before a `DISCONNECT_REQ` OVMX itself initiates:
    REQUIRED, and currently ABSENT.** On the wire the type-`8` sender is the
    disconnect initiator in 131 of 131 dialogues, and nothing else ever sits
    between the `8` and the `DISCONNECT_REQ`. OVMX has initiated no teardown in
    this library — every one of its `DISCONNECT_REQ` frames is the *matching*
    half, rank 1 (§4(h)(1f) table) — so it has never yet violated the invariant — but `vms-591` added
    `scs_disc_build_request()` and `vms-66f` wired the poller to send it, so the
    first OVMX-initiated teardown will send a `DISCONNECT_REQ` that no `8`
    preceded. **This is a first-class candidate for the `vms-abd` refusal**
    (`%PEA0, Inappropriate SCA Control Message`) and the cheapest test of it is
    a bracket with and without the `8`, which is what §1.4 of the design record
    already asks for.
  - **The credit value is DERIVED, never replayed.** `[48:50]` on the `8` and
    the `9` must carry the sender's outstanding-credit count from OVMX's own
    CDT (`src/vmsscs/scs_credit.c`), which happens to be `1` for the dialogues
    OVMX runs. Hard-coding `1` from these captures would be replay of a
    workload artifact, which is exactly what (1) above says the constant is.
  - **Do not name them in an emitter.** A builder may be called
    `scs_t8_build_request()` — an OVMX name for a wire codepoint — and must not
    be called a credit message, a quiesce, or anything else this document has
    not grounded.
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
  to read it as
  <!-- PPD-REFUTED-BEGIN --> "so the peer receives and **refuses** it"
  <!-- PPD-REFUTED-END -->, which was the only
  available reading while the response was believed absent. It is not available
  any more — the SCA layer answers 10/10 in under a millisecond, so whatever the
  port layer is complaining about, it is not a refusal of the disconnect.
  **Three of the four printed fields, the message's own meaning and its effect
  are GROUNDED as of `vms-0fe` — see the dedicated entry below. `OPC` itself is
  still undecoded, and `vms-0fe` could not reproduce the line at all.**
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
<!-- PPD-FIGURES-BEGIN -->
- **The `%PEA0, Inappropriate SCA Control Message` console line** (`vms-0fe`):
  **PARTLY DECODED, AND NOT REPRODUCIBLE ON A CURRENT LAB-2 RUN.** Two separate
  results; keep them separate.

  **(1) WHAT THE LINE IS — decoded, from VMS's own shipped decode tables.**
  Nothing here is a guess and nothing here came from a VSI binary: every figure
  below is output of a *documented* VMS utility run on a lab-2 VAX, or a
  published `$xxxDEF` definition macro of the kind CLAUDE.md rule 8 already
  names as a permitted source. Transcripts are host-only in
  `/data/training/vax/cluster/captures-lab2/vms0fe-vaxlab-3-oracles-20260806.txt`
  and `vms0fe-vaxlab3-analyze-errorlog-pea0-20260806.txt`.

  | figure | value | oracle |
  |---|---|---|
  | facility | `Cluster Port Driver` | `HELP/MESSAGE "Inappropriate SCA Control"` |
  | documented effect | *"The port driver closes the port-to-port virtual circuit to the remote port."* | same |
  | field names | `PPD$B_FLAGS` / `PPD$B_OPC` / `PPD$B_STATUS` / `PPD$B_PORT` | `ANALYZE/ERROR_LOG/INCLUDE=PEA0` decodes an `NI-SCS SUB-SYSTEM, _VAX1$PEA0:` entry into exactly these four symbols |
  | `PPD$B_PORT` renders as | `REMOTE NODE # n.` | same report |
  | `PPD$B_OPC` renders as | a symbolic opcode name (`UNKNOWN OPCODE` for `00`) | same report |
  | PPD header size | `SCS$S_PPD` = **16** bytes, at `SCS$B_PPD` = `-32` — i.e. a 16-byte port header *in front of* the SCS header (`SCS$W_LENGTH` −16, `SCS$W_MTYPE` −12, `SCS$W_CREDIT` −10, `SCS$L_DST_CONID` −8, `SCS$L_SRC_CONID` −4) | `LIBRARY/MACRO/EXTRACT=$SCSDEF SYS$LIBRARY:LIB.MLB` |
  | SCS message-type enum | `CON_REQ 0 · CON_RSP 1 · ACCP_REQ 2 · ACCP_RSP 3 · REJ_REQ 4 · REJ_RSP 5 · DISC_REQ 6 · DISC_RSP 7 · CR_REQ 8 · CR_RSP 9 · APPL_MSG 10 · APPL_DG 11` | same macro — an **independent confirmation** of §4(h)(1a)'s 0–7, which until now rested only on our own captures, and it extends the enum past 7 |

  So the quadruple is the **PPD (port-to-port driver) header**, a *different*
  16-byte header from the SCS header whose `[46:48]` message type §4(h)(1a)
  grounds. `OPC/22` is a `PPD$B_OPC`, and it can never be an SCS message type —
  the SCS enum stops at 11. **The reading "the peer refuses OVMX's
  DISCONNECT_REQ" is now dead twice over**: `vms-096` measured the SCA answer
  10/10, and the message's own documented meaning is a *port*-layer VC close,
  not an SCA-layer refusal.

  **`PPD$B_PORT` = the sender's PEDRIVER remote-node number, allocated
  descending from `0xDE`** (GROUNDED). `ANALYZE/ERROR_LOG` on `vaxlab-3`'s own
  `ERRLOG.SYS` pairs every `REMOTE STATION ADDRESS` with a `REMOTE SYSTEM ID`:

  | station | remote SCSSYSTEMID(s) seen | who |
  |---|---|---|
  | `0xDE` (222) | `0x402` = 1026 | VAX2 — always the first remote node |
  | `0xDD` (221) | `0x456`=1110, `0x52B`=1323, `0x642`=**1602 = OVMXP2** | first OVMX joiner of that VAX1 boot |
  | `0xDC` (220) | `0x406`=1030, `0x52C`=1324, `0x643`=**1603 = OVMXP3** | second |
  | `0xDB` … `0xCF` | `0x460`, `0x461`, … `0x477` | later ones, one step down each |

  The run that produced `PORT/DD` was a 2-node lab-2 pod, so `0xDD` **is OVMX** —
  the line is about a frame received *from us*. `PORT/DB` on the lab-1
  `coord5` run (`work/coord5.status`, `OVMXC5`/1174) is the same field on a
  3-VAX lab. **It is a per-boot discovery ordinal, not derived from any
  identity** — the same `0xDD` covers three different SCSSYSTEMIDs above — so do
  not try to read a node identity out of it, and do not treat `DD` vs `DB` as a
  difference between the two runs.

  **(2) REPRODUCTION — IT DID NOT REPRODUCE.** Four fresh lab-2 runs,
  `main`-built daemon `md5 ce1e0e484f7247d2234e2413dafff47a`, identities minted
  clear of every id in use (`OVMXP1`/1601 … `OVMXP3`/1603), captures and both
  consoles archived as `vms0fe-*` beside the epic's other lab-2 evidence:

  | run | pod | pod state | clean shutdown | joined | port-driver line on VAX1 |
  |---|---|---|---|---|---|
  | `0feA`  | `vaxlab-7` | 3 prior OVMX ids | n/a (no connection) | **no** | `Remote System Conflicts with Known System - REMOTE NODE OVMXP1` |
  | `0feA1` | `vaxlab-0` | virgin | yes — 3 `DISCONNECT_REQ` sent, 2 answered | **yes** (CN_3) | `Port has Closed Virtual Circuit - REMOTE NODE OVMXP1` |
  | `0feB1` | `vaxlab-3` | virgin | **no — `OVMX_NO_CLEAN_SHUTDOWN=1`, `req-sent=0`** | no | `Port has Closed Virtual Circuit - REMOTE NODE OVMXP2` |
  | `0feA2` | `vaxlab-3` | holds `OVMXP2` | yes | **yes** (CN_3) | `Port has Closed Virtual Circuit - REMOTE NODE OVMXP3` |

  **`Inappropriate SCA Control Message` appeared in none of the four.** What a
  disconnect-sending run draws today is the ordinary
  `Port has Closed Virtual Circuit` — and the **matched control `0feB1` draws
  that same line while putting ZERO `DISCONNECT_REQ` frames on the wire**
  (`SCSD-I-NOCLEANSHUT` logged, `DISCONNECT: req-sent=0`), so that message is
  the ordinary teardown of a node that went away and is **not** caused by
  OVMX's disconnect at all. `0feA2` also kills "the pod has to be virgin": a
  second OVMX identity joined a pod that already held one.

  **NAMED STATE DIFFERENCES A FUTURE SESSION CAN TEST**, in the order they are
  worth trying — the `vaxlab-4`/`coord5` runs differ from these four by:
  (a) **daemon build** — those ran the `vms-578`/`vms-70e2`/`vms-591` branch
  daemons, these ran `main` after `vms-096`/`vms-755`; (b) **which connection
  was still open at exit** — in the `vaxlab-4` runs the surviving
  `DISCONNECT_REQ` went to **VAX1** (`aa:00:04:00:01:04`), in `0feA1` VAX1's
  SCS$DIRECTORY connection had already reached CLOSED during the run and the
  only one left went to **VAX2**; (c) **`0feA`'s own failure mode**, below.
  Hypothesis (b) is the one the message text itself points at — an SCA control
  message is "inappropriate" for the *state*, and the sibling message
  `Received Connect Without Path-Block` shows this facility's other complaint of
  the same shape is precisely "control message arrived with no path block".

  **AND THE DECODE PROCEDURE IS NOW FULLY SPECIFIED.** Whoever reproduces it
  next does not have to guess at `OPC/22`: run
  `ANALYZE/ERROR_LOG/INCLUDE=PEA0 SYS$ERRORLOG:ERRLOG.SYS` on the complaining
  VAX afterwards and **read the symbolic name the report prints under
  `PPD$B_OPC 22`**. The formatter has the table; our labs have simply never
  logged a nonzero `PPD$B_OPC` (all entries in `vaxlab-3`'s log read
  `00 / UNKNOWN OPCODE`). No `$PPDDEF` exists in `LIB.MLB` or `STARLET.MLB` on
  V7.3, so the error-log report is the only decode oracle we have.

  **AND A SECOND, PREVIOUSLY UNRECORDED PORT-DRIVER REFUSAL WAS FOUND** (`0feA`,
  `vaxlab-7`): `%PEA0, Remote System Conflicts with Known System - REMOTE NODE
  OVMXP1`, documented as *"the configuration poller discovered a remote system
  with SCSSYSTEMID or SCSNODE equal to that of another system to which a virtual
  circuit is already open"*, whose documented user action warns of **CLUEXIT
  bugchecks elsewhere in the cluster**. OVMX then reached VC OPEN and never
  received the `0x4b` connect — the exact `vms-2f3` stall signature — while
  `0feA1`/`0feA2` on other pods joined normally. **This is NOT a claim about the
  `vms-2f3` cause** (`0feA2` joined a non-virgin pod, so "residue blocks the
  join" is already falsified as a general rule); it is a named, observed
  port-layer refusal that had never been written down, and it deserves its own
  item.
<!-- PPD-FIGURES-END -->
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

> **UPDATED 2026-08-09 (`vms-694`).** The `MEMBER ACHIEVED` result below is a
> pristine 3-node **FORMATION** (lab-1, all peers boot with OVMX). A **different**
> gap now blocks `NEW → MEMBER` on an **ESTABLISHED** cluster (peers already
> running before OVMX joins, lab-2): OVMX's own fixed-per-role Con.IDs
> (`OVMX_JOINER_CONID` etc.) collide across multiple pre-existing peers, so a
> second peer's reciprocal CM traffic is silently dropped as
> `SCS_DELIVER_SRC_MISMATCH` before it ever reaches the CM layer described
> below. See §4(x) for the live evidence (3/3 lab-2 runs) and the root cause in
> `scs_cdt.c`. **FIXED** by per-peer Con.ID allocation (`vms-694`, 2026-08-09) —
> see the RESOLVED subsection in §4(x).

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

### 4(w) The configuration poller's identity-conflict refusal (GROUNDED live, `vms-1ae`)

A refusal that is **not** the `vms-2f3` stall, but is indistinguishable from it
at the console — which is why it went unrecorded for so long. The peer prints

```
%PEA0, Remote System Conflicts with Known System - REMOTE NODE <ours>
```

on **both** VAX consoles, the VC opens, the peer's connect never comes, and
`CLUSTER_NODES` never moves. `HELP/MESSAGE` on the lab VAX gives VMS's own
explanation: *"The port driver configuration poller discovered a remote system
with SCSSYSTEMID or SCSNODE equal to that of another system to which a virtual
circuit is already open."*

**The rule, as measured (13 arms, 3 lab-2 pods, 2026-08-06).** The poller keeps
a record of every system it has recently seen on the wire, keyed on the pair
(`SCSNODE`, `SCSSYSTEMID`). A joiner is refused when it matches such a record
on **exactly one** of the two fields:

| our identity vs. a recently-seen system | conflict | arms |
|---|---|---|
| both fields differ (a genuinely new system) | **no** | `1aeB`, `1aeD`, `1aeU3` — all three also **joined** |
| same `SCSSYSTEMID`, different `SCSNODE` | **yes** | `1aeC` (pod 8), `1aeT2` (pod 7), `1aeU2` (pod 1) |
| same `SCSNODE`, different `SCSSYSTEMID` | **yes** | `1aeE` (pod 8) — **n=1, unreplicated** |
| **both fields the same — an exact rejoin** | **no** | `1aeG` (refused, but silently: 0 conflict lines) — **n=1, unreplicated** |

**Read the `arms` column before leaning on a row.** Only the `SCSSYSTEMID` row
has its specific field replicated across pods (three arms, three pods). The
`SCSNODE` row rests entirely on `1aeE` and the exact-rejoin row entirely on
`1aeG` — one arm each, both on `vaxlab-8`, and both inside the window discussed
in the caveat under point 4.

Four things follow, and the fourth is the one that matters to `vms-2f3`:

1. **Either field alone is sufficient.** The message names `SCSSYSTEMID or
   SCSNODE` and both halves were exercised separately. The `SCSSYSTEMID` half
   is the well-bracketed one: three arms on three pods, each against a matched
   fresh-identity control on the same pod minutes before and after. The
   `SCSNODE` half is `1aeE` alone, and its *trailing* control did not hold —
   see point 4.
2. **An open VC is *not* required, despite the wording.** In `1aeT2` the
   colliding system (`OVMXT0`) had been on the wire two minutes earlier and had
   **never been admitted** — it never reached `CLUSTER_NODES=3` and no VC to it
   was ever opened. The conflict fired anyway. Being *seen* is enough.
3. **`SHOW CLUSTER` is NOT the oracle for the conflicting set — GROUNDED.**
   Arm `1aeR` replayed the original `vms-0fe` collision (`OVMXP1`/1601 against
   `OVMXY1`/1601) on the same pod, `vaxlab-7`: **no conflict**, even though
   `SHOW CLUSTER` *still listed* `OVMXY1` as `BRK_NEW` at that moment. So the
   poller's set is strictly smaller than `SHOW CLUSTER`'s — the latter is a safe
   pre-flight, not a precise one.

   **Why `1aeR` drew nothing is NOT established — do not cite this as an aging
   result.** The tempting reading is that the record ages out: the same
   collision on the same pod fired at +49 min (the original `vms-0fe`
   observation) and not at +3.5 h (`1aeR`). But that is two points at two times
   with nothing held constant between them, no measurement of the interval, and
   a live confound — `vaxlab-7` refused *every* identity this session including
   the fresh `1aeT1`, so its peer state differs from the pods that produced the
   positive arms in at least one other way. Aging is one hypothesis; a
   fourteenth arm holding the pod and the collision fixed while varying only
   the delay is what would settle it, and it was not run.
4. **⭐ An exact rejoin does not draw this conflict.** Arm `1aeG` re-ran
   `OVMXZ4`/1804 eight minutes after that identity had actually **joined** the
   same pod, and was refused with **zero** conflict lines on either console. So
   the conflict signature does not fire on an exact-identity return the way it
   fires on a field collision: whatever refused `1aeG` is **not** the refusal
   this section describes, and **this section does not explain the `vms-2f3`
   stall.**

   **⚠ `1aeG` is NOT a cleanly bracketed arm — do not read it as ruling
   `vms-2f3` in or out.** The pod-8 sequence was `1aeD` joined at 04:43 →
   `1aeE` (`SCSNODE` collision, conflict) → **`1aeF`, a FRESH identity, FAILED
   to join at 04:49** → `1aeG` at 04:51. The trailing fresh-identity control
   therefore *did not hold*: by the time `1aeG` ran, `vaxlab-8` was refusing
   **everyone**, new identities included. `1aeG` drawing no conflict lines is
   consistent with the exact-rejoin reading, but it is equally consistent with
   a pod that had simply stopped admitting anything, and this arm cannot
   separate the two. The same failed control sits on the trailing edge of
   `1aeE`.

   What survives is the **narrow** claim: an exact-identity rejoin does not
   produce the *same* console signature as a field collision. What does **not**
   survive is any claim that `1aeG` positively demonstrates VMS distinguishing
   *the same system returning* from *a different system claiming a used key*,
   or that it rules `vms-2f3` out.

   Two further caveats. `1aeG` is **n=1**, and `conflictbracket.sh` deletes the
   prior-admission sidecar (§4d.2) on every arm, so it is a rejoin by a
   previously-admitted *identity* presenting as a first-timer, not the full
   `vms-2f3` reproducer. §4f.2 already refuted the sidecar as causal for the
   refusal. **Replicate `1aeG` on a virgin pod, with a trailing control that
   actually joins, before anything is built on it.**

**Reading it off a capture without a console.** The two refusals separate on the
wire. In a conflict arm the peers never emit our node name at all and total
0x6007 traffic collapses (577–693 frames over the same window against
3195–3729 for an admitted run); in a non-conflict refusal (`1aeF`, `1aeG`) the
peers *do* carry our name in their own frames (3–7 occurrences from the VAX
source MACs). Census per arm with
`strings -a <pcap> | grep -oE 'OVMX[A-Z0-9]{2}'` cross-tabulated by Ethernet
source — guardrail 4.

**Why this is a harness bug before it is a protocol bug.** `tests/lab/tools/mk_sysgen.py`
would mint any identity asked of it, so parallel agents collided:
`OVMXY1` and `OVMXP1` were both minted on `SCSSYSTEMID` 1601, and `OVMXP2`/`OVMXY2`
both on 1602. Every experiment run under a collided identity produces a null
result that reads as a `vms-2f3` stall. The uniqueness guard added in `vms-1ae`
refuses the collision at mint time (and names the store it collides with);
`tests/integration/test_lab_identity_unique.sh` is its gate.

#### 4(w.1) The conflict's VC-FORMATION signature: the peer WITHHOLDS its round-2 ACK (GROUNDED live, `vms-694`, 2026-08-09)

§4(w) reads the conflict at the console and off the aggregate capture. This grounds
what it looks like **one layer up, at the 0x41 VC-formation handshake** — because
that signature had been misread as a VC-formation *regression* and nearly re-filed
as a code defect (`docs/design-rejoin-mscp-silence.md` §2's "either a regression
since Aug-3 or a pod/timing artifact"). It is neither: it is §4(w).

**The dialogue, both ways, same build, same session.** The 0/0/1/1/2/2 config-round
handshake (§4g) completes only when the member sends its **round-2 ACK**. Whether it
does is the whole discriminator:

| run | pod | identity (`SCSSYSTEMID`) | member round-2 ACK (`STARTRX ack round=2`) | `START-ACK-SENT` | `CONNECT-REQ-SENT` | post-OPEN re-STARTs (`START received -> none, VC OPEN`) | outcome |
|---|---|---|---|---|---|---|---|
| clean | `vaxlab-9` | `OVMXW0` / **1877** (fresh) | **2** | **2** | **2** | **0** | **JOINED `CLUSTER_NODES=3` `XITDONE=1`, t+13 s** |
| clean | `vaxlab-9` | `OVMXX0` / **1888** (fresh) | **2** | **2** | **2** | **0** | **JOINED `CLUSTER_NODES=3` `XITDONE=1`, t+13 s** |
| conflict | `vaxlab-8` | `OVMXV0` / **1812** (reused — peer holds a record) | **0** | **0** | **0** | 29 | STALL `CLUSTER_NODES=2` |
| conflict | `vaxlab-8` | `OVMXR0` / **1812** (reused) | **0** | **0** | **0** | 59 | STALL `CLUSTER_NODES=2` |

On a **clean** identity the member sends round-0 START, round-1 STACK **and round-2
ACK**; OVMX's round-2 ACK then goes out (`start_acked` latches), and the join
sequencer's first `SCS$DIRECTORY` CONNECT-REQUEST ignites on that same edge. On a
**conflicting** identity (`vaxlab-8` had `%PEA0, Remote System Conflicts with Known
System - REMOTE NODE OVMXR0` on `SCSSYSTEMID` 1812) the member sends START and STACK
— OVMX's circuit reaches OPEN on the STACK — but then **withholds its round-2 ACK and
re-issues round-0 START indefinitely** (29 and 59 times across the two runs; the
clean control had zero). Because OVMX's default ACK trigger waits for the peer's own
round-2 frame (the pre-`vms-4071` fresh-join ordering; `OVMX_VC_EARLY_ACK` off),
`start_acked` never latches and the sequencer never fires — `CONNECT-REQ-SENT=0`.
This is the "VC-START stall": half of a formation the member is deliberately not
completing, not a formation bug on OVMX's side.

**Doc grounding.** Davis *VAXcluster Principles* p. 2-14 (host-only transcript
`part2.md:105-111`): "A response of either ACK or STACK will advance the circuit to
the OPEN state; and if the response is STACK, the port driver will issue an ACK," and
p. 2-14/2-16 (`part2.md:181`): after issuing its STACK the port driver "is waiting
for an ACK from the remote port driver" and "will simply reissue the STACK after its
timer expires." A member refusing admission never leaves that wait — hence the
endless re-START and the absent round-2 ACK. OVMX's FSM implements the STACK→issue-ACK
rule (`scs_vc.c` `scs_vc_fsm_recv`, START RECEIVED + STACK → `SEND_ACK`); it is the
daemon's `scsd_vc_ack_due()` gate that (correctly, for a clean join) withholds OVMX's
ACK until the peer's round-2 arrives.

**#221's fresh-join is INTACT on current `main`.** Both clean rows above are the same
`main` build (this session) that produced the conflict rows, default environment, no
`OVMX_JOIN_SEQ` — reaching `CLUSTER_NODES=3`/`XITDONE=1` at t+13 s. The VC-formation
FSM (`scs_vc.c`, `scs_start.c`) is byte-unchanged since the `f874b04` first-join proof
(§4(O.1)/§4(O.2)); there is no regression. The prior VC-START stalls were §4(w)
identity conflicts on a polluted pod (reused `SCSSYSTEMID` 1812), nothing more.

**The daemon now NAMES this instead of looping on it silently.** `scs_vc_noack_stall()`
(`scs_vc.c`) is true when the circuit is OPEN, `start_acked` is still 0, and the peer
has re-issued round-0 START ≥ `SCS_VC_NOACK_STALL_THRESHOLD` (3) times since OPEN
(counted in `fsm.start_after_open`). On that edge `scsd.c` emits **`SCSD-W-VCNOACK`**
once per peer, pointing at the `%PEA0` conflict and telling the operator to mint a
fresh identity. Log-only — nothing new on the wire. Bracketed live: it fired on both
conflict runs (naming 3 re-STARTs) and stayed silent on both clean joins. Unit test:
`tests/vmsscs/test_scs_vc.c::test_fsm_noack_stall_detect`.

### 4(x) NEW→MEMBER on an ESTABLISHED multi-peer cluster: the joiner's own Con.ID collides across peers (GROUNDED live, `vms-694`, 2026-08-09)

**What this is.** §4(L)(7) and §5(z) left `NEW → MEMBER` open. §5(z)'s own
`MEMBER ACHIEVED` result (`docs/HANDOFF-vms-760.md` §1) was on a **pristine
3-node lab-1 FORMATION** — all peers boot alongside OVMX. This section grounds a
**distinct** failure that only appears in an **ESTABLISHED** cluster (peers
already `MEMBER`/`MEMBER` before OVMX starts, lab-2 `vaxlab-0`, VAX1 sysid 1025 +
VAX2 sysid 1026): the join sequencer (`OVMX_JOIN_SEQ=1`) now reliably drives a
fresh identity through all 8 steps to `SCSD-I-CMCONFIG2` (deferred `op 0x02` to
the chosen peer, per §4(L)) and `SHOW CLUSTER` status `NEW` — but the peer's
reciprocal traffic **never arrives**, across three independent 150 s lab-2 runs:

| run (tag) | peer sent `op 0x02` | `CLUSTER_NODES` after 150–156 s | inbound CM frames from peers |
|---|---|---|---|
| `vms694A` (pre-cadence-fix) | — (stalled earlier, §1h) | 2 (unchanged) | 0 |
| `vms694fix` (post-#211 retx fix) | node 2 (VAX2, default `cm_pick_coordinator`) | 2 (unchanged) | 0 (`CM census` empty of inbound `cat 0x0*`) |
| `vms694memb1` (`OVMX_CFG2_PEER=1` forced to VAX1) | node 1 (VAX1) | 2 (unchanged) | 0 |

**The root cause, found in the daemon's OWN receive path, not the wire.** Every
run logs repeated `SCSD-W-RXSRCMISMATCH` for **both** peers on **both** of
OVMX's own client Con.IDs (`SCS$DIRECTORY` role and the `VMS$VAXcluster` joiner
role) — e.g. `vms694memb1`'s SCSD log:

```
[02:17:04.181] SCSD-W-RXSRCMISMATCH, application message for conid=0x68CA0002
  carries source conid=0xEABD000F, which is not that connection's remote
  handle (p. 2-35) -- not delivered
```

(the same signature appears for the default-peer run, `vms694cadence-scsd.log`
lines 117–187, and for the rejoin run, `vms694cadence-rejoin-scsd.log` lines
862–908 — three separate runs, 100% reproduction). `0x68CA0002` is
`OVMX_JOINER_CONID` (`scsd.c`); the two peers' frames addressed to it carry
**two different** source Con.IDs (`0x2F5E000A` from VAX2, `0xEABD000F` from
VAX1) because each peer independently allocates its own handle for the
connection **we** opened to **it**.

`scs_cdt.c`'s `scs_cdl_lookup()` keys a connection descriptor **purely on our
local Con.ID** and holds exactly one bound `remote_conid` per slot;
`scs_cdl_resolve()` accepts a frame only if its source Con.ID matches that one
stored value (`SCS_DELIVER_SRC_MISMATCH` otherwise). But `OVMX_JOINER_CONID` —
like `SCS_DIR_OVMX_JOINER_CONID`, `OVMX_LOCAL_CONID`, `OVMX_MSCP_CONID`, and the
other role Con.IDs in `scsd.c` — is a **single fixed value for the entire
daemon incarnation** (`ovmx_conid_base() | 0x0002u` etc.), shared by every peer.
With one pre-existing peer this is invisible; with **two or more**, whichever
peer's ACCEPT lands first wins that slot's binding and every other peer's
legitimate reciprocal frames on the same role are silently dropped as
`SRC_MISMATCH` forever after — which is exactly what all three runs show:
zero inbound CM traffic, regardless of which specific peer is asked to
reciprocate, because **neither** peer's Con.ID is ever the one already bound
(or one is bound and the config request goes to the other).

**This is not a fresh problem — it is the failure mode §4(t) already predicted.**
§4(t) (`vms-584`) grounded that a real node allocates Con.IDs from **one
monotonic counter per incarnation, shared across all service classes**, so no
two simultaneous connections — even to different peers, even in the same role —
ever share a value, and explicitly noted *"the peer binds whatever is offered
and never validates the value... that is what makes changing OVMX's allocation
safe."* `scsd.c`'s per-role **fixed** constants satisfy that grounding only in
the single-peer case; §4(t)'s own reasoning already flags the multi-peer case
as unsafe, and this section is the first live reproduction of the consequence.

**Why lab-1's `MEMBER ACHIEVED` (07-30, §5(z)) may not have hit this.** Not
established here — plausibly the coordinator-selection heuristic (`cm_pick_
coordinator`, §4(L) citing `vax3-2to3-established-join`) happened to target
the one peer whose Con.ID slot won the race in that run, or the FORMATION
choreography (peers and joiner boot together) binds connections in a different
order than an ESTABLISHED join does. Do not assume the 3-node success is
Con.ID-collision-free without checking its own capture for `SRC_MISMATCH`
equivalents — that check was not performed as part of this entry.

**Scoped next step (NOT done here — see below for why).** Replace the fixed
per-role joiner Con.IDs with a genuine per-peer allocation (e.g. a low-word
counter seeded at today's constant, so the *first* peer/connection in any run
keeps today's value and every existing single-peer test is unaffected; the
*second* and later peers get distinct values), and re-key `scs_cdl`'s
connection lookup — or thread a per-`peer_state` Con.ID — so two simultaneous
connections of the same role never collide. Candidates: `OVMX_JOINER_CONID`
(`scsd.c:493`), `SCS_DIR_OVMX_JOINER_CONID` (`scsd.c:566`), and the `OVMX_PS_*`
pure-server variants (`scsd.c:553-554`) — roughly 15 production call sites in
`scsd.c` plus ~30 test call sites in `tests/vmsscs/test_scsd_wire.c`,
`test_scs_cdt.c` and `test_scs_dir.c` that assert against the literal macro.

**Why the diagnosis entry above stopped at diagnosis.** The fix touches core CDT
plumbing shared by every wire test in this file (which this epic's own context
already flags as having rejected 12 rounds of under-grounded changes), and
correctness requires re-verifying against the full 19-capture/141k-frame replay
library (§4(m) precedent) before it can be trusted. The diagnosis is reproducible
and cheap to re-check without a lab: any unit test that opens two `peer_state`s
and drives both through the joiner-VC accept path, then delivers an app-message
from the second peer, surfaces `SCS_DELIVER_SRC_MISMATCH` on the pre-fix tree.

#### RESOLVED (`vms-694`, 2026-08-09) — per-peer Con.ID allocation

The scoped step above LANDED. Each `peer_state` now carries a `conid_off` low-word
block offset (`peer_slot_index * 0x10`), assigned in `peer_find_or_add`. OVMX's
handles are formed through `PS_*_CONID(ps)` macros (`ovmx_conid_base() |
ps->conid_off | <class>`) at every send site, and the receive path matches an
echoed handle with `ovmx_conid_is_class(conid, <class>)` (the frame is still
routed to its peer by src MAC, so this only confirms the class). **Peer slot 0
keeps offset 0 — i.e. the exact historical class-slot values — so every
single-peer capture and the full replay library replay byte-identically; only the
2nd+ concurrently-live peer moves into its own 0x10-wide block.** This is a
labeled OVMX derivation (Rule 8): the wire framing/echo/match rules are unchanged
and the block offset is opaque to the peer exactly as the per-boot high word is.

Converted classes (all now per-peer): `0x01` local VC, `0x02` joiner VC, `0x07`
dir-server, `0x08` dir-client, `0x09` poller (which had *also* been left on the
compile-time base by the vms-760 `#undef` — now on the per-boot base like its
siblings), `0x0A` MSCP client, `0x0B` MSCP server, `0x0C`/`0x0D` pure-server.

Regression proof: `tests/vmsscs/test_scsd_wire.c
::test_second_peer_gets_its_own_conid_not_a_collision` (which previously ASSERTED
the bug — that peer B was refused a CDT — and now asserts the fix) drives two
peers through the joiner accept path and checks BOTH bind distinct CDTs at
distinct Con.IDs, that peer B's echo resolves to peer B (not `SRC_MISMATCH`), and
that peer slot 0 still equals `OVMX_JOINER_CONID`. Full `vmsscs|scs` ctest: 50/50.

Residual (out of scope, distinct limitation): the SAME peer opening a SECOND
connection of the SAME class still reuses one per-peer slot per class, where a
real node allocates a fresh pair per connection (`vms-298`). That is a
per-connection concern, not the cross-peer collision this section is about.

### 4(y) The Rule of Total Connectivity — cluster admission is BIDIRECTIONAL (GROUNDED, `vms-694`, 2026-08-09)

**What this grounds.** Why a coordinator answers a joiner's `JS_MSCP_CONNECT` with
**silence** (neither ACCEPT nor REJECT) rather than a reject: it is not an SCS
connection-layer refusal at all, it is the connection *manager* **abandoning the
state transition** because the joiner failed the Rule of Total Connectivity.

**Doc grounding — Davis, *VAXcluster Principles* (1993), ch. 7 JOIN CLUSTER.**
- p. 7-37: a joiner "will attempt to join the existing cluster only after the
  number of cluster members it **has connectivity with** equals that count."
- p. 7-39 ("Rule of Total Connectivity"): the coordinator VAX_A "describes VAX_B
  to each of the current VAXcluster members, and they respond by indicating
  whether or not they have connectivity with VAX_B… **VAX_B must report having
  connectivity with all the current cluster members, and they must all report
  having connectivity with VAX_B. Otherwise, VAX_A abandons the state transition
  at this point.**"
- p. (ch. 2) definition: "An SCS connection exists (i.e., the local Connection
  Manager has connectivity) with the remote." So a member "has connectivity with
  the joiner" **only when its own SCS connection TO the joiner is established** —
  which requires the **joiner to accept the member's inbound connect**.

**Consequence for the wire.** Admission is bidirectional and mutual. A joiner that
drives only the OUTBOUND half (joiner→member: dir connect, MSCP client connect, VC
connect, add-member) reaches proposed/`NEW` but every member reports "no
connectivity with the joiner", so the coordinator abandons — the joiner sees
**silence** on the connect it is waiting on. "Abandon" emits nothing; the explicit
SCS reject (CONNECT_RSP/REJECT_REQ, §4(m)) is a *different* layer.

**Live corroboration (real-VAX SDA, our lab).** In `w1C.conn`/`w4B.conn` the
member's inbound connect to OVMX parks unaccepted — `VMS$DISK_CL_DRVR →
OVMX::MSCP$DISK`, state `con_sent`/`con_pend`, remote Con.ID `00000000` — while in
the success arm `w3A.conn` (OVMX servicing the inbound half) every CDT reads
`0002 open`. OVMX-side SUMMARY of a member-reaching run (`scsd-vms694memb1.log`):
`connect_scans=2` yet `MSCP-SERVER-ACCEPTS-SENT=0`, `no-such-sysap-sent=0`,
`RX-CDL no-cdt=22` — the members connected inbound, OVMX found the SYSAP, accepted
none, and dropped their follow-on messages. Full diagnosis + proposed fix:
`docs/design-rejoin-mscp-silence.md`.

**Clean-room.** Doc cites are the host-only Davis transcript (page cites only);
wire/peer-state claims are from our captures and real-VAX SDA. No VSI/HPE source
or binary examined.

> **CAVEAT (`vms-694`, 2026-08-09).** The Davis p.7-39 rule itself (admission is
> bidirectional; each member must have an SCS connection to the joiner) stands as
> documentation. What a live kill-switch bracket on a CLEAN pod with a FRESH
> identity FALSIFIED is the *application* used to explain the JS_MSCP_CONNECT
> silence — that the missing connectivity was the member's inbound **MSCP$DISK**
> connect. At HEAD OVMX already accepts that connect during admission, and with
> the accept fully disabled (`OVMX_MSCP_SERVER=0`) OVMX STILL joins as a full
> member (peer logs "removed from the VAXcluster" at exit → it had a CSID). The
> connectivity that gates membership is the VMS$VAXcluster/SCS$DIRECTORY layer,
> not MSCP$DISK. Full bracket + evidence paths:
> `docs/design-rejoin-mscp-silence.md` §7.

### 4(z) `SHOW CLUSTER` "NEW" tracks TRANSITION COMPLETION, not a missing display step — a COMPLETING join reaches the peer's CSB `member`; `NEW` reproduces only when the transition does not complete (`XITDONE=0`) (GROUNDED live, `vms-071`, 2026-08-09)

**What this resolves.** `vms-071` (filed 2026-08-09T03:38Z) carried the premise
"OVMX joins and reaches member-OPEN, but on the PEER a `SHOW CLUSTER` displays
OVMX's node as `NEW`, not `MEMBER`" — the frontier §4(L)(7)/§4(x) left. It
predates the completed per-peer-Con.ID fix stack (#221 `622fbe0` — §4(x)'s
RESOLVED, 2026-08-08 23:35 — plus #235/#237/#239, §4(O.6)–§4(O.8)). A live
bracket at HEAD shows the premise **conflates two distinct outcomes**, and the
`NEW`/`MEMBER` split is governed by whether OVMX's state transition **completes
(`XITDONE`)**, not by any display artifact or missing joiner emission.

**The discriminator, and why it is sound.** `SHOW CLUSTER`'s per-node status is
rendered **from the peer's CSB** (Davis 7-25: "The MEMBERS class of data
displayed by `SHOW CLUSTER` is obtained from the CSBs"; §4(L)(5), §4(q)). The
CSB carries a connectivity `State` (Davis 7-23/24: the ten states `NEW…OPEN`)
**and** membership `Flags`. The sound oracle is the **peer's** CSB read on the
peer — never OVMX's own display (a rejected-evidence class). `csbwatch.sh`
parks VAX1 in SDA sampling `SHOW CLUSTER/NODE=<ident>` *through* the join;
`csbcycle.sh` does the same across a **real** node's kill→rejoin control.

**Arm A — a COMPLETING join reaches `member`, byte-identical to a real member.**
Fresh identities on `vaxlab-11`/`vaxlab-12`, peers already `MEMBER`/`MEMBER`
(the §4(x) ESTABLISHED case), each `XITDONE=1` (all 12 barrier steps released,
`VAXCLMEMBER=2`):

| run | peer's CSB for OVMX (SDA) | samples |
|---|---|---|
| `OVMXS0` (`vaxlab-11`, `XITDONE=1`) | `State: 01 open`, `Flags: 02060002 member,selected,status_rcvd`, `CSID 00010003`, real `CDT 87A0BAC0`, live seq (`Next 00BE`/`ack 00BC`) | **17/17**, T+6 s→T+180 s, never regresses |
| `OVMXU0` (`vaxlab-11`, `XITDONE=1`) | `State: 01 open`, `Flags: 02060002 member,…` | t+60 s and t+120 s |
| real MEMBER control (VAX2, same dumps) | `State: 01 open`, `Flags: 02060002 member,selected,status_rcvd` | — |

OVMX's completing-join `Flags` (`02060002 member,selected,status_rcvd`) are
**byte-identical** to the real member VAX2's, and the DCL `SHOW CLUSTER`
*utility* renders VAX2's `02060002` CSB as `| VMS V7.3 | MEMBER |`. Because the
utility STATUS is a function of exactly those CSB flags, an OVMX CSB carrying
the identical flag word renders `MEMBER` too. Evidence:
`work/vms071A.csb`, `work/vms071dual.out`, `scsd-vms071{A,dual}.log`,
`d94-vms071{A,dual}.pcap` (identities proven on wire).

**Arm B — `NEW` reproduces only when the transition does NOT complete.** A
`OVMXT0` **rejoin** on `vaxlab-12` (its own first join there had `XITDONE`;
the rejoin did not) logged `XITDONE=0` — the barrier never released — and the
DCL utility on that quiet console printed `| OVMXT0 | VMX V0.1 | NEW |` beside
`VAX1`/`VAX2` `MEMBER`. `NEW` is the correct status of a node whose CSB was
allocated (Davis 7-23: "the CSB has just been allocated") but whose state
transition did not finish — not a display bug, and not a completing join
rendered wrong. Evidence: `work/vms071util2.out`, `scsd-util2.log`.

**Real-node control (Arm C).** `csbcycle` killed the real VAX2 and watched its
CSB on VAX1: `01 open` → `09 wait` (~100 s, connectivity lost) → `01 open` on
readmission. Steady MEMBER = `01 open`; a *retained* CSB reconnects
`wait→…→open` without passing back through `NEW`. `work/vms071ctl.csb`.

**Where the original `NEW` reading came from.** The pre-#221 §4(x) runs showed
`SHOW CLUSTER` `NEW` with `CLUSTER_NODES` stuck at 2 — the **Con.ID-collision**
bug (a distinct, now-fixed defect), and those runs also did not complete the
transition. Once #221 gave each concurrent peer its own Con.ID block, a fresh
join's add-member traffic is delivered both ways, the coordinator completes the
transition, and the CSB reaches `member` — exactly §4(q)'s prediction
("`NEW → MEMBER` is member-side state produced by the transition; there is
nothing extra for a joiner to emit").

**Honest caveat on the utility read.** The DCL `SHOW CLUSTER` *utility* is
directly readable only on a **non-completing** join, because a completing
transition floods the peer's operator console (OPCOM overrun, §4e.1) and the
utility never renders — which is precisely why the CSB is read in SDA. So the
`MEMBER` conclusion for a completing OVMX join rests on the SDA CSB (the
utility's own data source) carrying the member-identical `02060002` flags, not
on a direct utility `MEMBER` print for OVMX. The residual **rejoin does not
reliably complete** (`XITDONE=0`) is the separately-tracked
rejoin/reject-storm problem (§1h, `vms-2f3`), not a `SHOW CLUSTER` display
step.

**Disposition.** REFUTATION of the display-gap framing — no code change.
`NEW` is the accurate rendering of an incomplete transition; a completing join
reaches the peer's CSB `member` (byte-identical to a real member). `vmsscs|scs`
ctest 50/50 at HEAD (unchanged, no code touched). Clean-room: subject/peer
state from our SDA + our captures; CSB-state semantics from the host-only Davis
transcript (page cites only). No VSI/HPE source or binary examined.

### 4(aa) RECNXINTERVAL — surviving a virtual-circuit breakage without leaving the cluster (GROUNDED transcript, `vms-c7d`)

When the local Connection Manager (module `CNXMAN`) loses its SCS connection to
a remote Connection Manager for **any reason not involving a "last gasp"
datagram**, it does **not** expel the node. It holds the node's membership and
retries the connection on a fixed cadence for a bounded period; only if every
attempt fails does it start a state transition. This is the reconnect behavior
OVMX reproduces in `src/vmsscs/scs_recnx.c` and wires into the CM in `scsd.c`.

**The CSB connectivity states (GROUNDED, transcript ch7-part02 pp. 7-23/7-24).**
Each remote CM is represented locally by a Cluster System Block whose
"connectivity" field is one of **ten** states, listed by the transcript in this
order: `NEW`, `CONNECT`, `ACCEPT`, `OPEN` ("the normal state of a CSB"),
`DISCONNECT`, `WAIT` ("connectivity has been lost … a timeout is in progress"),
`RECONNECT` ("a reconnect attempt … is in progress"), `REACCEPT`, `DEAD` (an old
incarnation), `LOCAL` (the CSB for the local CM). `enum scs_csb_conn_state`
carries these exact names. (The `vms-c7d` brief named the loss/retry states
"TIMEOUT/RECONNECTING"; those map onto the book's `WAIT`/`RECONNECT`, and the
source-of-truth hierarchy keeps the book's names.)

**The reconnect loop (GROUNDED, transcript p. 7-30).** Verbatim: "the local
Connection Manager will attempt **once a second** to establish another
connection … If all such reconnect attempts fail, and **if no other Connection
Manager has already instituted a cluster state transition**, then the local
Connection Manager starts a state transition to reconfigure the cluster." The
bounded period is "the **maximum** of the local value for `RECNXINTERVAL` and a
port dependent number supplied by the remote Connection Manager." For a **LAN**
virtual circuit that remote number "was **fixed at 16** prior to Version 5.5.
But starting with Version 5.5, the remote system supplies the value of its
**`TIMVCFAIL`** parameter." Worked example (reproduced in the unit test): local
`RECNXINTERVAL` 20, remote `TIMVCFAIL` 10 → period = `max(10,20)` = 20 s. A
"last gasp" (BUGCHECK / `SHUTDOWN.COM`, p. 7-29) is the contrast: the port
driver closes the VC at once and a transition follows immediately — no reconnect
wait.

**OVMX design choices (labeled; §5 discipline — the book publishes the RULES,
not a numeric constant for any of this).** The `RECNXINTERVAL` fallback default
(20, `OVMX_RECNXINTERVAL` override) is OVMX configuration, not a claimed VMS
invariant; the once-a-second cadence and the pre-V5.5 LAN constant 16 are
PUBLISHED and used verbatim. On period expiry OVMX **proposes** a transition and
lets the transition decide removal — it does not itself drop the member mid-loop.
The reconnect **state machine, cadence and membership-hold are LIVE** and proven
on an injected clock (`tests/vmsscs/test_scs_recnx.c`: inject a VC breakage, drive
`scs_csb_reconnect_tick` — 1/sec for `max(RECNX,remote)`, membership held, then
propose; the `OVMX_NO_RECNX_RECONNECT=1` kill switch is the fail-pre arm, RUN not
asserted). The **emission of reconnect frames onto the wire and the live
observation that a real peer restores the VC within the interval are the
admission-gated 0.4 LIVE BRACKET** — CI has no `/dev/vms` and no lab, so OVMX
invents no new reconnect wire bytes here; a re-formation reuses the existing,
validated `scs_vc` formation path. Clean-room: state/timing from the host-only
transcript (page cites only) and the public *VMScluster Systems* manual; no
VSI/HPE source or binary examined.

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
