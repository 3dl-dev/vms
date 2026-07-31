# Oracle: privileges and privilege statuses on OpenVMS VAX V7.3

**Node:** VAX1 (`~/vax/cluster/vax1`), OpenVMS VAX V7.3, SIMH MicroVAX 3900.
**Date observed:** 2026-07-30.
**Item:** vms-2b8 (identity and privileges are enforced by the executive).
**Method:** documented tool output only — `F$MESSAGE`, `ANALYZE/SYSTEM` +
`READ SYS$SYSTEM:SYSDEF.STB` + `EVALUATE`, and `SHOW PROCESS/PRIVILEGES`.
No disassembly, no VSI source (CLAUDE.md Rule 8).

Everything below was run by the implementer of vms-2b8 against the live lab.
It is not recalled, and it is not second-hand.

---

## 1. Status code values (`F$MESSAGE` round-trip)

```
$ WRITE SYS$OUTPUT "36="+F$MESSAGE(36)
36=%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation
$ WRITE SYS$OUTPUT "532="+F$MESSAGE(532)
532=%SYSTEM-F-RESULTOVF, resultant string overflow
$ WRITE SYS$OUTPUT "1664="+F$MESSAGE(1664)
1664=%SYSTEM-W-NOTALLPRIV, not all requested privileges authorized
```

| Symbol             | Value | Message                                                          |
|--------------------|-------|------------------------------------------------------------------|
| `SS$_NOPRIV`       |    36 | `%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation` |
| `SS$_NOTALLPRIV`   |  1664 | `%SYSTEM-W-NOTALLPRIV, not all requested privileges authorized`   |
| (`SS$_RESULTOVF`)  |   532 | `%SYSTEM-F-RESULTOVF, resultant string overflow`                  |

**Defect this disproves.** `src/libvms/include/ssdef.h` carried
`SS$_NOTALLPRIV 532`. 532 is `RESULTOVF`. Corrected to 1664 under this item.
This is the same class of error vms-8019 found on eight other `ssdef.h`
constants and vms-982 found on nine lock-flag bits.

---

## 2. Privilege bit positions (`PRV$V_*` from `SYSDEF.STB`)

`SS$_*` symbols are not in `SYSDEF.STB`; `PRV$V_*` are. `EVALUATE` prints the
bit POSITION, not the mask.

```
$ ANALYZE/SYSTEM
SDA> READ SYS$SYSTEM:SYSDEF.STB
%SDA-I-READSYM, reading symbol table  SYS$COMMON:[SYSEXE]SYSDEF.STB;1
SDA> EVALUATE PRV$V_<name>
```

| Symbol            | Hex        | Decimal |
|-------------------|------------|---------|
| `PRV$V_CMKRNL`    | `00000000` |   0     |
| `PRV$V_CMEXEC`    | `00000001` |   1     |
| `PRV$V_DETACH`    | `00000005` |   5     |
| `PRV$V_LOG_IO`    | `00000007` |   7     |
| `PRV$V_GROUP`     | `00000008` |   8     |
| `PRV$V_SETPRI`    | `0000000D` |  13     |
| `PRV$V_SETPRV`    | `0000000E` |  14     |
| `PRV$V_TMPMBX`    | `0000000F` |  15     |
| `PRV$V_OPER`      | `00000012` |  18     |
| `PRV$V_NETMBX`    | `00000014` |  20     |
| `PRV$V_SYSPRV`    | `0000001C` |  28     |
| `PRV$V_BYPASS`    | `0000001D` |  29     |
| `PRV$V_ALTPRI`    | `0000000D` |  13     |

**Note on `ALTPRI` — and on `IMPERSONATE`, which is the same defect.** On this
VAX 7.3 node `PRV$V_ALTPRI` and `PRV$V_SETPRI` are the SAME bit (13) — they
are aliases on VAX. The in-tree `src/libvms/include/prvdef.h` gives `ALTPRI`
bit 36 and states it carries *Alpha* values, where the two are distinct.

The identical alias exists for `DETACH`: on VAX `PRV$V_DETACH` is bit 5 and
the oracle's own `SHOW PROCESS/PRIVILEGES` prints that bit under the name
`IMPERSONATE`, while `prvdef.h` gives `IMPERSONATE` the Alpha bit 37.

**Consequence, recorded so it is not mistaken for a decision.**
`src/vmsdcl/dcl_cmd_show.c` maps `ALTPRI`→bit 36 and `IMPERSONATE`→bit 37, so
against any VAX-encoded mask those two rows are unreachable and bits 5 and 13
print as nothing at all. The comment in that file says `DETACH` and `SETPRI`
are absent "because the oracle did not print them" — that is wrong in its
reasoning: the oracle DID print those bits, under their VAX alias names. The
display is wrong either way, and it is not fixed here: OVMX enforces neither
privilege, and the whole function is a `getenv("VMS_PRIVILEGES")`-fed stopgap
marked for deletion once the executive reader lands (vms-9fc). A later item
that DOES enforce priority or impersonation privileges must pin BOTH aliases
deliberately rather than inherit an unexamined constant.

**Defects this disproves.** Three in-tree tables disagreed with the oracle:

| File | Claim | Oracle |
|------|-------|--------|
| `src/kernel/vms_access.c` | `PRV_M_SETPRV = 1<<5` | bit 5 is `DETACH`; `SETPRV` is bit 14 |
| `src/kernel/vms_internal.h` | `VMS_DEFAULT_PRIVS = (1<<7)\|(1<<8)`, commented "TMPMBX \| NETMBX" | bits 7,8 are `LOG_IO`,`GROUP`; `TMPMBX`=15, `NETMBX`=20 |
| `src/vmsdcl/dcl_cmd_show.c` | local table: `DETACH`=15, `ACNT`=16, `ALTPRI`=21, `SETPRV`=22, `WORLD`=23, `SHARE`=24; "default TMPMBX\|NETMBX" = bits 0,1 | all wrong; bits 0,1 are `CMKRNL`,`CMEXEC` |

`src/libvms/include/prvdef.h` agreed with the oracle on every symbol above
except the `ALTPRI` alias noted, and is now the single source of truth,
static-asserted against the executive's copy.

---

## 3. `$SETPRV` semantics — enabling within the authorized mask

The question that decides how the OVMX executive must treat `perm_privs`:
**does re-enabling a privilege that is AUTHORIZED but not currently enabled
require `SETPRV`?**

Observed sequence (abridged; `SHOW PROCESS/PRIVILEGES` output trimmed to the
privileges in play):

```
$ SHOW PROCESS/PRIVILEGES
Authorized privileges:
 ... SETPRV ... SYSPRV ...
Process privileges:
 ... SETPRV               may set any privilege bit
 ... SYSPRV               may access objects via system protection

$ SET PROCESS/PRIVILEGE=(NOSETPRV,NOSYSPRV)
$ WRITE SYS$OUTPUT "STATUS="+$STATUS
STATUS=%X10000001
$ SHOW PROCESS/PRIVILEGES
Authorized privileges:
 ... SETPRV ... SYSPRV ...          ! AUTHORIZED mask UNCHANGED
Process privileges:
 ...                                ! SETPRV and SYSPRV both GONE

$ SET PROCESS/PRIVILEGE=SYSPRV      ! caller no longer holds SETPRV
$ WRITE SYS$OUTPUT "STATUS="+$STATUS
STATUS=%X10000001                   ! SS$_NORMAL — ALLOWED
$ SHOW PROCESS/PRIVILEGES
Process privileges:
 ... SYSPRV               may access objects via system protection
```

**Pinned:**

1. Disabling a privilege is always allowed.
2. Disabling from the CURRENT (process) mask does not touch the AUTHORIZED
   mask — the two masks are distinct and `SHOW PROCESS/PRIVILEGES` prints
   them under separate headings.
3. **Enabling a privilege that is already in the AUTHORIZED mask requires no
   `SETPRV`** and returns `SS$_NORMAL`. `SETPRV` is what authorizes exceeding
   the AUTHORIZED mask, not what authorizes using it.

`SS$_NOTALLPRIV`'s message text — "not all requested privileges authorized" —
is the condition for a request that reaches OUTSIDE the authorized mask, which
is why the OVMX executive returns it (not `SS$_NOPRIV`) when a caller without
`SETPRV` asks to enable a mixed set: the authorized subset is enabled and the
unauthorized remainder is not.

**Not observed on this node**, and therefore NOT implemented from a guess: the
exact status returned when a caller without `SETPRV` attempts to widen the
PERMANENT (authorized) mask. `SET PROCESS/PRIVILEGE` does not write the
authorized mask at all — that is `AUTHORIZE`'s job — so DCL offers no way to
provoke it here. The OVMX executive REFUSES the operation outright with
`SS$_NOPRIV` (36), which is the "insufficient privilege" condition whose text
matches, and applies no partial change. Flagged for operator sign-off: this is
a CHOICE of status for a condition the oracle did not show us, not a pin.

**Also flagged for operator sign-off, same class.** `VMS_IOCTL_SETIDENT` is an
OVMX-only interface — OpenVMS has no `$SETIDENT`, so there is no behaviour to
match for any of its failures. Two of its statuses are therefore CHOICES, not
pins, and both are listed here so the list is complete rather than partial:

| Condition | Status chosen | Why |
|-----------|---------------|-----|
| caller without `SETPRV` asks to widen its authorized mask or change its UIC | `SS$_NOPRIV` (36) | oracle-pinned *text* ("insufficient privilege"), applied to a condition the oracle did not show |
| user name buffer is not NUL-terminated, or is empty | `SS$_IVLOGNAM` | nearest existing "the name you gave is not a usable name" condition; nothing was measured |

Neither is presented as VMS-authentic. If the operator prefers different
statuses, both are one-line changes in `vms_ioctl_setident`.

---

## 4. `SHOW PROCESS/PRIVILEGES` output format (verbatim)

Recorded for the DCL reader that will replace the current fabricated output
(the `getenv("VMS_PRIVILEGES")` path). Header, then two privilege sections,
then two rights sections:

```
30-JUL-2026 13:09:07.80   User: SYSTEM           Process ID:   2020021A
                          Node: VAX1             Process name: "SYSTEM"

Authorized privileges:
 ACNT      ALLSPOOL  ALTPRI    AUDIT     BUGCHK    BYPASS    CMEXEC    CMKRNL
 IMPERSONATDIAGNOSE  DOWNGRADE EXQUOTA   GROUP     GRPNAM    GRPPRV    IMPORT
 LOG_IO    MOUNT     NETMBX    OPER      PFNMAP    PHY_IO    PRMCEB    PRMGBL
 PRMMBX    PSWAPM    READALL   SECURITY  SETPRV    SHARE     SHMEM     SYSGBL
 SYSLCK    SYSNAM    SYSPRV    TMPMBX    UPGRADE   VOLPRO    WORLD

Process privileges:
 ACNT                 may suppress accounting messages
 ALLSPOOL             may allocate spooled device
 ALTPRI               may set any priority value
 AUDIT                may direct audit to system security audit log
 BUGCHK               may make bug check log entries
 BYPASS               may bypass all object access controls
 CMEXEC               may change mode to exec
 CMKRNL               may change mode to kernel
 IMPERSONATE          may impersonate another user
 DIAGNOSE             may diagnose devices
 DOWNGRADE            may downgrade object secrecy
 EXQUOTA              may exceed disk quota
 GROUP                may affect other processes in same group
 GRPNAM               may insert in group logical name table
 GRPPRV               may access group objects via system protection
 IMPORT               may set classification for unlabeled object
 LOG_IO               may do logical i/o
 MOUNT                may execute mount acp function
 NETMBX               may create network device
 OPER                 may perform operator functions
 PFNMAP               may map to specific physical pages
 PHY_IO               may do physical i/o
 PRMCEB               may create permanent common event clusters
 PRMGBL               may create permanent global sections
 PRMMBX               may create permanent mailbox
 PSWAPM               may change process swap mode
 READALL              may read anything as the owner
 SECURITY             may perform security administration functions
 SETPRV               may set any privilege bit
 SHARE                may assign channels to non-shared devices
 SHMEM                may create/delete objects in shared memory
 SYSGBL               may create system wide global sections
 SYSLCK               may lock system wide resources
 SYSNAM               may insert in system logical name table
 SYSPRV               may access objects via system protection
 TMPMBX               may create temporary mailbox
 UPGRADE              may upgrade object integrity
 VOLPRO               may override volume protection
 WORLD                may affect other processes in the world

Process rights:
 SYSTEM                            resource
 INTERACTIVE
 LOCAL

System rights:
 SYS$NODE_VAX1
```

Notes for the future reader implementation:

- The `Authorized privileges:` block is a fixed 8-column grid of 10-character
  cells. `IMPERSONATE` is 11 characters and is printed CLIPPED to
  `IMPERSONAT` with no separating space (see row 2) — VMS does not widen the
  column for it. Reproduce the clipping; do not "fix" it.
- The `Process privileges:` block is one privilege per line, `" %-20s %s"`.
- The privileges in each block are alphabetical by symbol EXCEPT that
  `IMPERSONATE` sorts where `IMPERSONAT`… lands relative to `DIAGNOSE`, i.e.
  the sort is on the internal bit order, not the printed string.
- The header line pair is `User:`/`Process ID:` then `Node:`/`Process name:`.
  The current OVMX `SHOW PROCESS` prints `Process name:` on its own line and
  has no `Node:` field — a separate divergence, not fixed by this item.

---

## 5. Who may read another process's identity (`$GETJPI` / `SHOW SYSTEM`)

**Item:** vms-2b8 round 3. **Node:** VAX1, OpenVMS VAX V7.3, 2026-07-30.
**Method:** drive the CALLER's own privilege mask with `SET PROCESS/PRIVILEGE`
and read other processes with `F$GETJPI`. Documented tool output only.

The question this answers: OVMX's executive was handing any caller the user
name, UIC and privilege mask of every process in its table. What does VMS
actually require?

### 5.1 The targets

```
$ SHOW SYSTEM
...
2020020E AUDIT_SERVER    HIB     10 ...
20200218 TCPIP$FTP_1     LEF     10 ...
2020021A SYSTEM          CUR      7 ...

$ WRITE SYS$OUTPUT "FTP1="+F$GETJPI("20200218","UIC")
FTP1=[TCPIP$AUX,TCPIP$FTP]
$ WRITE SYS$OUTPUT "AUDIT="+F$GETJPI("2020020E","UIC")
AUDIT=[SYSTEM]
$ WRITE SYS$OUTPUT "SELF="+F$GETJPI("","UIC")
SELF=[SYSTEM]
```

`AUDIT_SERVER` is in the caller's own UIC group; `TCPIP$FTP_1` is not.

### 5.2 With NO privileges at all

```
$ SET PROCESS/PRIVILEGE=(NOALL)
$ SHOW PROCESS/PRIVILEGES
...
Process privileges:
                                        <- empty
```

```
$ X = F$GETJPI("2020020E","USERNAME")          ! same UIC group
$ WRITE SYS$OUTPUT "ST="+F$MESSAGE($STATUS)
ST=%CLI-S-NORMAL, normal successful completion
$ WRITE SYS$OUTPUT "X=["+X+"]"
X=[AUDIT$SERVER]

$ Y = F$GETJPI("20200218","USERNAME")          ! other UIC group
%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation
 \USERNAME\
$ WRITE SYS$OUTPUT "ST="+F$MESSAGE($STATUS)
ST=%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation

$ Z = F$GETJPI("","USERNAME")                  ! self
$ WRITE SYS$OUTPUT "SELF="+Z
SELF=SYSTEM
```

**A SAME-GROUP READ NEEDS NO PRIVILEGE.** A cross-group read is refused with
`SS$_NOPRIV` (36, pinned in §1).

### 5.3 The refusal is on the PROCESS, not on the item

Still with `NOALL`, every item tried against the cross-group process was
refused identically:

```
$ A1 = F$GETJPI("20200218","PRCNAM")
%SYSTEM-F-NOPRIV, ... \PRCNAM\
$ A2 = F$GETJPI("20200218","STATE")
%SYSTEM-F-NOPRIV, ... \STATE\
$ A3 = F$GETJPI("20200218","UIC")
%SYSTEM-F-NOPRIV, ... \UIC\
$ A4 = F$GETJPI("20200218","CURPRIV")
%SYSTEM-F-NOPRIV, ... \CURPRIV\
```

There is no partial answer to hand back, so the OVMX ioctl returns no row.

### 5.4 GROUP does NOT lift it — WORLD does

```
$ SET PROCESS/PRIVILEGE=(GROUP)
$ B2 = F$GETJPI("20200218","USERNAME")
%SYSTEM-F-NOPRIV, ... \USERNAME\
$ WRITE SYS$OUTPUT "GROUPONLY-ST="+F$MESSAGE($STATUS)
GROUPONLY-ST=%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation

$ SET PROCESS/PRIVILEGE=(NOGROUP,WORLD)
$ B3 = F$GETJPI("20200218","USERNAME")
$ WRITE SYS$OUTPUT "WORLD-ST="+F$MESSAGE($STATUS)+" V=["+B3+"]"
WORLD-ST=%CLI-S-NORMAL, normal successful completion V=[TCPIP$FTP   ]
```

**THE OBVIOUS GUESS IS WRONG.** "GROUP to read your own group, WORLD to read
anyone else" is what this item was dispatched believing, and it is not what
the oracle does. Same-group is free; GROUP alone does not help across groups;
WORLD alone is sufficient. OVMX therefore enforces WORLD and enforces nothing
over GROUP (CLAUDE.md Rule 10 — pin it or do not write it).

### 5.5 Enumeration is NOT privileged; identity is

In the same `NOALL` state, `SHOW SYSTEM` listed **every** process, including
`TCPIP$FTP_1` **with its process name**, even though `F$GETJPI` on that same
process was refused `PRCNAM` two commands earlier (§5.3).

```
$ SHOW SYSTEM                     ! process privileges: none
OpenVMS V7.3  on node VAX1 ...
  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages
20200201 SWAPPER         HIB     16 ...
...
20200217 TCPIP$INETACP   HIB     10 ...
20200218 TCPIP$FTP_1     LEF     10 ...
2020021A SYSTEM          CUR      7 ...
```

The SHOW SYSTEM columns are Pid, Process Name, State, Pri, I/O, CPU, page
faults and pages — **no user name, no UIC, no privilege mask**.

**Consequence for OVMX.** `VMS_IOCTL_PROCSCAN` carries a single row type that
spans both mechanisms, so a row the caller could not `$GETJPI` is returned
carrying only what `SHOW SYSTEM` displays of it (the process ID and the
process name) with the identity fields zeroed. It is not skipped — that would
hide a process VMS shows — and it is not returned whole. See
`vms_ioctl_procscan()` in `src/kernel/vms_proctab.c`.

### 5.6 Bit positions used by the above

Measured in the same session, same method as §2 (`ANALYZE/SYSTEM`,
`READ SYS$SYSTEM:SYSDEF.STB`, `EVALUATE`):

```
SDA> EVALUATE PRV$V_WORLD
Hex = 00000010   Decimal = 16
SDA> EVALUATE PRV$V_GROUP
Hex = 00000008   Decimal = 8
SDA> EVALUATE PRV$V_SETPRV
Hex = 0000000E   Decimal = 14
```

`PRV$V_WORLD` = 16 confirms `src/libvms/include/prvdef.h`, which is now
static-asserted against the executive's `VMS_PRV_V_WORLD`.

### 5.7 Not pinned — and therefore not implemented (see also §6)

Whether `WORLD` widens a `$GETJPI` **by process name** search beyond the
caller's UIC group was NOT measured (the DCL attempt tripped on the `$` in
`TCPIP$FTP_1` and was not retried). OVMX's `find_by_name()` therefore stays
group-scoped for every caller, which is the behaviour it already had. A later
item that wants the wide search must measure it first.

---

## 6. Plain `SHOW PROCESS` — verbatim, and what OVMX invented

**Item:** vms-2b8 round 6. **Node:** VAX1, OpenVMS VAX V7.3, 2026-07-30 16:42.
**Method:** boot VAX1 with `nodedrv.py`, log in prompt-synchronised as SYSTEM,
`SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST`, then run the command between
`===MARK===` writes and read the console log through `cat -A` so column
positions are counted, not eyeballed. Documented tool output only (Rule 8).
Node shut down cleanly with `@SYS$SYSTEM:SHUTDOWN` afterwards.

**Why this was measured.** The veracity adversary challenged round 5 for
pinning byte-exact assertions on a `Privileges:` line that plain `SHOW PROCESS`
prints in OVMX, and for a `" %-16s %s"` privilege-line format that contradicts
§4 of this very file. Neither could be settled by argument.

```
30-JUL-2026 16:42:44.38   User: SYSTEM           Process ID:   2020021A
                          Node: VAX1             Process name: "SYSTEM"

Terminal:           OPA0:
User Identifier:    [SYSTEM]
Base priority:      4
Default file spec:  SYS$SYSROOT:[SYSMGR]

Devices allocated:  VAX1$OPA0:
```

**Pinned:**

1. **Plain `SHOW PROCESS` prints NOTHING about privileges.** There is no
   `Privileges:` line and no privilege summary of any kind. Privileges appear
   only under `/PRIVILEGES` (§4), in two named blocks.
2. Plain `SHOW PROCESS` prints **no quota block** either. `Process quotas:`
   belongs to `/QUOTAS`.
3. Labels are left-justified in a **20-column** field: `Terminal:`,
   `User Identifier:`, `Base priority:` and `Default file spec:` all place
   their value at column 21.
4. `User Identifier:` shows the **rights identifier** (`[SYSTEM]`), not the
   octal UIC, when one exists for the UIC.
5. The header carries `Node:` and the line pair is
   `User:`/`Process ID:` then `Node:`/`Process name:`.
6. There is a `Devices allocated:` section.

**Acted on under vms-2b8 (round 6).** Only (1). `src/vmsdcl/dcl_cmd_show.c`'s
`Privileges:` line is DELETED rather than reformatted — VMS has no such output,
so there is nothing to match and the condition is made unreachable
(CLAUDE.md Rule 10). The whole-mask assertions in
`tests/qemu/test_syssvc_ident.c` moved onto the `Authorized privileges:` grid,
whose format §4 pins.

**Measured, recorded, NOT acted on** — all of (2)–(6) are real divergences that
pre-date this item and none of them is about identity ownership, which is what
vms-2b8 is scoped to. They are written down here so the item that does fix
`SHOW PROCESS`'s layout starts from a measurement instead of a guess. (4) in
particular needs RIGHTSLIST support that OVMX does not have.

### 6.1 `SHOW PROCESS/PRIVILEGES` re-captured through `cat -A`

Confirms §4's format claims byte for byte, which matters because the in-tree
code disagreed with §4 and one of the two had to be wrong:

- `Authorized privileges:` — one leading space, then **8 cells of exactly 10
  characters**. `IMPERSONATE` is 11 characters and is CLIPPED to `IMPERSONAT`,
  colliding with the next cell (` IMPERSONATDIAGNOSE  DOWNGRADE …`).
- A short final row is **not** padded: the last row ends `… VOLPRO    WORLD`
  with no trailing blanks. That is the only evidence about trailing padding,
  and OVMX trims to match it.
- `Process privileges:` — `" %-20s %s"`. Counted: ` ACNT` then 17 spaces then
  `may suppress…`, i.e. 21 columns before the description; ` IMPERSONATE` then
  10 spaces, same 21. `src/vmsdcl/dcl_cmd_show.c` printed `" %-16s %s"` and is
  corrected under this item.
- The separator between blocks is a line containing **one space**, not an
  empty line.
- With an empty current mask the `Process privileges:` heading is printed with
  **nothing under it** (§5.2). OVMX printed ` (no privileges enabled)`; that
  sentence is deleted — VMS does not emit it.

## 7. Who OWNS the VMS system tree, and MAXSYSGROUP

**Measured 30-JUL-2026 on VAX2** (`~/vax/cluster`, OpenVMS VAX V7.3; VAX2 shares
`SYS$COMMON` with VAX1 on the dual-ported `data/d0.dsk`, so these are the same
files VAX1 sees). Driven over the SIMH console FIFO, read-only queries only.

This section exists because `vms-2b8` made LOGINOUT drop to the authenticated
user's real credentials, and the first question that then has an answer is:
**who owns the files a VMS session is expected to be able to write?**

### 7.1 The system tree is owned by SYSTEM

```
$ DIRECTORY/OWNER/PROTECTION SYS$COMMON:[000000]SYSEXE.DIR,SYSLIB.DIR

Directory SYS$COMMON:[000000]

SYSEXE.DIR;1         [SYSTEM]                         (RWE,RWE,RE,RE)
SYSLIB.DIR;1         [SYSTEM]                         (RWE,RWE,RE,RE)

Total of 2 files.

$ DIRECTORY/OWNER/PROTECTION SYS$SYSROOT:[000000]SYSMGR.DIR

Directory SYS$SYSROOT:[000000]

SYSMGR.DIR;1         [SYSTEM]                         (RWE,RWE,RE,RE)

Directory SYS$COMMON:[000000]

SYSMGR.DIR;1         [SYSTEM]                         (RWE,RWE,RE,RE)

Grand total of 2 directories, 2 files.

$ WRITE SYS$OUTPUT F$FILE_ATTRIBUTES("SYS$COMMON:[000000]SYSEXE.DIR","PRO")
SYSTEM=RWE, OWNER=RWE, GROUP=RE, WORLD=RE
```

Files, not just directories:

```
$ DIRECTORY/OWNER/PROTECTION SYS$SYSTEM:LOGINOUT.EXE,AUTHORIZE.EXE,SYSUAF.DAT

Directory SYS$COMMON:[SYSEXE]

LOGINOUT.EXE;1       [SYSTEM]                         (RWED,RWED,RWED,RE)
AUTHORIZE.EXE;1      [SYSTEM]                         (RWED,RWED,RWED,RE)
SYSUAF.DAT;1         [SYSTEM]                         (RWE,RWE,,)

Total of 3 files.

$ DIRECTORY/OWNER/PROTECTION SYS$MANAGER:SYLOGIN.COM,SYSTARTUP_VMS.COM

Directory SYS$COMMON:[SYSMGR]

SYLOGIN.COM;3        [SYSTEM]                         (RWED,RWED,RWED,RE)
SYSTARTUP_VMS.COM;7
                     [SYSTEM]                         (RWED,RWED,RWED,RE)
```

And the roots on the system disk:

```
$ DIRECTORY/OWNER/PROTECTION SYS$SYSDEVICE:[000000]*.DIR

Directory SYS$SYSDEVICE:[000000]

000000.DIR;1         [1,1]                            (RWED,RWED,RE,E)
SYS0.DIR;1           [SYSTEM]                         (RWE,RWE,RE,RE)
SYS1.DIR;1           [SYSTEM]                         (RWE,RWE,RE,RE)
SYS10.DIR;1          [SYSTEM]                         (RWE,RWE,RE,RE)
SYS2.DIR;1           [SYSTEM]                         (RWE,RWE,RE,RE)
SYSEXE.DIR;1         [SYSTEM]                         (R,R,,)
TCPIP$FTP.DIR;1      [TCPIP$AUX,TCPIP$FTP]            (RWE,RWE,RE,E)
UNZIP.DIR;1          [1,1]                            (RWE,RWE,RE,E)
VMS$COMMON.DIR;1     [SYSTEM]                         (RWE,RWE,RE,RE)

Total of 9 files.
```

**The two facts OVMX has to reproduce**, and they are a pair — either alone
describes a broken system:

1. the system tree is owned by the SYSTEM account, so SYSTEM can create and
   delete in `SYS$SYSTEM:` and `SYS$MANAGER:`; and
2. WORLD gets `RE` and no `W`, so an ordinary user cannot.

`DIRECTORY/OWNER` prints the identifier NAME (`[SYSTEM]`) rather than the
numeric UIC, because RIGHTSLIST translates it; `F$FILE_ATTRIBUTES(...,"UIC")`
does the same. The numeric value was NOT pinned in this session — which is why
`src/ovmx_init/ovmx_init.c` reads SYSTEM's UIC out of SYSUAF instead of
hardcoding `[1,4]`. Nothing in OVMX depends on the number measured here.

### 7.2 MAXSYSGROUP

```
$ MCR SYSGEN SHOW MAXSYSGROUP
Parameter Name            Current    Default     Min.     Max.     Unit  Dynamic
--------------            -------    -------    -------  -------   ----  -------
MAXSYSGROUP                     8          8         1     32768 UIC Group  D
```

So the SYSTEM protection category on this system covers UIC groups 1 through 8.
`src/libvms/syssvc/sys_security.c` used `caller_uic == 0` ("UID 0 is treated as
SYSTEM"), which is not a VMS rule at all — VMS has no root and `[0,0]` is not a
valid UIC. It is replaced by the group comparison; root falls inside it
incidentally (`0 <= 8`).

**CORROBORATED AGAINST AN INDEPENDENT SOURCE (vms-2b8 round 4, 31-JUL-2026).**
The capture above was, until this round, the ONLY evidence for MAXSYSGROUP=8
anywhere in the tree — one branch citing its own transcript, which is
self-certification, not a pin (CLAUDE.md Rule 10). The lab was mid-experiment
(vms-760's 3-node cluster join) and could not be driven again safely, so this
round corroborated the value against the VSI OpenVMS Wiki instead
(https://wiki.vmssoftware.com/UIC_Protection, fetched 31-JUL-2026): "System
refers to users with the UIC group of 0 through the value of MAXSYSGROUP (10
by default; bear in mind that numbers in a UIC are octal)" — octal 10 is
decimal 8, matching this transcript, from a source independent of both this
tree and the lab. See `src/libvms/syssvc/sys_security.c`'s `OVMX_MAXSYSGROUP`
comment for the full account, and `tests/libvms/test_protection.c` for the
mutation that now proves the boundary is 8 and not merely "some small
number."

### 7.3 Session notes (so the next reader does not repeat them)

- VAX1 was being driven concurrently by another agent's cluster run, so these
  queries were sent to **VAX2's** console instead. VAX2 mounts the same
  `SYS$COMMON`, so the answers are the same files. Two SIMH instances on one
  disk image is a known corruption risk; one console, read-only DCL, is not.
- `MCR AUTHORIZE` on VAX2 fails (`%UAF-E-NAOFIL` — SYSUAF lives under
  `[SYS0]`, and VAX2 boots `[SYS1]`) and then sits at
  `Do you want to create a new file?` swallowing every subsequent line as an
  invalid response. Answer `NO`, then `EXIT`, before sending anything else.
- `F$FAO("!XL", F$GETJPI("","UIC"))` does NOT give the numeric UIC:
  `F$GETJPI(...,"UIC")` already returns the translated STRING `[SYSTEM]`, and
  `!XL` then formats its descriptor address (observed `7FFEC964`).
