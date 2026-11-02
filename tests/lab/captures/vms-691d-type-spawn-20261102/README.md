# vms-691d: TYPE with a file list, SPAWN /INPUT and /OUTPUT on OpenVMS VAX V7.3

Captured 2026-11-02 on a real VAX, node VAX1 (OpenVMS VAX V7.3 under SIMH), in its own lab pod
(`lab691d`, image `ovmx-vaxlab:7`, NODES=vax1, CPU request 200m). The console was driven only through
`tests/lab/tools/lab-console.sh`, logged in as SYSTEM, default `SYS$LOGIN` (`SYS$SYSROOT:[SYSMGR]`).
`console.log` is the console from the login on, with NULs, carriage returns and terminal escapes
removed. No bugcheck.

## TYPE

| Command | What VAX V7.3 does | `$STATUS` |
|---|---|---|
| `TYPE TA.TXT,TB.TXT` | TA's records; then a header for TB; then TB's records | `%X10000001` |
| `TYPE TA.TXT,NOSUCH.TXT,TB.TXT` | TA; `%TYPE-W-SEARCHFAIL, error searching for SYS$COMMON:[SYSMGR]NOSUCH.TXT;` + `-RMS-E-FNF, file not found`; TB with its header | `%X10951238` |
| `TYPE NOSUCH.TXT,TA.TXT` | the SEARCHFAIL pair; TA **with** a header | `%X10951238` |
| `TYPE NOSUCH.TXT` | the SEARCHFAIL pair | `%X10951238` |
| `TYPE TA.TXT,TB` | as the first: TB takes TA's file type | `%X10000001` |
| `TYPE TA.TXT+TB.TXT` | as the first | |
| `TYPE TA.TXT,TA.TXT` | TA, then TA again with a header | |
| `TYPE TA.TXT,,TB.TXT` | `%DCL-W-NULFIL, missing or invalid file specification - respecify` | `%X000380E0` |
| `TYPE TA.TXT TB.TXT` | `%DCL-W-MAXPARM, too many parameters ...` + ` \TB\` | `%X00038098` |
| `TYPE TA.TXT,TB.TXT/PAGE` | TA, then `Press RETURN to continue` before TB | |

The header is three records: a single blank, the file's full resultant name
(`SYS$SYSROOT:[SYSMGR]TB.TXT;1`), a single blank. The raw console bytes are
`alpha two\r\n \r\nSYS$SYSROOT:[SYSMGR]TB.TXT;1\r\n \r\nbravo one`. Every element of the list
after the first gets a header, even when the first element was a missing file. The first element never
gets one.

## SPAWN

| Command | What VAX V7.3 does | `$STATUS` after |
|---|---|---|
| `SPAWN/INPUT=NL:/OUTPUT=S1.LOG SHOW SYMBOL TCX` | SPAWNED / ATTACHED / RETURNED; `S1.LOG` holds `  TCX = "tcx-sym"` | `%X00000001` |
| `SPAWN/OUTPUT=S2.LOG FOOBAR` | SPAWNED / ATTACHED / RETURNED, then the creator shows `%DCL-W-IVVERB, unrecognized command verb - check validity and spelling` (no `\FOOBAR\`); `S2.LOG` holds the IVVERB line and ` \FOOBAR\` | `%X00038090` |
| `SPAWN/NOWAIT/OUTPUT=S3.LOG @TC.COM` | `%DCL-S-SPAWNED, process SYSTEM_1 spawned`; `S3.LOG` holds TC.COM's output | `%X00000001` |
| `SPAWN/INPUT=IN.COM/OUTPUT=S4.LOG` | `S4.LOG` holds IN.COM's output | `%X00010001` |
| `SPAWN/INPUT=IN.COM/OUTPUT=S5.LOG WRITE SYS$OUTPUT "from CMD"` | `S5.LOG`: `from CMD`, then `from IN` | |
| `SPAWN/INPUT=NOSUCH.COM SHOW TIME` | `%RMS-E-FNF, file not found`; no process | `%X00018292` |
| `SPAWN/NOWAIT/INPUT=NL:/OUTPUT=S6.LOG SHOW SYMBOL TCX` | SPAWNED; `S6.LOG` holds the symbol | `%X00000001` |

What this shows:

- /INPUT is a SPAWN qualifier.
- /OUTPUT makes the file the subprocess's SYS$OUTPUT, and its SYS$ERROR too: the `\FOOBAR\` line is
  only in the file.
- With a command string and /INPUT, the command runs first and then the input file.
- An input file that does not exist is RMS's status, and no process is created.

Not covered by rd vms-691d (observed here for the record): an attached SPAWN prints
`%DCL-S-SPAWNED`, `%DCL-S-ATTACHED` and `%DCL-S-RETURNED`, and leaves the subprocess's completion
status in `$STATUS`. DCL symbols (`TCX`) reach the subprocess.
