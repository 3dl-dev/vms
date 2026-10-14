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
