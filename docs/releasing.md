# Releasing OpenVMX

Every OpenVMX release is **built, proven, documented, and published by
repeatable machinery** — never hand-assembled per cut (epic `vms-a84`, the
release-engineering pillar). This document is the operator's runbook for that
machinery. Each step is a script or CI job; there is no manual "copy the
artifacts out and write the notes" step left.

## The machinery at a glance

| Stage | Tool / gate | What it guarantees |
|-------|-------------|--------------------|
| **Version** | `src/libvms/include/ovmx_identity.h` (`OVMX_PRODUCT_VERSION`) | Single source of truth (INV-1); every artifact and note is stamped from here. |
| **Cut** | `tools/cut-release.sh` | Reproducible, checksummed bundle (artifacts + `SHA256SUMS` + `release-manifest.json` + generated notes) from a clean tree. |
| **Notes** | `tools/gen_release_notes.py` | Release notes generated from merged git history since the previous release tag — never hand-maintained. |
| **Coverage** | `tools/compat/snapshot.py` + `render_compat.py --check` | The Compatibility Surface Register validates clean, and a per-cut coverage snapshot (`docs/compat/snapshots/<version>.json` + `compat-coverage.json` in the bundle) plus a compatibility-surface delta block (counts + V1 met, no percentages) in the notes are produced. See `docs/compat/REFRESH.md`. |
| **Prove** | CI: `cut-release-reproducible`, `release-acceptance`, `upgrade-e2e` | Two independent cuts are byte-identical; the cut artifact boots and reports the shipped version; a `0.N→0.N+1` upgrade preserves site config. |
| **Document** | `tools/check_guide_drift.py` + `guide_drift_gate`; the site-manual drift + grounding gates in `openvmx-site` | `docs/install-guide.md` / `docs/upgrade-guide.md` cannot drift from the e2e gates that prove them; the public Installation Guide's install commands are re-checked against `tests/qemu/test_product_install_e2e.sh` on every cut and on every docs PR; and at a major/minor cut its capability claims are checked against the compat register by `check_manual_grounding.py`. |
| **Publish** | `tools/publish-release.sh` + `.github/workflows/release.yml` | Bundle artifacts + generated notes attached to a GitHub Release; notes recorded in-tree under `docs/release-notes/`. |

## Cutting a release locally

```bash
# Build the reproducible bundle from HEAD (or --ref <tag>) into
# dist/release-<version>/.  ~25-30 min cold (full kernel + toolchain + QEMU).
tools/cut-release.sh
```

The bundle contains the four artifacts (`vmlinuz`,
`initramfs-ovmx-slim.cpio.gz`, `ovmx-distrib.img`, `ovmx-os.kit`), the OS-kit
internal manifest, `SHA256SUMS`, the machine-readable `release-manifest.json`,
and the generated `RELEASE-NOTES-<version>.md`.

As part of the cut, the compatibility-coverage snapshot is stamped and the
compatibility-surface delta block is folded into the notes:

```bash
# stamps docs/compat/snapshots/<version>.json, drops compat-coverage.json into
# the bundle, and prints the "Compatibility surface" block gen_release_notes.py
# embeds. Full procedure + the register-refresh runbook: docs/compat/REFRESH.md
python3 tools/compat/snapshot.py --out-dir dist/release-<version>/
```

## Publishing a release

Publishing is the one externally-visible step, so it is **triggered by a
deliberate human act**: pushing a release-shaped tag.

```bash
# 1. Bump OVMX_PRODUCT_VERSION in a normal PR, merge it, and let the
#    reproducibility / acceptance / upgrade gates go green on that commit.
# 2. Tag the merged commit and push the tag:
git tag 0.4 <merged-sha>
git push origin 0.4
```

The tag push fires `.github/workflows/release.yml`, which runs
`tools/cut-release.sh` then `tools/publish-release.sh` to create the GitHub
Release with all bundle assets attached and the generated notes as the release
body.

### Publishing by hand (or from an existing bundle)

```bash
# Publishes the newest dist/release-*/ bundle. Verifies SHA256SUMS and that the
# tag agrees with the bundle's product_version BEFORE any upload.
tools/publish-release.sh --tag 0.4

# See exactly what would happen without touching GitHub or git:
tools/publish-release.sh --tag 0.4 --dry-run

# A conservative first cut: create the release as a draft (nothing public until
# you click "publish" in the GitHub UI).
tools/publish-release.sh --tag 0.4 --draft
```

`publish-release.sh` refuses to publish a bundle whose checksums don't match,
or a tag that disagrees with the bundle's own `product_version` — a release can
never silently ship corrupt bytes or the wrong version label.

## Tracking release notes

`tools/gen_release_notes.py` derives notes mechanically from git history, so
they never drift from what actually shipped. `publish-release.sh` both
**publishes** those notes (as the GitHub Release body) and **records** them
in-tree under `docs/release-notes/RELEASE-NOTES-<version>.md` (staged, not
committed — you commit the record alongside the tag). That directory is the
version-controlled log of every published release's notes.

## Reviewing the public site manuals

The public documentation set lives in the `openvmx-site` repo, under its
`docs/`, not here — but it ships the same release. What release engineering
owes the manuals depends on the kind of cut.

**Major and minor cuts (0.4, 0.5, 1.0): write them.** Before the cut, the
release engineer writes or updates the site manuals so they describe what this
release actually does. New facilities get documented, changed behaviour gets
corrected, and a manual that was "in preparation" gets written once its subject
is implemented enough to describe. Ground every manual in what shipped: the
Compatibility Surface Register (`docs/compat/`) and the e2e gates are the source
of truth. Do not document a facility the register marks absent, partial, or
facade-risk as if it worked — that is the LARP the authenticity invariants
exist to stop. The manual-grounding gate described below enforces this
mechanically at the cut, so a stale or over-claiming manual fails the build
rather than shipping.

**Point releases (0.3-x): do not rewrite them.** A point cut is maintenance and
fixes; it carries no doc-authoring obligation. The mechanical checks below still
run, but no manual is expected to change.

The mechanical checks run on every cut:

- **The command-drift gate is green.** The public Installation Guide carries a
  hidden, machine-checkable block of its install commands, compared byte for
  byte against the `# GUIDE-STEP` commands in
  `tests/qemu/test_product_install_e2e.sh` at the release tag. It runs in
  `openvmx-site` on every cut (`track-release.yml`) and on every docs PR
  (`docs-drift.yml`).
- **Each manual's edition stamp matches the cut.** `track-release.yml` rewrites
  the `data-ovmx-version` token from the deployed tag, so the "Applies to" line
  follows the release. Confirm it landed.

At a major or minor cut, the grounding check runs too:

- **Manual claims match the register — the manual-grounding gate is green.** This
  is the mechanical backstop for the hand review below. Each manual declares, in
  a hidden manifest (`ovmx:covers` / `ovmx:not-yet`), the `docs/compat/` facility
  tokens it asserts as working and the ones it defers as not-yet-available.
  `openvmx-site`'s `tools/check_manual_grounding.py` renders
  `build/compat-surface.json` from the register at the release tag and checks the
  manifest against it: a deferred facility that has reached implemented/verified
  fails as a stale manual, and a claimed facility the register marks
  absent/designed/stub or facade-risk fails as an over-claim. The gate runs on
  major and minor cuts only — a point tag (`0.3-4`, `V0.3-9`) skips it, since a
  point release carries no doc-authoring obligation — and on every docs PR
  (`docs-drift.yml`), so grounding drift is caught before merge as well. It
  compares facility tokens, not prose, so a reworded manual cannot hide a claim
  the register does not support.

At a major or minor cut, also review by hand:

- **Appendix C and the capability claims match `docs/compat/`.** Re-read the
  Installation Guide's "not yet available" appendix, and any claim about what
  works, against the current Compatibility Surface Register. A facility that
  reached implemented this cut graduates out of the appendix; one that regressed
  goes back in. When you move a facility, update the manifest above so the gate
  tracks the change. Bump the edition and date in the revision-history table
  whenever the manual's content changed.
- **Drift found is filed, not shipped.** When a manual and the code disagree,
  open a documentation bug and hold the manual change. Do not edit the manual to
  match a claim the register does not carry.

The `# GUIDE-STEP` annotations in `tests/qemu/test_product_install_e2e.sh` are
read by the site's drift gate at the tag, so they matter to a repo they do not
live in. Do not remove or renumber them without matching the public Installation
Guide's command block.

## Related

- `docs/install-guide.md`, `docs/upgrade-guide.md` — the tested install/upgrade procedures.
- `docs/building.md` — building OpenVMX from source for development.
- `openvmx-site` `docs/installation/` — the public Installation Guide, drift-checked against `tests/qemu/test_product_install_e2e.sh`.
