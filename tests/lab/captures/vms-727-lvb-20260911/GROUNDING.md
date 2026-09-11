# vms-727 — LVB op-06 body[32:36] capture campaign (RAW DATA, no builder decision)

**Captured 2026-09-11 on own-lab `vaxlab-4` (ns `ovmx-lab`), 2-node real OpenVMS
VAX 7.3 VMScluster (VAX1, VAX2), `F$GETSYI("CLUSTER_NODES")==2`.** Executive-backed:
every value below is read from a real captured wire frame and cross-checked against
`SDA> SHOW LOCKS` on the master node; nothing templated ([[executive-backed-not-wire-plumbing]]).

Drivers used (recovered pre-existing on the shared system disk from the vms-c03 run,
`SYS$COMMON:[SYSEXE]DLMLVB3.EXE` / `DLMENQ.EXE` — no rebuild needed this session):
- `DLMENQ <flag><mode><resname>`: places a lock at `<mode>` on `<resname>` (flag `2`
  = NOQUEUE, used here to place a non-conflicting NL holder on vax2 so vax2 masters
  the resource).
- `DLMLVB3 <modedigit><resname>`: `$ENQW` at `<mode>` with `LCK$M_VALBLK`, writes the
  16-byte pattern `WROTEBYVAX1XXXXX` at LKSB+8, then `$ENQW` convert-down to NL with
  `LCK$M_VALBLK` (the op-06 write on the wire), then `$HIBER_S`.

Method: NL holder placed on vax2 first (`SPAWN/NOWAIT/INPUT=NL: ENQ 02<resname>`);
`tcpdump -i br0 -w <pcap> -U -s 0 'ether proto 0x6007'` run on vax1's side while
`SPAWN/NOWAIT/INPUT=NL: LVB3 <modedigit><resname>` drove the scenario from vax1;
decoded with `tools/cluster/dlm_body.py` (body-offset convention: `body[0]` =
frame abs offset 72).

## 1. Variant table (body[32:36] of the op-06 CONVERT-with-VALBLK frame)

| variant | resource | mode | master_lkid | body[32:36] hex | input differed from C1 |
|---|---|---|---|---|---|
| C1 baseline | `OVMXLV01` (8-char) | EX(5) | `0x04000669` | `1b 00 01 00` | — (baseline) |
| C2 (2nd write, same res) | `OVMXLV01` (8-char) | EX(5) | `0x570001b7` | `1c 00 01 00` | fresh process, same resource/mode/namelen |
| C3 (mode) | `OVMXLV02` (8-char) | PW(4) | `0x2600066b` | `1d 00 01 00` | from-mode EX→PW |
| C4 (namelen) | `OVX` (3-char) | EX(5) | `0x0e00066d` | `1e 00 01 00` | resname length 8→3 |

**Observation (raw, minimal interpretation):** across all four variants, body[32]
advances by exactly `+1` in run order (`0x1b → 0x1c → 0x1d → 0x1e`) *regardless* of
which single input changed. Byte body[33] is `0x00` in all four. Bytes
body[34:35] are `01 00` (constant) in all four — including C2, the repeat write to
the *same* resource, where an incrementing-per-resource sequence field would have
been expected to move to `02 00` and did not.

The value at body[32] also reappears **identically** at body[52] in every op-06
frame (verified byte-for-byte: `1b`/`1b`, `1c`/`1c`, `1d`/`1d`, `1e`/`1e`), each
time followed by the same 3-byte suffix `02 20 20` — i.e. body[32:36] and
body[52:56] are not independent 4-byte fields but the same repeating 1-byte value
inside two identically-shaped 4-byte sub-records that bracket the 16-byte
value-block payload (body[36:52]).

Cross-referenced against the vms-c03 capture on main
(`tests/lab/captures/vms-c03-dlm-opcodes-20260911/dlm-lvb3-20260911.pcap`, a
*different boot session*, resource `OVMXLVB3`, 8-char, EX): that frame's
body[32:36] is `1f 00 01 00` — a different absolute value from any of C1-C4 here,
consistent with a monotonic counter whose starting point is session-relative, not
a constant derived from resource identity/mode/namelen.

## 2. The LVB-READ crossing (C2 deliverable)

Frame `f13` in `c2-seq.pcap`: **opcode 0x01 (ENQ grant response)**, direction
**VAX2 (master) → VAX1 (requester)**. This is the grant reply to C2's fresh EX+
VALBLK `$ENQW` on `OVMXLV01` — the resource block on vax2 already carried a value
block from C1's earlier write (the resource stayed alive because vax2's own NL
holder, placed by the DLMENQ driver, never released it).

- **Carries the 16-byte value block back:** YES — `body[36:52]` of f13 is
  `WROTEBYVAX1XXXXX`, byte-identical to what C1 wrote. This is real evidence VMS
  returns the resource's current LVB content in the grant response when the
  requester's `$ENQW` asked for `LCK$M_VALBLK`.
- **Sequence-like field:** f13's own body[32:36] is `01 00 fa 00` — **not** the
  same value/shape as the op-06 write's body[32:36] field (`1c 00 01 00` for the
  op-06 write that came right after it in the same capture). The grant-response
  frame's body[32] is `0x01` and its trailing 2 bytes are `fa 00`, distinct from
  the write frame's `00 01`. So whatever body[32:36] encodes, an ENQ-grant-with-
  VALBLK frame and a convert-down-VALBLK-write frame do NOT share the same
  encoding at that offset — they are field-shape-similar (single byte + framing
  byte(s)) but not the same counter/value space.
- SDA correlation: `SHOW LOCKS` on vax2 lists `Lock id: 0A0003A4` on resource
  `OVMXLV01`, `Flags: VALBLK CONVERT SYSTEM`, `Granted at NL` — byte-identical to
  the wire's local reqid `0x0a0003a4` for C2's process. C1's process similarly
  correlates: `Lock id: 2B0000A3` on `OVMXLV01`, same flags, matches wire reqid
  `0x2b0000a3`.

## 3. Captures

All under `tests/lab/captures/vms-727-lvb-20260911/`:
- `c1-baseline.pcap` — EX ENQ+VALBLK write, `OVMXLV01` (8-char), master_lkid `0x04000669`.
- `c2-seq.pcap` — 2nd write to `OVMXLV01`; also contains the LVB-READ crossing (f13).
- `c3-mode.pcap` — PW(4) ENQ+VALBLK write, `OVMXLV02` (8-char), master_lkid `0x2600066b`.
- `c4-namelen.pcap` — EX ENQ+VALBLK write, `OVX` (3-char), master_lkid `0x0e00066d`.

## What this refutes / leaves open (facts only, no builder call)

- **H-mode refuted:** EX→PW (C1→C3) produced only the expected +1 ladder step, no
  extra deviation attributable to mode.
- **H-namelen refuted:** 8-char→3-char (C3→C4) likewise only the +1 ladder step.
- **H-seq (as stated, on body[34:36]) refuted:** repeating the write on the *same*
  resource (C1→C2) left body[34:35] at `01 00` both times.
- **Open (at the C1–C4 stage):** what body[32] (and its body[52] twin) actually
  counts. Resolved by C5, below.

## C5 — persistent single-holder multi-write (`c5-persistent-multiwrite.pcap`)

One EX+VALBLK lock on `OVMXLV05` (master_lkid `0x1000067a`), held WITHOUT releasing,
its value block written THREE times (distinct 16-byte payloads `WROTE#1..`,
`WROTE#2..`, `WROTE#3..`), each followed by a convert-down flush. All three op-0x06
frames carry the SAME `body[32:36] = 1f 00 01 00`, byte for byte, despite the value
block genuinely changing each time.

- **Per-resource / per-write VALBLK-sequence: REFUTED outright.** Three real writes
  on one held lock, zero movement at body[32:36].
- Combined with C1→C4 (body[32] advancing +1 only across DISTINCT lock instances),
  **body[32] is a per-LOCK serial assigned at acquisition and constant for that
  lock's lifetime**, re-stamped at body[52] (front == back). INFERRED provenance:
  equals the low byte of the lock's ENQ request id (C1 `0x2020021b`→`1b`, C5
  `0x2020021f`→`1f`; 2/2 of the captures that expose it), and the ENQ REQUEST for
  the same lock carries the identical `body[32:36]`.

## RESOLUTION (the builder decision, vms-727)

The codec offset map settled the rest: `body[0:2]`/`body[2:4]` are the SCS
send/ack message numbers (already built by a lower layer); `body[34]` is the
RESULT_STAMP position, carrying `0x01` on a cat-0x02 REQUEST (a REPLY carries
0xfa/0xf9). So of `body[32:36]` only body[32] was ever unknown, and it is a
per-lock serial sourced from the LKB. `body[52:56] = SERIAL,02,20,20` is
consistent across all five captures (a stable field, not stale). `body[56:88]`
is inconsistent between captures (VAX P1 stack addresses `0x7ff8....`) — stale
sender buffer, NOT a field.

Decision: **BUILD IT.** `vms_dlm_valblk_convert_build` emits every op-0x06 byte
from the LKB (req_lkid/master_lkid/mode/valblk/serial) or a grounded constant,
zero-fills the stale tail, and mints nothing (INV-6). A byte-identical host test
(`test_codec_dlm.c`) proves the builder reproduces this capture set's op-0x06
frame exactly (38 cited bytes). Safety: the ENQ builder never populates
body[32:36] yet peers accept our ENQ frames, so the field is not
receiver-correctness-critical — the builder cannot bugcheck a peer
(never-crash-a-peer). The remaining SCS-scope question about body[32] (per-VC vs
per-node) does not gate the build: the value is sender-private and stamped
front==back, so any LKB-sourced value is faithful and safe.
