# Oracle: RMS Prolog-3 indexed-file geometry (OpenVMS VAX V7.3 + Alpha V8.4)

**Nodes:**
- VAX1 — lab-2 replica `vaxlab-0`, OpenVMS VAX **V7.3**, SIMH MicroVAX 3900.
- ALPHA1 — lab-Alpha replica `alphalab-0`, OpenVMS Alpha **V8.4**, AXPbox AlphaServer ES40.
**Date observed:** 2026-08-17.
**Item:** vms-8438 (RMS Prolog-3 indexed-file geometry for the OVMX indexed-file engine).
**Method (clean-room, CLAUDE.md Rule 8):** documented tool output only —
`ANALYZE/RMS_FILE/FDL`, `ANALYZE/RMS_FILE/INTERACTIVE`, and `DUMP/BLOCKS` observed on the
live reference systems, plus the public *OpenVMS Record Management Utilities Reference Manual*
(ANALYZE/RMS_FILE structure descriptions) and the *Guide to OpenVMS File Applications*
(indexed-file / Prolog 3 concepts). **No disassembly, no VSI/HPE source.** The subject file is
`SYS$SYSTEM:SYSUAF.DAT`, a production Prolog-3 indexed file present on every OpenVMS system.
Every claim below cites the exact command that produced it.

---

## 0. Summary of the geometry

`SYS$SYSTEM:SYSUAF.DAT` is a **Prolog 3** (`PROLOG 3`) indexed file: multi-key, variable-length
records, key/record/index compression on the primary key. The prologue carries a fixed prolog
block, an array of **area descriptors** (64 bytes each), and an array of **key descriptors**
(one per key, each in its own VBN). Buckets carry a 14-byte header followed by records; the
primary data record leads with a record-control-flags byte and an RRV (record reference vector)
entry; secondary keys store **SIDR** (secondary index data record) buckets.

**VAX vs Alpha divergence: none in the on-disk record/key/bucket format.** Both are Prolog 3,
identical key definitions, identical bucket-header shape. The only differences are
file-*creation* policy (VAX SYSUAF was built with **3 areas**, the Alpha SYSUAF with **1 area**;
the Alpha FDL carries an `FDL_VERSION 02` line and `GLBUFF_*_V83` keywords the VAX one lacks).
Those are area-layout / FDL-dialect choices, **not** a format difference — a Prolog-3 reader is
identical on both. Recorded as a result per Rule 8.

---

## 1. File- and record-level geometry — `ANALYZE/RMS_FILE/FDL`

Method (both nodes):
```
$ ANALYZE/RMS_FILE/FDL SYS$SYSTEM:SYSUAF.DAT
$ TYPE SYSUAF.FDL
```

| Property | VAX V7.3 | Alpha V8.4 |
|---|---|---|
| ORGANIZATION | indexed | indexed |
| RECORD FORMAT | variable | variable |
| RECORD SIZE (max) | **1412** | **1412** |
| BLOCK_SPAN | yes | yes |
| CARRIAGE_CONTROL | none | none |
| BUCKET_SIZE (file) | 3 | 3 |
| Areas | **3** (0,1,2) | **1** (0) |
| KEY 0 PROLOG | **3** | **3** |

The 1412-byte maximum record and `variable` format are the RMS envelope the UAF record lives in
(the observed UAF records are 644 bytes; see `vax73-alpha84-uafdef.md`).

## 2. Key descriptors — `ANALYZE/RMS_FILE/FDL` + `/INTERACTIVE`

Four keys, identical positions/lengths on both architectures:

| Key | Name | TYPE | SEG0_POSITION | SEG0_LENGTH | DUPLICATES | Compression |
|---|---|---|---|---|---|---|
| 0 (primary) | Username | string | 4 | 32 | no | idx+key+data (all on) |
| 1 | UIC | bin4 (uns. longword) | 36 | 4 | yes | none |
| 2 | Extended User Identifier | bin8 | 36 | 8 | yes | none |
| 3 | Owner Identifier | bin8 | 44 | 8 | yes (null key, null=0) | none |

The on-disk **KEY DESCRIPTOR** blocks, from `ANALYZE/RMS_FILE/INTERACTIVE` (`DOWN` → RMS FILE
ATTRIBUTES → `DOWN` → FIXED PROLOG → `DOWN KEYS`), VAX V7.3:

```
KEY DESCRIPTOR #0 (VBN 1, offset %X'0000')
        Next Key Descriptor VBN: 2, Offset: %X'0000'
        Index Area: 1, Level 1 Index Area: 1, Data Area: 0
        Root Level: 1
        Index Bucket Size: 3, Data Bucket Size: 3
        Root VBN: 10
        Key Flags: KEY$V_DUPKEYS 0 | KEY$V_IDX_COMPR 1 | KEY$V_INITIDX 0
                   KEY$V_KEY_COMPR 1 | KEY$V_REC_COMPR 1
        Key Segments: 1   Key Size: 32   Minimum Record Size: 36
        Index Fill Quantity: 1536, Data Fill Quantity: 1536
        Segment Positions: 4   Segment Sizes: 32
        Data Type: string   Name: "Username"
        First Data Bucket VBN: 4

KEY DESCRIPTOR #1 (VBN 2, offset %X'0000')
        Next Key Descriptor VBN: 2, Offset: %X'0066'
        Index Area: 2, Level 1 Index Area: 2, Data Area: 2
        Root Level: 1   Index/Data Bucket Size: 2   Root VBN: 15
        Key Flags: KEY$V_DUPKEYS 1 | KEY$V_CHGKEYS 1 | KEY$V_NULKEYS 0 | KEY$V_IDX_COMPR 0 | KEY$V_KEY_COMPR 0
        Key Size: 4   Minimum Record Size: 40   Segment Positions: 36   Segment Sizes: 4
        Data Type: unsigned longword   Name: "UIC"   First Data Bucket VBN: 13
```

Observed facts to build against:
- **Key descriptors are one-per-VBN**: key 0 at VBN 1, key 1 at VBN 2 (`Next Key Descriptor VBN`
  chains them; multiple descriptors can share a VBN via the byte offset — key 2 is at VBN 2
  offset `%X'0066'`, i.e. **key-descriptor record length = 0x66 = 102 bytes**).
- **Root VBN** and **First Data Bucket VBN** are stored per key (key0: root 10, first data 4).
- **Key flags** are a bitfield: `KEY$V_DUPKEYS`(0), `KEY$V_CHGKEYS`(1), `KEY$V_NULKEYS`(2),
  `KEY$V_IDX_COMPR`(3), `KEY$V_INITIDX`(4), `KEY$V_KEY_COMPR`(6), `KEY$V_REC_COMPR`(7).

## 3. Fixed prolog + area descriptors — `ANALYZE/RMS_FILE/INTERACTIVE`

`DOWN` (FILE ATTRIBUTES) → `DOWN` (FIXED PROLOG), VAX V7.3:
```
FIXED PROLOG
        Number of Areas: 3, VBN of First Descriptor: 3
        Prolog Version: 3
```
The **area-descriptor array starts at VBN 3**. `DOWN AREAS` then `NEXT` walks it; the descriptors
are **0x40 (64) bytes apart** (`AREA DESCRIPTOR #0` at VBN 3 offset `%X'0000'`, #1 at `%X'0040'`,
#2 at `%X'0080'`), so **area-descriptor size = 64 bytes**. Each carries:
```
AREA DESCRIPTOR #0 (VBN 3, offset %X'0000')
        Bucket Size: 3
        Reclaimed Bucket VBN: 0
        Current Extent Start: 28, Blocks: 9, Used: 3, Next: 31
        Default Extend Quantity: 3
        Total Allocation: 18
```
(fields: bucket size, reclaimed-bucket VBN, current extent {start, blocks, used, next},
default extend quantity, total allocation).

## 4. Bucket header — `ANALYZE/RMS_FILE/INTERACTIVE`, `DOWN … DOWN DATA`

Primary-key **data** bucket (key 0, VBN 4), VAX V7.3:
```
BUCKET HEADER (VBN 4)
        Check Character: %X'4B'
        Key of Reference: 0
        VBN Sample: 4
        Free Space Offset: %X'03B4'
        Free Record ID: 11
        Next Bucket VBN: 28
        Level: 0
        Bucket Header Flags: BKT$V_LASTBKT 0
```
Primary-key **index** (root) bucket (key 0, VBN 10):
```
BUCKET HEADER (VBN 10)
        Check Character: %X'02'
        Key of Reference: 0    VBN Sample: 10
        Free Space Offset: %X'002E'    Free Record ID: 1
        Next Bucket VBN: 10    Level: 1
        Bucket Header Flags: BKT$V_LASTBKT 1 | BKT$V_ROOTBKT 1
        Bucket Pointer Size: 2
        VBN Free Space Offset: %X'05F5'
```

Observed bucket-header facts:
- **Records begin at bucket offset `%X'000E'` (14 bytes in)** — confirmed because the first
  record in VBN 4 is reported at `offset %X'000E'` (§5). So the bucket header is **14 bytes**.
- Header fields in order (as labelled): check character (1 byte), key-of-reference, VBN sample,
  **free space offset** (next-free byte within the bucket), **free record ID** (next ID to hand
  out), **next bucket VBN** (horizontal chain at this level), **level** (0 = data), flags
  (`BKT$V_LASTBKT` bit 0, `BKT$V_ROOTBKT` bit 1).
- **Index** buckets additionally expose **Bucket Pointer Size** (2 bytes here — index entries use
  2-byte VBN pointers) and a **VBN Free Space Offset**.

## 5. Record formats within a bucket

**Primary data record + RRV** (key 0, VBN 4, `DOWN` into records), VAX V7.3:
```
PRIMARY DATA RECORD (VBN 4, offset %X'000E')
        Record Control Flags:
                (2) IRC$V_DELETED 0 | (3) IRC$V_RRV 0 | (4) IRC$V_NOPTRSZ 0
                (5) IRC$V_RU_DELETE 0 | (6) IRC$V_RU_UPDATE 0
        Record ID: 10
        RRV ID: 10, 4-Byte Bucket Pointer: 4
        Key:  06 00 42 41 52 4F 4E 20 …   ("..BARON ")   [front/rear compressed]
```
- Each data record leads with a **record-control-flags** byte (`IRC$V_DELETED` bit 2,
  `IRC$V_RRV` bit 3, `IRC$V_NOPTRSZ` bit 4, `IRC$V_RU_DELETE` bit 5, `IRC$V_RU_UPDATE` bit 6),
  a **Record ID**, and an **RRV entry** (RRV ID + a bucket pointer whose width is 2 or 4 bytes;
  here 4-byte). The key follows, front/rear compressed because `KEY$V_KEY_COMPR`/`DATA_KEY_COMPRESSION`
  are on for key 0.

**Index record** (key 0 root, VBN 10):
```
INDEX RECORD (VBN 10, offset %X'000E')
        2-Byte Bucket Pointer: 4
        Key: 0C 00 4D 41 49 4C 24 53 45 52 56 45 52 …  ("MAIL$SERVER") [compressed]
```
- An index record is a **{bucket-pointer, high-key}** pair; pointer width matches the header's
  Bucket Pointer Size (2 bytes here), key is compressed when `KEY$V_IDX_COMPR` is on.

**SIDR — secondary index data record** (key 1 UIC, data bucket VBN 13):
```
SIDR RECORD (VBN 13, offset %X'000E')
        Key: 00 01 00 04     (UIC value [1,4] = SYSTEM)
```
Raw `DUMP/BLOCKS=(START:13,END:13)` of the SIDR bucket (VBN 13), first 0x30 bytes, VAX V7.3:
```
 00010004 000B0100 0000000D 00010096 000D0107   offset 000000
 …0080000B 00000004 00028200 01000800 0B000000 04000502…
```
The SIDR bucket header (14 bytes) is followed by SIDR records; each SIDR is `{key value, then an
array of pointers to the primary records that carry that key value}`. For a non-duplicate
secondary key each SIDR points to one primary RFA; the pointer array grows for duplicate values.
The pointer encoding (RFA vs bucket pointer, and the per-entry control byte) is visible in the
raw bytes above but **ANALYZE only labels the SIDR key**, so the exact per-pointer sub-layout is
**observed as raw bytes, documented for shape from the RMS Utilities manual, not individually
labelled by the tool** — flagged so OVMX grounds the pointer sub-fields against the manual, not
against a guessed decode.

## 6. What is *not* pinned here (OVMX design latitude)

- **Bucket check-character algorithm.** The header check character is observed (0x4B data,
  0x02 index) but ANALYZE does not publish how it is computed. OVMX must reproduce *a* valid
  check byte RMS accepts; the generator polynomial is **not pinned — OVMX design choice** unless
  a later observation pins it.
- **Compression byte-stream.** That key/record compression is *on* is pinned; the exact
  front/rear-compression byte encoding is only partially visible in the dumps and should be
  grounded against the RMS Utilities manual, not inferred from these samples alone.
- **Prologue slack.** VBN 1/2 slack after the key descriptors contained stale data (unrelated
  freed-block contents); RMS does not zero it. OVMX need not reproduce slack contents.
