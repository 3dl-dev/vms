#!/bin/bash
# fetch/gnv-sourceforge.sh - Download the original GNV (GNU Not VMS) project
# kits from SourceForge. https://sourceforge.net/projects/gnv/files/
# This is the upstream/older sibling of the Jake Hamby GNV forks fetched by
# fetch/gnv-jhamby.sh — the SourceForge kits are full PCSI-installable
# releases (complete AXP kit is ~166 MB) rather than individual source
# repos, and include bash, coreutils, gawk, grep, sed, make and more built
# as linked Alpha/Itanium OpenVMS images.
#
# License: GPL (varies by component, standard GNU licensing).
# Large kit — fetch-script only, never commit.
#
# Usage: fetch/gnv-sourceforge.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/gnv-sourceforge"
mkdir -p "${DEST}"

echo "Fetching GNV complete AXP kit (~166 MB) -> ${DEST}/gnv-axpvms-gnv-v0300-2-1.zip"
curl -sL --fail -o "${DEST}/gnv-axpvms-gnv-v0300-2-1.zip" \
    "https://sourceforge.net/projects/gnv/files/gnv/gnv-3.0.2/gnv-axpvms-gnv-v0300-2-1.zip/download"

echo "Done. Individual component update kits (gawk, bash, grep, sed, make, coreutils,"
echo "zip, bzip2, diffutils, ncompress, vmstar, ar_tools, ld_tools) are linked from"
echo "https://sourceforge.net/projects/gnv/files/ - browse and add curl calls as needed."
