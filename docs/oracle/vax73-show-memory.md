# Oracle: `SHOW MEMORY` on OpenVMS VAX V7.3

**Item:** vms-050 (DCL/SHOW UX-fidelity sweep). **Node:** VAX1, OpenVMS VAX
V7.3, 2026-08-29. **Lab:** lab-2 (`tests/lab`), isolated replica `vaxlab-1`.
**Method:** logged in as SYSTEM, `SET TERMINAL/PAGE=0/WIDTH=132`, commands
bracketed by markers, log read through `cat -A` -- column positions counted from
bytes. Documented tool output only (CLAUDE.md Rule 8).

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

## 2. Column geometry (measured from `cat -A`)

**Physical Memory Usage** -- the value columns right-justify to cols
**40 / 52 / 64 / 76** (each 12 apart). VMS pads the *header* label to 30 and its
first value field to **10** (`%-30s%10s%12s%12s%12s`), and the *data* label to
**28** with every value field 12 (`%-28s%12d%12d%12d%12d`); both land Total's
right edge on col 40 despite the header string being 2 columns longer than the
`  Main Memory (...)` data label.

**Paging File Usage** -- header `%-40s%12s%12s%12s` (Free/Reservable/Total right
edges on 52/64/76); data `  %-38s%12d%12d%12d`. (OVMX already matched this.)

## 3. OVMX rendering decisions (INV-6 -- real source or honest omission)

- **Physical Memory Usage**: rendered from `/proc/meminfo` (MemTotal/MemFree/
  Dirty -> VMS 512-byte pages). Column widths corrected to the geometry above.
- **Paging File Usage**: rendered from `/proc/swaps`. Reservable == Free
  (OVMX tracks no page-file reservations, so every free block is reservable --
  a real free count, not a fabricated reservation figure).
- **Virtual I/O Cache Usage, Slot Usage, Dynamic Memory Usage**, and the
  "permanently allocated to OpenVMS" trailer are **honestly omitted**: OVMX's
  substrate has no VMS XFC cache, no process-entry / balance-set slot table,
  and no VMS nonpaged/paged pool, so there is no faithful source. Printing
  plausible numbers would be fabrication (INV-6 / Rule 10).
