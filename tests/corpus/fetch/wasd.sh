#!/bin/bash
# fetch/wasd.sh - Download the WASD HTTP Server for OpenVMS.
#
# WASD is a full-featured VMS-native web server: AST-driven async I/O,
# process pooling, CGI/CGIplus, WebSockets. License: Apache 2.0
# (Copyright 1996-2026 Mark G. Daniel). Verified live 2026-08-04.
#
# Two kits:
#   WASD1240.ZIP      - full source kit (~9 MB), builds on VAX/Alpha/IA64/x86-64
#   WASD1240-X86.ZIP   - precompiled x86-64 OBJECT MODULES (226 .OBJ files,
#                        ~10.5 MB unpacked / 3.7 MB zipped), built with
#                        VSI C x86-64 V7.7-003 on OpenVMS x86_64 V9.2-3.
#                        NOTE: these are unlinked .OBJ modules, not images —
#                        WASD's INSTALL/UPDATE DCL procedures link them.
#                        Still the most directly relevant x86-64 VMS object
#                        code found during the vms-e86 corpus survey.
#
# Not committed to git: source kit exceeds the ~5MB commit threshold, and
# the -X86 kit is compiled binary content (repo size discipline, CLAUDE.md).
#
# Usage: fetch/wasd.sh [DEST_DIR]

set -euo pipefail
DEST="${1:-${OVMX_CORPUS_STAGE:-/home/baron/.claude/jobs/91acc983/tmp/corpus-staging}}/wasd"
mkdir -p "${DEST}"

echo "Fetching WASD source kit -> ${DEST}/WASD1240.ZIP"
curl -sL --fail -o "${DEST}/WASD1240.ZIP" "https://wasd.vsm.com.au/wasd/wasd1240.zip"

echo "Fetching WASD x86-64 object-module kit -> ${DEST}/WASD1240-X86.ZIP"
curl -sL --fail -o "${DEST}/WASD1240-X86.ZIP" "https://wasd.vsm.com.au/wasd/wasd1240-x86.zip"

echo "Done. Unzip WASD1240.ZIP first, then WASD1240-X86.ZIP into the same tree,"
echo "per the README embedded in the -X86 kit."
