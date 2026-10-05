# OpenVMS VAX V7.3 symbolic constants (oracle)

Each `<NAME>.txt` holds the `$EQU name value` lines printed on a live **OpenVMS VAX V7.3**
node (lab pod `corpuslab-1`, node VAX1, 2026-10-05) by

    $ LIBRARY/MACRO/EXTRACT=$<NAME> /OUTPUT=SYS$SCRATCH:X<NAME>.MAR SYS$LIBRARY:STARLET.MLB
    $ TYPE SYS$SCRATCH:X<NAME>.MAR

That is the node's own macro library, read through the OS's own LIBRARY command: an
observation of what the system defines for a published symbol, in the same class as
`F$MESSAGE`, `SHOW` output and wire captures. Nothing was disassembled, decompiled or
copied from VSI/HPE source or binaries (project Rule 8). Only the `$EQU` lines (symbol and
number) are kept.

Use: `tools/compat/check_oracle_constants.py` compares every literal constant the tree's
headers define against these files; `docs/oracle/constants-known-mismatch.txt` records the
values that are known to differ (rd vms-f811) and may only shrink. A name absent from
a file is not defined on VAX V7.3 (several constants in the corpus are Alpha/Itanium
additions); those need an Alpha/I64 oracle (`tests/lab-alpha`).
