# Oracle: `SHOW STATUS` on OpenVMS VAX V7.3

**Item:** vms-050 sweep finding for vms-df9c. **Node:** VAX1, OpenVMS VAX V7.3,
2026-08-29, lab-2 replica `vaxlab-1`. **Method:** interactive SYSTEM session,
`SET TERMINAL/WIDTH=132`, command bracketed by markers, log read via `cat -A`.
Documented tool output only (CLAUDE.md Rule 8).

---

## Verbatim

```
$ SHOW STATUS
  Status on  29-AUG-2026 15:50:33.44     Elapsed CPU :   0 00:00:00.22
  Buff. I/O :      224    Cur. ws. :     512    Open files :         0
  Dir. I/O :        31    Phys. Mem. :   228    Page Faults :     5989
```

## Layout

- Header line: `  Status on ` + date/time + `     Elapsed CPU : ` + CPU
  (`d hh:mm:ss.cc`).
- Then a **3-column grid**, two rows:
  - Row 1: `Buff. I/O :`, `Cur. ws. :`, `Open files :`
  - Row 2: `Dir. I/O :`, `Phys. Mem. :`, `Page Faults :`
- `Cur. ws.` is the **working-set size** (512); `Phys. Mem.` is the
  **resident page count** (228) -- distinct fields. Both differ from
  `Elapsed CPU`, `Buff./Dir. I/O`, `Open files`, `Page Faults`.

## OVMX gap (tracked in vms-df9c)

Current `cmd_show_status()` wires CPU / page faults / resident pages from
`$GETJPI`, but prints them one-per-line and labels the resident count
(`JPI$_PPGCNT`) as `Cur. ws.`. Faithful target: keep the 3-column grid; render
the resident count under `Phys. Mem.`; honestly OMIT the fields OVMX has no
source for (`Cur. ws.` working-set quota, `Buff. I/O`, `Dir. I/O`, `Open
files`) rather than mislabel or fabricate (INV-6). Deferred from the sweep as a
layout rework, not a one-liner.
