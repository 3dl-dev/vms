# Oracle transcript — event flag statuses and `$ASCEFC` semantics

**Node:** VAX1 (`~/vax/cluster`), OpenVMS VAX **V7.3**, SIMH MicroVAX 3900.
**Date:** 30-JUL-2026. **Items:** `vms-68c` (the status constants), `vms-2a8` (the wiring).
**Session banner:** `OpenVMS V7.3  on node VAX1  30-JUL-2026 21:32:10.60  Uptime  0 00:32:18`

Clean-room (CLAUDE.md Rule 8): everything below is **documented tool output** — the macro
library shipped with the OS, the `F$MESSAGE` lexical, and the online `HELP` library. No
binary was disassembled.

---

## 1. Status constants — two independent methods

### Method 1 — `LIBRARY/EXTRACT=$SSDEF` from `SYS$LIBRARY:STARLET.MLB`, then `SEARCH`

```
$ LIBRARY/MACRO/EXTRACT=$SSDEF/OUTPUT=SYS$SCRATCH:SSDEF_2A8.MAR SYS$LIBRARY:STARLET.MLB
$ SEARCH SYS$SCRATCH:SSDEF_2A8.MAR "SS$_WASCLR","SS$_WASSET","SS$_ILLEFC","SS$_UNASEFC","SS$_NORMAL","SS$_INSFMEM"
$EQU    SS$_NORMAL      1
$EQU    SS$_WASCLR      1
$EQU    SS$_WASSET      9
$EQU    SS$_ILLEFC      236
$EQU    SS$_INSFMEM     292
$EQU    SS$_UNASEFC     564
```

### Method 2 — `F$MESSAGE` round-trip on the numeric values

```
$ WRITE SYS$OUTPUT "1   -> " + F$MESSAGE(1)
1   -> %SYSTEM-S-NORMAL, normal successful completion
$ WRITE SYS$OUTPUT "5   -> " + F$MESSAGE(5)
5   -> %NONAME-?-NOMSG, Message number 00000005
$ WRITE SYS$OUTPUT "9   -> " + F$MESSAGE(9)
9   -> %SYSTEM-S-ACCVIO, access violation, reason mask=!XB, virtual address=!XL, PC=!XL, PSL=!XL
$ WRITE SYS$OUTPUT "44  -> " + F$MESSAGE(44)
44  -> %SYSTEM-F-ABORT, abort
$ WRITE SYS$OUTPUT "48  -> " + F$MESSAGE(48)
48  -> %SYSTEM-W-BADATTRIB, bad attribute control list
$ WRITE SYS$OUTPUT "236 -> " + F$MESSAGE(236)
236 -> %SYSTEM-F-ILLEFC, illegal event flag cluster
$ WRITE SYS$OUTPUT "292 -> " + F$MESSAGE(292)
292 -> %SYSTEM-F-INSFMEM, insufficient dynamic memory
$ WRITE SYS$OUTPUT "564 -> " + F$MESSAGE(564)
564 -> %SYSTEM-F-UNASEFC, unassociated event flag cluster
$ WRITE SYS$OUTPUT "2260-> " + F$MESSAGE(2260)
2260-> %SYSTEM-F-IDXFILEFULL, index file is full
```

### What this decides

| Symbol | Oracle | OVMX before | Verdict |
|---|---|---|---|
| `SS$_NORMAL`  | 1   | 1 (`ssdef.h`) | correct |
| `SS$_WASCLR`  | **1** | 1 (`ssdef.h`) / **5** (`vms_internal.h`) | kernel **wrong** — 5 is not a status at all (`NOMSG`) |
| `SS$_WASSET`  | 9   | 9 both | correct |
| `SS$_ILLEFC`  | **236** | **2260** (`ssdef.h`) / **44** (`vms_internal.h`) | **both wrong** — 2260 is `IDXFILEFULL`, 44 is `ABORT` |
| `SS$_UNASEFC` | 564 | 564 (`ssdef.h`) / **48** (`vms_internal.h`) | kernel **wrong** — 48 is `BADATTRIB` |
| `SS$_INSFMEM` | 292 | 292 (`ssdef.h`) / **0x2C = 44** (`vms_internal.h`) | kernel **wrong** — and its comment claimed 44 "matches real VMS" |

**`SS$_WASCLR` really is `SS$_NORMAL`.** Both are 1 in `$SSDEF`, and `F$MESSAGE(1)` has exactly
one rendering, `%SYSTEM-S-NORMAL`. So a caller of `$SETEF`/`$CLREF`/`$READEF` distinguishes
"was clear" from "was set" by testing against `SS$_WASSET` (9), **not** by expecting a
distinct WASCLR value. `vms-68c` was filed on the premise that the alias was an OVMX defect;
the oracle says the alias is VMS. The defect was only ever the kernel's 5.

**Also observed, NOT acted on** (raised as a finding, not fixed here): `F$MESSAGE(9)` renders
`%SYSTEM-S-ACCVIO`, because `SS$_WASSET` (9) and `SS$_ACCVIO` (12) share message number 1 and
differ only in severity. OVMX's `src/libvms/status.c` renders 9 as `WASSET`. Reconciling
OVMX's message tables with the oracle's severity-folded message file is its own question.

---

## 2. `$ASCEFC` — which `efn` selects which common cluster

`HELP/NOPROMPT SYSTEM_SERVICES $ASCEFC Arguments`, verbatim:

```
       To associate with common event flag cluster 2, specify any flag
       number in the cluster (64 to 95); to associate with common event
       flag cluster 3, specify any event flag number in the cluster (96
       to 127).
```

So **any** flag number in the range selects the cluster — not only the base numbers 64 and 96.
`src/kernel/vms_eflag.c` accepted exactly 64 or 96 and answered `SS$_ILLEFC` for 65..95 and
97..127, which are legal on VMS.

`HELP/NOPROMPT SYSTEM_SERVICES $ASCEFC`, verbatim:

```
       Associates a named common event flag cluster with a process
       to execute the current image and to be assigned a process-
       local cluster number for use with other event flag services.
       If the named cluster does not exist but the process has suitable
       privilege, the service creates the cluster.
```

and on `perm`:

```
         Permanent specifier that marks a common event flag cluster as
         either permanent or temporary. The perm argument is a longword
         value, which is interpreted as Boolean.

         The default value 0 specifies that the cluster is temporary. The
         value 1 specifies that the cluster is permanent.
```

## 3. `$DLCEFC`

`HELP/NOPROMPT SYSTEM_SERVICES $DLCEFC`, verbatim:

```
  $DLCEFC

       Marks a permanent common event flag cluster for deletion.

       Format

         SYS$DLCEFC  name
```

"Marks ... for deletion" — the cluster goes away when the last associated process
disassociates, which is exactly the refcount rule `src/kernel/vms_eflag.c` already applies to
temporary clusters. `$DLCEFC` is therefore "clear the permanent bit, then apply the ordinary
temporary-cluster lifetime", and it is implemented that way.

---

## 4. What `$WAITFR` does when the wait is interrupted — session 2

**Second session, same node.** Banner: `VAX/VMS V7.3  node VAX1`, login
`30-JUL-2026 22:19`, run for `vms-2a8` round 2. Same clean-room method: online `HELP`
plus the `$SSDEF` macro shipped with the OS. Nothing disassembled.

### 4.1 `$WAITFR` has exactly two outcomes, and neither is "interrupted"

```
$ HELP/NOPROMPT SYSTEM_SERVICES $WAITFR

SYSTEM_SERVICES

  $WAITFR

       Tests a specific event flag and returns immediately if the flag
       is set; otherwise, the process is placed in a wait state until
       the event flag is set.

       Format

         SYS$WAITFR  efn

       C Prototype

         int sys$waitfr  (unsigned int efn);

    Additional information available:

    Argument
```

Note what the "Additional information available" list contains: **`Argument`, and nothing
else.** There is no `Condition Values Returned` topic for `$WAITFR` in the online help,
which is consistent with the description — the service returns *when the flag is set*.

`$WFLOR` and `$WFLAND` are the same shape:

```
$ HELP/NOPROMPT SYSTEM_SERVICES $WFLOR
  $WFLOR
       Allows a process to specify a set of event flags for which it
       wants to wait.
    Additional information available:
    Arguments

$ HELP/NOPROMPT SYSTEM_SERVICES $WFLAND
  $WFLAND
       Allows a process to specify a set of event flags for which it
       wants to wait.
    Additional information available:
    Arguments
```

### 4.2 A wait state IS interruptible — and the process stays in it

```
$ HELP/NOPROMPT SYSTEM_SERVICES $HIBER

  $HIBER

       Allows a process to make itself inactive but to remain known to
       the system so that it can be interrupted; for example, to receive
       ASTs.
```

"remain known to the system so that it can be **interrupted**" — the interruption of a
VMS wait is an **AST**, the AST executes, and the process is still waiting afterwards.
Interruption is not an outcome the waiting service reports; it is not visible to the
caller of the wait at all.

### 4.3 The negative observation: there is no "wait interrupted" status

```
$ LIBRARY/MACRO/EXTRACT=$SSDEF/OUTPUT=SYS$SCRATCH:SSDEF_2A8B.MAR SYS$LIBRARY:STARLET.MLB
$ SEARCH SYS$SCRATCH:SSDEF_2A8B.MAR "WAIT","INTERRUPT","ABORTED"
$EQU    SS$_NOWAIT      10236
$EQU    SS$_WAITUSRLBL  2384
$EQU    SS$_WAIT_CALLERS_MODE   4018
$EQU    SS$_AVRWAIT     11040
```

Four hits in the whole of `$SSDEF`, none of them a wait-was-interrupted condition
(`SS$_NOWAIT` is the "would have blocked" answer of services that were asked not to wait,
`SS$_WAITUSRLBL` is a magnetic-tape user-label condition, `SS$_WAIT_CALLERS_MODE` is an
access-mode flag, `SS$_AVRWAIT` is an automatic-volume-recognition condition). No symbol
containing `INTERRUPT` or `ABORTED` exists at all.

### What this decides (`vms-2a8` round 2)

`src/kernel/vms_eflag.c` treated a `wait_event_interruptible()` return as terminal and
answered `SS$_NORMAL` — "the flag is set" — for a flag that was still clear. Under
CLAUDE.md Rule 10 that is the illegal third answer: a plausible-looking handler for a
condition VMS never faces. VMS offers no status to return here **and the oracle above is
how that is known, not assumed**, so the condition is made UNREACHABLE instead:

* the executive abandons the ioctl with `-ERESTARTSYS` and writes **no status at all**, and
* `src/libvmssys/vms_kif.c`'s `kif_wait_call()` re-enters the wait,

so `$WAITFR`/`$WFLOR`/`$WFLAND` cannot return while their predicate is false — which is
exactly §4.1's "the process is placed in a wait state until the event flag is set", with
§4.2's interruption running in between and leaving no trace in the caller's status.

---

## Reproducing

The lab console for VAX1 is reachable on the SIMH DZ mux at `127.0.0.1:2001`; log in
prompt-synchronised (`Username:` → `SYSTEM`, `Password:` → `system`) — see
`~/vax/cluster/README-lab.md` §"Operator runbook".
