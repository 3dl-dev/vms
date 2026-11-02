# Real-OpenVMS oracle: DCL file verbs with comma list in P1 (vms-ba2)

Node: pod vaxlab-3, node VAX1 (SIMH VAX), OpenVMS VAX V7.3, user SYSTEM, dir SYS$SYSROOT:[SYSMGR.BA2] (SYSMGR is the login dir; console was already logged in and was left so).
Transcript: CONSOLE.txt (verbatim console tail from session start; includes echo doubling and my MK.COM setup-procedure construction, whose first attempt had a quoting error and a stray open handle, closed and redone). $STATUS = value read by WRITE immediately after the command.
Note: `SHOW SYMBOL $STATUS` after an intervening WRITE shows the WRITE's status (%X00010001), so evidence uses WRITE directly after each probe.

| # | Command | Expected | Observed | |
|---|---------|----------|----------|---|
| 1 | DELETE A..J.TXT;* (10 elts) | all gone, %X1 | all ten gone (K,L,MK.COM,SUB.DIR,X1,X2 remain); $STATUS %X10000001 | MATCH (bit 28 set; low bit 1) |
| 2 | DELETE K.TXT;* L.TXT;* | MAXPARM `\L.TXT;*\`, none deleted | `%DCL-W-MAXPARM, too many parameters - reenter command with fewer parameters` / ` \L\`; $STATUS %X00038098; nothing deleted | DIFFER: text shows `\L\` only (not `L.TXT;*`) |
| 3 | TYPE A.TXT,B.TXT,C.TXT | A,B,C in order | A, B, C in order. No header for the first file; for 2nd+ a header line `SYS$SYSROOT:[SYSMGR.BA2]B.TXT;1` preceded and followed by a line holding a single space (blank). Output: `A`,` `,`<hdr B>`,` `,`B`,` `,`<hdr C>`,` `,`C`. $STATUS %X10000001 | MATCH (header detail new) |
| 4 | COPY A.TXT,B.TXT C2.TXT | one C2.TXT;1 = A,B | C2.TXT;1 only, TYPE gives `A`,`B`. %X00000001 | MATCH |
| 4b | COPY/LOG A.TXT,B.TXT C3.TXT | - | `%COPY-S-COPIED, SYS$SYSROOT:[SYSMGR.BA2]A.TXT;1 copied to SYS$SYSROOT:[SYSMGR.BA2]C3.TXT;1 (1 block)` then `%COPY-S-APPENDED, SYS$SYSROOT:[SYSMGR.BA2]B.TXT;1 appended to SYS$SYSROOT:[SYSMGR.BA2]C3.TXT;1 (1 record)`; %X00000001 | n/a |
| 5 | COPY A.TXT,B.TXT [.SUB]*.* | [.SUB]A, B | [.SUB] holds A.TXT;1 B.TXT;1; %X00000001 | MATCH |
| 6 | APPEND A.TXT,B.TXT C.TXT | C,A,B | TYPE C.TXT: C, A, B; %X00000001 | MATCH |
| 7 | DIRECTORY A.TXT,B.TXT,C.TXT | 1 header, 3 files, total | `Directory SYS$SYSROOT:[SYSMGR.BA2]` blank, `A.TXT;1  B.TXT;1  C.TXT;1` (columns), blank, `Total of 3 files.`; %X00000001 | MATCH |
| 8 | PURGE A.TXT,B.TXT (;2 made) | only ;2 left | A.TXT;2 B.TXT;2 remain; silent; %X10000001 | MATCH |
| 9 | RENAME A.TXT,B.TXT *.NEW | A.NEW;1 B.NEW;1 | A.NEW;1 B.NEW;1 (version reset to ;1); silent; %X00000001 | MATCH |
| 10 | DELETE [.SUB]A.TXT;*,B.TXT;* | B from [.SUB] | [.SUB] emptied; [.BA2]A.TXT and B.TXT still present; %X10000001 | MATCH (sticky dir) |
| 11 | DELETE A.TXT;*,NOPE.TXT;*,B.TXT;* | A,B deleted, warning | `%DELETE-W-SEARCHFAIL, error searching for SYS$COMMON:[SYSMGR.BA2]NOPE.TXT;*` / `-RMS-E-DNF, directory not found` / `-SYSTEM-W-NOSUCHFILE, no such file`; A,B both deleted; $STATUS %X10931238 (warning/severity 0, with bit 28) | MATCH |
| 12 | DELETE/LOG X1.TXT;*,X2.TXT;* | FILDEL x2 + TOTAL | `%DELETE-I-FILDEL, SYS$SYSROOT:[SYSMGR.BA2]X1.TXT;1 deleted (9 blocks)`, same for X2, then `%DELETE-I-TOTAL, 2 files deleted (18 blocks)`; %X10000001 | MATCH |
| 13 | TYPE A.TXT B.TXT | (new) | `%DCL-W-MAXPARM, too many parameters - reenter command with fewer parameters` / ` \B\`; %X00038098; nothing printed | n/a |
| 14 | DIRECTORY [.SUB]A.TXT,B.TXT (A in SUB only; B in parent only) | (new) | Header `Directory SYS$SYSROOT:[SYSMGR.BA2.SUB]` first; B looked up in [.SUB]: `%DIRECT-E-OPENIN, error opening SYS$COMMON:[SYSMGR.BA2.SUB]B.TXT;* as input` / `-RMS-E-DNF, directory not found` / `-SYSTEM-W-NOSUCHFILE, no such file` appears right after header (before the file list); then `A.TXT;1`, blank, `Total of 1 file.`; %X1001C04A | n/a (sticky dir confirmed) |
| 15 | COPY A.TXT B.TXT,C.TXT (list in P2) | (new) | `%DCL-W-NOLIST, list of parameter values not allowed - check use of comma (,)` / ` \B.TXT,C\`; %X000380C0; no files created/changed (B.TXT, C.TXT still ;1 only) | n/a |
| 16 | DELETE A.TXT;* B.TXT;* | MAXPARM | same text as #2: `%DCL-W-MAXPARM, too many parameters - reenter command with fewer parameters` / ` \B\`; %X00038098; A and B untouched | n/a (`\B\`, not `\B.TXT;*\`) |
| 17 | PURGE/LOG A.TXT,B.TXT (two versions each) | (new) | `%PURGE-I-FILPURG, SYS$SYSROOT:[SYSMGR.BA2]A.TXT;2 deleted (9 blocks)`, same for B.TXT;2, then `%PURGE-I-TOTAL, 2 files deleted (18 blocks)`; ;3 remain; %X10000001 | n/a |

## DIFFERs
- MAXPARM trailing line is `\L\` / `\B\`: the first token's name only (no type or version), with one leading space on the line.

Cleanup: all files, [.SUB] and BA2.DIR deleted; default restored to [SYSMGR]; console left logged in as found (not logged out). Deviation from "one command per call": sends were batched in small sequential groups within a call (still via lab-console.sh, each line waited on).
