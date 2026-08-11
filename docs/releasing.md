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
| **Prove** | CI: `cut-release-reproducible`, `release-acceptance`, `upgrade-e2e` | Two independent cuts are byte-identical; the cut artifact boots and reports the shipped version; a `0.N→0.N+1` upgrade preserves site config. |
| **Document** | `tools/check_guide_drift.py` + `guide_drift_gate` | `docs/install-guide.md` / `docs/upgrade-guide.md` cannot drift from the e2e gates that prove them. |
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

## Related

- `docs/install-guide.md`, `docs/upgrade-guide.md` — the tested install/upgrade procedures.
- `docs/building.md` — building OpenVMX from source for development.
