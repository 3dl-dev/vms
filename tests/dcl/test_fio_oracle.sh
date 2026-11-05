#!/bin/bash
# TEST: DCL file I/O (OPEN, READ, WRITE, CLOSE) behaves as VAX V7.3 did
# EXPECT: contains:FIO_ORACLE_MATCH
# EXPECT_NOT: contains:FIO_ORACLE_DIFF
#
# The SAME procedure the real VAX ran (tools/oracle/semantic/comgen.py over
# specs-dcl/fio.py) is run by OVMX's DCL here, and its transcript must equal the
# VAX golden docs/oracle/semantics/fio/vax73.txt line for line -- except a case
# listed in docs/oracle/semantics/known-diff.d/fio-* (the semantic-oracle
# ratchet), which must still DIFFER: a listed case that now matches fails too,
# so the list only shrinks. The booted semantic-oracle gate
# (tests/qemu/test_semantic_oracle.sh) checks the same family on the real
# executive; this is the fast host-side twin.
set -u
VMSDCL="${VMSDCL:-vmsdcl}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GOLD="$ROOT/docs/oracle/semantics/fio/vax73.txt"
TDIR="dcl_semfio_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
python3 "$ROOT/tools/oracle/semantic/comgen.py" "$ROOT/tools/oracle/semantic/specs-dcl/fio.py" \
    > "/vms/$TDIR/sp_fio.com"
got=$({ echo "SET DEFAULT SYS\$SYSDEVICE:[$VDIR]"; echo "@SP_FIO.COM"; } | "$VMSDCL" 2>&1 |
      tr -d '\r' | awk '/^=== SEMPROBE fio BEGIN ===$/{on=1} on{print} /^=== SEMPROBE fio END ===$/{exit}')
rm -rf "/vms/$TDIR"
want=$(grep -v '^#' "$GOLD")
known=$(cat "$ROOT"/docs/oracle/semantics/known-diff.d/fio-* 2>/dev/null | grep -v '^#' | awk '$1=="fio"{print $2}')
bad=0
ncase=$(printf '%s\n' "$want" | grep -c '^FIO\.')
[ "$ncase" -gt 0 ] || { echo "no golden cases"; bad=1; }
while IFS= read -r w; do
    id=${w%% *}
    g=$(printf '%s\n' "$got" | grep -F -x -- "$w" || true)
    if printf '%s\n' "$known" | grep -qxF -- "$id"; then
        if [ -n "$g" ]; then echo "STALE known-diff (now matches): $id"; bad=1; fi
        printf '%s\n' "$got" | grep -q "^$id " || { echo "MISSING: $id"; bad=1; }
    elif [ -z "$g" ]; then
        echo "DIFF: want [$w] got [$(printf '%s\n' "$got" | grep "^$id " || echo '<none>')]"; bad=1
    fi
done <<< "$want"
if [ "$bad" = 0 ]; then echo FIO_ORACLE_MATCH; else echo FIO_ORACLE_DIFF; fi
