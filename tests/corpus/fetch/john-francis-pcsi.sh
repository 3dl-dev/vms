#!/bin/bash
# fetch/john-francis-pcsi.sh - Download John Francis's PCSI-kit builds of
# common open-source libraries for OpenVMS Alpha (AXP/) and Itanium (IA64/).
# Each .ZIP contains a PCSI kit (VMS package installer format) with the
# library prebuilt as a VMS-native shareable image, PLUS full source under
# SOURCES/. This is the best available source of *linked* Alpha/Itanium
# OpenVMS binaries outside VSI's own gated download portal.
#
# Format: .ZIP containing .PCSI$COMPRESSED kit(s) - "format": "pcsi" in
# inventory.json. Underlying libraries carry their own upstream licenses
# (zlib license, libpng license, BSD-ish libjpeg, GPL for MySQL, PSF for
# Python, Apache-ish for OpenSSL 0.9.7/0.9.8 — all EOL versions, note
# security relevance is nil, this is purely a compat-test target).
#
# Total size across both directories is several hundred MB (Python and
# MySQL kits alone are 15-45 MB each) — fetch-script only, parameterized
# so callers can pull only what they need.
#
# Usage:
#   fetch/john-francis-pcsi.sh [DEST_DIR] [small|all]
#     small (default) - zlib, libpng, libbz2, gdchart, freetype, libimaging,
#                        libxslt, swish-e, webware (each well under 5 MB)
#     all              - everything, including libxml2/mysql/python (tens
#                        of MB each) for both AXP and IA64

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/john-francis-pcsi"
MODE="${2:-small}"
BASE="http://www.vsm.com.au/ftp/jfp/kits"

SMALL_FILES="ZLIB-V0102-7-1.ZIP LIBPNG-V0105-13-1.ZIP LIBBZ2-V0100-6-1.ZIP GDCHART-V0011-4-1.ZIP FREETYPE-V0203-7-1.ZIP LIBIMAGING-V0101-6-1.ZIP LIBXSLT-V0101-28-1.ZIP SWISH_E-V0204-3-1.ZIP WEBWARE093-V0100-0-1.ZIP LIBJPEG-V0804--1.ZIP LIBGD-V0200-35-1.ZIP"
LARGE_FILES="LIBXML2-V0209-1-1.ZIP MYSQL051-V2301-0-1.ZIP PYTHON278-V0100-0-1.ZIP OPENSSL098A-V0102-0-1.ZIP"

for arch in AXP IA64; do
    mkdir -p "${DEST}/${arch}"
    for f in ${SMALL_FILES}; do
        echo "Fetching ${arch}/${f}"
        curl -sL --fail -o "${DEST}/${arch}/${f}" "${BASE}/${arch}/${f}" || echo "  (not present for ${arch}, skipping)"
    done
    if [ "${MODE}" = "all" ]; then
        for f in ${LARGE_FILES}; do
            echo "Fetching ${arch}/${f} (large)"
            curl -sL --fail -o "${DEST}/${arch}/${f}" "${BASE}/${arch}/${f}" || echo "  (not present for ${arch}, skipping)"
        done
    fi
done

echo "Done. Exact filenames drift between AXP/ and IA64/ (version suffixes differ) —"
echo "re-check http://www.vsm.com.au/ftp/jfp/kits/<ARCH>/ and adjust the file lists above if a curl fails."
