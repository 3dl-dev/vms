#!/bin/sh
#
# cluster_dlm_hash_gate.sh - ONE CALLER RULE for the DLM resource-name hash
# (rd vms-b5b0).
#
# WHAT IT ENFORCES, AND WHY THE RULE IS WORTH A GATE
#
# src/kernel-core/vms_dlm_hash.c exports the determined VMS resource-name hash
# twice:
#
#   vms_dlm_name_hash()         the ARITHMETIC, ungated. It answers for ANY
#                               identity, including ones no VMS node has ever
#                               been watched hashing. It stays ungated on
#                               purpose: reproducing a captured value is how
#                               the function is tested, and scoring a fresh
#                               capture is how its coverage is widened -- both
#                               have to be able to evaluate identities the wire
#                               has not shown, precisely to find out whether it
#                               ever does.
#   vms_dlm_name_hash_proven()  the same arithmetic BEHIND THE PROVEN-COVERAGE
#                               masks, which is the only entry point a value
#                               that will reach a wire may come from.
#
# A frame carrying a value for an identity outside the proven coverage does not
# fail locally: it makes the receiving directory node scan the wrong chain,
# miss the name, and install the SENDER as master of a resource somebody else
# already masters -- the 35-frames-a-second grant storm in operator memory
# cluster-promotion-gap. So the EXECUTIVE must never reach the ungated entry
# point, and that is a one-line property a grep can hold forever:
#
#   in src/ (outside vms_dlm_hash.{c,h} -- the TU that defines it and the
#   header that declares it), `vms_dlm_name_hash(` may not appear. Tests,
#   tools and docs may call it freely.
#
# Run: sh tools/ci/cluster_dlm_hash_gate.sh
#      sh tools/ci/cluster_dlm_hash_gate.sh --self-test
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
impl="src/kernel-core/vms_dlm_hash.c"
decl="src/kernel-core/vms_dlm_hash.h"

scan() {
    # Every src/ reference to the UNGATED entry point, excluding the
    # implementation TU (which defines it and is called BY the gated one) and
    # excluding the gated name itself (vms_dlm_name_hash_proven contains the
    # ungated name as a prefix, so the pattern must exclude it explicitly).
    ( cd "$1" && grep -rn 'vms_dlm_name_hash *(' src/ 2>/dev/null ) |
        grep -v "^${impl}:" |
        grep -v "^${decl}:" |
        grep -v 'vms_dlm_name_hash_proven' |
        grep -v 'vms_dlm_name_hash_coverage' || true
}

if [ "${1-}" = "--self-test" ]; then
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/src/kernel-core"
    cp "$root/$impl" "$tmp/$impl"
    cp "$root/$decl" "$tmp/$decl"
    cat > "$tmp/src/kernel-core/fake_caller.c" <<'EOF'
/* an executive TU reaching the UNGATED entry point */
void f(void) { (void)vms_dlm_name_hash(0, 0, 0, 0, 0); }
EOF
    if [ -z "$(scan "$tmp")" ]; then
        echo "SELF-TEST FAILED: the gate did not see an injected ungated caller" >&2
        exit 1
    fi
    echo "SELF-TEST PASSED: an executive caller of vms_dlm_name_hash() is detected"
    exit 0
fi

hits=$(scan "$root")
if [ -n "$hits" ]; then
    echo "FAIL: the executive must reach the DLM resource-name hash only through" >&2
    echo "      vms_dlm_name_hash_proven() -- a value outside the PROVEN COVERAGE" >&2
    echo "      must never reach a frame (rd vms-b5b0, src/kernel-core/vms_dlm_hash.h)." >&2
    echo "$hits" >&2
    exit 1
fi

n=$( ( cd "$root" && grep -rln 'vms_dlm_name_hash_proven *(' src/ 2>/dev/null ) | wc -l | tr -d ' ')
echo "OK: no src/ caller of the ungated vms_dlm_name_hash(); ${n} file(s) use"
echo "    vms_dlm_name_hash_proven(), the coverage-gated entry point."
