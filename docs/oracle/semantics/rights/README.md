# Rights database: observations (rd vms-8d1, for vms-7d5a)

Captured on real OpenVMS VAX V7.3 and Alpha V8.4 lab nodes. Clean-room: this is
observed output only.

| file | what |
|---|---|
| `vax73.txt`, `alpha84.txt` | goldens of the service probe `tools/oracle/semantic/specs/rights.py` (gated) |
| `vax73-rightslist.txt`, `alpha84-rightslist.txt` | `capture.py --dcl`: `ANALYZE/RMS_FILE/FDL` of `SYS$SYSTEM:RIGHTSLIST.DAT`, then AUTHORIZE `ADD/IDENTIFIER SP_RDB_DUMP`, `GRANT/IDENTIFIER` to SYSTEM and SYSTEST, `DUMP/RECORDS`, then the clean-up (reference only, not gated) |

What the captures show (identical on both systems unless noted):

1. **Keys** (FDL). Indexed file, prolog 3, variable-length records, max 64 bytes.
   * Key 0 `IDENTIFIER`: one segment, `bin4` at 0, length 4, **DUPLICATES yes**. There is no second segment.
     The identifier record and its holder records share the value at offset 0.
   * Key 1 `HOLDER`: `string` at 8, length 8, DUPLICATES yes.
   * Key 2 `NAME`: `string` at 16, length 32, DUPLICATES no.
2. **Records** (DUMP/RECORDS after granting SP_RDB_DUMP to SYSTEM and SYSTEST):
   * The identifier record is 48 bytes: `+0` value (`80010013`), `+4` attributes, `+8` holder quadword 0,
     `+16` name blank-padded to 32.
   * Each holder record is 16 bytes: `+0` the identifier value, `+4` attributes, `+8` holder quadword
     (`00010004 00000000` for [1,4], then `00010007` for [1,7]).
   * In key-0 order the holder records follow their identifier record, in holder order.
   * The record after the per-node `SYS$NODE_*` identifiers is the maintenance record (64 bytes, record 1).
3. **Services** (probe, 58 cases; VAX and Alpha transcripts are identical):
   * `$ADD_IDENT`
     * Duplicate name: `SS$_DUPLNAM` (0x94).
     * Duplicate value, or a UIC value already present: `SS$_DUPIDENT` (0x222C).
     * Empty name: `SS$_IVIDENT` (0x2224).
     * A name starting with a digit, and a 32-character name, are accepted.
   * `$ADD_HOLDER`
     * Holder already present: `SS$_DUPIDENT`.
     * Unknown holder or unknown identifier: `SS$_NOSUCHID` (0x21EC).
   * `$FIND_HOLDER` / `$FIND_HELD`
     * They walk in holder / identifier order and end with `SS$_NOSUCHID`.
     * At the end the context is cleared, so a following `$FINISH_RDB` returns `SS$_IVCHAN` (0x13C) with ctx 0.
     * `$FINISH_RDB` of a zero context also returns `SS$_IVCHAN`.
   * `$GRANTID` / `$REVOKID` return `SS$_WASCLR` / `SS$_WASSET` (1 / 9) on both a first and a repeated call.
     A missing name gives `SS$_NOSUCHID`.
   * With every privilege disabled:
     * `$GRANTID` returns `SS$_NOPRIV` (0x24).
     * The rights-database writes still succeed, because the probe runs as SYSTEM and its system UIC
       has access to RIGHTSLIST.DAT by file protection.
     * A no-privilege test of the writes needs a non-system UIC (not covered here).
