# Oracle: `SHOW MEMORY` on OpenVMS VAX V7.3

**Items:** vms-050 (DCL/SHOW UX-fidelity sweep) + vms-a3cd (VAX physical-memory
data source). **Node:** VAX1, OpenVMS VAX V7.3, 2026-08-29. **Lab:** lab-2
(`tests/lab`), isolated replica `vaxlab-1`. **Method:** logged in as SYSTEM,
`SET TERMINAL/PAGE=0/WIDTH=132`, commands bracketed by markers, log read through
`cat -A` -- column positions counted from bytes. Documented tool output only
(CLAUDE.md Rule 8). Two independent captures were taken (below); they agree on
every stable figure (128.00Mb, 262144 total pages, 28761 permanently allocated)
and differ only in the naturally-varying Free/In Use/Modified counts.

---

## 1. `SHOW MEMORY` (full), verbatim

```
              System Memory Resources on 29-AUG-2026 15:49:56.19

Physical Memory Usage (pages):     Total        Free      In Use    Modified
  Main Memory (128.00Mb)          262144      216382       42527        3235

Virtual I/O Cache Usage (pages):   Total        Free      In Use     Maximum
  Cache Memory                      6241         188        6053      220429

Slot Usage (slots):                Total        Free    Resident     Swapped
  Process Entry Slots                310         289          21           0
  Balance Set Slots                  279         260          19           0

Dynamic Memory Usage (bytes):      Total        Free      In Use     Largest
  Nonpaged Dynamic Memory        3257856     1330368     1927488     1141376
  Paged Dynamic Memory           2675712     1751232      924480     1750416

Paging File Usage (pages):                      Free  Reservable       Total
  DISK$SYSDSK1:[SYS0.SYSEXE]SWAPFILE.SYS       46496       46496       46496
  DISK$SYSDSK1:[SYS0.SYSEXE]PAGEFILE.SYS      122000       80630      122000

Of the physical pages in use, 28761 pages are permanently allocated to OpenVMS.
```

A second capture (15:24:32) matched section-for-section; its Physical Memory row
read `262144 217028 42694 2422` -- same Total, run-to-run variance in the rest.

## 2. Column geometry (measured from `cat -A`)

**Physical Memory Usage** -- the value columns right-justify to cols
**40 / 52 / 64 / 76** (each 12 apart). VMS pads the *header* label to 30 and its
first value field to **10** (`%-30s%10s%12s%12s%12s`), and the *data* label to
**28** with every value field 12 (`%-28s%12d%12d%12d%12d`); both land Total's
right edge on col 40 despite the header string being 2 columns longer than the
`  Main Memory (...)` data label.

**Paging File Usage** -- header `%-40s%12s%12s%12s` (Free/Reservable/Total right
edges on 52/64/76); data `  %-38s%12d%12d%12d`. (OVMX already matched this.)

Section/column labels for the other three sections (for reference): Virtual I/O
Cache Usage (pages) Total/Free/In Use/Maximum, `Cache Memory`; Slot Usage (slots)
Total/Free/Resident/Swapped, `Process Entry Slots` / `Balance Set Slots`; Dynamic
Memory Usage (bytes) Total/Free/In Use/Largest, `Nonpaged`/`Paged Dynamic Memory`.
Footer: `Of the physical pages in use, <N> pages are permanently allocated to
OpenVMS.`

## 3. OVMX rendering decisions (INV-6 -- real source or honest omission)

- **Physical Memory Usage**: rendered from real memory state, converted to VMS
  512-byte pages, at the column geometry above. **The data SOURCE is
  arch-specific** (vms-a3cd): x86_64/Alpha read Linux `/proc/meminfo`
  (MemTotal/MemFree/Dirty); NetBSD-VAX reads the executive's uvm counters through
  a `$GETSYI`-style KIF (see Section 4) -- `/proc/meminfo` is absent on VAX, and
  reading it unconditionally printed all-zeros (the bug vms-a3cd fixes). On VAX
  the **Modified** column has no maintained source and is honestly omitted, so
  the VAX row is Total/Free/In Use only.
- **Paging File Usage**: rendered from `/proc/swaps` on the Linux arches
  (Reservable == Free -- OVMX tracks no page-file reservations, so every free
  block is reservable, a real free count, not a fabricated reservation figure).
  On NetBSD-VAX `/proc/swaps` is absent, so the section is simply not printed.
- **Virtual I/O Cache Usage, Slot Usage, Dynamic Memory Usage**, and the
  "permanently allocated to OpenVMS" trailer are **honestly omitted**: OVMX's
  substrate has no VMS XFC cache, no process-entry / balance-set slot table,
  and no VMS nonpaged/paged pool, so there is no faithful source. Printing
  plausible numbers would be fabrication (INV-6 / Rule 10).

## 4. Source mapping for the OVMX/NetBSD-VAX fix (vms-a3cd)

The bug: `cmd_show_memory` (`src/vmsdcl/dcl_cmd_show.c`) read Linux `/proc/meminfo`
with no arch branch -> absent on NetBSD-VAX -> all zeros (INV-6 lie-of-absence).
The fix wires the VAX path to the executive's uvm counters via a new
`$GETSYI`-style KIF (`VMS_IOCTL_GETSYIMEM` -> `ovmx_sysmem_bytes()`, the dedicated
uvm-only TU `src/kernel-netbsd/vms_sysmem_netbsd.c`).

What the NetBSD executive sources honestly, via the **maintained** uvm accessor
(the `kernel-accounting-maintained-accessor` rule -- read the counter the kernel
maintains, not the raw field a map names), and what it omits:

- **Physical Memory** -- REAL: Total = `uvmexp.npages` (managed pages, set once at
  boot). Free = `uvm_availmem(true)` -- **NOT** the raw `uvmexp.free`. On NetBSD 10
  `uvmexp.free` is a lazily-synced per-CPU counter; the kernel's own vmstat/sysctl
  path reads free through `uvm_availmem(true)` (`uvm_meter.c`), whose `true` forces
  `cpu_count_sync()` before `cpu_count_get(CPU_COUNT_FREEPAGES)`. Reading the raw
  field would risk a stale snapshot -- the same discipline as `vm_resident_count`
  in vms-601. In Use = Total - Free. Both cross the KIF as **bytes**
  (`npages * PAGE_SIZE`), so no VMS/host page-size skew crosses the wire; the
  renderer divides by 512 for VMS pages.
- **Modified** -- the VMS modified page-list has no maintained NetBSD-VAX analogue,
  so the column is **honestly omitted** on VAX (not fabricated).
- **Slot Usage / Dynamic Memory / Virtual I/O Cache / Paging File** -- no faithful
  NetBSD-VAX source -> honestly omitted (INV-6). Only a section whose figures are
  real is rendered.

INV-6: render a section only when its numbers are real executive/uvm data; never a
fabricated 0 or a Linux-shaped figure. The shared acceptance battery asserts the
Physical Memory Total is REAL and non-zero (arch-common: x86_64/Alpha always had
it, VAX now does too) -- so the VAX run of the battery moves from would-fail
(Total 0) to pass.
