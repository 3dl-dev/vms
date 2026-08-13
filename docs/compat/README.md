# `docs/compat/` — Compatibility Surface Register (source of truth)

This directory is the **structured source of truth** for the OVMX Compatibility
Surface Register: every VMS functional/compatibility surface × its OVMX status,
scoped to 1.0. Everything human-facing is **generated** from here — never hand-edit
the generated files.

## Layout

```
domains.yaml          8 domains + the controlled vocabularies (status / authenticity /
                      scope_1_0 / kind) + the facility registry. The generator enforces it.
facilities/*.yaml     one file per facility. facilities/str.yaml is the gold-standard example.
```

Generated outputs (do not edit by hand):

```
docs/compatibility-surface.md   internal comprehensive register (regenerated)
build/compat-surface.json       machine export → website + corpus-compat lookup
```

## Regenerate / validate

```bash
python3 tools/compat/render_compat.py          # rewrite the .md and .json
python3 tools/compat/render_compat.py --check   # validate only (CI gate); non-zero on error
```

`--check` reports **errors** (bad vocab, missing `verified_against`, domain mismatch —
these fail CI) and **warnings** (evidence file absent on the current tree → possible drift;
`last_reviewed` older than the staleness threshold). Note: run against a real `main`
checkout — a stale/pinned checkout produces false "evidence not found" warnings for files
that exist only on `main`.

## Add or update a surface

1. Edit the relevant `facilities/<token>.yaml` (or add one; register the token in
   `domains.yaml` under the right domain).
2. Each item needs: `id` (the canonical symbol/command/feature — the lookup key), `kind`,
   `vms` (one-clause target), `status`, `authenticity`, `evidence` (file path on `origin/main`).
   Set `scope_1_0` if it differs from the facility default. `status: verified` **requires**
   `verified_against` (name the oracle/test).
3. Bump the facility's `last_reviewed`. Status is a *dated observation against `origin/main`*,
   not a standing belief — re-measure, don't recall.
4. Regenerate and commit the `.md`/`.json` alongside the YAML.

## The two axes (don't collapse them)

- `status` = **how much** is implemented (coverage): absent → designed → stub → partial →
  implemented → verified.
- `authenticity` = **how honestly**: real / advisory / facade-risk / n/a. A row can be
  `implemented` + `facade-risk` — that is a bug, and it is exactly what
  `docs/draper-faithfulness-register.md` hunts. The generated `⚠ Facade-risk index` is the join.

Full design + rationale: `docs/design-compat-surface-register.md`.
Ownership: VMS Parity Program `vms-8ad`; register epic `vms-963a`.
