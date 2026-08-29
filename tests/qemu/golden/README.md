# UX-fidelity golden captures (epic vms-050)

Oracle-driven fidelity goldens for OVMX user-visible surfaces. Each golden is
captured **verbatim** from a real OpenVMS reference lab (clean-room Rule 8:
observation of the display output only — nothing is disassembled or
decompiled), keyed by *surface* and *architecture*, and is the ground truth an
OVMX gate diffs against. INV-6: real data, oracle-faithful, never fabricated.

## Provenance

Captured 2026-08-29 from the project reference labs:

| File | Oracle | Lab |
|------|--------|-----|
| `oracle-vax-v7.3.console.txt`   | OpenVMS **VAX V7.3**   | lab-2 `vaxlab-0` (`tests/lab/`) |
| `oracle-alpha-v8.4.console.txt` | OpenVMS **Alpha V8.4** | lab-Alpha `alphalab-0` (`tests/lab-alpha/`) |

Both architectures were captured because a single-architecture answer is half a
result (see `tests/lab-alpha/README.md`). For the surfaces below the two
architectures **agree** — recorded as a result, not assumed.

## `show_version_ivkeyw.golden` — the machine-checkable golden

The DCL `SHOW` command has **no `VERSION` and no `VERIFY` keyword** on OpenVMS.
`SHOW VERSION`, `SHOW VERIFY`, and every abbreviation of either (`SH VER`,
`SH VERI`) are *unrecognized keywords* and produce the identical two-line
message — identical on VAX V7.3 and Alpha V8.4:

```
%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling
 \VERSION\
```

Fidelity points, each a divergence OVMX had before vms-050:

- severity is **WARNING** (`-W-`), not `-E-`;
- the text is the generic *"unrecognized keyword - check validity and
  spelling"* — it does **not** say "SHOW", and does **not** carry the keyword;
- the offending keyword appears **UPCASED** on a **continuation line** as
  `\ \KW\` (one leading space), even when typed in lower case
  (`show version` -> `\VERSION\`).

This is what `SHOW VERSION` being "missing" actually looks like on real VMS:
the faithful behaviour is to reject it, not to invent a `SHOW VERSION` command.
The machine version token is instead read with `F$GETSYI("VERSION")` (already
faithful per-arch in OVMX), and the verify state with `F$VERIFY()` /
`F$ENVIRONMENT("VERIFY_PROCEDURE")`.

The gate `tests/dcl/test_show_version_ivkeyw_golden.sh` diffs OVMX's DCL output
against `show_version_ivkeyw.golden` byte-for-byte.

## Related non-fidelity note captured at the same time

The login banner and `SHOW TIME` / `SHOW USERS` timestamps on both oracles show
the **real current date/time** (e.g. `29-AUG-2026`). OVMX reads the same real
clock (`clock_gettime(CLOCK_REALTIME)`); the `1-JAN-2010` seen on the hosted
web demo is that demo snapshot's frozen virtual RTC, not an OVMX defect — see
the PR description. The verbatim banner/SHOW-USERS lines are preserved in the
per-arch console captures for future date-format fidelity work.
