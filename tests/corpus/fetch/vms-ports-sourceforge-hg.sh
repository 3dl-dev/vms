#!/bin/bash
# fetch/vms-ports-sourceforge-hg.sh - Clone the vms-ports SourceForge project
# repos (Mercurial). https://sourceforge.net/projects/vms-ports/
# Project status per its SourceForge page reads "Status: Planning" with
# sparse recent activity; repos may be stale or partially abandoned. License
# varies per sub-project (mostly open, verify individually) — flagged for
# per-repo license confirmation, not blanket cleared.
#
# Requires `hg` (Mercurial) on PATH.
#
# Usage: fetch/vms-ports-sourceforge-hg.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/vms-ports-sourceforge"
mkdir -p "${DEST}"

if ! command -v hg >/dev/null 2>&1; then
    echo "ERROR: mercurial (hg) not found on PATH. Install it first (apt install mercurial)." >&2
    exit 1
fi

REPOS="ncurses s3270 osu dmpipe comm_rtl cpython hg libffi sqlite"

for repo in ${REPOS}; do
    echo "Cloning vms-ports/${repo} -> ${DEST}/${repo}"
    hg clone "https://sourceforge.net/p/vms-ports/${repo}/" "${DEST}/${repo}" || echo "  (clone failed for ${repo} - repo may have moved or been retired, check https://sourceforge.net/p/vms-ports/wiki/Home/)"
done

echo "Done."
