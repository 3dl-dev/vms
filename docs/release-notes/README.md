# Release notes (tracked record)

> **Not the canonical location.** `tools/publish-release.sh` can stage a copy
> here via `--record-notes`, but the tag-triggered `release.yml` job runs with
> `--no-record-notes`, so this directory is not populated by the actual
> release flow. The real version-controlled log of published OpenVMX release
> notes is `docs/RELEASE-NOTES-<version>.md` (repo `docs/` root), added as
> part of each release's version-bump PR.

Each `RELEASE-NOTES-<version>.md` is the exact notes body published to the
corresponding GitHub Release. They are **generated**, not hand-written:
`tools/gen_release_notes.py` derives them from the merged git history since
the previous release tag. Do not hand-edit these files — re-run the tooling
against the cut commit instead.

See `docs/releasing.md` for the full release runbook.
