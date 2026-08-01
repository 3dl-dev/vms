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
