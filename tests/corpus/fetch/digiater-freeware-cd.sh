#!/bin/bash
# fetch/digiater-freeware-cd.sh - Download individual packages from the
# Digiater.nl mirror of the historic VSI/DEC OpenVMS Freeware CD collection
# (versions v1.0 through v8.0, VAX/Alpha/Itanium era, snapshotted 2009).
# https://www.digiater.nl/openvms/freeware/
#
# The v8.0 index alone lists 600+ packages (source, VMS-native .EXE binaries,
# and BACKUP savesets mixed per-package) under a wide variety of licenses
# (mostly GPL, but package-by-package — LICENSE FIRST: verify each package's
# own terms before treating it as redistributable). This script does not
# attempt to enumerate all 600+; it fetches a curated high-value subset by
# name, and documents how to browse for more.
#
# Usage: fetch/digiater-freeware-cd.sh [DEST_DIR]
#
# To browse and add more packages:
#   https://www.digiater.nl/openvms/freeware/v80/<package-name>/

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/digiater-freeware-v80"
BASE="https://www.digiater.nl/openvms/freeware/v80"
mkdir -p "${DEST}"

# Curated subset: build tools, compression, and VMS-idiom-heavy utilities
# not already covered by other corpus sources (MMK, GNV are elsewhere).
PACKAGES="find flist gzip bzip2 unzip zip"

for pkg in ${PACKAGES}; do
    echo "Mirroring ${BASE}/${pkg}/ -> ${DEST}/${pkg}/"
    wget -q -r -np -nH --cut-dirs=3 -P "${DEST}" "${BASE}/${pkg}/" || echo "  (wget failed for ${pkg}, check the URL manually)"
done

echo "Done. Each package directory carries its own license — check before use."
echo "Full catalog: https://www.digiater.nl/openvms/freeware/v80/"
