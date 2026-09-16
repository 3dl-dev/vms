# LANE STATE — vms-813bf (product/release/roadmap docs) — wave 1

Seat C. Worktree: `.worktrees/cleanup-docs-product`, branch
`work/vms-813bf-cleanup-docs-product`, off origin/main @ d60633a6.

## Roadmap-dedup decision

**Canonical roadmap of record: `docs/release-roadmap-to-1.0.md`** — it already
self-declared this ("Roadmap of record... single entry point") and its
`GENERATED:BEGIN/END` block is machine-written by `tools/roadmap/reconcile.py`
from rd + git tags (single-ledger compliant). No change needed to that
decision; it predates this pass.

The other 4 "roadmap" docs were **already triaged by that canonical doc's own
table** (§ "The roadmap doc set"), which I found rather than re-deriving from
scratch:

| Doc | Action | Why |
|---|---|---|
| `roadmap-reconcile-workflow.md` | Kept, untouched | The tooling's own runbook, not a competing roadmap. |
| `roadmap-v1.md` | Kept, untouched | Marked historical/superseded already; still cited as the origin of the R1–R6 gate framing and by `design-dcl-fidelity.md`. |
| `roadmap-source-compat.md` | Kept, untouched | Canonical doc's own table says "retained as design reference," and it's cited by `design-self-host-mmk-spine.md`, `design-decnet-ovmx.md`, `docs/index.md`. Deleting it would break those. |
| `roadmap-waves.md` | **git-rm'd** | Canonical doc's own table already called it "Retire candidate" (superseded by the rd dep graph + `orchestration-conductor.md`, stale invariants). Nothing else depended on it. Updated the table row (now 4 docs, not 5) with a one-line retirement note. |
| `design-authenticity-roadmap.md` | **Left alone, flagging for conductor** | This is in my file list but it is NOT a release/status roadmap — it's the active design/spec doc for the `vms-898` authenticity pillar, and two integration tests (`tests/integration/test_frozen_identity_tokens.sh`, `test_identity_ssot.sh`) cite it by section number ("sec 4.5 INV-1"). It belongs with Seat B's design-doc set, not this dedup. No content changes made. |

Result: 6 → 5 files (4 roadmap docs + 1 workflow doc), zero contradictions
between the survivors (the 2 historical ones already carry superseded banners
pointing at the canonical doc).

I also re-ran `python3 tools/roadmap/reconcile.py` to refresh the canonical
doc's `GENERATED` block (was 3 days stale, 2026-09-13 → 2026-09-16) — pure
regeneration, diff is 100% inside the `GENERATED:BEGIN/END` markers, sourced
from rd + git tags, nothing hand-typed.

## Release-notes normalization

**Convention: uppercase `docs/RELEASE-NOTES-<version>.md`** — this is what the
active tooling generates (`tools/cut-release.sh`: `RELEASE_NOTES_FILE="RELEASE-NOTES-$PRODUCT_VERSION.md"`)
and what `docs/releasing.md` and `docs/release-notes/README.md` already
document as canonical.

Renamed (git mv, content untouched) to match:
- `docs/release-notes-0.2.md` → `docs/RELEASE-NOTES-0.2.md`
- `docs/release-notes-0.5.md` → `docs/RELEASE-NOTES-0.5.md`
- `docs/release-notes-0.5-1.md` → `docs/RELEASE-NOTES-0.5-1.md`
- `docs/release-notes-0.5-2.md` → `docs/RELEASE-NOTES-0.5-2.md`

Updated the one live reference (`docs/index.md`'s release-notes line) to the
new paths. `tools/gen_release_notes.py` has a docstring comment mentioning the
old `docs/release-notes-0.2.md` path as history ("the problem this replaces")
— left untouched, it's describing a past event accurately and is outside my
doc-file scope (tools/ is not in my file list).

`docs/release-notes/` (the dir) — **kept as-is, not merged.** Its README
already correctly documents that it's the optional `--record-notes` tracked
copy (currently unused by the live `release.yml`, which runs
`--no-record-notes`), while the real canonical copy is the flat
`docs/RELEASE-NOTES-<version>.md` files. It's empty of actual notes today, so
there was no overlap/duplication to collapse — nothing to do here.

**Gap not fixed (flagging, not guessing):** most `V0.6-1`..`V0.6-16` point
tags have no `RELEASE-NOTES-0.6-N.md` file at all (only the base `0.6` has
one), same for the whole `0.4` series. Backfilling those would mean running
`tools/gen_release_notes.py` against each historical tag — a release-engineering
action, not a docs-narrative one, and risks producing notes nobody reviewed at
cut time. Left alone; conductor/release-eng call whether to backfill.

## Narrative correctness — drifted claims found and fixed

1. **README.md / getting-started.md — broken command.** Both quick-starts told
   readers to run `./distro/boot/run-qemu.sh dist/vmlinuz dist/initramfs-ovmx.cpio.gz`.
   The Dockerfile's `runner` stage (the `docker build -o dist .` export target)
   puts everything under `/boot/` (`COPY --from=builder /boot/vmlinuz /boot/vmlinuz`,
   etc.) — confirmed against `distro/Dockerfile.bootable`. The correct paths
   (already used correctly in `docs/architecture.md` and `docs/building.md`)
   are `dist/boot/vmlinuz` / `dist/boot/initramfs-ovmx.cpio.gz`. **This would
   have failed for anyone who copy-pasted the README.** Fixed in both files.

2. **`docs/capabilities-v0.6.md` — stale surface count + hard-rule negative
   framing.** Said "459 surfaces"; `python3 tools/compat/render_compat.py`
   currently reports 461. Fixed. Also rewrote the "What's not there yet" /
   "What's honest-but-partial" gap-listing paragraphs per the operator's hard
   rule (describe only what IS, delete rather than negate): kept the positive
   facts (quorum recompute + enforcement is real and grounded — `vms-b6d`,
   confirmed in the compat register: "ENFORCEMENT NOW EXISTS... on real quorum
   loss the executive STALLS clustered $ENQ grants... and RESUMES... on
   regain"; MSCP disk serving does real pread/pwrite; cross-node locking today
   is daemon-choreographed), dropped bare "absent" clauses (cluster-wide
   logicals, FDL, task-to-task DECnet) with no positive content to keep.

3. **`docs/product-vision.md` — overclaim risk on real-VAX cluster join.**
   Original line 5 said cluster join was "the North Star target... not yet a
   proven capability," which is itself now stale (V0.6 shipped a cluster join).
   But I did **not** write the stronger claim either — see the flag below, this
   is a live contradiction in the repo's own sources. I worded it conservatively:
   OVMX↔OVMX (OVMX^n) cluster formation/join is proven; joining an existing
   VSI-coordinated cluster remains the still-being-climbed target. Also removed
   the "### Not near-term" section (VSI support/certs, deep clustering
   internals) — pure negative-framing per the hard rule, nothing positive to
   preserve, and both points were already implied by the surrounding text.

4. **`docs/architecture.md`** — reworded the PID-1 boot-sequence description
   away from "does NOT install... does NOT read SYSUAF... is NOT SYSTEM" to a
   positive statement of PID 1's actual (narrow) identity scope. Same facts,
   no negation.

## CONDUCTOR-GATE — needs a call before/at merge

**Real-VAX cluster-join claim contradicts itself across two sources I'm not
authorized to silently reconcile (Rule 10):**

- `docs/release-roadmap-to-1.0.md`'s **V0.6-11** shipped-release bullet (this
  text is tool-mastered — it lives in `tools/roadmap/reconcile.py`'s
  `EDITORIAL_CONFIG`, not hand-edited in the doc, so I did not touch it)
  claims: *"a booted OpenVMX node joins a real, running OpenVMS Cluster... the
  existing members' own accounting counted it: VAX1's F$GETSYI('CLUSTER_NODES')
  reported 3."* That's a specific, falsifiable, real-VAX-side proof claim.
- The **compat register**, `connection-manager` facility, reviewed 2026-09-10
  (more recent than V0.6-11's ship): *"connection-manager$real-vax-join,
  absent — this proof is OVMX^n only... no row has read MEMBER against a
  genuine OpenVMS node."*

These cannot both be true as stated. I deliberately did **not** guess — I kept
`product-vision.md`'s claim scoped to the register's (more recent, presumably
more authoritative per this project's own compat-is-SSOT convention) OVMX^n-only
framing, and left the V0.6-11 editorial text alone since it's tool-owned, not
mine to hand-edit. **Someone needs to determine: was the V0.6-11 claim an
overclaim later walked back, or is the register stale/wrong, and then fix
whichever source is wrong** (register review or `reconcile.py`'s
`EDITORIAL_CONFIG`, neither of which is in my file scope). Recommend filing an
rd item if one doesn't already exist tracking this specific reconciliation.

## Not touched / out of scope, flagging why

- `design-authenticity-roadmap.md` — see table above; it's Seat B's design-doc
  territory even though it was in my file list.
- `docs/install-0.1.md` — already self-marked SUPERSEDED with correct pointers
  to `getting-started.md`/`install-guide.md`; treated it like a preserved
  historical record (like a release note) rather than editing its frozen body
  for the negative-framing rule.
- `site/` — did not find tracked narrative prose to touch (mostly untracked
  per the task brief); no action taken.
- Did not touch `docs/building.md`, `docs/building-multiarch.md`,
  `docs/install-guide.md`, `docs/upgrade-guide.md`, `AGENTS.md`,
  `docs/releasing.md` — read them, found no drift or negative-framing issues
  worth a change.

## Remaining / next actions (for a follow-up wave, or a successor)

- Verify with the conductor whether the V0.6-11 vs. compat-register
  contradiction above needs an immediate correction before this PR merges, or
  can land as a tracked follow-on.
- Consider whether to backfill missing `RELEASE-NOTES-0.6-N.md` files for
  V0.6-1..16 (release-eng decision, not mine).
- A full negative-framing sweep of `docs/index.md`'s "Internal / Design"
  section and any other public-facing doc outside my explicit scope was not
  performed (out of lane).
