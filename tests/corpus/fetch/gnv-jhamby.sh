#!/bin/bash
# fetch/gnv-jhamby.sh - Clone Jake Hamby's maintained OpenVMS ports of the
# GNU userland (GNV components). These are patched forks of GNU tools with
# OpenVMS-specific build/runtime fixes; the repos themselves carry no root
# LICENSE file (GitHub reports NOASSERTION), but the underlying tool license
# is well-established:
#   gnv-coreutils, gnv-bash, gnv-grep, gnv-sed, gnv-gnumake, gnv-gzip,
#   gnv-diffutils, gnv-gnutar          -> GNU GPL (v2/v3 per upstream tool)
#   gnv-bzip2                          -> bzip2 license (BSD-style)
#   gnv-zip, gnv-unzip                 -> Info-ZIP License (permissive)
#   gnv-ar_tools, gnv-ld_tools         -> wrapper scripts, no separate license
#
# Sizes range ~50KB (ar_tools) to ~23MB (coreutils) — fetch-script only.
# See docs/vms-source-code-corpus.md Tier 3 (#6 GNV) and Tier 6 (#15) for
# background. Upstream SourceForge GNV project (Mercurial, larger/older
# kits including a full AXP PCSI kit) is covered separately by
# fetch/gnv-sourceforge.sh.
#
# Usage: fetch/gnv-jhamby.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/gnv-jhamby"
mkdir -p "${DEST}"

REPOS="gnv-coreutils gnv-bash gnv-grep gnv-sed gnv-gnumake gnv-gzip gnv-bzip2 gnv-diffutils gnv-zip gnv-unzip gnv-ar_tools gnv-ld_tools gnv-gnutar"

for repo in ${REPOS}; do
    echo "Cloning ${repo} -> ${DEST}/${repo}"
    git clone --depth 1 "https://github.com/jhamby/${repo}.git" "${DEST}/${repo}"
done

echo "Done. ${REPOS} cloned into ${DEST}."
