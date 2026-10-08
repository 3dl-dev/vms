#!/bin/sh
#
# test_semantic_oracle_static.sh - the buildless half of the semantic oracle (rd vms-8d1).
# The OVMX-vs-real-VMS transcript diff needs a booted runtime and runs in CI's
# persistent-boot job (tests/qemu/test_semantic_oracle.sh + semantic_diff.py).
# This per-PR half proves the instrument itself:
#   1. semantic_diff.py --selftest  -- the ratchet goes red on an unlisted
#      difference, a stale known-diff line, a missing case, an orphan line;
#   2. semantic_diff.py --goldens   -- every golden is well formed, carries a
#      provenance header, and was captured from the CURRENT spec (a spec edited
#      without re-capturing on the real node fails here);
#   3. every spec generates MACRO-32 and C, and the C compiles against the
#      OVMX headers (the OVMX probe build in distro/Dockerfile.bootable).
#   4. known-diff.txt names only cases that exist (via --goldens + selftest).
set -eu
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
SEM="$SRC/tools/oracle/semantic"
python3 "$SEM/semantic_diff.py" --selftest
python3 "$SEM/semantic_diff.py" --goldens
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CC=${CC:-cc}
for spec in "$SEM"/specs/*.py; do
    fam=$(basename "$spec" .py)
    python3 "$SEM/spgen.py" --mar "$spec" > "$TMP/$fam.mar"
    grep -q '^        .END    START$' "$TMP/$fam.mar"
    python3 "$SEM/spgen.py" --c "$spec" > "$TMP/$fam.c"
    "$CC" -std=gnu99 -fsyntax-only -I"$SRC/src/libvms/include" -I"$SRC/src/vmsrms/include" "$TMP/$fam.c"
    n=$(python3 "$SEM/spgen.py" --list "$spec" | wc -l)
    [ -d "$SRC/docs/oracle/semantics/$fam" ] || { echo "FAIL: spec $fam has no goldens in docs/oracle/semantics/$fam"; exit 1; }
    echo "OK: $fam ($n cases) generates MACRO-32 + C; C compiles"
done
