# Session 6 pre-registration: DCL under PARSE_STYLE (rd vms-0b0)

Written before the capture. Lab: one disposable ovmx-alphalab pod (ods5alpha-6),
OpenVMS Alpha V8.4, an LD-backed volume initialised /STRUCTURE=5 as in session 1.
Sources for the expectations: the OpenVMS DCL Dictionary (SET PROCESS/PARSE_STYLE,
F$GETJPI item PARSE_STYLE_PERM / PARSE_STYLE_IMAGE) and sessions 1-4 here.

| # | Command | Expected |
|---|---------|----------|
| 1 | F$GETJPI("","PARSE_STYLE_PERM") / _IMAGE at login | "TRADITIONAL" / "TRADITIONAL" |
| 2 | SET PROCESS/PARSE_STYLE=EXTENDED, then the two items | "EXTENDED" / "EXTENDED" |
| 3 | CREATE MixedCase.Txt (one line), TYPE mixedcase.txt | file named MixedCase.Txt; TYPE finds it case-blind |
| 4 | CREATE c^+^+config.h, TYPE C^+^+CONFIG.H, DIRECTORY c^+^+config.h | created as c++config.h, listed c^+^+config.h;1 |
| 5 | COPY NL: a.b.c | a^.b.c;1 (last dot starts the type) |
| 6 | DELETE c^+^+config.h;*, DELETE MixedCase.Txt;* | both gone; DIRECTORY no longer lists them |
| 7 | SET PROCESS/PARSE_STYLE=TRADITIONAL, the two items | "TRADITIONAL" / "TRADITIONAL" |
| 8 | (TRADITIONAL) TYPE x^+y.h | %DCL-W-PARMDEL \^\ (session 1 saw it for TYPE c^+^+config.h) |
| 9 | (TRADITIONAL) COPY NL: lower.txt | LOWER.TXT;1 |
| 10 | SET PROCESS/PARSE_STYLE=BOGUS | a DCL error naming the bad keyword (IVKEYW expected) |
| 11 | SET PROCESS/PARSE_STYLE (no value) | a value-required error (VALREQ expected) |
