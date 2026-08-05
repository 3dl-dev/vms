#!/bin/bash
# fetch/plibsys.sh - Clone plibsys, a cross-platform C system library with
# an OpenVMS Alpha/IA64 (DEC C) backend: threads, sockets, IPC, hashing.
# License: MIT. ~8.3 MB — over the commit threshold, so fetch-script only.
#
# Usage: fetch/plibsys.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}"
mkdir -p "${DEST}"

echo "Cloning plibsys -> ${DEST}/plibsys"
git clone --depth 1 https://github.com/saprykin/plibsys.git "${DEST}/plibsys"
echo "Done. VMS build notes: platforms/vms-general in the clone, and"
echo "https://github.com/saprykin/plibsys/wiki/OpenVMS"
