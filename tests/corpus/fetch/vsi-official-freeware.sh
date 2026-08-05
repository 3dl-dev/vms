#!/bin/bash
# fetch/vsi-official-freeware.sh - Download packages from VSI's own current
# freeware page: https://vmssoftware.com/community/freeware/
# (served from vmssoftware.com/software/freeware/<NAME>.zip)
#
# FLAGGED — license status unclear. The page states VSI "does not own or
# support" this software and disclaims warranties, but does not itself
# grant redistribution rights; the underlying tools (Bison, Flex = GPL;
# BYACC = public-domain-ish; MMK = BSD-3-Clause, already in our corpus from
# GitHub) likely carry the rights, but VSI's specific repackaging has not
# been checked against each tool's actual bundled license text. Do not
# commit until reviewed. See tests/corpus/INVENTORY.md flagged list.
#
# Usage: fetch/vsi-official-freeware.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/vsi-official-freeware"
BASE="https://vmssoftware.com/software/freeware"
mkdir -p "${DEST}"

FILES="MMK.zip BISON.zip FLEX.zip BYACC.zip CURL.zip OPENSSL_Z.zip"

for f in ${FILES}; do
    echo "Fetching ${f}"
    curl -sL --fail -o "${DEST}/${f}" "${BASE}/${f}" || echo "  (failed for ${f}, check ${BASE}/ manually)"
done

echo "Done. Review each package's bundled license before any use beyond local testing."
