# vms-94a: the clusterwide logical-name database pull that hung a real VAX's startup

The demo's real OpenVMS VAX V7.3 (node VAXC) **never offered `Username:`** in any of the
eight orders of the V0.7-9 visitor gate. This directory is the wire evidence of why, read
off a diagnostic re-run of that gate with the in-page L2 switch recording every 0x6007
frame byte for byte (openvmx-site run **38577678113**, case `page-order`, tag V0.7-9).

## The hang, in four measured steps

1. **Where VAXC stops.** `VAXC.console.log` ends at
   `%SECSRV-E-NOPROXYDB ... %RMS-E-FNF` and prints nothing for the remaining 39 minutes of
   the 40-minute run. The next line a V7.3 prints there is
   `%SECSRV-I-CIACRECLUDB, security server created cluster intrusion database`
   (`../vms-94a-cwlnm-oracle-20261013/dc1b-vax1-console.log`, the same volume family) --
   the creation of `LNM$CLUSTER_INTRUSION_TABLE`, a **clusterwide** logical-name table.
   The VAX is blocked on a clusterwide logical-name operation.

2. **The transaction nobody closed.** `cwlnm-frames.jsonl` (every cat-0x0b frame of the run
   and the SCA block transfers they cause, 31 frames) holds the whole exchange: OVMXA sends
   `0b/05`, VAXC answers `8b/05` and pushes `0b/06`, OVMXA REQUEST DATAs the buffer, VAXC
   sends it in one 568-byte DATA frame -- and **no `8b/04` ever goes back**. OVMXA says so
   on its own console (`OVMXA.console.log`):
   `%CNXMAN, the clusterwide logical name database was not answered: a record in it is not
   one this node can decode`. This is rd vms-db1's class of hang exactly: a real VAX whose
   request an OVMX member leaves open does not finish starting up.

3. **Why OVMX could not decode it.** `vaxc-dbpush-484.bin` is the 484 bytes that arrived.
   Its container header is well formed -- `e4 01 | 05 00 | 01 00 00 00 | 01 00 00 00 |
   02 00 00 00 | 01 00 01 00`: 484 bytes, function 5, **two** records. Its first record is
   the `$CRELNT` of `LNM$CLUSTER_TABLE` that every V7.3 pushes (compare
   `tests/cluster/host/test_cnxman_join.c`'s `lnm_db2000`, record 0, byte-for-byte the same
   structure) **minus one byte at record offset 41** -- the second byte of the pointer
   `p7 = 0x804db29c` -- with everything after it shifted down one. Two independent proofs:
   re-inserting that byte makes all nine argument pointers and both strings land exactly
   where the lab's record has them, and the SECOND record's own length byte (`0xec` = 236,
   and 20 + 228 + 236 = 484) is found at offset **247** instead of 248.

4. **Whose byte it was.** The frame was recorded by the page's L2 switch, which taps the
   hub BEFORE any OVMX port -- so the bytes were already short when they left the pcjs
   VAX's transmit path, and OVMX's codec and the capture agree about what arrived. It is
   the only corrupt frame in the run (36,766 frames; no other block transfer or 204-byte CM
   frame is malformed). **The corruption is in the demo's emulated datapath, not in OVMX**,
   and it is being taken by the demo-datapath lane.

## What this node does about it now (conductor ruling, 2026-10-14)

Refusing to decode was right; returning without answering was not. Since
`vms-94a`'s ruling the executive does both halves:

- **the member's transaction is always closed** -- the `0x8b/0x04` goes out whatever this
  node made of the records, because a member left waiting hangs, and no real answer carries
  a status field any capture grounds (so there is no failure value to write);
- **this node tells the truth at home** -- a database it could not take whole marks its
  clusterwide tables NOT SYNCHRONIZED (`vms_lnm_cluster_desynchronize`, published in the
  arena header so userspace readers see it), every clusterwide translation and create is
  refused `SS$_UNSUPPORTED` rather than answered out of a partial table, the reason is
  logged and counted, and the pull is re-armed at the next membership change.

Pinned by `test_94a_a_corrupt_database_is_answered_and_refused` (driven with
`vaxc-dbpush-484.bin` itself), by `test_lnm_cluster`, and by the negative controls
`join-lnm-db-corrupt-unanswered`, `join-lnm-db-corrupt-not-reported` and
`lnm-unsync-still-serves`.

## Files

- `cwlnm-frames.jsonl` -- the demo switch's record of the cat-0x0b exchange and its block
  transfers (31 frames). `jsonl2pcap.py <in> <out.pcap>` turns it into a libpcap file the
  lane's pcap tools read unchanged, e.g.
  `../vms-94a-cwlnm-oracle-20261013/dbpush.py trim.pcap .`
- `vaxc-dbpush-484.bin` -- the pushed database as it arrived.
- `dbparse.c` -- runs the REAL codec (`vms_cm_lnm_db_next` + `vms_cm_lnm_rec_parse`) over a
  dumped database and says which record it refuses and why. Build:
  `gcc -Isrc/kernel-core -o dbparse dbparse.c src/kernel-core/vms_cluster_codec_cm.c
  src/kernel-core/vms_cluster_codec.c`
- `VAXC.console.log`, `OVMXA.console.log` -- the two consoles of the same run.

## For the demo-datapath lane: everything measured about the deletion

Asked (2026-10-14): is there anything below the frame level, and are other VAXC frames
shifted the same way?

- **No sub-frame instrumentation.** The only instrument was the demo page's own L2 switch,
  which records each 0x6007 frame's bytes as it carries them. No VAXC memory, no XQ
  descriptor state, no pcjs-internal trace. What is below is everything there is.
- **The deletion is at a FIXED ABSOLUTE FRAME OFFSET, in two independent runs.** Both
  pushes are a single SCA block DATA frame (`op 0x0d`, `src_off 0`, one segment):

  | run | frame length | payload | deleted byte | absolute frame offset |
  |---|---|---|---|---|
  | V0.7-9 `page-order` (run 38577678113) | 568 | 484 | `0xb2`, payload offset 61 | **145** (0x91) |
  | V0.7-10 `page-order` (run 38582830937) | 1496 | 1412 | `0xb2`, payload offset 61 | **145** (0x91) |

  Same offset in a 568-byte frame and a 1496-byte frame, so it is not length-dependent and
  not a segment boundary. Both frames carry the identical 24 bytes at abs 136..159
  (`64b24d8080b24d809c4d80a0b24d80010000000000000011`), which is where the hole shows.
- **It is a DELETION, not an overwrite.** The frame keeps its declared length: everything
  after abs 145 is shifted down one byte and one junk byte appears at the end. Proof:
  re-inserting `0xb2` at payload offset 61 makes the V0.7-10 push decode completely -- six
  records, exactly the clusterwide tables a V7.3 holds, matching the container's own count
  of 6 (`dbparse` after the repair). The sender's own length accounting (container length,
  each record length, the record count) agrees with the UNCORRUPTED data, so the buffer
  VAXC meant to send was well formed.
- **Known-correct test vectors**, for a headless repro:
  - `vaxc-datafrm-received.txt` -- both frames, byte for byte, as they crossed the switch.
  - `vaxc-dbpush-repaired.txt` -- the same two payloads with the byte re-inserted. These
    decode; the received ones do not. Any datapath that carries the repaired payload
    unchanged is correct.
- **The other VAXC block transfers in those runs are 136-byte close records** (13 in the
  V0.7-9 run, 2 in the V0.7-10 run). All 15 carry the measured `rec[8:12] = 88 00 65 64`
  signature, so they are not mangled at the front -- but this lane has no published oracle
  for their bytes AROUND offset 61, so they are NOT evidence either way about a deletion
  there. If the fault is at absolute frame offset 145 regardless of content, those frames
  are 220 bytes long and reach that offset too, which makes them the cheapest place to
  look next.
- No 204-byte connection-manager frame in either run decodes to a bogus category or
  opcode (2,013 and 2,215 of them), so whatever this is, it is not mangling every frame.
