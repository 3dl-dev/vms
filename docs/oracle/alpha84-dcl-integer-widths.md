# Oracle capture — DCL integer width on OpenVMS Alpha V8.4 (`vms-580`)

**Question (rd `vms-580`, authenticity pillar `vms-898`):** every integer-width
fact OVMX pinned was measured against OpenVMS **VAX** V7.3, and VAX is 32-bit.
"What VMS does" and "what 32-bit VMS does" were the same sentence. This capture
re-asks the width-bearing DCL behaviours of a **64-bit** oracle — OpenVMS
**Alpha** V8.4 — so the two sentences can be told apart with a *recorded*
transcript rather than an assumption.

**Why this file exists specifically:** `src/vmsdcl/dcl_cmd_show.c` (the
`SHOW SYMBOL` integer renderer) already carried a flat comment claiming these
values were "MEASURED ON TWO ARCHITECTURES … architecture-invariant at 32 bits",
but **no `docs/oracle/` transcript backed it** — a provenance hole (`vms-580`'s
done-condition requires a recorded transcript on both nodes). This file closes
that hole: the Alpha claim is now grounded, reproducible, and cited from the
code.

## Oracles

| node | OS | arch | how driven |
|---|---|---|---|
| lab-Alpha `alphalab-0` / `alpha1` | OpenVMS Alpha **V8.4** | Alpha (LP64), AlphaServer ES40 (AXPbox) | `tests/lab-alpha/README.md` FIFO console |
| lab-2 `vaxlab-2` / `vax1` | OpenVMS **VAX** V7.3 | VAX (32-bit), SIMH | `tests/lab/README.md` FIFO console |

Architecture confirmed on the Alpha node before quoting it:

```
$ WRITE SYS$OUTPUT F$GETSYI("ARCH_NAME")+" / "+F$GETSYI("HW_NAME")
Alpha / AlphaServer ES40
```

## Result — DCL integer width is ARCHITECTURE-INVARIANT at 32 bits (a longword), measured on both

Every question below answers **byte-identically** on OpenVMS Alpha V8.4 and
OpenVMS VAX V7.3. There is **no divergence**: a DCL integer is a 32-bit longword
on the 64-bit Alpha exactly as on the 32-bit VAX. A 64-bit VMS does **not**
widen it. This is a result, not a formality — it is the difference between the
`SHOW SYMBOL` rendering being *correct* and being *accidentally correct on the
only machine anyone had asked*.

### Q1 — `SHOW SYMBOL` Hex/Octal column widths (re-confirm; anchors the batch)

Alpha V8.4 (`alpha1`), verbatim:

```
$ IDENT_L = %X80000004
$ SHOW SYMBOL IDENT_L
  IDENT_L = -2147483644   Hex = 80000004  Octal = 20000000004
$ IDENT_D = 8388736
$ SHOW SYMBOL IDENT_D
  IDENT_D = 8388736   Hex = 00800080  Octal = 00040000200
```

VAX V7.3 (`vaxlab-2` / `vax1`), verbatim:

```
$ IDENT_L = %X80000004
$ SHOW SYMBOL IDENT_L
  IDENT_L = -2147483644   Hex = 80000004  Octal = 20000000004
$ IDENT_D = 8388736
$ SHOW SYMBOL IDENT_D
  IDENT_D = 8388736   Hex = 00800080  Octal = 00040000200
```

**Identical.** Hex is 8 digits (a longword), octal is 11 — on both
architectures. (This also independently reproduces the original `vms-c71`
side-by-side recorded in `src/vmsdcl/dcl_cmd_show.c`.)

### Q2 — DCL integer arithmetic width (overflow wraps in a longword)

Alpha V8.4, verbatim:

```
$ X = 2147483647
$ Y = X + 1
$ SHOW SYMBOL Y
  Y = -2147483648   Hex = 80000000  Octal = 20000000000
$ Z = 4294967300
$ SHOW SYMBOL Z
  Z = 4   Hex = 00000004  Octal = 00000000004
```

`2147483647 + 1` wraps to `-2147483648`; assigning `4294967300` truncates to its
low longword, `4`.

VAX V7.3 (`vaxlab-2` / `vax1`), verbatim:

```
$ X = 2147483647
$ Y = X + 1
$ SHOW SYMBOL Y
  Y = -2147483648   Hex = 80000000  Octal = 20000000000
$ Z = 4294967300
$ SHOW SYMBOL Z
  Z = 4   Hex = 00000004  Octal = 00000000004
```

**Identical. No widening on the 64-bit machine.**

### Q3 — `F$INTEGER` on a value above 2^31 (longword, wraps)

Alpha V8.4, verbatim:

```
$ WRITE SYS$OUTPUT F$INTEGER("2147483647")
2147483647
$ WRITE SYS$OUTPUT F$INTEGER("2147483648")
-2147483648
$ WRITE SYS$OUTPUT F$INTEGER("4294967296")
0
$ BIG = 2147483648
$ SHOW SYMBOL BIG
  BIG = -2147483648   Hex = 80000000  Octal = 20000000000
```

`F$INTEGER` and the DCL lexer are both longword: `2147483648` (2^31) wraps to
`-2147483648`, `4294967296` (2^32) wraps to `0`. `F$INTEGER` does **not** widen
to a quadword on Alpha.

VAX V7.3 (`vaxlab-2` / `vax1`), verbatim — **identical**:

```
$ WRITE SYS$OUTPUT F$INTEGER("2147483647")
2147483647
$ WRITE SYS$OUTPUT F$INTEGER("2147483648")
-2147483648
$ WRITE SYS$OUTPUT F$INTEGER("4294967296")
0
$ BIG = 2147483648
$ SHOW SYMBOL BIG
  BIG = -2147483648   Hex = 80000000  Octal = 20000000000
```

### Q4 — high-bit longword / identifier rendering (`%X8000000N` is negative)

Alpha V8.4, verbatim:

```
$ U1 = %X80000001
$ SHOW SYMBOL U1
  U1 = -2147483647   Hex = 80000001  Octal = 20000000001
$ U6 = %X80000006
$ SHOW SYMBOL U6
  U6 = -2147483642   Hex = 80000006  Octal = 20000000006
$ WRITE SYS$OUTPUT F$FAO("!SL", %X80000001)
-2147483647
$ WRITE SYS$OUTPUT F$FAO("!UL", %X80000001)
2147483649
```

VAX V7.3 (`vaxlab-2` / `vax1`), verbatim — **identical**:

```
$ U1 = %X80000001
$ SHOW SYMBOL U1
  U1 = -2147483647   Hex = 80000001  Octal = 20000000001
$ U6 = %X80000006
$ SHOW SYMBOL U6
  U6 = -2147483642   Hex = 80000006  Octal = 20000000006
$ WRITE SYS$OUTPUT F$FAO("!SL", %X80000001)
-2147483647
$ WRITE SYS$OUTPUT F$FAO("!UL", %X80000001)
2147483649
```

`%X80000001` renders as the negative signed longword `-2147483647` on both,
independently reproducing the `vms-2f8` VAX pin (`%X80000001..6` are negative as
DCL renders them). `F$FAO("!SL"/"!UL")` give the signed/unsigned **longword**
interpretations — 32-bit on both fields, no quadword widening.

## Consequence for OVMX

- `src/vmsdcl/dcl_cmd_show.c`'s "architecture-invariant at 32 bits" comment is
  **confirmed, not extrapolated** — it now cites this transcript. OVMX's existing
  longword-masked `SHOW SYMBOL` rendering is correct against *both* oracles.
- No Alpha-specific branch is needed in `SHOW SYMBOL` / `F$INTEGER` integer
  width. (Had Alpha widened, this would instead have been a de-fab requiring an
  arch branch — it did not.)

## Reproduce

Alpha: `kubectl -n ovmx-lab scale sts/alphalab --replicas=1`, then drive
`alpha1` per `tests/lab-alpha/README.md` (login `SYSTEM` / `ovmxlab2026`), run
the commands above. VAX: `kubectl -n ovmx-lab scale sts/vaxlab --replicas=3`,
drive `vaxlab-2` / `vax1` per `tests/lab/README.md` (login `SYSTEM` / `system`).
Scale both back to their prior replica counts when done (AXPbox and SIMH each
burn a host core per emulated node).
