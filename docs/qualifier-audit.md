# DCL Command Qualifier Audit

> **⚠ SUPERSEDED — do not maintain or trust the tables that used to live here (retired 2026-09-14).**
>
> This file was a hand-maintained, per-verb qualifier status table last updated 2026-03-05 — a
> single-ledger violation (a parallel copy of status data whose authoritative source is the compat
> register) that had drifted (it predates, e.g., the IVQUAL structural-enforcement work, Phase 1
> `vms-097`). Per the OVMX single-ledger invariant (INV-LEDGER; see `docs/design-compat-surface-register.md` §1),
> there is **one** source per surface and everything else is generated or retired.
>
> **Authoritative source of truth for DCL qualifier status:**
> [`docs/compat/facilities/dcl-qualifiers.yaml`](compat/facilities/dcl-qualifiers.yaml) (the SSOT).
> Render the human view with `python3 tools/compat/render_compat.py` (→ `docs/compatibility-surface.md`).
>
> Verb-level status lives alongside it in [`docs/compat/facilities/dcl-verbs.yaml`](compat/facilities/dcl-verbs.yaml).
> Do not re-add a hand table here; add or update rows in the register instead.
