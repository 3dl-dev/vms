# Oracle: the SYSUAF record ($UAFDEF) on OpenVMS VAX V7.3 + Alpha V8.4

**Nodes:** VAX1 (lab-2 `vaxlab-0`, OpenVMS VAX **V7.3**), ALPHA1 (lab-Alpha `alphalab-0`,
OpenVMS Alpha **V8.4**).
**Date observed:** 2026-08-17.
**Item:** vms-db8 (SYSUAF/RIGHTSLIST rebuild — the $UAFDEF record layout).
**Method (clean-room, CLAUDE.md Rule 8):** `DUMP/RECORDS` of `SYS$SYSTEM:SYSUAF.DAT`
(observing on-disk bytes = observing tool output, allowed) correlated with
`AUTHORIZE> SHOW` and with the public `$UAFDEF` symbols documented in the *OpenVMS
Guide to System Security* and `SYS$LIBRARY:STARLET`. Field **offsets were positively located by
controlled edit**, not by memory: throwaway accounts were created with known values and the
records diffed. **No disassembly, no VSI/HPE source.**

The RMS envelope (Prolog-3 indexed, keys, buckets) is in `vax73-alpha84-rms-prolog3.md`. This
file is about the **644-byte logical UAF record** those buckets carry.

---

## 0. Result: the record layout is architecture-invariant

Every field below sits at the **same byte offset on VAX V7.3 and Alpha V8.4**, same widths,
same key definitions. The UAF record is **644 bytes** on both (within the 1412-byte RMS
maximum). **No VAX-vs-Alpha divergence** — recorded as a measured result, not assumed.

## 1. Keys (from the RMS key descriptors — see the RMS doc)

| Key | Field | Record offset | Width | Role |
|---|---|---|---|---|
| 0 (primary) | UAF$T_USERNAME | **0x04** (4) | 32 | username, blank-padded, ASCII |
| 1 (secondary) | UAF$L_UIC | **0x24** (36) | 4 | UIC longword (member word + group word) |
| 2 | Extended User Identifier | 0x24 (36) | 8 | overlays UIC as an 8-byte identifier |
| 3 | Owner Identifier | 0x2C (44) | 8 | null key (null value 0) |

## 2. The record head — `DUMP/RECORDS/COUNT=1`, correlated with `SHOW`

Throwaway account created with `AUTHORIZE> ADD A1ORA /PASSWORD=KNOWNPW12 /UIC=[3777,1]`.
`DUMP/RECORDS/COUNT=1 SYS$SYSTEM:SYSUAF.DAT` (VAX; account sorts first so it is record 1):

```
Record number 1, 644 (0284) bytes, RFA(0004,0000,000B)
 …20202041 524F3141 00000101   offset 000000   ("..A1ORA")
 …00000000 07FF0001 20202020   offset 000020
```
Decoded (VMS DUMP shows longwords low-address-rightmost, little-endian within each):

| Offset | Bytes (A1ORA) | Field | Pinned by |
|---|---|---|---|
| 0x00 | `01` | UAF$B_RTYPE (record type = 1) | value observed |
| 0x01 | `01` | UAF$B_VERSION (= 1) | value observed |
| 0x02–0x03 | `00 00` | reserved / word | value observed (not separately exercised) |
| 0x04–0x23 | `41 31 4F 52 41 20…` | **UAF$T_USERNAME** = "A1ORA" + spaces | key 0; ASCII matches `SHOW` |
| 0x24–0x25 | `01 00` | **UAF$W_MEM** = 0x0001 (member) | UIC [3777,**1**] from `SHOW` |
| 0x26–0x27 | `FF 07` | **UAF$W_GRP** = 0x07FF (group = 3777 octal) | UIC [**3777**,1] from `SHOW` |

`UAF$L_UIC` @ 0x24 = `0x07FF0001` = `[3777,1]` — exactly the `/UIC` given. String fields follow
(all blank-padded, self-identifying against `SHOW`): default device `SYS$SYSDEVICE:` and
directory `[USER]` around 0x60–0x90, LGICMD `LOGIN` ~0xC0, CLI `DCL` ~0x100, command tables
`DCLTABLES` ~0x120. (These are counted/blank-padded strings; their exact sub-offsets are not
load-bearing for authentication and are left to the $UAFDEF documentation.)

## 3. The password area — offsets pinned by controlled edit (the load-bearing result)

`UAF$Q_PWD` was located **positively**, not from memory: A1ORA's password was changed with
`AUTHORIZE> MODIFY A1ORA /PASSWORD=NEWPWXY34` and the record re-dumped. **Exactly one 8-byte
region changed** — record offset **0x154**:

```
before (KNOWNPW12):  offset 0x154 = 59 1C 07 3C C0 BD 6C 71   (Q = 0x716CBDC03C071C59)
after  (NEWPWXY34):  offset 0x154 = 41 18 DC FF EE 33 B5 01   (Q = 0x01B533EEFFDC1841)
```

Comparing A1ORA vs a second account A2ORA (same password `KNOWNPW12`, different username/UIC)
isolated the **salt** and confirmed the **algorithm byte**:

```
                     0x154 (PWD quad)          0x166 (salt)  0x168 (encrypt)
A1ORA  KNOWNPW12     716CBDC0 3C071C59          4D63          03
A2ORA  KNOWNPW12     84A8F6D1 35BE8B58          4EE2          03
```

The $UAFDEF password block, byte-confirmed on VAX V7.3 **and** Alpha V8.4 (same offsets):

| Field | Record offset (hex / dec) | Width | Observed | How pinned |
|---|---|---|---|---|
| **UAF$Q_PWD** | **0x154 / 340** | 8 | hashed password quadword | changed iff password changed (MODIFY diff) |
| **UAF$W_SALT** | **0x166 / 358** | 2 | 0x4D63 (A1ORA), 0x4EE2 (A2ORA) | differs per account (A1ORA/A2ORA diff) |
| **UAF$B_ENCRYPT** | **0x168 / 360** | 1 | **0x03 = PURDY_S** | constant across accounts & both arches |
| **UAF$B_PWD_LENGTH** | **0x16A / 362** | 1 | 0x06 | equals `Pwdminimum: 6` in `SHOW` |
| **UAF$Q_PWD2** | **0x16C / 364** | 8 | 0 (no secondary password) | zero when no 2nd password set |

`UAF$B_ENCRYPT = 3` is `UAI$C_PURDY_S` (salted Purdy) — the modern default (`UAI$C_AD_II`=0,
`UAI$C_PURDY`=1, `UAI$C_PURDY_V`=2, `UAI$C_PURDY_S`=3, public $UAIDEF). See
`purdy-hash-vectors.md` for the (password, username, salt) → quadword vectors.

**Finding — the salt is stable across a password change.** Changing A1ORA's password left
`UAF$W_SALT` (0x4D63) **unchanged** while `UAF$Q_PWD` changed. So on both V7.3 and V8.4 the salt
is assigned per account and persists across `MODIFY /PASSWORD`; it is not re-drawn each time the
password is set.

## 4. Quota / flags region (correlated, not exhaustively mapped)

The dump region from ~0x200 correlates cleanly with the `SHOW` quota block, e.g.
`0x012C = 300 = Fillm`, `0x0028 = 40 = BIOlm/DIOlm/ASTlm/TQElm`, `Prclm 2`. The full quota
offset map is not required for the SYSUAF/authentication rebuild and is left to the documented
$UAFDEF; the region location is recorded so OVMX places it correctly.

## 5. Alpha cross-check (width oracle)

Same account set created and dumped on Alpha V8.4. The record is **644 bytes**, `UAF$T_USERNAME`
@0x04/32, `UAF$L_UIC` @0x24, **`UAF$Q_PWD` @0x154 (8 bytes)**, **`UAF$W_SALT` @0x166**,
**`UAF$B_ENCRYPT` @0x168 = 0x03**. The 64-bit machine stores the password hash as the **same
8-byte quadword at the same offset** as the 32-bit VAX — the width question lab-Alpha exists to
answer: **no divergence**, the UAF record is not widened on Alpha.

## 6. Method note (clean-room + trap)

Only `DUMP`, `ANALYZE/RMS_FILE`, and `AUTHORIZE SHOW` output were read — all documented tool
output. No image was disassembled or decompiled. Trap hit and recorded: `MC AUTHORIZE` failed
`%UAF-E-NAOFIL` until `DEFINE SYSUAF SYS$SYSTEM:SYSUAF.DAT` was issued (the `SYSUAF` logical was
not defined in these lab images); and `DUMP` output silently truncated under terminal paging
until `SET TERMINAL/DEVICE_TYPE=VT100/PAGE=0` — the early record bytes (username + password)
were the ones lost, so this is a correctness trap, not a cosmetic one.
