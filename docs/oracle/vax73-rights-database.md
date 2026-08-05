# Oracle: the rights database on OpenVMS VAX V7.3

**Node:** VAX1 (`/data/training/vax/cluster/vax1`), OpenVMS VAX V7.3, SIMH MicroVAX 3900.
**Date observed:** 2026-08-05.
**Item:** vms-2f8 (F$IDENTIFIER's source — the rights database OVMX ships and nothing reads).
**Method:** behaviour observation (live DCL `F$IDENTIFIER`) plus documented tool output
(`AUTHORIZE SHOW/IDENTIFIER/FULL`). No disassembly, no VSI source (CLAUDE.md Rule 8).

This supersedes nothing in `vms-2f8`'s earlier notes; it *extends* them. The 2026-08-02
round pinned the F$IDENTIFIER **miss** values and the SYSTEM/DEFAULT pair. This round asks
the question that round deferred: **what is actually in the rights database**, and what do
the identifiers OVMX ships resolve to on real VMS.

---

## 1. `F$IDENTIFIER` round trip, every identifier OVMX ships

```
$ NUM = F$IDENTIFIER("<name>","NAME_TO_NUMBER")
$ BK  = F$IDENTIFIER(NUM,"NUMBER_TO_NAME")
$ WRITE SYS$OUTPUT "IDMAP <name> = ''NUM' -> ''BK'"

IDMAP SYSTEM      = 65540       -> SYSTEM
IDMAP DEFAULT     = 8388736     -> DEFAULT
IDMAP INTERACTIVE = -2147483645 -> INTERACTIVE
IDMAP BATCH       = -2147483647 -> BATCH
IDMAP NETWORK     = -2147483643 -> NETWORK
IDMAP LOCAL       = -2147483644 -> LOCAL
IDMAP REMOTE      = -2147483642 -> REMOTE
IDMAP DIALUP      = -2147483646 -> DIALUP
```

DCL prints the longword signed. As unsigned hex:

| Identifier    | DCL decimal   | Value        |
|---------------|---------------|--------------|
| `BATCH`       | `-2147483647` | `%X80000001` |
| `DIALUP`      | `-2147483646` | `%X80000002` |
| `INTERACTIVE` | `-2147483645` | `%X80000003` |
| `LOCAL`       | `-2147483644` | `%X80000004` |
| `NETWORK`     | `-2147483643` | `%X80000005` |
| `REMOTE`      | `-2147483642` | `%X80000006` |
| `SYSTEM`      | `65540`       | `%X00010004` = UIC `[1,4]` octal |
| `DEFAULT`     | `8388736`     | `%X00800080` = UIC `[200,200]` octal |

**Every one of these round-trips.** That is the finding that matters for point 3 below.

## 2. The values OVMX's shipped `RIGHTSLIST.DAT` assigned

```
$ R = F$IDENTIFIER(<v>,"NUMBER_TO_NAME")
$ WRITE SYS$OUTPUT "REV <v> = [''R']"

REV 1       = []
REV 2       = []
REV 3       = []
REV 4       = []
REV 5       = []
REV 8388736 = [DEFAULT]
REV 65540   = [SYSTEM]
```

OVMX shipped `INTERACTIVE:1 BATCH:2 NETWORK:3 LOCAL:4 REMOTE:5`. **On real VMS none of
1–5 is an identifier at all** — every one answers the null string. The shipped file was an
invention, and wiring `F$IDENTIFIER` to it unchanged would have shipped six wrong answers
under the banner of reading a real facility.

## 3. `8388736 -> "DEFAULT"` is now measured

The 2026-08-02 round deliberately declined to add the `NUMBER_TO_NAME` direction for
`DEFAULT`, on the grounds that the oracle had been asked only the `NAME_TO_NUMBER`
direction and symmetry is not evidence. That was the right call then. It has now been
asked, and the answer is `DEFAULT` (§1 and §2). The mapping is measured, not inferred.

## 4. The whole rights database, documented tool output

```
$ DEFINE SYSUAF SYS$SYSTEM:SYSUAF.DAT
$ DEFINE RIGHTSLIST SYS$SYSTEM:RIGHTSLIST.DAT
$ MC AUTHORIZE SHOW/IDENTIFIER/FULL *
  Name                             Value           Attributes
  BARON                            [002000,002000]
  BATCH                            %X80000001
  DECNET                           [000376,177777]
  DECWINDOWS                       %X80000007
  DEFAULT                          [000200,000200]
  DIALUP                           %X80000002
  FIELD                            [000001,000010]
  INTERACTIVE                      %X80000003
  LOCAL                            %X80000004
  MAIL$SERVER                      [000376,000374]
  MIRRO$SERVER                     [000376,000367]
  NETWORK                          %X80000005
  NML$SERVER                       [000376,000371]
  PHONE$SERVER                     [000376,000372]
  REMOTE                           %X80000006
  SECSRV$CLIENT                    %X96EE0001
  SECSRV$COMMUNICATION             %X96EE0003
  SECSRV$OBJECT                    %X96EE0002
  SYS$NODE_VAX1                    %X80010000
  SYS$NODE_VAX2                    %X80010001
  SYS$NODE_VAX3                    %X80010002
  SYSTEM                           [000001,000004]
  SYSTEST                          [000001,000007]
  TCPIP$AUX                        [003655,177777]
  TCPIP$FTP                        [003655,000001]
  VMS$BUFFER_OBJECT_USER           %X80000008
  VMS$MEM_RESIDENT_USER            %X80000009
  VPM$SERVER                       [000376,000370]
```

Three properties this listing establishes, none of which OVMX had:

1. **There are two kinds of row.** UIC identifiers render as `[group,member]` in **octal**;
   general identifiers render as `%X8xxxxxxx`. AUTHORIZE prints them in the notation each
   kind is written in — which is the same octal convention `vms-e60` pinned for SYSUAF.
2. **Every UIC identifier here corresponds to a UAF account** (`BARON`, `SYSTEM`,
   `SYSTEST`, `FIELD`, `DEFAULT`, the `TCPIP$`/`$SERVER` accounts). On VMS the two are
   maintained together by AUTHORIZE.
3. **The environmental identifiers carry no attributes.** The Attributes column is empty
   for all of them. OVMX marked all five of its invented rows `RESOURCE`.

## 5. What this does *not* answer

- Whether OVMX should ship `DECWINDOWS`, `VMS$BUFFER_OBJECT_USER` or
  `VMS$MEM_RESIDENT_USER`. Those name facilities OVMX does not have. Observing that VMS
  ships them is not a reason for OVMX to, and inventing holders for them would be Rule 10's
  illegal third answer. Left out, deliberately.
- The site-specific rows (`BARON`, `SYS$NODE_VAX*`, `TCPIP$*`, `SECSRV$*`, the `$SERVER`
  accounts). Those are this lab's, not VMS's.
- The on-disk format. Real `RIGHTSLIST.DAT` is an indexed RMS file; nothing here observes
  its bytes and nothing here needs to. OVMX's text form is an OVMX design choice and is
  labelled as one in the file itself.

## 6. Method note

The first pass at this transcript left the console inside AUTHORIZE's
`Do you want to create a new file?` prompt (SYSUAF was not reachable under the bare
`MC AUTHORIZE` at the time). Every subsequent command was consumed by that prompt and
answered `%UAF-E-INVRSP` — while *looking*, in the raw log, like a list of commands that
had run. Recovering meant answering `NO` and re-driving from a verified `$` prompt. Same
family as the handoff's standing rule: confirm which thing actually ran before reading the
output as a result.
