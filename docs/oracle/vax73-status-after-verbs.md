# Oracle: what `$STATUS` holds after a DCL command, OpenVMS VAX V7.3

**Item:** vms-390. **Node:** VAX1, OpenVMS VAX V7.3, 2026-10-21. **Method:** each command
below was typed on the console as SYSTEM and followed by `SHOW SYMBOL $STATUS`; the verbatim
transcript is `tests/lab/captures/status-battery-20261021/vax1-status-battery.txt`. Observed
tool output only (Rule 8).

## What it shows

1. **`SHOW SYMBOL` writes `$STATUS` with `==`** -- it is a global symbol, and SHOW SYMBOL
   shows any global symbol with `==` (`  $STATUS == "%X10000001"`). OVMX now does the same
   for every global symbol (rd vms-390).
2. **There is no single rule for the high bit.** `%X10000001` (STS$M_INHIB_MSG set) and
   `%X00000001` both follow successful commands, per utility: TYPE, PURGE, DELETE, DUMP, SHOW
   LOGICAL/PROCESS/DEVICE/USERS/STATUS/TERMINAL/MEMORY and SET TERMINAL/PROCESS/MESSAGE leave
   `%X10000001`; COPY, DIRECTORY, APPEND, SORT, RENAME, CREATE/DIRECTORY, DEFINE, ASSIGN,
   DEASSIGN, OPEN, DEALLOCATE, WAIT and HELP leave `%X00000001`. DCL's own commands leave
   `%X00030001` (SHOW TIME/DEFAULT/TRANSLATION/PROTECTION, SET DEFAULT/PROMPT, an assignment,
   ALLOCATE) or `%X00010001` (WRITE, CLOSE, SET PROTECTION/DEFAULT). Failures that a utility
   reported itself carry the bit (`%X1067109A` COPY, `%X10951238` TYPE, `%X10018290`
   DIRECTORY, `%X10931238` DELETE, `%X10788019` SHOW LOGICAL); DCL's own failures need not
   (`%X000001BC` DEASSIGN of a missing name; `%X00038060` IVKEYW, set-protection capture).
   So matching VMS here is a per-verb table, not a global "set the bit" switch -- tracked as
   its own item (vms-c894), not changed by vms-390.

## The table

| Command | `$STATUS` |
|---|---|
| `SET TERMINAL/NOBROADCAST` | `%X10000001` |
| `COPY NL: OVMXS.TMP` | `%X00000001` |
| `TYPE OVMXS.TMP` | `%X10000001` |
| `DIRECTORY OVMXS.TMP` | `%X00000001` |
| `APPEND NL: OVMXS.TMP` | `%X00000001` |
| `DIFFERENCES OVMXS.TMP OVMXS.TMP` | `%X006C8009` |
| `SEARCH OVMXS.TMP ZZZQ` | `%X08D78053` |
| `SORT OVMXS.TMP OVMXT.TMP` | `%X00000001` |
| `RENAME OVMXT.TMP OVMXU.TMP` | `%X00000001` |
| `PURGE OVMXS.TMP` | `%X10000001` |
| `DELETE OVMXU.TMP;*` | `%X10000001` |
| `DUMP/BLOCK=COUNT=0 OVMXS.TMP` | `%X10000001` |
| `CREATE/DIRECTORY [.OVMXSD]` | `%X00000001` |
| `SHOW TIME` | `%X00030001` |
| `SHOW DEFAULT` | `%X00030001` |
| `SHOW LOGICAL SYS$LOGIN` | `%X10000001` |
| `SHOW TRANSLATION SYS$LOGIN` | `%X00030001` |
| `SHOW PROCESS` | `%X10000001` |
| `SHOW DEVICE OPA0:` | `%X10000001` |
| `SHOW USERS` | `%X10000001` |
| `SHOW STATUS` | `%X10000001` |
| `SHOW PROTECTION` | `%X00030001` |
| `SHOW TERMINAL` | `%X10000001` |
| `SHOW MEMORY/FILES` | `%X10000001` |
| `SET DEFAULT SYS$LOGIN` | `%X00030001` |
| `SET PROMPT="$ "` | `%X00030001` |
| `SET PROTECTION=(S:RWED,O:RWED,G:RE,W)/DEFAULT` | `%X00010001` |
| `SET TERMINAL/WIDTH=80` | `%X10000001` |
| `SET PROCESS/NAME="SYSTEM"` | `%X10000001` |
| `SET MESSAGE/TEXT` | `%X10000001` |
| `DEFINE OVMXL SYS$LOGIN` | `%X00000001` |
| `ASSIGN SYS$LOGIN OVMXM` | `%X00000001` |
| `DEASSIGN OVMXL` | `%X00000001` |
| `DEASSIGN OVMXM` | `%X00000001` |
| `WRITE SYS$OUTPUT "W"` | `%X00010001` |
| `OVMXSYM = 1` | `%X00030001` |
| `OPEN/READ OVMXF OVMXS.TMP` | `%X00000001` |
| `CLOSE OVMXF` | `%X00010001` |
| `ALLOCATE TTD3:` | `%X00030001` |
| `DEALLOCATE TTD3:` | `%X00000001` |
| `WAIT 0:0:1` | `%X00000001` |
| `HELP/NOPROMPT/OUTPUT=NL: DCL` | `%X00000001` |
| `COPY NOSUCHQ.TMP X.TMP` | `%X1067109A` |
| `TYPE NOSUCHQ.TMP` | `%X10951238` |
| `DIRECTORY NOSUCHQ.TMP` | `%X10018290` |
| `DELETE NOSUCHQ.TMP;*` | `%X10931238` |
| `SHOW LOGICAL NOSUCHQLOG` | `%X10788019` |
| `SET DEFAULT NOSUCHDEVQ:[X]` | `%X00030001` |
| `DEASSIGN NOSUCHQLOG` | `%X000001BC` |
| `DELETE OVMXS.TMP;*` | `%X10931238` |
| `DELETE OVMXSD.DIR;1` | `%X10931238` |
