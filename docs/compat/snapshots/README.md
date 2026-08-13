# `docs/compat/snapshots/` — per-release coverage snapshots

Each `<version>.json` is the Compatibility Surface Register's coverage **as of a
release cut** — per-item status + per-domain and overall rollups. They are
**generated** by `tools/compat/snapshot.py` at cut time (release train, pillar
`vms-a84`); do not hand-edit.

They exist so coverage can be tracked **over time**: `snapshot.py` diffs the
current register against the previous snapshot to produce the "Compatibility
coverage" block in the release notes, and the website renders the trend.

`baseline-2026-08-13.json` seeds the series — the register's coverage on the day
it was created (honestly dated; not retroactively attributed to an already-shipped
release). The first cut through the machinery writes the first version-labelled
snapshot and diffs against this baseline.
