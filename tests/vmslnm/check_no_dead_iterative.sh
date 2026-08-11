#!/bin/bash
# vms-240 guard: the dead, filespec-blind lnm_translate_iterative() is GONE.
#
# It was replaced by lnm_translate_filespec(). This guard fails if any C source
# or header (src/ or tests/) still declares, defines, or calls it -- i.e. if the
# dead path was resurrected or a reference was left behind.
#
# The ONE intentional surviving mention is the RETIRED symbol-vector slot in
# src/vmslink/mk_vmslnm_shr.sh (lnm_translate_iterative=PRIVATE_PROCEDURE), kept
# in place to preserve the append-only GSMATCH index contract. That is a .sh
# build recipe, not C, so it is excluded here by file extension.
set -u

# Locate the repo root (this script lives in tests/vmslnm/).
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

hits="$(grep -rn --include='*.c' --include='*.h' \
        'lnm_translate_iterative' "$ROOT/src" "$ROOT/tests" 2>/dev/null || true)"

if [ -n "$hits" ]; then
    echo "FAIL: dead lnm_translate_iterative is still referenced in C sources:"
    echo "$hits"
    exit 1
fi

echo "PASS: no C-level references to the removed lnm_translate_iterative"
exit 0
