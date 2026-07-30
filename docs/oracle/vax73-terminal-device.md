# Oracle capture: the console terminal as a VMS device (OpenVMS VAX V7.3)

**Item:** `vms-d0b` · **Captured:** 30-JUL-2026 · **Oracle:** the `~/vax/cluster` reference lab,
OpenVMS VAX V7.3 on SIMH, nodes **VAX1** (system id 1025) and **VAX2** (1026), driven over the SIMH
console by `nodedrv.py` (transcripts `/tmp/clean-vax1-test/vax1.log`, `vax2.log`).

**Method (CLAUDE.md rule 8 — clean room).** Everything below is *observed behaviour of the running
system through its documented operator interface* (DCL `SHOW TERMINAL`, `SHOW DEVICE`,
`SET TERMINAL`). No VSI/HPE binary was disassembled, decompiled or read. Nothing here is a byte
layout: these are the names, the formats and the values VMS itself prints.

**Why it exists.** OVMX's executive device table (`src/kernel/vms_devtab.c`) has to answer questions
about a terminal — its name, its class, its owner, its characteristics — and rule 10 gives exactly
two legal ways to answer: match VMS, or do not have the thing at all. This file is the "match VMS"
side of that: the record of what VMS actually says, so no later reader has to invent it.

---

## 1. The console terminal is `OPA0:`

`SHOW TERMINAL` on the console of both lab nodes prints the physical device name with the leading
underscore:

```
Terminal: _OPA0:      Device_Type: LA36          Owner: SYSTEM
```

## 2. `SHOW TERMINAL`, verbatim — pristine console (VAX2, never had `SET TERMINAL` issued)

```
Terminal: _OPA0:      Device_Type: LA36          Owner: SYSTEM

   Input:     300     LFfill:  0      Width: 132      Parity: None
   Output:    300     CRfill:  0      Page:   24      

Terminal Characteristics:
   Interactive        Echo               Type_ahead         No Escape
   No Hostsync        TTsync             Lowercase          No Tab
   Wrap               Hardcopy           No Remote          No Eightbit
   Broadcast          No Readsync        No Form            Fulldup
   No Modem           No Local_echo      No Autobaud        No Hangup
   No Brdcstmbx       No DMA             No Altypeahd       Set_speed
   No Commsync        Line Editing       Insert editing     No Fallback
   No Dialup          No Secure server   No Disconnect      No Pasthru
   No Syspassword     No SIXEL Graphics  No Soft Characters No Printer Port
   Numeric Keypad     No ANSI_CRT        No Regis           No Block_mode
   No Advanced_video  No Edit_mode       No DEC_CRT         No DEC_CRT2
   No DEC_CRT3        No DEC_CRT4        No DEC_CRT5        No Ansi_Color
   VMS Style Input
```

Notes taken from this, not inferred:

- The characteristic list is **two-state per name**, printed in four columns, with the inactive form
  spelled `No <name>` — except `Lowercase`/`Uppercase`, `Fulldup`/`Halfdup`, and `Insert editing`,
  where the pair is a different word. `Interactive` is printed first and has no `No` form here.
- There is **no `Scope` characteristic** in the V7.3 list. `Hardcopy` is the axis.
- The header carries three fields (`Terminal:`, `Device_Type:`, `Owner:`); the second block carries
  `Input:`/`Output:` speed, `LFfill:`/`CRfill:`, `Width:`, `Page:`, `Parity:`.

**The in-tree `src/vmsdcl/dcl_terminal.c` `char_display[]` table does not match this list.** It
invents `Scope`, `Holdscreen`, `Mechtab`, `Oper`, `Page`, `Runout`, `AltTypeAhd` and omits most of
the names above. That is tracked as follow-up work; the table above is the oracle.

## 3. Device type

The console reports `Device_Type: LA36` on the lab because the VAX console *is* an LA36 hardcopy
terminal. An unidentified terminal is displayed as **`Unknown`** (capital U), observed by setting it
and reading it back on VAX1:

```
$ SET TERMINAL/DEVICE_TYPE=UNKNOWN
$ SHOW TERMINAL
Terminal: _OPA0:      Device_Type: Unknown       Owner: SYSTEM
```

With the device type Unknown, the characteristics VMS reports **set** are:

```
Interactive  Echo  Type_ahead  TTsync  Lowercase  Wrap  Hardcopy  Broadcast
Fulldup  Set_speed  Insert editing  Numeric Keypad  VMS Style Input
```

(everything else `No ...`; notably `No Line Editing`, which V7.3 clears when the type becomes
Unknown, and `Hardcopy`, which persists because it describes the physical port). The device type
also redefaults geometry: switching back to `LA36` moved `Page` from 24 to 66.

VMS's own login procedure fails to identify a console of this kind, and says so:

```
%SET-W-NOTSET, error modifying OPA0:
-SET-I-UNKTERM, unknown terminal type
```

## 4. `SHOW DEVICE` for a terminal

```
$ SHOW DEVICE OPA0:

Device                  Device           Error
 Name                   Status           Count
OPA0:                   Online               0
```

`SHOW DEVICE TT` (class prefix) returns the same single row on these nodes.

### 4.1 The brief listing's `Device Status` column, and the one thing that changes it

**Captured 30-JUL-2026, node VAX1, for `vms-fb9`.** Section 4 above records only the idle row, so
"what else can that column say" was unrecorded — and a reader that prints a constant `Online` for
every row is a self-certified generalization, not a measurement. It is measured here.

Column geometry, byte-exact (`OPA0:` at 0-4, the status at 24, the error count's last digit at 45,
line length 46 in both cases):

```
$ SHOW DEVICE OPA0:                       ! nobody has ALLOCATEd it

Device                  Device           Error
 Name                   Status           Count
OPA0:                   Online               0

$ ALLOCATE OPA0:
%DCL-I-ALLOC, _VAX1$OPA0: allocated
$ SHOW DEVICE OPA0:

Device                  Device           Error
 Name                   Status           Count
OPA0:                   Online alloc         0
```

**`alloc` is a property of the DEVICE, not of the asking process — measured A-writes/B-reads.** A
second process was created while the interactive job held the allocation, exactly as in section 7:

```
$ RUN/DETACHED/INPUT=SYS$SYSROOT:[SYSMGR]DEVPROBE.COM -
       /OUTPUT=SYS$SYSROOT:[SYSMGR]DEVPROBE.LOG/PROCESS_NAME=DEVPROBE SYS$SYSTEM:LOGINOUT.EXE
```

`DEVPROBE.COM` is three lines (`$ SET NOON`, `$ SHOW DEVICE OPA0:`, `$ LOGOUT`). Its log, while
the *other* process holds the allocation:

```
OPA0:                   Online alloc         0
```

and after that other process issued `DEALLOCATE OPA0:`, a fresh detached process printed:

```
OPA0:                   Online               0
```

Three facts, all from those four captures:

1. The status column reads **`Online`** for an idle terminal and **`Online alloc`** when the device
   is allocated. `alloc` is appended, not substituted.
2. The word is visible to a process that did **not** allocate the device, and disappears for it when
   the allocation ends. This is the brief listing behaving as a reader of shared executive state.
3. **Ownership alone does not show here.** In *every* capture above the console was owned by the
   interactive job (by channel, section 7.4 — `SHOW DEVICE/FULL` reports `Owner process "SYSTEM"`
   throughout), yet the unallocated rows read plain `Online`. So the brief column distinguishes
   allocation, not ownership. A reader must not print `alloc` for a merely-owned device.

**Not measured, so not claimed:** what this column reads for any device that is not a terminal (the
oracle's disks report `Mounted`), for an offline device, or for a status string longer than
`Online alloc`. OVMX's device table contains exactly one device, a terminal, and it is never
offline — so those are conditions OVMX does not have, and it prints nothing for them (rule 10).

## 5. `SHOW DEVICE/FULL` for a terminal

VAX1 (its console is enabled as an operator terminal):

```
$ SHOW DEVICE/FULL OPA0:

Terminal OPA0:, device type LA36, is online, enabled as operator terminal,
    record-oriented device, carriage control.

    Error count                    0    Operations completed                373
    Owner process           "SYSTEM"    Owner UIC                      [SYSTEM]
    Owner process ID        2020021A    Dev Prot              S:RWPL,O:RWPL,G,W
    Reference count                2    Default buffer size                 132
```

VAX2 (not an operator terminal — the clause is simply absent, not replaced):

```
Terminal OPA0:, device type LA36, is online, record-oriented device, carriage
    control.
```

So the device carries, and a reader may print: error count, operations completed, owner process
*name*, owner UIC, owner process ID, device protection, reference count, default buffer size.

## 6. A device that does not exist

```
$ SHOW DEVICE ZZA0:
%SYSTEM-W-NOSUCHDEV, no such device available
```

---

## 7. Ownership, `$ASSIGN` and `$ALLOC` (captured 30-JUL-2026, node VAX2)

This section exists because `src/kernel/vms_devtab.c` twice asserted an ownership rule as VMS fact
with nothing behind it — first "the first channel to an unowned device makes its holder the owner",
then, after that was deleted, "another process merely holding channels refuses `$ALLOC`". The right
answer was not to argue about it a third time but to run the experiment; **the raw console log is
`/tmp/clean-vax1-test/vax2.log` on the lab host and every claim below cites its line numbers.**

> **Note on the first claim.** It turns out to have been *right for the wrong reason* and was deleted
> on evidence that did not bear on it (a **shareable** device). Section 7.4 restores it, measured, on
> a **non-shareable** one. Sections 7.3 and 7.4 together are why: shareability is the criterion, and
> neither the original claim nor its deletion had tested that.

Method: a **second process** was created on VAX2 with

```
$ RUN/DETACHED/INPUT=...DET.COM/OUTPUT=...DET.LOG/PROCESS_NAME=DEVPROBE SYS$SYSTEM:LOGINOUT.EXE
```

while the interactive job held the console, and it issued `$ASSIGN` through a MACRO-32 program
(`$ASSIGN_S DEVNAM=DEVDSC,CHAN=CHAN` followed by `LIB$SIGNAL` of R0, so VMS prints its own message).
No VSI binary was examined; this is the running system answering through its documented interfaces.

### 7.1 `$ASSIGN` to a terminal another process owns SUCCEEDS

`SHOW DEVICE/FULL OPA0:` as seen from the detached process — the console is owned by the interactive
job, PID `20400216`:

```
Terminal OPA0:, device type LA36, is online, record-oriented device, carriage
    control, device is busy.

    Error count                    0    Operations completed                293
    Owner process           "SYSTEM"    Owner UIC                      [SYSTEM]
    Owner process ID        20400216    Dev Prot              S:RWPL,O:RWPL,G,W
    Reference count                2    Default buffer size                 132

--- ASSIGN OPA0: FROM A SECOND PROCESS ---
%SYSTEM-S-NORMAL, normal successful completion
```

An RMS `OPEN/WRITE X OPA0:` from the same process also succeeded (`OPEN-OK`).

### 7.2 `$ALLOC` in the same situation returns `SS$_DEVALLOC`

Immediately afterwards, from that same detached process:

```
--- ALLOCATE OPA0: ---
%SYSTEM-W-DEVALLOC, device already allocated to another user
```

So `SS$_DEVALLOC` is `$ALLOC`'s condition, not `$ASSIGN`'s. **A terminal owned by another process is
assignable but not allocatable.**

*Caveat recorded honestly:* `OPA0:`'s protection is `S:RWPL,O:RWPL,G,W` and the probing process ran
as `SYSTEM`. This capture therefore pins the *allocation* rule and says nothing about what an
unprivileged process gets — device protection is a separate gate OVMX does not implement.

### 7.3 A channel to a SHAREABLE device does not confer ownership

`NLA0:` — `SHOW DEVICE/FULL` calls it *"record-oriented device, **shareable**, mailbox device"* —
before, during and after a channel was held by the observing process (`vax2.log` l.562-592, and
again l.1172-1181):

```
$ SHOW DEVICE/FULL NLA0:            Owner process ""  Owner process ID 00000000  Reference count 2
$ OPEN/WRITE X NLA0:
$ SHOW DEVICE/FULL NLA0:            Owner process ""  Owner process ID 00000000  Reference count 3
$ CLOSE X
$ SHOW DEVICE/FULL NLA0:            Owner process ""  Owner process ID 00000000  Reference count 2
```

The owner fields never moved; only the reference count did. **Reference count is one per assigned
channel.**

### 7.4 A channel to a NON-SHAREABLE device DOES confer ownership

`TTA0:` is a terminal, and its status clause carries **no** `shareable`. The identical DCL sequence
gives the opposite answer (`vax2.log` l.1115-1167, verbatim):

```
$ SHOW DEVICE/FULL TTA0:

Terminal TTA0:, device type unknown, is online, record-oriented device, carriage
    control.

    Error count                    0    Operations completed                  0
    Owner process                 ""    Owner UIC                      [SYSTEM]
    Owner process ID        00000000    Dev Prot              S:RWPL,O:RWPL,G,W
    Reference count                0    Default buffer size                  80

$ OPEN/WRITE X TTA0:
$ WRITE SYS$OUTPUT "R4-OPEN-ST=" + F$MESSAGE($STATUS)
R4-OPEN-ST=%SYSTEM-S-NORMAL, normal successful completion
$ SHOW DEVICE/FULL TTA0:

Terminal TTA0:, device type unknown, is online, record-oriented device, carriage
    control.

    Error count                    0    Operations completed                  0
    Owner process           "SYSTEM"    Owner UIC                      [SYSTEM]
    Owner process ID        20400216    Dev Prot              S:RWPL,O:RWPL,G,W
    Reference count                1    Default buffer size                  80

$ DEALLOCATE TTA0:
%SYSTEM-W-DEVNOTALLOC, device not allocated
$ CLOSE X
$ SHOW DEVICE/FULL TTA0:
    ...  Owner process ""   Owner process ID 00000000   Reference count 0
```

Four facts, all in that one block:

1. **A channel alone makes the assigner the OWNER** of a non-shareable device that nobody owns.
2. It is **not an allocation**: the status clause still reads only *"is online, record-oriented
   device, carriage control"*, and `DEALLOCATE` at that instant is refused `%SYSTEM-W-DEVNOTALLOC`.
3. Ownership like this **costs no extra reference** — one channel, reference count 1.
4. **Returning the last channel ends it**: `CLOSE` put the device back to `Owner ""` / count 0.

This is why the console `OPA0:` shows `Owner process "SYSTEM"` on a system where nobody has ever run
`ALLOCATE` — the login job holds channels to it.

### 7.5 `$ALLOC` is refused while another process owns the device by channel alone

Independently reached from the other direction (`vax2.log` l.979-1038). `CHANHOLD` is a detached
process running a MACRO-32 image whose entire body is `$ASSIGN_S` to `TTA0:` followed by `$HIBER_S`
— one channel, no allocation:

```
$ SHOW DEVICE/FULL TTA0:

Terminal TTA0:, device type unknown, is online, record-oriented device, carriage
    control.
    ...
    Owner process         "CHANHOLD"    Owner UIC                      [SYSTEM]
    Owner process ID        20400218    Dev Prot              S:RWPL,O:RWPL,G,W
    Reference count                1    Default buffer size                  80

$ ALLOCATE TTA0:
%SYSTEM-W-DEVALLOC, device already allocated to another user

$ STOP CHANHOLD
$ SHOW DEVICE/FULL TTA0:
    ...  Owner process ""   Owner process ID 00000000   Reference count 0
```

So there is **one** refusal, and it is about ownership: a device somebody else owns cannot be
allocated, whether that owner allocated it (7.2) or merely assigned a channel to it. Ownership also
dies with its owner.

### 7.6 WITHDRAWN: "foreign channels alone are enough to refuse `$ALLOC`"

An earlier revision of this document claimed, as section 7.4, that `ALLOCATE NLA0:` →
`%SYSTEM-W-DEVALLOC` (`vax2.log` l.622-623) proved that channels held by *other* processes refuse an
allocation. **That was an inference presented as a measurement, and it is withdrawn.** `NLA0:` was
at its idle baseline at the time — `Owner process ""`, reference count 2, the same 2 it had before
and after the observer's own `OPEN`/`CLOSE` — and nothing established that those two references
belonged to other processes. `NLA0:` is also `shareable`, which is a likelier reason `ALLOCATE`
refused it. **Why `ALLOCATE NLA0:` fails remains unexplained here and OVMX models nothing on it.**
The rule OVMX does implement is 7.5, which was measured directly on a non-shareable device.

## 8. `ALLOCATE` sets `allocated`, adds a reference, and is idempotent

On the interactive job, which already owned `OPA0:` — by channel, per 7.4 — but had not allocated it:

```
$ ALLOCATE OPA0:
%DCL-I-ALLOC, _VAX2$OPA0: allocated
$ SHOW DEVICE/FULL OPA0:
Terminal OPA0:, device type LA36, is online, allocated, ... Reference count 3

$ ALLOCATE OPA0:
%DCL-I-ALLOC, _VAX2$OPA0: allocated
$ SHOW DEVICE/FULL OPA0:
Terminal OPA0:, device type LA36, is online, allocated, ... Reference count 3

$ DEALLOCATE OPA0:
$ SHOW DEVICE/FULL OPA0:
Terminal OPA0:, device type LA36, is online, ...            Reference count 2

$ DEALLOCATE OPA0:
%SYSTEM-W-DEVNOTALLOC, device not allocated
```

Four things are pinned here: allocation adds the word **`allocated`** to the status clause;
allocation is worth **one reference**; re-allocating a device you already have allocated succeeds and
changes nothing; and `DEALLOCATE` **does not unown the device** — the full row after the first
`DEALLOCATE` (l.689-695) is still

```
    Owner process           "SYSTEM"    Owner UIC                      [SYSTEM]
    Owner process ID        20400216    Dev Prot              S:RWPL,O:RWPL,G,W
    Reference count                2    Default buffer size                 132
```

because the job still holds channels to the console. Allocation and ownership come apart again here,
exactly as they did in 7.4.

## 9. Condition values, from VMS's own message facility

Asked directly, by scanning `F$MESSAGE(n)` on the running V7.3 system:

```
   312 %SYSTEM-W-IVCHAN, invalid I/O channel
   316 %SYSTEM-F-IVCHAN, invalid I/O channel
   320 %SYSTEM-W-IVDEVNAM, invalid device name
   324 %SYSTEM-F-IVDEVNAM, invalid device name
  2112 %SYSTEM-W-DEVALLOC, device already allocated to another user
  2116 %SYSTEM-F-DEVALLOC, device already allocated to another user
  2120 %SYSTEM-W-DEVASSIGN, device has channels assigned
  2136 %SYSTEM-W-DEVNOTALLOC, device not allocated
  2312 %SYSTEM-W-NOSUCHDEV, no such device available
  2316 %SYSTEM-F-NOSUCHDEV, no such device available
  2648 %SYSTEM-W-NOMOREDEV, no more devices
  2652 %SYSTEM-F-NOMOREDEV, no more devices
```

**This contradicts `src/libvms/include/ssdef.h` in several places.** The file's `SS$_NOMOREDEV`
(2648) is right; its `SS$_DEVALLOC` (2316), `SS$_NOSUCHDEV` (2680), `SS$_IVCHAN` (602) and
`SS$_IVDEVNAM` (608) are not. Only `SS$_DEVALLOC` was corrected as part of `vms-d0b` — it is the
constant this work introduces a use for, and its two existing consumers
(`src/vmsdcl/dcl_cmd_misc.c`, `src/vmsfs/vmsfs_device.c`) name the symbol rather than the number, so
the correction does not break them. The rest have a blast radius across the kernel module, its
client and its tests, and are tracked separately; do not "fix" them without running the whole QEMU
suite.

`2120 %SYSTEM-W-DEVASSIGN, device has channels assigned` is listed above because VMS's own message
facility printed it. **No probe ever provoked it**, so nothing in OVMX returns it: a condition known
only by its text is not a condition we can claim to reproduce (rule 10). Carried in `vms-d0b`'s
findings for filing: the probe that would settle it has to find the operation that raises
`DEVASSIGN`, not assume one.

---

## What OVMX took from this, and what it deliberately did not

| Oracle fact | OVMX (`src/kernel/vms_devtab.c`) |
|---|---|
| Console is `OPA0:` | Executive creates `OPA0:` at module init |
| Terminal is device class terminal | `DC$_TERM` (6), mirroring `src/libvms/include/dcdef.h` |
| Unidentified type displays `Unknown` | Console registers with device type 0 = Unknown |
| Characteristic **names** and their two-state form | `VMS_TTC_*` in `src/kernel/vms_ioctl.h`, one bit per oracle name |
| Absent device → `%SYSTEM-W-NOSUCHDEV` | `SS$_NOSUCHDEV` from `$ASSIGN`/`$GETDVI` |
| `$ASSIGN` succeeds on a device another process owns, and does not move ownership (7.1) | `vms_ioctl_assign` returns `SS$_NORMAL`; ownership is granted only when the device is unowned |
| A channel to a **shareable** device confers nothing (7.3) | `dev->shareable` guards the ownership grant in `vms_ioctl_assign` |
| A channel to a **non-shareable** device makes the assigner the owner, unallocated, at no extra reference (7.4) | `vms_ioctl_assign` sets `owner_*` when `!dev->shareable && owner_linux_pid == 0` |
| Returning the last channel unowns it; so does the owner's death (7.4, 7.5) | `device_release_implicit_owner_locked`, from `device_release_channel` and `vms_proc_release_channels` |
| `$ALLOC` of a device another process owns → `SS$_DEVALLOC`, whether they allocated it (7.2) or only assigned a channel (7.5) | one refusal in `vms_ioctl_alloc`, on `owner_linux_pid` |
| Re-`$ALLOC` by the owner succeeds, no extra reference (8) | idempotent branch in `vms_ioctl_alloc` |
| `$ALLOC` by a process that owns the device by channel adds `allocated` and one reference (8) | `vms_ioctl_alloc` |
| `$DALLOC` of a device we have not ALLOCATED → `SS$_DEVNOTALLOC`, including one we own by channel (7.4, 8) | `vms_ioctl_dalloc` |
| `$DALLOC` drops the allocation and its reference but **not** ownership, while a channel is held (8) | `device_dealloc_locked` then the implicit rule |
| Reference count = channels + allocation; ownership itself is free (7.3, 7.4, 8) | `refcnt` in the executive |

Deliberately **not** taken:

- **Characteristic bit positions.** The public documentation available to this work does not publish
  the `$TTDEF` byte layout, so OVMX defines its own vector and labels it as its own (rule 8). Only
  the names are VMS's.
- **`Hardcopy` on the OVMX console.** It is set on the lab because that console is a printing
  terminal. OVMX's console is not, so claiming it would be a false statement about our hardware.
- **`Input:`/`Output:` speed, `Parity`, `LFfill`/`CRfill`.** OVMX's console is a serial line with no
  such parameters to report, so the executive carries no value for them and no reader can print one
  (rule 10: hide it rather than report a plausible number).
- **`Width: 132` / `Page: 24`** were taken from the pristine VAX2 console, but with the caveat that
  that console is a 132-column LA36. This is the one pair of constants in the device table that is
  an analogy rather than a measurement of OVMX's own hardware, and it is flagged for operator
  sign-off.
