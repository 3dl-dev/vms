# Oracle: the RIGHTSLIST record ($RDBDEF/$KGBDEF) on OpenVMS VAX V7.3 + Alpha V8.4

**Nodes:** VAX1 (lab-2 `vaxlab-0`, OpenVMS VAX **V7.3**), ALPHA1 (lab-Alpha `alphalab-0`,
OpenVMS Alpha **V8.4**).
**Date observed:** 2026-08-17.
**Item:** vms-db8 (RIGHTSLIST rebuild — the $RDBDEF identifier + holder record layout).
**Method (clean-room, CLAUDE.md Rule 8):** `ANALYZE/RMS_FILE/FDL` for geometry and
`DUMP/RECORDS SYS$SYSTEM:RIGHTSLIST.DAT` for on-disk bytes, correlated with
`AUTHORIZE> SHOW/IDENTIFIER` and with the public `$RDBDEF`/`$KGBDEF` symbols in the *OpenVMS
Guide to System Security*. Records with a **RESOURCE attribute** and a **holder** were created
on purpose (`ADD/IDENTIFIER`, `GRANT/IDENTIFIER`) so both record kinds could be observed. **No
disassembly, no VSI/HPE source.** This supersedes nothing in `docs/oracle/vax73-rights-database.md`
(which deliberately did *not* observe the on-disk bytes); it **adds** the byte layout that doc
left open.

---

## 0. Result: architecture-invariant

RIGHTSLIST is a **Prolog-3 indexed file** with identical geometry, keys, and record byte layout
on VAX V7.3 and Alpha V8.4. **No divergence** — recorded as a measured result.

## 1. File geometry + keys — `ANALYZE/RMS_FILE/FDL`

`ANALYZE/RMS_FILE/FDL SYS$SYSTEM:RIGHTSLIST.DAT` — indexed, Prolog 3, three keys (same on both
nodes):

| Key | Name | TYPE | SEG0_POSITION | SEG0_LENGTH | DUPLICATES |
|---|---|---|---|---|---|
| 0 (primary) | IDENTIFIER | bin4 (longword) | 0 | 4 | no |
| 1 | HOLDER | string | 8 | 8 | yes (null key, null=0) |
| 2 | NAME | string | 16 | 32 | no (null key, null=0) |

Three keys ⇒ RIGHTSLIST is queried three ways: by identifier **value** (key 0), by **holder**
(key 1 — "what does this UIC hold?"), and by identifier **name** (key 2 — the `%X…`/name lookup).

## 2. Two record kinds, one structure

RIGHTSLIST stores **one physical record shape** with three keys; two logical kinds are
distinguished by whether the HOLDER field (offset 8) is zero:

### 2a. Identifier-definition record — 48 bytes (0x30), HOLDER = 0

`DUMP/RECORDS SYS$SYSTEM:RIGHTSLIST.DAT`, VAX V7.3. Environmental identifier `DIALUP`:
```
 20202020 20202020 20205055 4C414944 00000000 00000000 00000000 80000002   ("..DIALUP")  000000
 …20202020 (name padding) …                                                              000020
```
Purpose-built identifier `OVMXRES` (created `ADD/IDENTIFIER OVMXRES /ATTRIBUTES=RESOURCE`):
```
 20202020 20202020 20534552 584D564F 00000000 00000000 00000001 80010003   ("..OVMXRES")  000000
```
Layout (little-endian within each longword):

| Offset | Width | Field | DIALUP | OVMXRES |
|---|---|---|---|---|
| 0x00 | 4 | **RDB$L_IDENTIFIER** (identifier value, key 0) | `0x80000002` | `0x80010003` |
| 0x04 | 4 | **attribute flags** (`$KGBDEF`) | `0x00000000` | `0x00000001` (RESOURCE) |
| 0x08 | 8 | HOLDER (key 1; **0** for a definition record) | 0 | 0 |
| 0x10 | 32 | **RDB$T_NAME** (identifier name, key 2) | `"DIALUP"` | `"OVMXRES"` |

- **Attribute flags @0x04** are the `$KGBDEF` bitfield: `KGB$V_RESOURCE` = **bit 0**,
  `KGB$V_DYNAMIC` = bit 1, etc. OVMXRES shows `0x00000001` and `SHOW/IDENTIFIER OVMXRES` prints
  `RESOURCE` — the bit and the label agree. Environmental identifiers carry `0x00000000` (no
  attributes), consistent with `vax73-rights-database.md` §4.
- Record length 48 = 0x10 (id + attr + holder) + 0x20 (name).

### 2b. Holder record — 16 bytes (0x10), HOLDER ≠ 0

Created `GRANT/IDENTIFIER OVMXRES A1ORA /ATTRIBUTES=RESOURCE` (A1ORA = UIC `[3777,1]`). The
resulting 16-byte record:
```
 00000000 07FF0001 00000001 80010003   offset 000000
```
| Offset | Width | Field | Value |
|---|---|---|---|
| 0x00 | 4 | RDB$L_IDENTIFIER (which identifier is held) | `0x80010003` (OVMXRES) |
| 0x04 | 4 | grant attribute flags (`$KGBDEF`) | `0x00000001` (RESOURCE) |
| 0x08 | 8 | **HOLDER** (key 1) = holder UIC as 8 bytes | `0x0000000007FF0001` = `[3777,1]` |

A holder record has **no NAME field** (it stops at 16 bytes); the name lives only in the
definition record. The primary key (identifier value) is shared between the definition record
and every holder record for that identifier — key 0 has `DUPLICATES no`, but the holder records
are reached through key 1 (HOLDER), so the RMS uniqueness is on the {value,holder} pairing in
practice.

## 3. Identifier-value encoding — from the dumped values

Correlating the dumped `RDB$L_IDENTIFIER` longwords with `SHOW/IDENTIFIER`:

| Kind | Bit 31 | Encoding | Examples observed |
|---|---|---|---|
| **UIC identifier** | 0 | `(group << 16) \| member` | A1ORA `[3777,1]` = `0x07FF0001`; A2ORA `0x07FF0002` |
| **Environmental** | 1 | `0x8000_00xx` | DIALUP `0x80000002`, INTERACTIVE `0x80000003`, LOCAL `0x80000004` |
| **Facility/general** | 1 | `0x8001_xxxx`, `0x96EE_xxxx`, … | SYS$NODE_VAX1 `0x80010000`, OVMXRES `0x80010003`, SECSRV$CLIENT `0x96EE0001` |

`0x07FF0001` decodes as group `0x7FF` = 2047 = **3777 octal**, member `0x0001` — exactly the
`/UIC=[3777,1]` given. Bit 31 set marks a non-UIC (environmental/general) identifier, matching
the `%X8xxxxxxx` rendering `AUTHORIZE` uses.

## 4. The `$$MAINTENANCE_RECORD` (special metadata record)

Record 1 of RIGHTSLIST is a **64-byte** record named `$$MAINTENANCE_RECORD`, identifier value
`0x80010004`:
```
 45525F45 434E414E 45544E49 414D2424 00000000 00000000 00000000 00000000   ("$$MAINTENANCE_RE")
 80010004 00BA9055 AF08EE40 00000101 20202020 … 44524F43                    ("CORD" … + a date qword + 0x0101)
```
It carries an extra 16 bytes beyond a normal identifier record: a VMS **quadword date**
(`0x40EE08AF5590BA00`) and a `0x0101` version/type pair. OVMX should reproduce a
`$$MAINTENANCE_RECORD` so a real VMS `AUTHORIZE` accepts an OVMX-written RIGHTSLIST; its exact
extra-field semantics beyond "name + a maintenance date" are **not pinned by these observations —
treat the date/flag sub-fields as documented-shape, OVMX-generated**.

## 5. Alpha cross-check

Same `DUMP/RECORDS` on Alpha V8.4: identifier records are the same 48-byte
`{value@0, attr@4, holder@8, name@16}` shape (A1ORA `0x07FF0001` name@0x10; DIALUP
`0x80000002`), same 3-key Prolog-3 geometry. **No width or offset divergence** — the
identifier value stays a longword and the holder stays 8 bytes on the 64-bit machine.

## 6. What is *not* pinned (OVMX design latitude)

- The `$$MAINTENANCE_RECORD` date/flag sub-fields (see §4).
- The exact `$KGBDEF` bit assignments beyond `RESOURCE`(0)/`DYNAMIC`(1) were not each exercised
  here; ground the remaining attribute bits against the published `$KGBDEF`, not against a
  guessed decode.
