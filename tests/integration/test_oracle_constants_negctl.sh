#!/bin/sh
# Proves test_oracle_constants.sh can go red: a tree constant changed to a wrong value,
# and a known-mismatch entry that no longer mismatches, are both rejected.
set -u
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
rc=0
mk() {
    rm -rf "$TMP/t"; mkdir -p "$TMP/t/docs/oracle" "$TMP/t/src"
    cp -r "$SRC/docs/oracle/vax73-starlet-defs" "$SRC/docs/oracle/alpha84-starlet-defs" "$TMP/t/docs/oracle/"
    cp "$SRC/docs/oracle/constants-known-mismatch.txt" "$TMP/t/docs/oracle/"
    mkdir -p "$TMP/t/src/libvms" "$TMP/t/src/vmsrms/include" "$TMP/t/src/vmsprocess/include"
    cp -r "$SRC/src/libvms/include" "$TMP/t/src/libvms/"
    cp -r "$SRC/src/vmsrms/include/rms" "$TMP/t/src/vmsrms/include/"
    cp -r "$SRC/src/vmsprocess/include/vms" "$TMP/t/src/vmsprocess/include/"
}
gate() { python3 "$SRC/tools/compat/check_oracle_constants.py" --root="$TMP/t" >/dev/null 2>&1; }
mk; gate || { echo "NEGCTL FAIL: the unmutated copy is rejected -- the control proves nothing"; exit 1; }
red() { if gate; then echo "NEGCTL FAIL: $1 was ACCEPTED"; rc=1; else echo "ok: $1 is rejected"; fi; }
mk; sed -i 's/^\(#define SS\$_NORMAL[[:space:]]*\)1\b/\1 3/' "$TMP/t/src/libvms/include/ssdef.h"
red "a wrong value for SS\$_NORMAL"
mk; echo 'SS$_NORMAL 3 1' >> "$TMP/t/docs/oracle/constants-known-mismatch.txt"
red "a known-mismatch entry that already matches"
[ "$rc" -eq 0 ] && echo "PASS: the oracle-constants gate rejects new wrong values and stale list entries"
exit $rc
