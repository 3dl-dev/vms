# Oracle: `SHOW SYSTEM` and `SHOW PROCESS` on OpenVMS VAX V7.3

**Item:** vms-6a7. **Node:** VAX1, OpenVMS VAX V7.3, 2026-07-30 19:53–20:05.
**Method:** the node was already up; logged in as SYSTEM on the console,
`SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST`, then each command was run between
`===Mn===` writes so the console log could be sliced and read through `cat -A`.
Column positions below are **counted from the `cat -A` output**, not eyeballed.
Documented tool output only — no disassembly, no VSI source (CLAUDE.md Rule 8).
Lab left as found: the scratch `SYS$LOGIN:SLP6A7.COM` was deleted, the caller's
privilege mask was restored with `SET PROCESS/PRIVILEGE=(ALL)` and verified with
`SHOW PROCESS/PRIVILEGES`, and `SET TERMINAL/BROADCAST` was re-issued.

**Why this was measured.** vms-8019 round 4 deleted the `---` placeholders from
OVMX's `SHOW SYSTEM` State/Pri/I-O/Page-flts/Pages columns under Rule 10 (a
not-available marker is neither "match VMS" nor "hide it") and left a comment
naming the verbatim question for this item:

> "What does OpenVMS VAX 7.3 SHOW SYSTEM print in the State, Pri, I/O, Page
> flts and Pages columns — what are the exact column widths and header spelling
> — and is any of it sourceable from what the OVMX executive actually holds?"

This file answers it, and answers the same question for `SHOW PROCESS` with a
target.

---

## 1. `SHOW SYSTEM`, verbatim

```
$ SHOW SYSTEM
OpenVMS V7.3  on node VAX1  30-JUL-2026 19:53:53.04  Uptime  0 02:02:59
  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages
20200201 SWAPPER         HIB     16        0   0 00:00:00.05         0      0   
20200206 CLUSTER_SERVER  HIB     13       11   0 00:00:00.01       206    325   
20200207 CONFIGURE       HIB     10       21   0 00:00:00.01       131    207   
20200208 LANACP          HIB     13       57   0 00:00:00.02       421    516   
2020020A IPCACP          HIB     10        7   0 00:00:00.01        97    183   
2020020B ERRFMT          HIB      8       77   0 00:00:00.02       150    227   
2020020C CACHE_SERVER    HIB     16       11   0 00:00:00.00        83    141   
2020020D OPCOM           HIB      8      230   0 00:00:00.03       358    187   
2020020E AUDIT_SERVER    HIB     10       54   0 00:00:00.03       521    816   
2020020F JOB_CONTROL     HIB     10       36   0 00:00:00.02       245    404   
20200210 QUEUE_MANAGER   HIB     10       39   0 00:00:00.04       848   1224   
20200211 SECURITY_SERVER HIB     10       22   0 00:00:00.06       794   1178   
20200212 SMISERVER       HIB      9       34   0 00:00:00.05       350    551   
20200213 TP_SERVER       HIB      8      495   0 00:00:00.01       203    323   
20200214 NETACP          HIB     10       29   0 00:00:00.11       161    378   
20200215 EVL             HIB      6       55   0 00:00:00.04       413    507  N
20200216 REMACP          HIB      8        8   0 00:00:00.00        86     62   
20200217 TCPIP$INETACP   HIB     10       49   0 00:00:00.05       295    237   
20200218 TCPIP$FTP_1     LEF     10      107   0 00:00:00.16      2117    662  N
2020021C SYSTEM          CUR      7      238   0 00:00:00.29      5843    393   
```

### 1.1 Field geometry (0-based columns, counted through `cat -A`)

The heading line is **77** characters; every process row is **80**.

| Field          | Columns | Justification | Format                        |
|----------------|---------|---------------|-------------------------------|
| Pid            | 0–7     | —             | `%08X`, uppercase hex, **no leading space** |
| (separator)    | 8       | —             | one space                     |
| Process Name   | 9–23    | left          | `%-15s`                       |
| (separator)    | 24      | —             | one space                     |
| State          | 25–29   | left          | `%-5s` (all observed values are 3 chars: `HIB`, `LEF`, `CUR`) |
| Pri            | 30–34   | right         | `%5d`                         |
| I/O            | 35–43   | right         | `%9d`                         |
| CPU            | 44–59   | —             | `%4d %02d:%02d:%02d.%02d` — days, space, `hh:mm:ss.cc` |
| Page flts      | 60–69   | right         | `%10d`                        |
| Pages          | 70–76   | right         | `%7d`                         |
| (process type) | 77–79   | —             | three spaces, or `  N`        |

The heading words sit at: `Pid`@2, `Process Name`@9, `State`@25, `Pri`@32,
`I/O`@41, `CPU`@51, `Page flts`@61, `Pages`@72. Note that `Pri`, `I/O`,
`Page flts` and `Pages` are right-aligned on their value fields while `CPU` is
**centred** over its 16-column field (44–59, centre 51.5 → the word starts at
51). The trailing process-type column has **no heading**.

Rows are padded to the full 80 columns: a row with no type letter ends in three
spaces (visible in the `cat -A` capture as `      0   ^M$`). The heading is
**not** padded past `Pages`.

`EVL` and `TCPIP$FTP_1` carry `N` in the type column; both are network
processes. The other letters DCL documents for this column (`B` batch,
`S` subprocess) were not observed in this session and are **not** pinned here.

### 1.2 Enumeration is not privileged — and NOTHING in the row is

Re-run with `SET PROCESS/PRIVILEGE=(NOALL)` in force, `SHOW SYSTEM` printed the
identical table — every process, including `TCPIP$FTP_1` in another UIC group,
**with its process name** — while `SHOW PROCESS/ID=20200218` in the same state
was refused (§3.3). This re-confirms `vax73-privileges.md` §5.5 from a second
session: enumeration is not privileged, identity is.

**AND IT GOES FURTHER THAN §5.5 INFERRED.** Verbatim, from the `NOALL`
capture:

```
20200217 TCPIP$INETACP   HIB     10       49   0 00:00:00.05       295    237   
20200218 TCPIP$FTP_1     LEF     10      107   0 00:00:00.16      2117    662  N
```

Every column is present — State, Pri, I/O, **CPU**, Page flts, Pages — for a
process the same caller could not `$GETJPI` a single item from two commands
earlier. So on VMS **no part of a `SHOW SYSTEM` row is privileged**, not just
the process name.

`vax73-privileges.md` §5.5 concluded that a redacted row should carry "only
what SHOW SYSTEM displays of it (the process ID and the process name)". The
first half of that sentence is right and the parenthesis is wrong: SHOW SYSTEM
displays the *whole row*. OVMX's `vms_ioctl_procscan()` zeroes `linux_pid` on a
redacted row, so `cpu_time_of()` cannot source a CPU figure for it and
`SHOW SYSTEM` prints none — a **measured divergence from VMS**, whose fix is in
the executive (carry the accounting datum on a redacted row) and not in the
display. Raised by vms-6a7; it is not vms-6a7's to change, because it alters
the redaction policy vms-8019 landed.

### 1.3 Every process has a name

Across every capture in this session, **no row had an empty Process Name
column** — including `SWAPPER`, which no user created. This is the oracle side
of vms-d0e ("OVMX assigns no default process name at creation, so
`JPI$_PRCNAM` is empty where VMS always has a name"): on VMS the blank-name
condition **does not occur**, so there is no VMS rendering of it to copy.

**NOT PINNED, and the attempt is recorded so the next agent does not repeat
it.** What the executive assigns to a process created with **no** `prcnam` was
not established. The attempt was
`RUN/DETACHED/INPUT=SYS$LOGIN:SLP6A7.COM/OUTPUT=NL: SYS$SYSTEM:LOGINOUT.EXE`
(no `/PROCESS_NAME`); it returned `%RUN-S-PROC_ID, identification of created
process is 2020021D`, but the process had already exited by the next
`SHOW SYSTEM` and `SHOW PROCESS/ID=2020021D` answered
`%SYSTEM-W-NONEXPR, nonexistent process`. A detached `LOGINOUT.EXE` with
`/OUTPUT=NL:` swallows its own failure, so the next attempt needs a real
`/OUTPUT` file (or a long-lived image that is not `LOGINOUT`) to see why it
died. **vms-d0e still owns this question; it is not answered here.**

---

## 2. Plain `SHOW PROCESS`, verbatim

```
$ SHOW PROCESS

30-JUL-2026 19:56:39.23   User: SYSTEM           Process ID:   2020021C
                          Node: VAX1             Process name: "SYSTEM"

Terminal:           OPA0:
User Identifier:    [SYSTEM]
Base priority:      4
Default file spec:  SYS$SYSROOT:[SYSMGR]

Devices allocated:  VAX1$OPA0:
```

There is a **blank line before the header pair** and a blank line after it —
both are real output lines, not console noise. This reproduces
`vax73-privileges.md` §6 from an independent session and adds the leading
blank line, which §6's transcript did not show.

### 2.1 Field geometry

Header line 1: `%2d-%s-%04d %02d:%02d:%02d.%02d   User: %-17sProcess ID:   %08X`

- the date/time occupies columns 0–22, then **three** spaces;
- `User:` @26, its value @32 in a **17-column left-justified field** (so the
  next word starts at 49);
- `Process ID:` @49, then **three** spaces, then `%08X` @63.

Header line 2: 26 spaces, then
`Node: %-17sProcess name: "%s"` — `Node:` @26, value @32 (same 17-column
field), `Process name:` @49, one space, then the quoted name @63.

Body lines: `%-20s%s` — the label **including its colon** is left-justified in
a 20-column field, so every value starts at column 20. Confirmed on
`Terminal:`, `User Identifier:`, `Base priority:`, `Default file spec:` and
`Devices allocated:`.

### 2.2 What plain `SHOW PROCESS` does NOT print

- **Nothing about privileges.** No `Privileges:` line, no summary of any kind
  (`vax73-privileges.md` §6 (1), independently reproduced here).
- **No quota block.** `Process Quotas:` belongs to `/QUOTAS` and `/ALL`.
- `Devices allocated:` is **absent entirely** for a process that has none —
  see the `AUDIT_SERVER` captures in §3, which have no such section.

---

## 3. `SHOW PROCESS` with a target

### 3.1 By name, and by `/IDENTIFICATION`

```
$ SHOW PROCESS AUDIT_SERVER

30-JUL-2026 19:55:40.10   User: AUDIT$SERVER     Process ID:   2020020E
                          Node: VAX1             Process name: "AUDIT_SERVER"

Terminal:           
User Identifier:    [SYSTEM]
Base priority:      8
Default file spec:  Not available
```

`SHOW PROCESS/ID=2020020E` produced a byte-identical block (only the timestamp
differs). Three further properties were measured:

- **`Default file spec:  Not available`** — the literal VMS prints when it
  cannot read the target's default directory. This is VMS's own rendering of
  exactly the condition a cross-process reader is in; it is not a placeholder.
- **`Terminal:` is printed with an EMPTY value** (the label padded to 20
  columns, then nothing) for a process with no terminal.
- **The name parameter is upcased.** `SHOW PROCESS audit_server` resolved
  `AUDIT_SERVER`.
- **`/IDENTIFICATION` wins over a name parameter.**
  `SHOW PROCESS/ID=2020020E SYSTEM` reported `AUDIT_SERVER` (pid 2020020E),
  not `SYSTEM`.

### 3.2 A target that does not exist

```
$ SHOW PROCESS NOSUCHPROC
%SYSTEM-W-NONEXPR, nonexistent process

$ SHOW PROCESS/ID=20200999
%SYSTEM-W-NONEXPR, nonexistent process
```

Severity **W**, facility `SYSTEM` — matching `$SSDEF` `SS$_NONEXPR` = 2280
(2280 & 7 = 0 = warning), already pinned in `vax73-privileges.md` §1.

A process ID is matched **whole**: `SHOW PROCESS/ID=21C`, the low 12 bits of
the live `2020021C`, also answered `%SYSTEM-W-NONEXPR` rather than resolving
the caller. There is no partial or short-form match.

### 3.2.1 `/IDENTIFICATION=0` means the caller

```
$ SHOW PROCESS/ID=0

30-JUL-2026 20:08:20.75   User: SYSTEM           Process ID:   2020021C
                          Node: VAX1             Process name: "SYSTEM"

Terminal:           OPA0:
User Identifier:    [SYSTEM]
Base priority:      4
Default file spec:  SYS$SYSROOT:[SYSMGR]

Devices allocated:  VAX1$OPA0:
```

Zero selects the calling process — the same rule `$GETJPI` documents for a
`pidadr` of 0, surfaced at the DCL layer. Note the **full** self display
(real `Terminal:`, real `Default file spec:`, `Devices allocated:` section):
selecting yourself by ID is not a "remote" read.

### 3.2.2 A malformed `/IDENTIFICATION` value

```
$ SHOW PROCESS/ID=ZZZZ
%SHOW-E-INVQUAVAL, value 'ZZZZ' invalid for /IDENTIFICATION qualifier
```

Facility **SHOW** (the command, not `SYSTEM`), severity **E**, and the
offending value is quoted with single quotes. This is a DCL-layer rejection
that never reaches `$GETJPI`.

### 3.3 A target the caller may not read — and the two answers differ

With `SET PROCESS/PRIVILEGE=(NOALL)` in force, against `TCPIP$FTP_1`
(pid `20200218`), which is in a **different UIC group** from the caller:

```
$ SET PROCESS/PRIVILEGE=(NOALL)
$ SHOW PROCESS TCPIP$FTP_1
%SYSTEM-W-NONEXPR, nonexistent process
$ SHOW PROCESS/ID=20200218
%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation
```

**THE TWO SELECTORS ANSWER DIFFERENTLY, AND THE DIFFERENCE IS THE POINT.**

- **By NAME**, an out-of-group process is **not found at all** — `NONEXPR`,
  the same answer as a name that was never created. The name search is scoped
  to the caller's UIC group. This closes `vax73-privileges.md` §5.7, which
  recorded the by-name scope as unmeasured, in the direction OVMX's
  `find_by_name()` already implements.
- **By PID**, the process *is* found and the identity read is then refused —
  `NOPRIV` (`SS$_NOPRIV` = 36, 36 & 7 = 4 = severe/F).

In the same `NOALL` state a **same-group** target was reported in full
(`SHOW PROCESS AUDIT_SERVER` printed its whole block), re-confirming
`vax73-privileges.md` §5.2: a same-group identity read needs no privilege.

---

## 4. `SHOW PROCESS/ALL`, verbatim — recorded, NOT implemented

`/ALL` means "all information about **this** process", **not** "all
processes". OVMX's `/ALL` prints a `Process Name / PID / UIC / State` table
containing one fabricated row — the wrong shape *and* a fabrication. That is
tracked as **vms-70eb** and was deliberately left alone by vms-6a7; this
capture is recorded so vms-70eb starts from a measurement.

```
$ SHOW PROCESS/ALL

30-JUL-2026 19:58:22.97   User: SYSTEM           Process ID:   2020021C
                          Node: VAX1             Process name: "SYSTEM"

Terminal:           OPA0:
User Identifier:    [SYSTEM]
Base priority:      4
Default file spec:  SYS$SYSROOT:[SYSMGR]

Devices allocated:  VAX1$OPA0:

Process Quotas:
 Account name: SYSTEM  
 CPU limit:                      Infinite  Direct I/O limit:       100
 Buffered I/O byte count quota:     47872  Buffered I/O limit:     100
 Timer queue entry quota:              30  Open file quota:        300
 Paging file quota:                 38808  Subprocess quota:        10
 Default page fault cluster:           64  AST quota:               98
 Enqueue quota:                       200  Shared file limit:        0
 Max detached processes:                0  Max active jobs:          0

Accounting information:
 Buffered I/O count:       273  Peak working set size:        812
 Direct I/O count:          49  Peak virtual size:           5320
 Page faults:             6310  Mounted volumes:                0
 Images activated:          13
 Elapsed CPU time:          0 00:00:00.32
 Connect time:              0 02:01:40.18
 
Authorized privileges:
 ... (as vax73-privileges.md §4)
 
Process privileges:
 ... (as vax73-privileges.md §4)
 
Process rights:
 SYSTEM                            resource
 INTERACTIVE                       
 LOCAL                             
 
System rights:
 SYS$NODE_VAX1                     
 
Auto-unshelve: on
 
Image Dump: off
 
Scheduling class name: none

Process Dynamic Memory Area  
    Current Size (bytes)         51200    Current Total Size (pages)     100
    Free Space (bytes)           46720    Space in Use (bytes)          4480
    Size of Largest Block        46640    Size of Smallest Block          24
    Number of Free Blocks            3    Free Blocks LEQU 64 Bytes        2

There is 1 process in this job: 

  SYSTEM (*)
```

Note the section heading here is `Process Quotas:` (capital Q), where the
`/QUOTAS` heading recorded elsewhere in the tree is `Process quotas:`. Both
spellings are as captured; neither was normalised.

---

## 5. What OVMX took from this — per column, and what it deliberately did not

The OVMX executive's process row (`struct vms_procinfo`,
`src/kernel/vms_ioctl.h`) holds: `vms_pid`, `linux_pid`, `prcnam`, `uic`,
`current_mode`, `cur_privs`, `perm_privs`, `username`, plus a `redacted` flag.
`JPI$_CPUTIM` is derived from the row's `linux_pid`. Nothing else exists.

### 5.1 `SHOW SYSTEM`

| VMS column   | Sourceable from the executive? | OVMX |
|--------------|-------------------------------|------|
| Pid          | **yes** (`vms_pid`)           | printed, `%08X` at column 0, VMS geometry |
| Process Name | **yes** (`prcnam`)            | printed, `%-15s` at columns 9–23, VMS geometry |
| State        | no — OVMX has no VMS scheduler state, and mapping a Linux task state onto `CUR`/`COM`/`LEF`/`HIB` would be an unpinned invention | **absent** |
| Pri          | no — the executive holds no VMS priority | **absent** |
| I/O          | no                            | **absent** |
| CPU          | **yes** (`JPI$_CPUTIM`)       | printed, VMS's own `%4d %02d:%02d:%02d.%02d` |
| Page flts    | no — `/proc` has a figure, but a VMS command is a reader of an *executive* facility (Rule 11), not a second source | **absent** |
| Pages        | no                            | **absent** |
| type (B/N/S) | no                            | **absent** (it has no heading on VMS either) |

The retained columns keep **VMS's own width, justification and spelling**; the
unsourceable columns are removed **whole** — heading text and field together —
and the remaining columns close up. That leaves a table that is visibly
narrower than VMS's rather than a VMS-shaped table with invented content in it,
which is the Rule 10 answer the `---` markers were not.

### 5.2 `SHOW PROCESS`

| VMS line             | OVMX |
|----------------------|------|
| header pair (date, `User:`, `Process ID:`, `Node:`, `Process name:`) | **printed**, at the §2.1 geometry, every value read from the target's executive row |
| `Terminal:`          | **absent** — the executive holds no terminal for a process; OVMX's terminal is still a per-process `VMS_TERMINAL` environment variable (vms-d0b), which is not an executive facility to read |
| `User Identifier:`   | **printed**, from `JPI$_UIC`, as the octal `[group,member]`. VMS shows the *rights identifier* (`[SYSTEM]`) when one exists; OVMX has no RIGHTSLIST, a pre-existing divergence recorded in `vax73-privileges.md` §6 (4) and not owned by vms-6a7 |
| `Base priority:`     | **absent** — same reason as the `Pri` column above |
| `Default file spec:` | **printed**: the caller's own default directory for its own row, and VMS's own literal `Not available` for any other process — the same condition VMS renders that way |
| `Devices allocated:` | **absent**, which is what VMS prints for a process with none (§3.1) |
| `Privileges:`        | never existed on VMS (§2.2); the invented OVMX line is **deleted** |
| `Process quotas:`    | belongs to `/QUOTAS` (§2.2); the inline fabricated block is **deleted** from plain `SHOW PROCESS` |

### 5.3 Divergences this oracle work EXPOSED but did not close

Every row below is a place where OVMX does **not** match what §1–§4 measured.
They are listed here, in the oracle document itself, because a divergence that
lives only in a source comment is one the test suite reports as green. Each
names its owning item; **none is owned by vms-6a7**, which is the *reader* of
the executive's process row.

| Divergence | What VMS does (measured) | What OVMX does | Owner |
|---|---|---|---|
| **Empty identity is RENDERED** | Never occurs. §1.3: across every VAX1 capture no process had an empty Process Name, not even `SWAPPER`; §2: `User:` is populated. | Prints `User:` blank and `Process name: ""`, because `$CREPRC` stamps no identity onto the executive row. This is a **Rule 10 illegal third answer** — a plausible rendering of a state VMS never reaches — and the correct fix is to make the state unreachable, not to render it better. | **vms-afd** (identity propagation), vms-d0e (default process name), vms-2b8/vms-d0b (identity enforcement) |
| **`Terminal:` omitted** | Always printed (§2, §3.1). | Absent. The executive holds no terminal; OVMX's terminal is a per-process `VMS_TERMINAL` environment variable, which is not an executive facility to read (Rule 11). Removing the line whole is the vms-8019 round-4 ruling ("remove the unsourceable column WHOLE") applied to `SHOW PROCESS`'s body rather than `SHOW SYSTEM`'s table — an **extension of that ruling made by the implementer, and flagged for the operator rather than assumed**. | vms-d0b |
| **`Base priority:` omitted** | Always printed (§2, §3.1). | Absent, same reason as the `Pri` column in §5.1: the executive holds no VMS priority. Same extension-of-ruling caveat as `Terminal:`. | unowned — file with vms-d0b's cascade |
| **No CPU figure on a redacted row** | VMS prints the **complete** row including CPU for a process whose identity the caller may not read — measured in §1.2 with no privileges held. | The row is listed (enumeration is unprivileged, §1.2) but its CPU field is blank, because `procscan` zeroes `linux_pid` on a redacted row and `JPI$_CPUTIM` is derived from it. **Deliberately left blank**: carrying the accounting datum on a redacted row would widen what the executive discloses, and that belongs to the owner of the redaction rule. Operator ruling on vms-6a7 round 2: "I would rather ship a disclosed narrower row than quietly widen what a redacted row leaks." Guarded by `tests/qemu/test_syssvc_procnam.c` block P12. | redaction-rule owner (vms-2b8 cascade) |
