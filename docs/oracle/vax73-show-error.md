# Oracle capture: `SHOW ERROR` device error counts (OpenVMS VAX V7.3)

**Item:** `vms-050` · **Captured:** 29-AUG-2026 · **Oracle:** lab-2 (`tests/lab`, `vms-a5c`),
OpenVMS VAX V7.3 on SIMH, an isolated k3s StatefulSet replica (`vaxlab-1`), nodes **VAX1**
(system id 1025) and **VAX2** (1026), driven over the console by `nodedrv.py` via the per-node
input FIFO (`k8s-labs/vaxlab-1/logs/vax1.log`, `vax2.log`).

**Method (CLAUDE.md rule 8 — clean room).** Everything below is *observed behaviour of the running
system through its documented operator interface* (DCL `SHOW ERROR`, `SHOW ERROR/FULL`,
`F$GETDVI(...,"ERRCNT")`, `HELP SHOW ERROR`). No VSI/HPE binary was disassembled, decompiled or
read. Nothing here is a byte layout of any internal structure: these are the names, the format and
the values VMS itself prints at the console.

**Why it exists.** OVMX's `SHOW ERROR` (`src/vmsdcl/dcl_cmd_show.c`, `cmd_show_error`) previously
ignored the system and unconditionally printed a fabricated banner ending in `No errors logged.`
(vms-050). This file is the "match VMS" record used to replace that lie with a real reading of the
executive device table's per-device error count — the same `errcnt` field `F$GETDVI ERRCNT` and
`SHOW DEVICE` already read.

---

## 1. Bare `SHOW ERROR` — devices with a non-zero error count only

Verbatim console capture (`SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST` first), both nodes identical:

```
$ SHOW ERROR
Device                           Error Count
PUA0:                                    1
PTA0:                                    1
```

There is **no** "Summary" title, no dashed separator, and **no** trailing message. The two rows are
the CI/port devices, which accrue a cluster-traffic error; every other device on the node was
absent from the listing.

## 2. Filtering to non-zero is real, not incidental

VMS's own HELP is explicit:

```
$ HELP SHOW ERROR
       Displays the error count for all devices with error counts
       greater than zero.
       Format
         SHOW ERROR
```

Confirmed against a zero-count device: `F$GETDVI("DUA0:","ERRCNT")` returned `0`, and `DUA0:` did
**not** appear in the bare `SHOW ERROR` listing above.

## 3. Column geometry, byte-exact

Every data row is 42 characters wide and the count's **units digit lands on column 41** (0-based),
independent of the device-name length — measured across names from 5 chars (`PUA0:`) to 21 chars
(`$2$DUA0: (VAX1, VAX2)`, from the `/FULL` capture below). The header's `Error Count` begins at
column 33.

```
Device                           Error Count      <- "Device" cols 0-5; "Error Count" begins col 33
PUA0:                                    1        <- name %-33s, count %9u; units digit col 41
```

`printf("%-33s%9u\n", devnam, errcnt)` reproduces every measured row; the header is the literal
line `Device` + 27 spaces + `Error Count`.

## 4. `SHOW ERROR/FULL` (context only — a separate rung, not rendered by OVMX yet)

`/FULL` additionally lists zero-count devices and the `CPU`/`MEMORY` pseudo-devices, same columns:

```
$ SHOW ERROR/FULL
Device                           Error Count
CPU                                      0
MEMORY                                   0
$2$DUA0: (VAX1, VAX2)                    0
$2$DUA1: (VAX1, VAX2)                    0
```

Recorded so the column measurement above rests on varied name widths. OVMX renders only the bare
form; `/FULL` (and the `CPU`/`MEMORY`/cluster-served-name detail it needs) is a follow-up.

## 5. NOT captured — the all-zero bare case

The reference cluster's port devices always carry a cluster-traffic error, so an `SHOW ERROR` with
**every** device at zero could not be produced on this oracle. Whether VMS emits a distinct
"no errors" message in that case is therefore **unmeasured**. OVMX reports the empty set as the
header alone with no rows — the documented semantic applied to an empty set, fabricating no
message (Rule 10). Pinning the all-zero wording to a capture is a follow-up (vms-050).
