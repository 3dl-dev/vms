# ODS-5 oracle capture: pre-registered expectations (written before capture)

rd vms-7e7e (epic vms-810). Written 2026-10-22, before the lab-Alpha session
that produced `CONSOLE.txt` in this directory. Each line is a prediction from
public documentation (OpenVMS System Manager's Manual and Guide to OpenVMS
File Applications, ODS-5 chapters; the public Files-11 description). The
capture decides. Where the capture disagrees, the code follows the capture and
`RESULTS.md` records the miss.

Procedure: an LD container on the ODS-5 system disk, `INITIALIZE/STRUCTURE=5`,
mount it, `SET PROCESS/PARSE_STYLE=EXTENDED`, create files with extended names,
then `DIRECTORY/FULL`, `DUMP/HEADER` and `DUMP/DIRECTORY` on each.

| # | prediction |
|---|---|
| P1 | Home block structure-level word is `0x0501` (level 5, version 1). Format string stays `DECFILE11B  `. |
| P2 | SCB structure-level word is `0x0501`. |
| P3 | File headers of files created on the ODS-5 volume carry structure level 5 in `FH2$W_STRUCLEV`. |
| P4 | The ident area of an ODS-5 header is a different layout from ODS-2's FI2: it carries a name-type/control byte and a name-length byte ahead of the name, and the name is stored case-preserved, not upper-cased and not space-padded to a fixed width. |
| P5 | A name longer than the ident area's in-place name field continues elsewhere in the header (a name-extension area), up to the 236-character limit. |
| P6 | Directory records keep the ODS-2 shape (size, version limit, flags, name count, name, then {version, FID} entries) and encode the name type in `DIR$B_FLAGS`. An 8-bit (ISO Latin-1) name and a UCS-2 name are distinguished there. |
| P7 | Directory names are stored case-preserved (`c++config.h`, `MixedCase.Txt`), and the directory is ordered case-blind, so `MixedCase.Txt` sorts as `MIXEDCASE.TXT` would. |
| P8 | `c++config.h` is stored as the 11 bytes `c++config.h` -- no `^` escapes on disk; the escape is a DCL/RMS parse-time convention only. |
| P9 | A multi-dot name `a.b.c.d` is stored with all its dots literally; the last dot starts the type. |
| P10 | A name containing a character above U+00FF is stored UCS-2 (two bytes per character); `é` (U+00E9) alone stays one byte (ISO Latin-1). |
| P11 | `DIRECTORY/FULL` shows the name as created (case preserved) with `PARSE_STYLE=EXTENDED`, and the same file is still reachable by a different-case spelling (case-blind lookup). |
| P12 | With `PARSE_STYLE=TRADITIONAL`, a name like `c++config.h` cannot be typed without `^` escapes, but `DIRECTORY` still lists it. |
| P13 | Reserved files are the same ten as ODS-2 plus nothing new visible in `[000000]`. |

## Session 2 (written after session 1, before session 2)

Session 1 showed directory records ordered case-blind with the one UCS-2 name
last. Session 2 probes the ordering and lookup rules the ACP needs.

| # | prediction |
|---|---|
| Q1 | ISO Latin-1 letters fold case too: `é` (E9) and `É` (C9) compare equal, so `É2.txt` sorts next to `é.txt`, after every ASCII `E`/`F` name. |
| Q2 | All UCS-2 names sort after all 8-bit (ODS-2 and ISL-1) names, whatever their characters. |
| Q3 | Among UCS-2 names, order follows the 16-bit character value. |
| Q4 | Creating `e.TXT` when `E.txt;1` exists makes `;2` in the SAME directory record (case-blind match); the record keeps the case of the first creation. |
| Q5 | `DIRECTORY ^C9.TXT` finds `é.txt` (Latin-1 case-blind lookup). |
