# OpenVMS Alpha V8.4 symbolic constants (oracle)

Each `<NAME>.txt` holds `$EQU name value` lines printed on a live **OpenVMS Alpha V8.4**
node (lab pod `corpusalpha-1`, ALPHA1 under AXPbox, 2026-10-05) by

    $ LIBRARY/MACRO/EXTRACT=$<NAME> /OUTPUT=SYS$SCRATCH:X<NAME>.MAR SYS$LIBRARY:STARLET.MLB
    $ TYPE SYS$SCRATCH:X<NAME>.MAR

the node's own macro library read through its own LIBRARY command -- an observation, in
the same class as `F$MESSAGE`, `SHOW` output and wire captures. Nothing was disassembled,
decompiled or copied from VSI/HPE source or binaries (project Rule 8). Only the symbol and
number are kept. The Alpha macro library prints masks as `<^X..>`; they are normalised to
decimal here (6 lines that were not a plain number were dropped).

Use: `tools/compat/check_oracle_constants.py` compares every value constant the tree defines
against `docs/oracle/vax73-starlet-defs/` first and falls back to this directory for names
VAX V7.3 does not define (the Alpha additions: SYI$_ACTIVE_CPU_*, VA$_, CAP$, IEEE$,
PSCAN$, ...). A name on both uses the VAX value; the places the two architectures disagree
are structure sizes/offsets and `$_MAX_*` sentinels (not compared), the PSL$ layout (the
processor status word differs by architecture) and a handful of high SS$/SYI$ codes.

UAFDEF, UTCDEF and FPDEF come from `SYS$LIBRARY:LIB.MLB` (same command, other library).
