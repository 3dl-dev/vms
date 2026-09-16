# op-0x0d record codec — grounding findings (conductor decode, 2026-09-15)

Fixtures: rejoin-rebuild.pcap (307 real op-0x0d, VAX1->VAX2), sda-before-vax1.txt (148 locks).
dump_catop.py body = payload[58:190]; cat/op @ body b[8:10]=02 0d.

## SOLID (grounded, correlated to SDA)
- 304/307 op-0x0d frames' resource field correlates to a REAL VAX1 held lock by resname.
- RESOURCE @ b[48], a binary VMS resource descriptor (name + qualifier bytes; e.g. SYS$SYS_ID
  carries trailing 01 04). Length indicator ~b[47] (SYS$SYS_ID->16 == SDA Length 16).
- Header: b[8:10]=02 0d; b[12:16]=01 00 04 00 constant across frames; b[16:18]=40 02 or 20 02.

## HARD / NOT YET PINNED (iterative RE — do NOT guess; pin via twin-test round-trip)
- mode: NOT at a single offset (best b[24]=91/303≈chance). SDA "Granted at" mode does not map
  to a constant body byte -> the wire mode encoding/position is nontrivial (or is requested vs
  granted). Pin by twin-test, not correlation.
- req_lkid: the wire handle is NOT the SDA "Lock id" (naive LE/BE search: 1/304 hit, only
  SYS$SYS_ID@43). The body carries VMS LKB ADDRESSES (e.g. 0x8794da78/0x8794da80 in VCC$'s
  record) -> the "handle" is address-derived, node-local; requester-side, not SDA id.
- ENQ offsets DO NOT PORT: op-01 req_lkid@data[92]/master@96 matched 0/203 shared resnames.
  op-0x0d is its OWN layout.
- PARENT resource present for some lock classes: F11B$aSYSDSK1's record carries "DIRECTORY" at
  ~b[24] (a parent-resname field); MSCP/VCC records carry binary/address data there instead.
  -> the record is VARIABLE by lock type (parent-present flag likely governs the middle region).

## HOW TO FINISH (the twin-test forces correctness, no fabrication)
Build the op-0x0d parse/build codec; TWIN TEST: for all 307 captured bodies, decode->re-encode
-> assert byte-identical. That grounds the FULL layout structurally without needing to name
every field. Then for the SENDER's live-fill fields {resnam,mode,req_lkid,req_csid,master_lkid},
identify each offset by the twin-test's decoded value ↔ SDA/known-state correlation (resnam done;
mode/lkid via the parent-present branch + address-derivation, pinned by the round-trip).

## UPDATE 2026-09-16 — send-fields NOT VAX1-SDA-groundable (real limit, not a fail)
- resnam@b[48]: SOLID (155/156 per-class match).
- mode: does NOT correlate to SDA "Granted at" mode at ANY offset, even segmented by the 4
  structural classes (b16,b18,b22 → 156/52/52/47 frames); best ~chance (27/155). The wire mode
  is not VAX1's granted-mode-as-a-byte.
- lkid/csid handles: remote/master-side (lane: 0 hits vs VAX1 SDA Lock-id AND LKB-addr).
=> The send-field OFFSETS cannot be pinned from THIS (VAX1-only) capture — the shared value is
   only the resname. Pinning them needs the VAX2-side oracle OR the §6 OVMX-as-survivor
   comparison capture (OVMX emits for lock L; assert byte-identical to the real-VMS record for
   the SAME shared system lock — SYS$SYS_ID/F11B$/MSCP$ are node-identical).
## REVISED CODEC APPROACH (byte-identical, field-naming secondary)
- Codec grounds the op-0x0d record by BYTE-IDENTICAL round-trip against the 307 captures (the
  verbatim body is already preserved; structured getters add resnam, and the send fields are
  identified by "which overlay makes the output byte-match a real record", not by naming).
- SENDER is TEMPLATE-DRIVEN + byte-identical-tested: emit a record for a held lock, assert it
  byte-matches the captured real-VMS record for that shared lock. The twin-test forces
  correctness without needing every field named -> crash-safe (never a mis-shift).
