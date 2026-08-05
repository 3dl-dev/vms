#!/bin/bash
# fetch/vmsbackup.sh - Clone the two independent VMS BACKUP save-set readers.
# Both are small, real-world VMS file-format parsers (.BCK save sets) but
# NEITHER repo declares a license (GitHub reports license: null for both).
# FLAGGED — do not commit source into the corpus until redistribution rights
# are confirmed (contact the authors or find an in-repo license statement
# missed by GitHub's detector). See tests/corpus/INVENTORY.md flagged list.
#
#   FreddieAkeroyd/vmsbackup   ~137 KB, C, last pushed 2017
#   TonyBUK/VMSBackup          ~146 KB, C + Python, actively maintained
#
# Usage: fetch/vmsbackup.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/vmsbackup"
mkdir -p "${DEST}"

echo "Cloning FreddieAkeroyd/vmsbackup -> ${DEST}/vmsbackup-freddieakeroyd"
git clone --depth 1 https://github.com/FreddieAkeroyd/vmsbackup.git "${DEST}/vmsbackup-freddieakeroyd"

echo "Cloning TonyBUK/VMSBackup -> ${DEST}/vmsbackup-tonybuk"
git clone --depth 1 https://github.com/TonyBUK/VMSBackup.git "${DEST}/vmsbackup-tonybuk"

echo "Done. LICENSE STATUS UNCLEAR for both - do not redistribute further without confirming rights."
