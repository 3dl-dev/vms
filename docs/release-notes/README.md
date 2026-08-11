# Release notes (tracked record)

This directory is the **version-controlled log of published OpenVMX release
notes** (epic `vms-a84`, the release-engineering pillar).

Each `RELEASE-NOTES-<version>.md` here is the exact notes body that was
published to the corresponding GitHub Release. They are **generated**, not
hand-written: `tools/gen_release_notes.py` derives them from the merged git
history since the previous release tag, and `tools/publish-release.sh` copies
the generated file here (and stages it) when it publishes a release. Do not
hand-edit these files — re-run the tooling against the cut commit instead.

See `docs/releasing.md` for the full release runbook.
