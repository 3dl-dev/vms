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

This section exists because the first cut of `src/kernel/vms_devtab.c` asserted an ownership rule as
VMS fact with nothing behind it ("the first channel to an unowned device makes its holder the
owner"). It was measured rather than argued. Method: a **second process** was created on VAX2 with

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

### 7.3 A channel does NOT confer ownership

`NLA0:` before, during and after a channel was held by the observing process:

```
$ SHOW DEVICE/FULL NLA0:            Owner process ""  Owner process ID 00000000  Reference count 2
$ OPEN/WRITE X NLA0:
$ SHOW DEVICE/FULL NLA0:            Owner process ""  Owner process ID 00000000  Reference count 3
$ CLOSE X
$ SHOW DEVICE/FULL NLA0:            Owner process ""  Owner process ID 00000000  Reference count 2
```

The owner fields never moved; only the reference count did. **Reference count is one per assigned
channel.**

### 7.4 Foreign channels alone are enough to refuse `$ALLOC`

With `NLA0:` unowned but at reference count 2 (channels held by other processes):

```
$ ALLOCATE NLA0:
%SYSTEM-W-DEVALLOC, device already allocated to another user
```

## 8. `ALLOCATE` sets the owner, adds a reference, and is idempotent

On the interactive job, which already owned `OPA0:` but had not allocated it:

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

Three things are pinned here: allocation adds the word **`allocated`** to the status clause;
allocation is worth **one reference**; and re-allocating a device you already have allocated
succeeds and changes nothing.

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
constant this work introduces a use for, and it had no other consumer to break. The rest have a
blast radius across the kernel module, its client and its tests, and are tracked separately; do not
"fix" them without running the whole QEMU suite.

---

## What OVMX took from this, and what it deliberately did not

| Oracle fact | OVMX (`src/kernel/vms_devtab.c`) |
|---|---|
| Console is `OPA0:` | Executive creates `OPA0:` at module init |
| Terminal is device class terminal | `DC$_TERM` (6), mirroring `src/libvms/include/dcdef.h` |
| Unidentified type displays `Unknown` | Console registers with device type 0 = Unknown |
| Characteristic **names** and their two-state form | `VMS_TTC_*` in `src/kernel/vms_ioctl.h`, one bit per oracle name |
| Absent device → `%SYSTEM-W-NOSUCHDEV` | `SS$_NOSUCHDEV` from `$ASSIGN`/`$GETDVI` |
| `$ASSIGN` succeeds on a device another process owns (7.1) | `vms_ioctl_assign` returns `SS$_NORMAL` and does not touch ownership |
| Ownership comes from `$ALLOC`, never `$ASSIGN` (7.3, 8) | `vms_ioctl_alloc` sets `owner_pid`/`allocated`; `$ASSIGN` does not |
| `$ALLOC` of a device another process owns → `SS$_DEVALLOC` (7.2) | `vms_ioctl_alloc` returns `SS$_DEVALLOC` |
| `$ALLOC` refused while another process holds channels (7.4) | `vms_ioctl_alloc` walks `dev->chanlist` for a foreign holder |
| Re-`$ALLOC` by the owner succeeds, no extra reference (8) | idempotent branch in `vms_ioctl_alloc` |
| `$DALLOC` of an unallocated device → `SS$_DEVNOTALLOC` (8) | `vms_ioctl_dalloc` |
| Reference count = channels + allocation (7.3, 8) | `refcnt` in the executive |

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
