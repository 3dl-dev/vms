# ODS-5 on a real OpenVMS Alpha V8.4 system (2026-10-22)

Evidence for rd vms-7e7e (epic vms-810). Predictions were written first, in
`PREREGISTERED.md`. This file records what the real system did, prediction by
prediction, and the on-disk facts the codec now relies on.

## How it was captured

Two disposable lab pods (`ods5alpha-1`, `ods5alpha-2`, one AlphaServer ES40
each, cloned from the lab-Alpha golden image, `tests/lab-alpha/README.md`),
driven only through `ovmx-lab-console`. Both pods were deleted afterwards.

On each: `@SYS$STARTUP:LD$STARTUP`, a contiguous LD container on the ODS-5
system disk, `LD CONNECT` it as `LDA1:`, `INITIALIZE/STRUCTURE=5`,
`MOUNT/SYSTEM`, `SET PROCESS/PARSE_STYLE=EXTENDED`, create the files, list and
dump them, then `DISMOUNT` and `LD DISCONNECT`. `DUMP/HEADER` of the container
gave its LBN on `DQA0:`, and the volume was copied byte for byte out of the
pod's disk image with `dd` at that LBN.

| file | what |
|---|---|
| `CONSOLE.txt` | session 1: the whole console, login to `END-OF-CAPTURE` |
| `CONSOLE-2.txt` | session 2 |
| `ods5t.dsk` | session 1 volume `ODS5T` (2000 blocks): `[TEST]` with ten extended names |
| `ods5e.dsk` | session 1 volume `ODS5E` (2000 blocks): `INITIALIZE/STRUCTURE=5` and nothing else |
| `ods5s.dsk` | session 2 volume `ODS5S` (1000 blocks): `[S]`, the ordering probes |

md5: `ods5t.dsk` a6ad9506473840eabbf4b5a55a8f68e0, `ods5e.dsk`
c343d5e6108ab9dc1e877cd264462054, `ods5s.dsk` 6d137209c06e7bbe632efde483aef1ab.

Clean-room: observation only. Nothing was disassembled, and no VSI/HPE source,
header or binary was read.

## Results against the predictions

| # | verdict | observed |
|---|---|---|
| P1 | hit | home block `hm2_struclev` = `0x0501`, format `DECFILE11B  ` |
| P2 | hit | SCB `scb_struclev` = `0x0501` |
| P3 | partial | user files and directories: `0x0501`. The ten reserved files (INDEXF.SYS .. SECURITY.SYS, 000000.DIR) keep `0x0201` headers with the ODS-2 FI2 ident area. |
| P4 | hit | FI5 ident area, layout in R4 |
| P5 | hit, refined | the name does not move to a separate extension: it runs on contiguously past byte 76 and the map area moves down (R4) |
| P6 | hit | name type in bits 3..5 of `dir_flags`: 0x00 ODS-2, 0x08 ISO Latin-1, 0x18 UCS-2 |
| P7 | hit | case preserved on disk, records ordered case-blind (R7) |
| P8 | hit | `c++config.h` is the 11 bytes `c++config.h` in the directory and `c++config.h;1` in the header |
| P9 | hit | `a.b.c.d` stored literally; DIRECTORY shows `a^.b^.c.d` |
| P10 | hit | `é` alone is one byte (0xE9, ISL-1); `^U4E2D.txt` is UCS-2, 2 bytes per character, little-endian |
| P11 | hit | `TYPE C^+^+CONFIG.H` reads `c++config.h`; `DIRECTORY MIXEDCASE.TXT` lists `MixedCase.Txt;1` |
| P12 | hit | under `PARSE_STYLE=TRADITIONAL`, `DIRECTORY` lists every name; `TYPE c^+^+config.h` fails `%DCL-W-PARMDEL` |
| P13 | hit | `[000000]` holds the ten reserved files and the user directory, nothing new |
| Q1 | hit | Latin-1 letters fold: `é.txt` and `É2.txt` sort together, after `~.txt` |
| Q2 | **miss** | UCS-2 names do not sort after all 8-bit names. `a中.txt` sorts first in `[S]`, before `E.txt` (R7) |
| Q3 | hit, refined | all names, 8-bit and UCS-2, are ordered by their upper-cased character values (R7) |
| Q4 | hit | `COPY NL: e.TXT` when `E.txt;1` exists makes `E.txt;2` in the same record; the record keeps `E.txt` |
| Q5 | hit | `DIRECTORY ^C9.TXT` lists `é.txt;1` |

Extra observations (not pre-registered):

- DCL: `OPEN/WRITE F "c++config.h"` upcases a quoted name (`LDA1:"C++CONFIG.H".;`,
  `%RMS-F-FNM`); unquoted `c++config.h` fails `%DCL-W-NOCCAT` (`+` is DCL
  concatenation). `c^+^+config.h` works. Evidence for vms-b9c / vms-0b0.
- `MixedCase.Txt`, `README.`, `PLAIN.TXT`, `E.txt`, `z.txt`, `_.txt` are name
  type ODS-2 even when lower or mixed case. `^.txt`, `~.txt`, `a b.txt`,
  `a.b.c.d`, `c++config.h` and a 64-character name part are ISL-1.

## On-disk facts

**R1. Home block and SCB.** `hm2_struclev` and `scb_struclev` are `0x0501`.
Format string unchanged.

**R2. Reserved files.** INDEXF.SYS, BITMAP.SYS, BADBLK.SYS, 000000.DIR,
CORIMG.SYS, VOLSET.SYS, CONTIN.SYS, BACKUP.SYS, BADLOG.SYS, SECURITY.SYS keep
`0x0201` headers and the FI2 ident area (space-padded `NAME.TYP;1`). Their MFD
records have `dir_flags` 0 and `dir_verlimit` 1, as on ODS-2.

**R3. Header area.** ODS-5 headers have `fh2_idoffset` = 40 words (ident at
byte 80, straight after `fh2_highwater`) and `fh2_acoffset` = `fh2_rsoffset` =
255 when there is no ACL. `fh2_mpoffset` is 100 for names up to 44 bytes.

**R4. FI5 ident area** (offsets from the start of the ident area):

| off | size | field (DUMP/HEADER label) | observed |
|---|---|---|---|
| 0 | 1 | File name type | 0 ODS-2, 1 ISL-1, 3 UCS-2 |
| 1 | 1 | File name length | bytes of `name.type;version` (UCS-2: 2 per char) |
| 2 | 2 | Revision number | 1 for a written file, 0 for `TEST.DIR` |
| 4 | 8 | Creation date | |
| 12 | 8 | Revision date | |
| 20 | 8 | Expiration date | 0 |
| 28 | 8 | Backup date | 0 |
| 36 | 8 | Last access date | |
| 44 | 8 | Last attribute change date | |
| 52 | 8 | Extended RMS record attributes | 0 |
| 60 | 8 | File length hints: Record count | 1 for `c++config.h`; all-ones = "none specified" |
| 68 | 8 | File length hints: Data byte count | 19 for `c++config.h`; all-ones = none |
| 76 | n | File name | case preserved, NUL padded to 44 bytes |

A name longer than 44 bytes continues in place: the 73-byte
`this_is_a_long_..._of_39.header;1` gives an ident area of 76 + 74 = 150 bytes
and `fh2_mpoffset` = 40 + 75 = 115.

**R5. Directory record.** Same shape as ODS-2 (`dir_size`, `dir_verlimit`,
`dir_flags`, `dir_namecount`, name, word-aligned `{version, FID}` entries in
descending version order). The name type is in bits 3..5 of `dir_flags`
(ODS-2 0x00, ISL-1 0x08, UCS-2 0x18). `dir_namecount` is in bytes. User
entries have `dir_verlimit` 0x7FFF.

**R6. Name type choice.** A name is type ODS-2 when it is a legal ODS-2 name
ignoring case: at most 39 characters before a single dot and 39 after, drawn
from `A-Z a-z 0-9 $ - _`. Any other name whose characters are all <= U+00FF is
ISL-1. A name with a character above U+00FF is UCS-2.

**R7. Ordering.** Records are ordered by the name's characters as code points
(an ISL-1 byte is its own code point, a UCS-2 unit its 16-bit value), each
mapped to upper case, compared left to right; a name that is a prefix of
another sorts first. Session 2 `[S]`, in directory order:

```
a中.txt  E.txt  f.txt  z.txt  ^.txt  _.txt  ~.txt  é.txt  É2.txt
é中.txt  Ā.txt  ÿ.txt  中.txt
```

`z` (as `Z`, 0x5A) sorts before `^` (0x5E): the fold is to upper case. `é`
and `É` fold together. `ÿ` (U+00FF) sorts after `Ā` (U+0100), which fits its
Unicode upper case `Ÿ` (U+0178) and does not fit a byte value of 0xFF. The
fold for other characters (µ, and anything above U+00FF) was not probed; the
codec maps a-z, U+00E0..U+00FE except U+00F7, U+00FF to U+0178 and U+00B5 to
U+039C, and leaves the rest alone. That last part is an assumption, not an
observation.

**R8. Lookup.** Lookup is case-blind under the same fold:
`TYPE C^+^+CONFIG.H`, `DIRECTORY MIXEDCASE.TXT` and `DIRECTORY ^C9.TXT` all
found the lower-case files. A new version of `e.TXT` goes into the existing
`E.txt` record, which keeps its case.

## Session 3: RMS and the XQP on ODS-5 and ODS-2 (pod `ods5alpha-3`)

Predictions in `PREREGISTERED-3.md` (for rd vms-b9c), console in
`CONSOLE-3.txt`. Two LD volumes: `LDA1:` INITIALIZE/STRUCTURE=5, `LDA2:` plain
INITIALIZE (ODS-2). Probes are DCL F$PARSE / F$SEARCH, which call RMS $PARSE /
$SEARCH. The first login attempt timed out (the pod had 0.7 CPU), so the console
shows one round of commands typed at `Username:`; the real run follows the
second login.

| # | verdict | observed |
|---|---|---|
| R1 | hit | EXTENDED `F$PARSE("c^+^+config.h")` = `LDA1:[TEST]c^+^+config.h;` |
| R2 | hit | TRADITIONAL: `LDA1:[TEST]C^+^+CONFIG.H;` |
| R3 | hit | EXTENDED `F$PARSE("a.b.c.d")` = `LDA1:[TEST]a^.b^.c.d;` |
| R4 | **miss** | TRADITIONAL does not fail: `LDA1:[TEST]A^.B^.C.D;` (upcased, dots escaped) |
| R5 | hit | NAME `a^.b^.c`, TYPE `.d` |
| R6 | hit | `F$SEARCH("C^+^+CONFIG.H")` = `LDA1:[TEST]c^+^+config.h;1` |
| R7 | hit | `F$SEARCH("*.*;*")` walks the directory order: `a^_b.txt`, `a^.b^.c.d`, `c^+^+config.h`, `caf<E9>.txt` (the Latin-1 byte itself, not escaped), `libstdc^+^+.a`, `MixedCase.Txt`, `^U4E2D.txt` |
| R8 | refined | F$PARSE takes 236 and 237 characters alike (lengths 248 / 249 with `LDA1:[TEST]`); it is the create that refuses 237: `%COPY-E-OPENOUT ... -RMS-E-CRE, ACP file create failed -SYSTEM-W-BADFILENAME, bad file name syntax`. The 236-character file is created. |
| R9 | hit | on the ODS-2 volume F$PARSE expands as on ODS-5; creating `c^+^+config.h` fails `-RMS-E-CRE -SYSTEM-W-BADFILENAME`, `a.b.c.d` fails `-RMS-E-CRE -SYSTEM-W-BADFILEVER` -- the ACP (XQP), not RMS, refuses them |
| R10 | hit | on the ODS-2 volume `lower.txt` and `MixedCase.Txt` are stored `LOWER.TXT;1`, `MIXEDCASE.TXT;1`; `F$SEARCH("lower.txt")` finds `LOWER.TXT;1` |
| R11 | **miss** | `F$PARSE("c++config.h")` returns "": RMS itself needs `^+`, not only the DCL command line |

Extra: `F$PARSE("a b.txt")` (an unescaped space) gives `ab.txt;`;
`F$PARSE("^u4e2d.TXT")` gives `^U4E2D.TXT;` (hex upper-cased, the type's case
kept); `F$SEARCH("CAF^C9.TXT")` finds `caf<E9>.txt;1`.

For the ACP (rd vms-7e7e): a 237-character name on ODS-5 is SS$_BADFILENAME
(2072, `docs/oracle/alpha84-starlet-defs/SSDEF.txt`). On an ODS-2 volume the
XQP upper-cases a name and refuses ODS-5-only characters (SS$_BADFILENAME) and
extra dots (SS$_BADFILEVER, 2080). OVMX's RMS upper-cases before the ACP today,
so that ODS-2 refusal becomes reachable only when RMS passes extended names
through (rd vms-b9c).

## Session 4: which characters need `^` (pod `ods5alpha-4`)

Predictions C1-C3 at the end of `PREREGISTERED-3.md`, console in
`CONSOLE-4.txt`. Each probe is `F$PARSE("a<c>b.t")` and `F$PARSE("a^<c>b.t")`
under PARSE_STYLE=EXTENDED, default device `SYS$SYSROOT:[SYSMGR]`.

| character | unescaped | escaped `^c` |
|---|---|---|
| space | `ab.t` (the space is dropped) | `a^_b.t` |
| `! # & ' ( ) + , / : ; < = > @ [ \ ] ^ { } "`, backquote, vertical bar | fails ("") | kept as `^c` |
| `$` `-` | kept | `^` dropped: `a$b.t`, `a-b.t` |
| `~` | kept | `^` dropped: `a~b.t` |
| `_` | kept | `^_` is a SPACE: `a^_b.t` |
| `%` | kept (a wildcard) | kept as `^%` (a literal) |
| `*` | kept (a wildcard) | fails |
| `?` | becomes `%` | fails |

Hex escapes: `^20` -> `^_`, `^5F` -> `_`, `^2B` -> `^+`, `^41` -> `A`, `^61` ->
`a`, `^E9` -> the raw byte E9. A type takes escapes too: `ab.t^+u` parses,
`ab.t+u` fails. Under TRADITIONAL the same table holds with the result upper-cased
(`A^+B.T`, `A~B.T`, `A^#B.T`, `A^&B.T`; `a+b.t` still fails).

Verdicts: C1 **partly missed** (`~` needs no escape; `^` before `$ - ~` is
dropped; `^_` is a space, not an underscore; `^*` and `^?` fail); C2 hit; C3 hit.

## Session 5: the DEC C RTL's name translation (pod `ods5alpha-5`)

Predictions in `PREREGISTERED-5.md`, console in `CONSOLE-5.txt`, probe source
`TOVMS.MAR` (our own MACRO-32: decc$to_vms on eleven UNIX names with an action
routine that prints each result, decc$translate_vms on seven OpenVMS names). It
was run five times: no feature logicals; DECC$EFS_CHARSET=ENABLE; plus
DECC$EFS_CASE_PRESERVE=ENABLE; plus DECC$FILENAME_UNIX_REPORT=ENABLE; and
DECC$FILENAME_UNIX_REPORT alone.

| # | verdict | observed |
|---|---|---|
| E1 | hit | at the defaults decc$to_vms gives nothing for `bits/c++config.h` (refused, no callback) |
| E2 | **miss** | at the defaults `a.b.c.d` is refused too; there is no underscore mapping. `dir/sub/MixedCase.Txt` -> `[.DIR.SUB]MIXEDCASE.TXT` (upper-cased); `file` -> `FILE` |
| E3 | partly missed | EFS_CHARSET escapes as predicted but upper-cases: `[.BITS]C^+^+CONFIG.H`, `A^.B^.C.D`, `FOO^.TAR.GZ`, `[.INCLUDE.C^+^+.14^.2^.0.BITS]C^+^+CONFIG.H`, `A^_B.TXT`, `X~Y.H` (`~` plain) |
| E4 | hit, refined | with EFS_CHARSET, translate_vms(`[.bits]c^+^+config.h`) = `./bits/c++config.h` (escapes removed, a relative directory written `./`) |
| E5 | **miss** | EFS_CASE_PRESERVE is what keeps case in BOTH directions: `[.bits]c^+^+config.h`, `/LDA1/TEST/c++config.h.1`, `README.` |

Also observed: translate_vms keeps the version as `.N`
(`[TEST]MixedCase.Txt;2` -> `/sys$disk/test/mixedcase.txt.2`) and a null type's
dot (`README.` -> `readme.`); at the defaults it lower-cases and refuses every
name holding an escape; DECC$FILENAME_UNIX_REPORT changed nothing in these two
routines.

## Session 6: DCL under the parse style (pod `ods5alpha-6`)

Predictions in `PREREGISTERED-6.md`, console in `CONSOLE-6.txt`. A fresh LD
volume `ODS5U` was initialised `/STRUCTURE=5` on LDA1:, and the battery ran in
`LDA1:[TEST]`.

| # | verdict | observed |
|---|---|---|
| 1 | hit | at login `F$GETJPI("","PARSE_STYLE_PERM")` and `_IMAGE` are `TRADITIONAL` |
| 2 | hit | after `SET PROCESS/PARSE_STYLE=EXTENDED` both are `EXTENDED` |
| 3 | hit | `OPEN/WRITE F MixedCase.Txt` makes `MixedCase.Txt;1`; `TYPE mixedcase.txt` finds it |
| 4 | hit | `OPEN/WRITE F c^+^+config.h`; `TYPE C^+^+CONFIG.H` prints it; `DIRECTORY c^+^+config.h` lists `c^+^+config.h;1` |
| 5 | hit | `OPEN/WRITE F a.b.c` makes `a^.b.c;1` |
| 6 | hit | `DELETE c^+^+config.h;*` and `DELETE MixedCase.Txt;*` remove both |
| 7 | hit | `SET PROCESS/PARSE_STYLE=TRADITIONAL`: both items `TRADITIONAL` |
| 8 | hit | under TRADITIONAL `TYPE x^+y.h` is `%DCL-W-PARMDEL, invalid parameter delimiter - check use of special characters` then ` \^\` |
| 9 | hit | under TRADITIONAL `OPEN/WRITE F lower.txt` makes `LOWER.TXT;1` |
| 10 | hit | `SET PROCESS/PARSE_STYLE=BOGUS` is `%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling` then ` \BOGUS\` |
| 11 | **miss** | a bare `SET PROCESS/PARSE_STYLE` is accepted silently and sets `TRADITIONAL` (from EXTENDED too); `=EXT` and `=(EXTENDED)` set `EXTENDED` |

SHOW PROCESS does not show the parse style.
