#!/bin/sh
#
# test_symvec_freeze.sh — bead vms-bd1 standing gate: the b65 shareable symbol
# vectors are FROZEN and APPEND-ONLY, so GSMATCH LEQUAL stays sound as vmsrms/DCL
# bind universal indices in production.
#
# A shareable image's symbol vector is an ORDINAL contract: a consumer binds an
# imported universal by its INDEX, and the OpenVMS upward-compatibility rule for
# a GSMATCH LEQUAL image (docs/design-link-native-toolchain.md §5, from the
# public VSI OpenVMS Linker Utility Manual -- clean-room, CLAUDE.md Rule 8) is
# that an index NEVER moves: you may only APPEND new universals at the end, or
# RETIRE one in place (PRIVATE_PROCEDURE/PRIVATE_DATA) -- never reorder, never
# delete. This gate proves that rule held, purely from the source tree (no build,
# no docker, no /dev/vms).
#
# For every canonical manifest src/vmslink/*.vec it asserts:
#   (A) APPEND-ONLY: the committed golden <manifest>.frozen is still an EXACT
#       ordered PREFIX of the live manifest. Reorder or drop within the frozen
#       block -> the prefix diverges -> RED. Appending lines at the end -> the
#       golden is still a prefix -> GREEN.
#   (B) NO DUPLICATE names in a manifest (a repeat is an ambiguous index).
# And, tree-wide:
#   (C) SINGLE SOURCE: no build recipe or native-link harness has re-grown an
#       inline copy of the LIBVMSSYS$SHR vms_kif_* vector. Before vms-bd1 that
#       vector lived in SEVEN hand-kept places that had already drifted; every
#       consumer now derives it from the manifest via symvec_emit.sh.
#
# Negative control (proves this gate has teeth): test_symvec_freeze_negctl.sh.
#
# Usage: test_symvec_freeze.sh [source-root]   (defaults to the repo root)
set -eu

SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
VECDIR="$SRC/src/vmslink"
TESTDIR="$SRC/src/imgact/test"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0

# Strip `#` comments (incl. trailing) + all whitespace; drop blank lines.
extract() { sed 's/#.*//' "$1" | sed 's/[[:space:]]//g' | grep -v '^$' || true; }

[ -d "$VECDIR" ] || { echo "FAIL: no $VECDIR"; exit 1; }
found=0
for manifest in "$VECDIR"/*.vec; do
    [ -f "$manifest" ] || continue
    found=$((found+1))
    name=$(basename "$manifest")
    frozen="$manifest.frozen"

    if [ ! -f "$frozen" ]; then
        echo "FAIL [$name]: no committed golden $name.frozen (append-only proof baseline)"
        rc=1; continue
    fi

    extract "$manifest" > "$TMP/live"
    # The golden is already pure NAME=CLASS, but normalise it the same way so a
    # stray edit (comment/whitespace) can never mask a real reorder.
    extract "$frozen" > "$TMP/gold"

    FN=$(wc -l < "$TMP/gold" | tr -d ' ')
    LN=$(wc -l < "$TMP/live" | tr -d ' ')

    # (A) append-only: the golden must be an exact ordered PREFIX of the live file.
    if [ "$LN" -lt "$FN" ]; then
        echo "FAIL [$name]: live manifest has $LN entries, fewer than the $FN frozen -- an entry was DROPPED"
        rc=1; continue
    fi
    head -n "$FN" "$TMP/live" > "$TMP/prefix"
    if ! cmp -s "$TMP/gold" "$TMP/prefix"; then
        echo "FAIL [$name]: frozen prefix changed -- an existing index was REORDERED or DROPPED (append-only violated):"
        diff "$TMP/gold" "$TMP/prefix" | sed 's/^/    /' || true
        echo "    -> Only APPEND at the end, or RETIRE in place (PRIVATE_PROCEDURE/PRIVATE_DATA)."
        rc=1; continue
    fi

    # (B) no duplicate universal names.
    dups=$(cut -d= -f1 "$TMP/live" | sort | uniq -d || true)
    if [ -n "$dups" ]; then
        echo "FAIL [$name]: duplicate universal name(s) -> ambiguous index:"
        printf '%s\n' "$dups" | sed 's/^/    /'
        rc=1; continue
    fi

    APP=$((LN-FN))
    echo "OK   [$name]: $FN frozen entries intact (append-only), $APP appended, no dups"
done

if [ "$found" -eq 0 ]; then
    echo "FAIL: no *.vec manifests under $VECDIR"
    exit 1
fi

# (C) single source of truth: the comma-joined vms_kif_* vector may appear in NO
# script -- only one-per-line in the *.vec manifests. A hit means a 7th inline
# copy re-grew (the exact drift vms-bd1 removed).
if [ -d "$TESTDIR" ] || [ -d "$VECDIR" ]; then
    hits=$(grep -rEl 'vms_kif_enq=PROCEDURE,vms_kif_deq' "$VECDIR" "$TESTDIR" 2>/dev/null \
             | grep -vE '\.vec(\.frozen)?$' || true)
    if [ -n "$hits" ]; then
        echo "FAIL: inline copy of the LIBVMSSYS\$SHR vms_kif_* vector re-grew (derive it from the manifest via symvec_emit.sh):"
        printf '%s\n' "$hits" | sed 's/^/    /'
        rc=1
    else
        echo "OK   single-source: no inline vms_kif_* vector copies outside the manifest"
    fi
fi

[ "$rc" -eq 0 ] && echo "PASS: symbol vectors are frozen + append-only + single-sourced"
exit "$rc"
