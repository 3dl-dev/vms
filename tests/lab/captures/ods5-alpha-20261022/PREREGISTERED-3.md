# RMS on ODS-5: pre-registered expectations (written before capture)

rd vms-b9c (epic vms-810). Written 2026-10-22 before the lab-Alpha session that
produced `CONSOLE-3.txt` here (session 3, pod `ods5alpha-3`). Predictions come from public documentation (Guide
to OpenVMS File Applications, extended file specifications; DCL Dictionary,
F$PARSE / F$SEARCH / SET PROCESS/PARSE_STYLE). The capture decides; RESULTS.md
records each verdict.

Probes run through DCL's F$PARSE and F$SEARCH, which call RMS $PARSE and
$SEARCH, on an ODS-5 volume (LDA1:, files as in ods5-alpha-20261022) and an
ODS-2 volume (LDA2:), under PARSE_STYLE=EXTENDED and =TRADITIONAL.

| # | prediction |
|---|---|
| R1 | EXTENDED: `F$PARSE("c^+^+config.h")` keeps case and escapes: `LDA1:[TEST]c^+^+config.h;`. |
| R2 | TRADITIONAL: the same spec is upcased: `LDA1:[TEST]C^+^+CONFIG.H;`. |
| R3 | EXTENDED: `F$PARSE("a.b.c.d")` gives `...a^.b^.c.d;` -- the last dot starts the type, the others are escaped in the expanded string. |
| R4 | TRADITIONAL: `F$PARSE("a.b.c.d")` fails (returns ""): the second dot starts a version that is not a number. |
| R5 | `F$PARSE(spec,,,"NAME")` of `a.b.c.d` is `a^.b^.c`, `"TYPE"` is `.d`. |
| R6 | `F$SEARCH("C^+^+CONFIG.H")` finds the file and returns the stored case: `LDA1:[TEST]c^+^+config.h;1`. |
| R7 | `F$SEARCH("*.*")` returns names in directory order with `^` escapes for `+`, space (`^_`), extra dots, Latin-1 letters as `^E9`-style escapes or raw bytes, UCS-2 as `^U4E2D`. |
| R8 | A 236-character name parses; 237 characters fail F$PARSE. |
| R9 | On the ODS-2 volume, EXTENDED `F$PARSE("LDA2:[000000]c^+^+config.h")` still returns an expanded string (syntax is volume-independent), but creating the file fails with an RMS name error (`%RMS-F-FNM` or `-SYN`). |
| R10 | On the ODS-2 volume a lower-case name created under EXTENDED is stored upper case (`COPY NL: lower.txt` lists `LOWER.TXT;1`). |
| R11 | `F$PARSE("c++config.h")` (an unescaped `+` inside a quoted string) under EXTENDED is accepted as the name `c++config.h` -- RMS needs no escape for `+`, only DCL's command line does. |

## Session 4 (written after session 3, before session 4): which characters need `^`

| # | prediction |
|---|---|
| C1 | Under EXTENDED, every printable ASCII character other than letters, digits, `$`, `-` and `_` makes an UNESCAPED name fail F$PARSE, and is accepted after `^`, the expanded string keeping `^c`. |
| C2 | `^20` (space) expands to `^_`; `^5F` expands to `_`; `^E9` expands to the raw byte E9. |
| C3 | A type takes the same characters as a name: `F$PARSE("ab.t^+u")` = `...ab.t^+u;`. |
