# ACLs on Files-11 ODS-2: on-disk format and access decisions (OpenVMS VAX V7.3 oracle)

Captured 2026-10-08 on a disposable lab pod (`corpusvax-1`, a clone of the vaxlab
golden disks) for rd vms-d404. Everything below is the observed behaviour of the
real system: `SET ACL` then `DUMP/HEADER/NOFORMATTED`, `SHOW ACL`, `DIRECTORY/ACL`,
`DIRECTORY/SECURITY`, and a batch job run as an unprivileged user. Nothing was
disassembled (AGENTS.md clean-room rule).

Raw transcripts, verbatim, in `vax73-acl/`:

| file | what |
|---|---|
| `ace-format.txt` | `SET ACL` + `DUMP/HEADER` (formatted and hex) for UIC, wildcard-UIC and DEFAULT_PROTECTION ACEs; `SHOW ACL`, `DIRECTORY/ACL`, `DIRECTORY/SECURITY` |
| `access-setup.txt` | user CORPTST [200,201] and the 14 probe files with their protections and ACLs |
| `access-decisions.txt` | identifiers CORPUSID/CORPUS2, more ACEs and hex dumps, and the probe's result as CORPTST |
| `PROBE.COM` | the batch probe (`OPEN/READ` or `OPEN/APPEND` per file, prints granted or `$STATUS`) |
| `ace-editing.txt` | replacement, `/DELETE` of one ACE and of all, ACCESS display order, a 35-ACE list (spills to an extension header) |
| `ace-syntax-control.txt`, `PROBE3.COM` | `$PARSE_ACL` errors as SET reports them, PROTECTED surviving `/DELETE`, CONTROL access for a non-owner (PROBE3 as CORPTST) |
| `ace-wildcards.txt` | `IDENTIFIER=*` vs `[*,*]` vs `[1,*]` encodings |
| `access-system-privileged.txt`, `PROBE2.COM` | the same probe run by a system-group user CORPSYS [7,7], a SYSPRV user CORPPRV [300,1], and SYSTEM (BYPASS) |
| `acedef.txt` | `$ACEDEF` from the node's `SYS$LIBRARY:STARLET.MLB` (the constants below) |

## Where the ACL lives

- The ACL is in the file header's **access control area**, from `fh2_acoffset`
  (words) to `fh2_rsoffset`. With no ACL both are 255 (A.TXT before SET ACL).
- The area is placed at the **end** of the header, just below the checksum word:
  one 12-byte ACE gives acoffset 249 (byte 498) with rsoffset 255; three give
  acoffset 237. The map area keeps its offset (100); the ACL takes words from
  the top.
- ACEs are stored back to back in display order (`SHOW ACL` order). `SET ACL`
  without a position puts the new ACE **first**.
- Adding an ACE whose identifier(s) match an existing ACE **replaces** it
  (F13 in `access-decisions.txt`: READ+WRITE, then NONE, then `/DELETE` of the
  NONE ACE leaves the ACL empty).

## ACE layout (`$ACEDEF`)

| offset | field | |
|---|---|---|
| 0 | `ACE$B_SIZE` | bytes in this ACE |
| 1 | `ACE$B_TYPE` | `ACE$C_KEYID` 1 (identifier ACE), `ACE$C_DIRDEF` 9 (DEFAULT_PROTECTION) |
| 2 | `ACE$W_FLAGS` | `ACE$M_DEFAULT` 0x100, `PROTECTED` 0x200, `HIDDEN` 0x400, `NOPROPAGATE` 0x800 |
| 4 | `ACE$L_ACCESS` | `READ` 1, `WRITE` 2, `EXECUTE` 4, `DELETE` 8, `CONTROL` 0x10 |
| 8 | `ACE$L_KEY` | identifiers, one longword each; size = 8 + 4 x count |

Observed encodings:

| ACE as SHOW ACL prints it | bytes |
|---|---|
| `(IDENTIFIER=[SYSTEM],ACCESS=READ+WRITE)` | size 12, type 1, flags 0, access 3, key 0x00010004 |
| `(IDENTIFIER=[*,*],ACCESS=EXECUTE)` | key 0x3FFFFFFF (group wildcard 0x3FFF, member wildcard 0xFFFF) |
| `(IDENTIFIER=*,ACCESS=NONE)` | key 0xFFFFFFFF (displayed as `*`, distinct from `[*,*]`) |
| `(IDENTIFIER=[1,*],ACCESS=WRITE)` | key 0x0001FFFF (displayed numerically: no group name) |
| `(IDENTIFIER=[200,*],ACCESS=READ)` | key 0x0080FFFF |
| `(IDENTIFIER=CORPUSID+CORPUS2,ACCESS=READ)` | size 16, keys 0x80010003, 0x80010004 |
| `(IDENTIFIER=INTERACTIVE,ACCESS=READ)` | key 0x80000003 |
| `(IDENTIFIER=[CORPTST],OPTIONS=DEFAULT+PROTECTED+NOPROPAGATE,ACCESS=READ+EXECUTE+DELETE+CONTROL)` | flags 0x0B00, access 0x1D, key 0x00800081 |
| `(DEFAULT_PROTECTION,SYSTEM:RWED,OWNER:RWED,GROUP:RE,WORLD:)` | size 24, type 9, flags 0, spare longword 0, then S/O/G/W longwords 0x10, 0x10, 0x1A, 0x1F (deny bits, CONTROL 0x10 always set) |

`OPTIONS=HIDDEN` is refused from DCL: `%SET-E-NOHIDDEN, cannot modify hidden ACEs`.

## Access decisions (user CORPTST [200,201], holds CORPUSID, not CORPUS2; batch job)

| file | owner | protection | ACL (first ACE first) | asked | result |
|---|---|---|---|---|---|
| F1 | [1,4] | S:RWED,O:RWED,G,W | [200,201] READ | read | granted |
| F2 | [1,4] | ...,G:RE,W:RE | [200,201] NONE | read | denied |
| F3 | [1,4] | ...,G:RE,W:RE | [300,*] NONE | read | granted |
| F4 | [200,1] | S:RWED,O:RWED,G:RE,W | [200,*] NONE | read | denied |
| F5 | [200,201] | S:RWED,O:RWED,G,W | [200,201] NONE | read | **granted** |
| F6 | [1,4] | ...,G,W | [200,201] READ; [200,*] NONE | read | granted |
| F7 | [1,4] | ...,G,W | [200,*] NONE; [200,201] READ | read | denied |
| F8 | [1,4] | ...,G,W:RWED | [200,201] READ | write | denied |
| F9 | [1,4] | ...,G,W | CORPUSID READ | read | granted |
| F10 | [1,4] | ...,G,W | CORPUSID+CORPUS2 READ | read | denied |
| F11 | [1,4] | ...,G,W:RE | CORPUS2 NONE | read | granted |
| F12 | [1,4] | ...,G,W:RE | [200,201] READ+WRITE | write | granted |
| F13 | [1,4] | ...,G,W | (empty, see above) | read | denied |
| F14 | [1,4] | ...,G,W | INTERACTIVE READ; [CORPTST] DEFAULT... R+E+D+C; CORPUSID+CORPUS2 NONE | read | denied |

Denials are `%X1001829A` (`RMS$_PRV`) at `OPEN`.

What the table shows:

1. Identifier ACEs are searched in order; the **first** whose identifiers the
   process **all** holds decides (F6/F7, F10). A UIC identifier matches the
   process UIC with `*` members/groups as wildcards (F4, F6).
2. A matching ACE that grants every requested access grants it (F1, F9, F12),
   even where the protection code would not.
3. A matching ACE that does not grant it is final for the **group and world**
   categories (F2, F4, F8 -- world RWED does not rescue F8), but the **owner**
   (F5) and the **system** category (G1, G3 below) still get what the protection
   code's owner and system fields allow.
4. No matching ACE: the protection code decides as before (F3, F11).
5. An ACE carrying `OPTIONS=DEFAULT` is not used for the file's own access
   check (F14: its `[CORPTST]` ACE grants READ, but the file denies). The
   batch job does not hold INTERACTIVE (F14's first ACE does not match).

## System category and privileges (`access-system-privileged.txt`)

| file | protection | ACL | CORPSYS [7,7] | CORPPRV [300,1] +SYSPRV | SYSTEM (BYPASS) |
|---|---|---|---|---|---|
| G1 | S:RWED,O:RWED,G,W | [7,7] NONE | granted | granted | granted |
| G2 | S,O:RWED,G,W:RE | [7,7] READ | granted | granted | granted |
| G3 | S:RWED,O:RWED,G,W | [300,1] NONE | granted | granted | granted |
| G4 | S,O,G,W | [*,*] NONE | denied | denied | granted |

A system-group UIC (G1) or SYSPRV (G3) keeps the system field after a denying
ACE; with the system field empty (G4) neither helps; BYPASS grants regardless.

## Editing (`ace-editing.txt`, `ace-syntax-control.txt`)

- `SET ACL/ACL=ace` puts the ACE first; an existing ACE for the same
  identifier(s) is removed from where it was (C.TXT: `[*,*]` moved to the top).
- `SET ACL/DELETE/ACL=ace` for an ACE that is not there:
  `%SET-W-NOSUCHACE ... does not exist` then `%SET-F-WRITEERR`.
- `SET ACL/DELETE` (no ACE) removes every ACE except `OPTIONS=PROTECTED` ones;
  an emptied list puts `fh2_acoffset` back to 255.
- `ACCESS=` is shown `READ+WRITE+EXECUTE+DELETE+CONTROL` in that order, `NONE`
  when empty; input accepts `R+W+E+D+C`, lower case and blanks after commas.
- Parse errors: `ACCESS=BOGUS` and `(FOO)` give `%SET-F-SYNTAX, error parsing
  '<rest>'` + `-SYSTEM-F-IVACL`; an unknown identifier name `-SYSTEM-F-NOSUCHID`.
- Changing the ACL needs CONTROL: the owner may (H3); a world user may not
  (H1: `%SET-E-OPENIN ... -SYSTEM-F-NOPRIV`); an ACE granting CONTROL lets a
  non-owner change it (H2). `SHOW ACL` of a file with no ACL is
  `%SYSTEM-W-ACLEMPTY` (`$STATUS` %X100009D0).
- An ACL longer than the primary header holds (25 twelve-byte ACEs on a header
  with an empty map) continues in an extension header.
