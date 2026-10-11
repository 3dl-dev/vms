#!/bin/sh
# vec_release_cut.sh -- pin the unreleased universals at a release cut.
#
# For each OVMX-ordinal manifest src/vmslink/<name>.vec (never the VMS-ordinal
# src/vmslink/vms_vectors/*.vec, which carry VMS's own offsets): append the
# unreleased fragments <name>.d/<NAME>, sorted by name -- exactly the order
# symvec_emit.sh gives them in this tree -- to BOTH <name>.vec and its .frozen
# golden, then delete the fragments. For libvms_shr, whose builder also appends
# any discovered universal not listed (mk_libvms_shr.sh, sorted by name after
# the fragments), pass that build's list with --libvms-append FILE
# (<image>.append.vec): it is appended after the fragments, again in the
# build's order. Run in the release-cut PR only (the version bump); the freeze
# gate (tests/integration/test_symvec_freeze.sh) refuses an append anywhere else.
#
# usage: tools/vmslink/vec_release_cut.sh <release-name> [--libvms-append FILE]
set -eu
REL=${1:?usage: vec_release_cut.sh <release-name> [--libvms-append FILE]}
shift
LIBVMS_APPEND=""
while [ $# -gt 0 ]; do
    case "$1" in
        --libvms-append) LIBVMS_APPEND=${2:?}; shift 2 ;;
        *) echo "usage: vec_release_cut.sh <release-name> [--libvms-append FILE]" >&2; exit 2 ;;
    esac
done
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V="$ROOT/src/vmslink"
for vec in "$V"/*.vec; do
    name=$(basename "$vec" .vec)
    d="$V/$name.d"
    new=""
    if [ -d "$d" ]; then
        for f in $(cd "$d" && LC_ALL=C ls); do
            cls=$(sed 's/#.*//' "$d/$f" | sed 's/[[:space:]]//g' | grep -v '^$' | head -1)
            new="$new$f=$cls
"
        done
    fi
    if [ "$name" = libvms_shr ] && [ -n "$LIBVMS_APPEND" ]; then
        new="$new$(sed 's/#.*//;s/[[:space:]]//g' "$LIBVMS_APPEND" | grep -v '^$' || true)
"
    fi
    new=$(printf '%s' "$new" | grep -v '^$' || true)
    [ -n "$new" ] || { echo "$name: nothing unreleased"; continue; }
    { echo "# released in $REL"; printf '%s\n' "$new"; } >> "$vec"
    printf '%s\n' "$new" >> "$vec.frozen"
    [ -d "$d" ] && rm -r "$d"
    echo "$name: pinned $(printf '%s\n' "$new" | wc -l | tr -d ' ') universal(s) in $REL"
done
