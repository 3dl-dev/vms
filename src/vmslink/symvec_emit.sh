#!/bin/sh
# symvec_emit.sh — emit a LINK.EXE --symbol-vector string from a frozen
# symbol-vector manifest (bead vms-bd1, pillar vms-ade).
#
# THE single parser for the *.vec manifests (libvmssys_shr.vec, libvms_shr.vec).
# Every build recipe (mk_*_shr.sh) and every native-link test harness that used
# to carry its own hand-typed vector now derives it from the manifest through
# THIS script, so the ordered universal set is single-sourced and provably
# append-only (see the manifest headers + docs/design-link-native-toolchain.md
# §5, the GSMATCH LEQUAL upward-compatibility contract).
#
# Reads the manifest, drops `#` comments and blank lines, and joins the
# remaining `NAME=CLASS` entries with commas — the exact syntax LINK.EXE
# --symbol-vector expects. Order is preserved verbatim: a manifest line's
# position IS its symbol-vector index.
#
# Usage:  symvec_emit.sh <manifest.vec>
# Output: NAME=CLASS,NAME=CLASS,...   (no trailing newline)
set -e

MANIFEST=${1:?usage: symvec_emit.sh <manifest.vec>}
[ -f "$MANIFEST" ] || { echo "symvec_emit: manifest not found: $MANIFEST" >&2; exit 1; }

# Strip comments (`#...`) and blank/whitespace-only lines; join with commas.
# `sed 's/#.*//'` also allows trailing inline comments on an entry line.
VEC=$(sed 's/#.*//' "$MANIFEST" | sed 's/[[:space:]]//g' | grep -v '^$' | paste -sd, -)
[ -n "$VEC" ] || { echo "symvec_emit: empty vector from $MANIFEST" >&2; exit 1; }
printf '%s' "$VEC"
