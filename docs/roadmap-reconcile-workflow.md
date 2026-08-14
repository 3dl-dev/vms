# Roadmap reconcile + publish — checkpoint workflow

**Run this on every roadmap checkpoint.** It reconciles the release roadmap with rd (the
single source of truth) and regenerates the published status — in-repo and on the public
site. It is idempotent: re-running with unchanged rd produces byte-identical output.

- Tool: `tools/roadmap/reconcile.py` (stdlib only, no host installs)
- Tracks: rd item `vms-8747` (repeatable reconcile+publish), built by `vms-7851`

## What is the source of truth

**rd is authoritative.** Nothing about release status is hand-maintained. The tool reads
one snapshot (`rd list --all --json`) and derives everything from two signals:

1. **Milestones** — items carry a label `rel-0.5` … `rel-1.0`. Each milestone's status is
   the rollup of its labelled items (done / open / blocked, and a completion signal).
2. **1.0-gate workstreams** — seven epics, rolled up over their `parent_id` descendant
   trees:

   | Workstream | Epic | Lands by |
   |---|---|---|
   | Executive substrate | `vms-6b8` | 0.5 |
   | Command-surface parity | `vms-8ad` | continuous (0.3-x) |
   | Self-hosting toolchain | `vms-678` | 0.5 → 1.0 |
   | Cluster configuration | `vms-098` | 0.5 → 1.0 |
   | TCP/IP networking | `vms-67f` | 0.5 → 1.0 |
   | DECnet Phase IV | `vms-30e` | 1.0 |
   | Kernel substrate | `vms-19e` | 0.5 |

Shipped releases come from `git tag` (release-like tags only), newest first.

The **editorial** parts — milestone theme lines, workstream summaries, release notes — are
stable curation in the `EDITORIAL CONFIG` block at the top of the script. They are *names*,
not derived status; every "done / open / blocked / %" number is re-derived each run. If you
add a milestone or gate epic, add its editorial entry too, and the gap report will remind
you (see below).

## What it writes

| Output | Path | Audience |
|---|---|---|
| Roadmap status block | `docs/release-roadmap-to-1.0.md` (between `GENERATED:BEGIN/END` markers) | in-repo, detailed — carries item IDs + the gap report |
| Canonical export | `build/roadmap.json` (git-ignored) | machine export with IDs; feeds the site step |
| Public site data | `<site>/data/roadmap.json` | **milestone-level, trademark-scrubbed, no internal IDs** |

The hand-written narrative in the roadmap doc (§2–§7, the appendix) is never touched — only
the `GENERATED` block between the markers is rewritten.

### The public/private split (curation rule)

The **site view is milestone-level**: milestone theme + status + progress, workstream
summaries + progress, and recent releases. It carries **no internal item IDs, no
confidential notes, no half-finished internal debate**, and is trademark-scrubbed (same
contract as `data/compat-surface.json`: `grep -c OpenVMS` and `grep -cw VSI` must be `0`).
The in-repo roadmap doc is the detailed view and may carry IDs and the labelling-gap report.

The site's `roadmap/` and `status/` pages `fetch('/data/roadmap.json')` and render it — the
same data-driven pattern as `/compat/`. The page HTML is stable; only the data is
regenerated each checkpoint.

## How to run the checkpoint

From a checkout of `vms` at the ref you are publishing (a release tag, or `main`):

```sh
# 1. Regenerate the in-repo roadmap status + the canonical export.
python3 tools/roadmap/reconcile.py

# 2. Same run, plus the public site data (point at your openvmx-site checkout).
python3 tools/roadmap/reconcile.py --site-dir ../openvmx-site

# 3. Prove idempotency (must exit 0, "clean"): a second run changes nothing.
python3 tools/roadmap/reconcile.py --check --site-dir ../openvmx-site

# just the rd-labelling gap report, no writes:
python3 tools/roadmap/reconcile.py --print-gaps
```

Flags:

- `--as-of YYYY-MM-DD` — the only date stamp in the output (a date, not a time). Defaults to
  today (UTC). Pin it to reproduce a prior run byte-for-byte, or so a same-day CI `--check`
  is deterministic.
- `--rd-json <file>` — read a snapshot from a file instead of calling `rd` (pin the input in
  CI, or reconcile an offline snapshot).
- `--check` — exit non-zero if regenerating would change any file. Use as a drift gate.

Then: commit the roadmap doc in `vms`; open a **separate PR** on `openvmx-site` with the
regenerated `data/roadmap.json`. **Do not auto-publish the site** — public-facing content is
reviewed before it goes live (the conductor gates it).

## Keeping rd complete so the source stays accurate

The tool cannot invent a milestone for an item that carries no `rel-*` label, and it prints
a **labelling-gap report** on every run (also written into the roadmap doc). Two gaps to
keep at zero:

1. **Gate epics missing a `rel-*` label.** These get an *editorial* band from the script's
   `GATE_EPICS` table, not a derived one. As of writing, `vms-67f`, `vms-678`, `vms-30e`,
   and `vms-8ad` (intentionally "continuous") carry no `rel-*` label. Label the ones that
   belong to a single milestone.
2. **Children of unlabelled gate epics with no `rel-*` of their own.** They inherit the
   editorial band; label them to make rd authoritative.

When the report is empty, the generated view is derived entirely from rd. Until then, treat
the flagged bands as editorial and reconcile the labels — do **not** hand-edit the generated
block to paper over a labelling gap.

## Cadence

Run at every roadmap checkpoint: when a milestone is cut, when a gate epic changes state, or
on a periodic sweep. The workflow is the mechanism behind the standing "reassess the 1.0
roadmap to current reality + publish" item (`vms-043`).
