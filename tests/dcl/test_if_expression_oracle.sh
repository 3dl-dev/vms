#!/bin/bash
# TEST: IF evaluates expression operands (+ - * / bind tighter than the comparisons) exactly as VAX V7.3 did (rd vms-c17a)
# EXPECT: contains:IFX_ORACLE_MATCH
# EXPECT_NOT: contains:IFX_ORACLE_DIFF
#
# The SAME procedure the real VAX ran (tools/oracle/semantic/comgen.py over
# specs-dcl/ifx.py) is run by OVMX's DCL here, and its transcript must equal
# the VAX golden docs/oracle/semantics/ifx/vax73.txt line for line. The booted
# semantic-oracle gate (tests/qemu/test_semantic_oracle.sh) checks the same
# family on the real executive; this is the fast host-side twin.
VMSDCL="${VMSDCL:-vmsdcl}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GOLD="$ROOT/docs/oracle/semantics/ifx/vax73.txt"
got=$(python3 "$ROOT/tools/oracle/semantic/comgen.py" "$ROOT/tools/oracle/semantic/specs-dcl/ifx.py" |
      grep -v '^\$!' | sed 's/^\$ //' | "$VMSDCL" 2>&1 | tr -d '\r' |
      awk '/^=== SEMPROBE ifx BEGIN ===$/{on=1} on{print} /^=== SEMPROBE ifx END ===$/{exit}')
want=$(grep -v '^#' "$GOLD")
if [ -n "$want" ] && [ "$got" = "$want" ]; then
    echo IFX_ORACLE_MATCH
else
    echo IFX_ORACLE_DIFF
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got")
fi
