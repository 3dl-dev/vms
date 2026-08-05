#!/bin/bash
# fetch/vms-ports-jhamby.sh - Clone Jake Hamby's other OpenVMS ports (beyond
# GNV/GNU userland — see fetch/gnv-jhamby.sh for those). Mix of libraries,
# benchmarks, and toys, in-progress ports to 64-bit OpenVMS (Alpha/IA64/x86).
# Small items (vms-laxdriver, vms-ipc_benchmark, vms-memtester, cmatrix) are
# already committed directly under tests/corpus/tier6-*/ — do not re-fetch
# those here. This script covers the larger/less-certain-license remainder:
#
#   vms-libxml2       MIT            ~4.7 MB
#   vms-libjpeg-turbo  (no root license; libjpeg-turbo is itself BSD-style) ~2.3 MB
#   vms-libuv          MIT            ~15 MB
#   vms-gmp            GPL-2.0        ~4 MB
#   vms-regina         NOASSERTION (Regina Rexx is public-domain-ish; no root file here) ~4.4 MB
#   vms-glextrusion    NOASSERTION    ~1.2 MB
#   vms-ramspeed-smp   NOASSERTION    ~117 KB
#   vms-coremark       NOASSERTION -- EEMBC CoreMark license has usage
#                       restrictions on publishing scores; FLAGGED, see
#                       tests/corpus/INVENTORY.md flagged list. ~496 KB
#   vms-coremark-pro   same EEMBC caveat as vms-coremark. ~15 MB
#   vms-dmpipe         no license file  ~117 KB
#   vms-halls-of-zk    no license file  ~236 KB
#   vms-xscreensaver   no license file (upstream XScreenSaver is BSD-ish
#                       with a specific redistribution clause) ~78 MB
#   vms-mesa-demos     (Mesa demos, SGI/MIT-style upstream) ~25 MB
#   vms-boost          BSL-1.0 (Boost Software License, permissive) ~136 MB
#   vms-perl5          NOASSERTION (Perl is dual Artistic/GPL upstream) ~346 MB
#   vms-cpython        NOASSERTION (CPython is PSF-2.0 upstream) ~393 MB
#
# License-first: items above marked NOASSERTION/no-license-file are usable
# as read-only reference corpus only, matching the tier3-netlib precedent
# in LICENSE-AUDIT.md, until upstream license terms are confirmed per repo.
#
# Usage: fetch/vms-ports-jhamby.sh [DEST_DIR] [small|all]
#   small (default) - skips vms-boost/vms-perl5/vms-cpython/vms-xscreensaver
#                      (100+ MB each); pass "all" to include them.

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/vms-ports-jhamby"
MODE="${2:-small}"
mkdir -p "${DEST}"

SMALL_REPOS="vms-libxml2 vms-libjpeg-turbo vms-libuv vms-gmp vms-regina vms-glextrusion vms-ramspeed-smp vms-coremark vms-coremark-pro vms-dmpipe vms-halls-of-zk vms-mesa-demos"
LARGE_REPOS="vms-xscreensaver vms-boost vms-perl5 vms-cpython"

for repo in ${SMALL_REPOS}; do
    echo "Cloning ${repo} -> ${DEST}/${repo}"
    git clone --depth 1 "https://github.com/jhamby/${repo}.git" "${DEST}/${repo}"
done

if [ "${MODE}" = "all" ]; then
    for repo in ${LARGE_REPOS}; do
        echo "Cloning ${repo} (large) -> ${DEST}/${repo}"
        git clone --depth 1 "https://github.com/jhamby/${repo}.git" "${DEST}/${repo}"
    done
else
    echo "Skipping large repos (${LARGE_REPOS}); pass 'all' as the second argument to include them."
fi
