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

# THE RELEASED LIST + UNRELEASED FRAGMENTS (rd: conflict-free vector additions).
# <manifest>.vec holds the RELEASED universals, in index order, and changes only
# in a release cut (tools/vmslink/vec_release_cut.sh). A universal added since
# the last release is one file, <manifest>.d/<NAME>, holding its CLASS: a new
# file never conflicts with another PR's. They follow the released list SORTED
# BY NAME, so their indices are tree-local until the cut pins them -- nothing
# outside the tree binds an unreleased index (every image is built from this
# tree). VECTORS THAT CARRY VMS'S OWN ORDINALS (src/vmslink/vms_vectors/*.vec:
# SYS$PUBLIC_VECTORS, LIBRTL, DECC$SHR, SECURESHRP) are pinned to VMS's values
# and are never assembled here.
case "$MANIFEST" in
    */vms_vectors/*) echo "symvec_emit: refusing $MANIFEST: a VMS-ordinal vector is pinned to VMS's offsets, never name-sorted" >&2; exit 1 ;;
esac
if grep -qE '^[[:space:]]*0x[0-9A-Fa-f]+' "$MANIFEST"; then
    echo "symvec_emit: refusing $MANIFEST: it carries explicit VMS offsets" >&2; exit 1
fi

REL=$(sed 's/#.*//' "$MANIFEST" | sed 's/[[:space:]]//g' | grep -v '^$' || true)
FRAGDIR="${MANIFEST%.vec}.d"
FRAG=""
if [ -d "$FRAGDIR" ]; then
    for f in $(cd "$FRAGDIR" && LC_ALL=C ls); do
        cls=$(sed 's/#.*//' "$FRAGDIR/$f" | sed 's/[[:space:]]//g' | grep -v '^$' | head -1)
        case "$cls" in
            PROCEDURE|DATA|PRIVATE_PROCEDURE|PRIVATE_DATA) ;;
            *) echo "symvec_emit: fragment $FRAGDIR/$f: class '$cls' is not PROCEDURE/DATA/PRIVATE_PROCEDURE/PRIVATE_DATA" >&2; exit 1 ;;
        esac
        if printf '%s\n' "$REL" | grep -q "^$f="; then
            echo "symvec_emit: fragment $FRAGDIR/$f names a universal the released list already has" >&2; exit 1
        fi
        FRAG="$FRAG
$f=$cls"
    done
fi
VEC=$(printf '%s%s\n' "$REL" "$FRAG" | grep -v '^$' | paste -sd, -)
[ -n "$VEC" ] || { echo "symvec_emit: empty vector from $MANIFEST" >&2; exit 1; }
printf '%s' "$VEC"
