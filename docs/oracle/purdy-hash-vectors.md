# Oracle: Purdy password-hash test vectors (OpenVMS VAX V7.3 + Alpha V8.4)

**Nodes:** VAX1 (lab-2 `vaxlab-0`, OpenVMS VAX **V7.3**), ALPHA1 (lab-Alpha `alphalab-0`,
OpenVMS Alpha **V8.4**).
**Date observed:** 2026-08-17.
**Item:** vms-db8 (Purdy hash, for byte-exact OVMX authentication).
**Method (clean-room, CLAUDE.md Rule 8):** throwaway accounts created in `AUTHORIZE` with
**known passwords**, then `DUMP/RECORDS SYS$SYSTEM:SYSUAF.DAT` to read the resulting on-disk
`UAF$Q_PWD` quadword, `UAF$W_SALT`, and `UAF$B_ENCRYPT`. This observes tool output only. The
algorithm itself (Purdy polynomial over GF(2^64), salted) is described publicly in the
*OpenVMS Guide to System Security* ("password encryption") — that description, plus these
input→output vectors, is what OVMX's Purdy is verified against. **No disassembly, no VSI/HPE
source.** Field offsets (`UAF$Q_PWD`@0x154, `UAF$W_SALT`@0x166, `UAF$B_ENCRYPT`@0x168) are
established in `vax73-alpha84-uafdef.md`.

---

## 0. Algorithm identity across architectures

`UAF$B_ENCRYPT = 0x03 = UAI$C_PURDY_S` (salted Purdy) on **both** VAX V7.3 and Alpha V8.4, and
the hash is an **8-byte quadword** at the same record offset on both. The algorithm and output
width are architecture-invariant — a divergence would have been a result; there is none.

## 1. The hash inputs (what OVMX must feed its Purdy)

For `PURDY_S`, the hash is a function of **(password, username, salt)**:
- **password** — the plaintext, upcased, blank-padded (VMS upcases and space-pads to the
  internal field width before hashing; this is documented behaviour).
- **username** — the account name, blank-padded to 32; folded into the hash so two accounts with
  the same password get different hashes (demonstrated below: A1ORA and A2ORA share
  `KNOWNPW12` yet hash differently).
- **salt** — the 16-bit `UAF$W_SALT`, an additional per-account input.

## 2. Test vectors — VAX V7.3

Each row: created with `ADD/MODIFY … /PASSWORD=<pw>`, read back by `DUMP/RECORDS`.
`UAF$B_ENCRYPT = 0x03 (PURDY_S)` for all. The quadword is given as the little-endian on-disk
byte sequence at record offset 0x154 **and** as the assembled 64-bit value.

| # | username | password | salt (W, hex) | UAF$Q_PWD bytes @0x154 | Q (64-bit) |
|---|---|---|---|---|---|
| V1 | `A1ORA` | `KNOWNPW12` | `4D63` | `59 1C 07 3C C0 BD 6C 71` | `0x716CBDC03C071C59` |
| V2 | `A2ORA` | `KNOWNPW12` | `4EE2` | `58 8B BE 35 D1 F6 A8 84` | `0x84A8F6D135BE8B58` |
| V3 | `A1ORA` | `NEWPWXY34` | `4D63` | `41 18 DC FF EE 33 B5 01` | `0x01B533EEFFDC1841` |
| V4 | `A3ORA` | `DIFFPWXY99` | `506C` | `A4 ED 6D EB 66 23 2C 38` | `0x382C2366EB6DEDA4` |

What each pair proves:
- **V1 vs V2** — same password, different username **and** different salt → different hash.
  Confirms username and/or salt are folded in (VMS folds both).
- **V1 vs V3** — same username, same salt, different password → different hash, and **only** the
  hash quadword changed in the record (salt unchanged). Isolates `UAF$Q_PWD` and shows the salt
  is not re-drawn on a password change.

## 3. Test vectors — Alpha V8.4

Same account set, same passwords, on the 64-bit oracle. `UAF$B_ENCRYPT = 0x03 (PURDY_S)`.

| # | username | password | salt (W, hex) | UAF$Q_PWD bytes @0x154 | Q (64-bit) |
|---|---|---|---|---|---|
| A1 | `A1ORA` | `KNOWNPW12` | `F52E` | `AD D7 B5 D9 97 06 73 C0` | `0xC0730697D9B5D7AD` |
| A2 | `A2ORA` | `KNOWNPW12` | `F6D7` | `EA 1D 1D 4D E8 23 7F 78` | `0x787F23E84D1D1DEA` |
| A3 | `A3ORA` | `DIFFPWXY99` | `F834` | `75 36 98 7A 86 A9 F7 46` | `0x46F7A9867A983675` |

## 4. Reading these correctly (important caveats)

- **The VAX and Alpha quadwords differ for the same (username, password)** because the salt
  differs (V1 salt 0x4D63 vs A1 salt 0xF52E). This is **not** an architecture divergence in the
  algorithm — the salt is an auto-generated input, and `AUTHORIZE` does not let you set it. The
  algorithm identity is established by (a) identical `UAF$B_ENCRYPT`=3 and quadword width on
  both, and (b) the requirement that **OVMX's single Purdy implementation reproduce every row in
  both tables** from its (password, username, salt) inputs. If it reproduces all seven, the
  algorithm is byte-identical across the two architectures. Reproducing only one table would
  leave the cross-arch claim unproven.
- **Salt is not cryptographically random here — it is monotonic within a session.** Observed
  salts, in creation order: VAX `4D63 → 4EE2 → 506C` (steps ≈ +0x17F, +0x18A); Alpha
  `F52E → F6D7 → F834` (steps ≈ +0x1A9, +0x15D). Consistent with a time/counter-derived seed,
  not a PRNG. OVMX should **not** hard-code these particular salts; it must accept the stored
  salt as an input. The *generator* is **not pinned — OVMX design choice** (any salt OVMX writes
  is fine as long as its own hash verifies against its own stored salt; only cross-checking
  against a real VMS SYSUAF requires reading that file's stored salt, which OVMX does).

## 5. Verification contract for OVMX

An OVMX Purdy is byte-faithful iff, for each row above, `purdy(upcase_pad(password),
pad32(username), salt) == Q`. Implement over the documented GF(2^64) Purdy polynomial; verify
against all seven vectors (4 VAX + 3 Alpha) before claiming SYSUAF interop. A green unit test
that only reproduces OVMX's own output is not a VMS-authenticity proof (INV-6) — these
real-VMS vectors are.
