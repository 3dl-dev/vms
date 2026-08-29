# Oracle: `SHOW PROCESS/QUOTAS`, `SHOW WORKING_SET` and the JIB quota set on OpenVMS VAX V7.3

**Item:** vms-14a (parent vms-050). **Node:** VAX1, OpenVMS VAX V7.3 (lab-2
k3s replica `vaxlab-1`), 2026-08-29 08:33.
**Method:** the node was already up; logged in as `SYSTEM` on the console
(prompt-synchronised: `Username: SYSTEM`, `Password: system`), then each
command was run and the console log read back through `tail`. Values below are
copied verbatim from the console log, not eyeballed off a screen.
Documented tool output only — no disassembly, no VSI source (CLAUDE.md Rule 8).
Lab left as found: the scaled-up replica `vaxlab-1` was scaled back down
(`kubectl -n ovmx-lab scale sts/vaxlab --replicas=1`) on teardown; `vaxlab-0`
(another tenant's live investigation) was never touched.

**Why this was measured.** #884 excised the fabricated `SHOW PROCESS/QUOTAS`
block (seven hardcoded constants shown for every process) and wired the read
path to the executive's per-process JIB quota vector, gated on
`VMS_PI_V_QUOTA` — a bit with **zero setters** ("no quota facility yet"), so the
lines honestly OMIT rather than fabricate. #890 did the same for
`SHOW WORKING_SET`. Both left a comment naming the verbatim question for this
item: *what are SYSTEM's real quota values, and the exact layout, on the
oracle?* This file answers it, and is the golden spec the seed
(`tools/mksysuaf.c`) and the acceptance gate assert against.

---

## 1. `SHOW PROCESS/QUOTAS`, verbatim

```
$ SHOW PROCESS/QUOTAS

29-AUG-2026 08:33:12.95   User: SYSTEM           Process ID:   2020021A
                          Node: VAX1             Process name: "SYSTEM"

Process Quotas:
 Account name: SYSTEM
 CPU limit:                      Infinite  Direct I/O limit:       100
 Buffered I/O byte count quota:     47872  Buffered I/O limit:     100
 Timer queue entry quota:              30  Open file quota:        300
 Paging file quota:                 38808  Subprocess quota:        10
 Default page fault cluster:           64  AST quota:               98
 Enqueue quota:                       200  Shared file limit:        0
 Max detached processes:                0  Max active jobs:          0
```

## 2. `SHOW WORKING_SET`, verbatim

```
$ SHOW WORKING_SET
  Working Set      /Limit=512   /Quota=1024    /Extent=28700
  Adjustment enabled      Authorized Quota=1024  Authorized Extent=28700
```

---

## 3. AUTHORIZED vs REMAINING — the load-bearing distinction

`SHOW PROCESS/QUOTAS` prints the **current remaining** value for a deductible
(pooled) quota, not the authorized limit the account was created with.
`F$GETJPI` of the `…LM`/`…QUOTA` item returns the **authorized limit**. They
differ by whatever the live process has charged so far:

```
$ WRITE SYS$OUTPUT "BYTLM="+F$STRING(F$GETJPI("","BYTLM"))+" PGFLQUO="+...+" ASTLM="+...
BYTLM=47872 PGFLQUO=40960 ASTLM=100
```

| item            | `SHOW PROCESS/QUOTAS` (remaining) | `F$GETJPI` (authorized) |
|-----------------|-----------------------------------|-------------------------|
| Paging file     | 38808                             | **40960**               |
| AST             | 98                                | **100**                 |
| Buffered I/O byte | 47872                           | 47872 (none charged yet)|

**What OVMX seeds and shows.** OVMX stores the **authorized** limits in SYSUAF
and does **not** enforce/charge quotas (enforcement is a separate facility,
out of scope for vms-14a — INV-6 note). Nothing is charged, so on OVMX
`remaining == authorized`. The seed and the acceptance gate therefore use the
**authorized** values (the `F$GETJPI` column), which are stable and
account-configured, never the charge-dependent `SHOW PROCESS/QUOTAS`
remaining snapshot.

## 4. The authorized JIB quota set for SYSTEM ([1,4]) — the seed values

Captured with `F$GETJPI("","<item>")`:

```
BYTLM=47872 PGFLQUO=40960 ASTLM=100
BIOLM=100 DIOLM=100 ENQLM=200 FILLM=300
PRCLM=10 TQCNT=30 DFWSCNT=512
```
plus `SHOW WORKING_SET`: `/Quota=1024`, `/Extent=28700`.

Mapped onto `struct vms_jib_quota` (src/kernel/vms_ioctl.h), the twelve
longwords the OVMX quota facility carries:

| `vms_jib_quota` field | JPI / display item          | value  |
|-----------------------|-----------------------------|--------|
| `astlm`               | JPI$_ASTLM  (AST quota)      | 100    |
| `biolm`               | JPI$_BIOLM  (Buffered I/O limit) | 100 |
| `bytlm`               | JPI$_BYTLM  (Buffered I/O byte count) | 47872 |
| `diolm`               | JPI$_DIOLM  (Direct I/O limit) | 100  |
| `enqlm`               | JPI$_ENQLM  (Enqueue quota)  | 200    |
| `fillm`               | JPI$_FILLM  (Open file quota)| 300    |
| `pgflquota`           | JPI$_PGFLQUOTA (Paging file quota) | 40960 |
| `prclm`               | JPI$_PRCLM  (Subprocess quota) | 10   |
| `tqelm`               | JPI$_TQCNT  (Timer queue entry quota) | 30 |
| `wsdefault`           | JPI$_DFWSCNT (Default working set) | 512 |
| `wsquota`             | JPI$_WSQUOTA (Working set quota) | 1024 |
| `wsextent`            | JPI$_WSEXTENT (Working set extent) | 28700 |

## 5. Fields the display shows that the 12-field JIB struct does NOT yet carry

The real `SHOW PROCESS/QUOTAS` layout (Section 1) also prints five cells the
current `struct vms_jib_quota` has no slot for:

| display label               | value (SYSTEM) | note |
|-----------------------------|----------------|------|
| CPU limit                   | Infinite (0)   | no per-process CPU-time limit |
| Default page fault cluster  | 64             | JPI$_DFPFC |
| Shared file limit           | 0              | JPI$_SHRFILLM |
| Max detached processes      | 0              | JPI$_MAXDETACH |
| Max active jobs             | 0              | JPI$_MAXJOBS |

**These stay honestly OMITTED in this pass.** vms-14a builds the source for the
twelve-field JIB struct that #884/#890 already scaffolded and wired into the
display. Adding these five cells means growing `struct vms_jib_quota` (and with
it the `vms_procinfo` ioctl ABI on both kernels) and rewriting the display to
the exact VAX two-column geometry above — a further facility increment, filed
as follow-up. Until then the OVMX display keeps its reviewed six-line layout
(real VSI DCL Dictionary labels, values now real) and does not fabricate the
five it cannot source.

## 6. What this capture does and does not establish

- It establishes SYSTEM's authorized quota set on a real OpenVMS VAX V7.3
  system, which is what OVMX seeds and displays.
- It is **VAX 32-bit**. The values are account configuration, not
  architecture-dependent, and the SYSUAF quota region is a fixed-width
  little-endian byte layout that decodes identically on ILP32 VAX and LP64
  Alpha (the codec is `uint32_t`/byte, never a native `long`). The layout is
  proven by `_Static_assert` in `sysuaf.h`, not by this capture.
- The `SHOW PROCESS/QUOTAS` two-column geometry (Section 1) is recorded here
  for the follow-up increment; the OVMX display is not pinned to it in this
  pass (Section 5).
