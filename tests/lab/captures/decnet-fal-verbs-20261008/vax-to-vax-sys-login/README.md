# VAX to VAX: SYS$LOGIN wildcards, VFC files, refusals and NOFILES (2026-10-08)

**Provenance.** This was captured by the operator's DECnet lane on lab pod `dnlab-1` on 2026-10-08. VAX1 (1.1) and VAX2 (1.2) are stock OpenVMS VAX V7.3. VAX1 ran each command below against VAX2's own FAL, using VAX2's default DECnet account DNTEST [200,201] with no access-control string. No credentials appear anywhere in the capture. VAX2's SYS$LOGIN for that account is SYS$SYSDEVICE:[SYSMGR], which holds:
- BRK1.TXT and BRK2.TXT, both owned by DNTEST and protected (S:RWED,O:RWED,G:RE,W)
- BRKP.TXT, owned by [1,4] and protected (S:RWED,O:RWED,G,W)

Clean-room (Rule 8): this records wire behaviour only.

| file | what |
|---|---|
| `falverbs-v2v-vax1-console.txt` | Each command and what VAX1's console printed |
| `falverbs-v2v-wire.txt` | Every NSP connect and DAP segment, 15 links, from `nspdump` |

## What the wire shows

| VAX command | what VAX1 sends | VAX2 FAL's answer |
|---|---|---|
| DIRECTORY/FULL BRK\*.TXT | DIRLIST (DISPLAY 0x839) | For BRK1 and BRK2: NAMEs, ATTRIBUTES (VFC, FSZ 2), SUMMARY, DATE AND TIME, PROTECTION. For BRKP: NAME, then STATUS `09 00 55 40 00 00 01 24`. VAX1 answers CONTINUE TRANSFER (skip), and the FAL replies ACCESS COMPLETE. |
| DIRECTORY BRK\*.TXT | DIRLIST | NAME volume `SYS$SYSDEVICE:`, NAME directory `[SYSMGR]`, three NAME(file), ACCESS COMPLETE |
| TYPE BRK\*.TXT | a DIRLIST, then on ONE link a CONFIGURATION + ATTRIBUTES + ACCESS(OPEN) of each resultant by name | BRK1 and BRK2 are served. BRKP is refused with STATUS `09 00 55 40 00 00 01 24`, which is PRV, not FNF. |
| RENAME BRK2 BRK3 | a DIRLIST, then ACCESS RENAME + NAME | NAME(old) ACK NAME(new) ACK ACCESS COMPLETE |
| DELETE BRK3.TXT;\* | two DIRLISTs, then ACCESS ERASE of `...BRK3.TXT;1` by name | NAME ACK ACCESS COMPLETE |
| DELETE BRKP.TXT;\* | the same, ERASE by name | STATUS `09 00 55 40 00 00 01 24` |
| DIRECTORY NOSUCH.TXT | DIRLIST | NAME volume / directory / `NOSUCH.TXT;*`, then STATUS `4032` STV `0910`. VAX1 prints `%DIRECT-W-NOFILES`. |

A VMS client never sends a wildcard spec except in a DIRLIST. It resolves the wildcard itself and names each file. It still refuses `DELETE ;*` (RMS-F-WLD) against a FAL that does not advertise SYSCAP bit 38.

Both VAXes speak DAP 7.2. Every segment after a CONFIGURATION ends with 2 bytes that DAP 5.6 does not define, and DIRLIST carries a per-file ACK that a DAP 5.6 client rejects (see `../live-bracket/`). `tests/vmsdecnet/test_dnet_fal_server.c` test 6 replays all 15 links.
