#!/bin/bash
# fetch/openssl-curl-vms.sh - Clone upstream OpenSSL and curl, both of which
# carry maintained OpenVMS support (DEC C, VMS descriptors, ODS-5 paths).
# OpenSSL: Apache 2.0. curl: curl/MIT-style license. Both are large, actively
# developed trees (tens of MB) — fetch-script only, never commit.
#
# VMS-specific docs:
#   https://github.com/openssl/openssl/blob/master/NOTES-VMS.md
#   https://sourceforge.net/p/vms-ports/wiki/HaxxCurl/  (VMS curl port notes)
#
# Usage: fetch/openssl-curl-vms.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}"
mkdir -p "${DEST}"

echo "Cloning OpenSSL (shallow) -> ${DEST}/openssl"
git clone --depth 1 https://github.com/openssl/openssl.git "${DEST}/openssl"

echo "Cloning curl (shallow) -> ${DEST}/curl"
git clone --depth 1 https://github.com/curl/curl.git "${DEST}/curl"

echo "Done. See NOTES-VMS.md in openssl/, and the vms-ports HaxxCurl wiki for curl's VMS build steps."
