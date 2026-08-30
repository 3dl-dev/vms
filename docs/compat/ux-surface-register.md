# OVMX User-Visible Surface Register (UX fidelity) — `vms-05a`

A prioritized, **hollow-first** checklist of every OVMX user-visible surface — the
DCL `SHOW` subcommands, `DIRECTORY`, `SET`, `F$` lexicals, the utilities
(`AUTHORIZE`/`SYSGEN`/`MONITOR`/`MAIL`), and the banner/prompts — to
oracle-verify against real OpenVMS (live VAX V7.3 + Alpha V8.4 labs, CLAUDE.md
Rule 8; OpenVMS DCL Dictionary). It drives the `vms-050` UX-fidelity fix backlog
and is the checklist the standing golden-comparison gate (`vms-c38`) runs via
`tools/oracle/diff_surface.sh <surface>` against `docs/oracle/golden/<surface>`.

This is an **index**, not a ledger: the authoritative status of each surface
lives in its rd item and (for captured surfaces) its oracle golden + the live
`diff_surface` classification. Rows link back; they don't duplicate.

## How a surface is classified

Once a surface has an oracle golden, `diff_surface` classifies OVMX's output
(most-damning first): **MISSING** (not implemented), **HOLLOW** (runs but empty
where the oracle has data — INV-6 lie-of-absence), **ARTIFICE-TELL** (matches a
declared fabrication signature), **FORMAT-DIVERGENT** (real data, wrong shape),
**MATCH** (VMS-faithful). Until then a row carries a *suspected* status from its
rd item.

The compare is **structure-tolerant** (`diff_surface` `structure_norm`): a
byte-exact column-geometry gate is impossible cross-system (OVMX's values
legitimately differ from the VAX/Alpha oracle — wider numbers, different machine
strings), so the gate proves **structural fidelity** — same sections, labels,
headers, field-structure — tolerant of value differences via digit-run collapse +
whitespace-normalize + grounded per-surface `MACHINE_MASK`, but **never** of a
missing or HOLLOW field (a blank keeps no digits → still reds; a masked field that
is blank/absent → still reds).

The `vms-c38` gate runs in two tiers (conductor ruling 2026-08-30, Option A):
**hard-gated** surfaces are proven genuinely faithful and a divergence FAILS the
leg (regression-proof); **report-only** surfaces are not-yet-faithful — the gate
classifies + logs them LOUD and routes each to its fidelity item every run (driving
this backlog) but does not hard-fail the required leg, so a known gap can't
permanently-red main. A surface graduates to hard-gated when its fidelity item
lands and it genuinely MATCHes. **Hollow-first**: suspected HOLLOW/ARTIFICE surfaces are Priority 1 —
they are the fabrications the operator's "Apple II BASIC clone of VMS" concern
names, and the ones a hand-written `must_have` passes vacuously on.

Status legend: `golden-seeded` = oracle golden captured, ready for the gate to
classify OVMX; `fixed` = oracle-verified faithful; `suspected-*` = rd-item
diagnosis, not yet golden-gated; `pending` = not yet triaged/captured.

## Priority 1 — suspected fabrications + the core SHOW battery

| surface | VMS command | status | golden | rd item |
|---|---|---|---|---|
| show-users | `SHOW USERS` | **suspected-HOLLOW/ARTIFICE** — fabricates a user-process summary with no executive | — (capture next) | vms-6a1, vms-8146 |
| show-memory | `SHOW MEMORY` | **HOLLOW** (vms-c38 round-3 diff) — OVMX omits the Dynamic Memory + Paging File sections + the "permanently allocated" footer, though it HAS pool + a pagefile → real lie-of-absence, not substrate-absent. Gate REPORT-only, routed. | `vax-show-memory` | vms-352 |
| show-system | `SHOW SYSTEM` | **HOLLOW** (vms-c38 round-3 diff) — column header omits the State/Pri/I/O columns; banner (OpenVMX/node/version) is machine-varying (fine). Gate REPORT-only, routed. | `vax-show-system` | vms-6b8e |
| show-cpu | `SHOW CPU` | **fixed — HARD-GATED** (vms-c38): genuinely faithful, greens through the full pipeline (model / MP-state / CPU-ID-list are machine-varying values, masked; the labelled structure MATCHes the DCL-Dictionary-pinned oracle). Regression-proof. | `vax-show-cpu` | vms-277 |

## Priority 2 — the rest of the SHOW battery + DIRECTORY/SET

| surface | VMS command | status | golden | rd item |
|---|---|---|---|---|
| show-device | `SHOW DEVICE` | **MISSING** (vms-c38 round-3 diff) — OVMX returns `%SYSTEM-W-NOSUCHDEV` for the queried device (device-name model: golden's `$1$DUAn:` vs OVMX's `DKA0:`/`VDA0:`, ties vms-9f5). Gate REPORT-only, routed. | `vax-show-device` | vms-ddc, vms-9f5, vms-e6f, vms-f4b, vms-fe0 |
| show-process | `SHOW PROCESS` | **HOLLOW** (vms-c38 round-3 diff) — omits Terminal / Base priority / Devices allocated lines, and shows a numeric UIC `[n,n]` not resolved to the identifier name `[SYSTEM]`; node is machine-varying (fine). Gate REPORT-only, routed. | `vax-show-process` | vms-1f7, vms-c47 |
| show-quota | `SHOW QUOTA` | **fixed** — oracle-faithful `%SYSTEM-F-QFNOTACT` (was a NODISKQUOTA fabrication) | — (deterministic) | vms-73c4 (#939) |
| directory | `DIRECTORY` | leak premise **REFUTED** (measure-first, 2026-08-30) — header derives `DEV:[DIR]` from VMS-side inputs via `dcl_directory_header_spec` (vms-272), no `/vms`/`[VMS.]` leak; zero-match = honest `%DIRECT-W-NOFILES`. Byte-fidelity still wants a golden. | pending (Alpha capture) | vms-38d, vms-28c |
| set-default | `SET DEFAULT` / `SHOW DEFAULT` | pending — concealed-form round-trip canonicalizer | pending | vms-ee0 |
| set-verbs | `SET` (real mutations, not fake-success) | **fixed** — measure-first vs origin/main (2026-08-30): all fake-success facades remediated, 0 live-fab (ASSIGN→real LNM, MOUNT→ACP, PRODUCT, SET ACCOUNTING/PASSWORD real, SET AUDIT→`SS$_UNSUPPORTED`, HELP/STOP/INQUIRE real; INV-DCL sweep) | — (deterministic) | vms-6ad (closed) |

## Priority 3 — thinner SHOW commands, F$ lexicals, utilities, banner

| surface | VMS command / surface | status | rd item |
|---|---|---|---|
| show-time | `SHOW TIME` | pending — needs a real clock/date (demo-disk build stamp) | vms-441 |
| show-symbol | `SHOW SYMBOL` (integer Hex/Octal width) | **fixed** — longword width oracle-verified on VAX+Alpha | vms-580, vms-c71 |
| show-terminal | `SHOW TERMINAL` | pending | vms-223 (adjacent), vms-79b3 |
| show-rms/key/error | `SHOW RMS_DEFAULT` / `SHOW KEY` / `SHOW ERROR` | suspected-MISSING (absent pair) | vms-79b3 |
| f$-lexicals | `F$GETSYI` / `F$GETDVI` / `F$GETJPI` / `F$MESSAGE` / … | mixed — GETSYI widths verified; GETDVI free-blocks/vol adjacency suspect | vms-580, vms-fe0 |
| authorize | `AUTHORIZE` (SYSUAF authoring) | mixed — real binary SYSUAF/Purdy; World-denied SYSUAF open wall | vms-381, vms-beae |
| sysgen | `SYSGEN` (params) | pending — SCS params authored the VMS way | vms-37b, vms-495 |
| monitor | `MONITOR` | pending | (uncatalogued) |
| mail | `MAIL` | pending | (uncatalogued) |
| banner-prompt | login banner / `$` prompt / boot console | mixed — faithful boot console standing work | vms-603, vms-16a, vms-21a7 |
| loginout | `LOGINOUT` welcome / last-login / accounting | mixed — SYSUAF auth real; LOGOUT accounting summary | vms-274, vms-10cf |

## Next captures (hollow-first, disk-safe off vaxlab-2)

1. **`SHOW USERS`** (vms-6a1/vms-8146) — the highest-priority suspected fabrication; capture the real VAX layout, then `diff_surface` will classify OVMX's as HOLLOW/ARTIFICE and drive the fix.
2. `SHOW TERMINAL`, `SHOW TIME` — thin deterministic surfaces.
3. `DIRECTORY` (a listing surface — needs a stable test directory + normalization of dates/sizes).
4. Alpha-side re-capture of the above (HELD on the 99% disk; AXPbox last).

## Cross-references

- Capture + mask + classify mechanics: `tools/oracle/README.md` (capture_oracle + diff_surface).
- Field-geometry pins already documented: `docs/oracle/vax73-show-system-process.md` (SHOW SYSTEM/PROCESS columns).
- Authenticity (facade) axis: `docs/draper-faithfulness-register.md` (INV-6/INV-DCL).
- The DCL-command plan/sequencing: `docs/design-vms-parity-map.md` (vms-8ad).
- Parent program: `vms-050`; the standing gate that consumes this: `vms-c38`.
