#!/bin/bash
# TEST: CONVERT does not claim a conversion it did not perform (vms-558e).
# A plain CONVERT is a record-for-record copy and is silent on success, as on
# VMS; /FDL (a real record-format conversion) is rejected as an unrecognized qualifier, never "accepted but
# ignored", and writes nothing.
# EXPECT: contains:CVT_PLAIN_OK
# EXPECT: contains:CVT_FDL_REFUSED
# EXPECT: contains:CVT_FDL_NO_OUTPUT
# EXPECT_NOT: contains:CONVERTED
# EXPECT_NOT: contains:accepted but ignored
# EXPECT_NOT: contains:CVT_FAIL
VMSDCL="${VMSDCL:-vmsdcl}"
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
cd "$d" || exit 1
printf 'alpha\nbeta\ngamma\n' > in.txt

out=$(printf 'DEFINE CVTDIR "%s"\nSET DEFAULT CVTDIR:[000000]\nCONVERT in.txt out.txt\n' "$d" | $VMSDCL 2>&1)
echo "$out"
if cmp -s in.txt out.txt; then echo CVT_PLAIN_OK; else echo "CVT_FAIL plain copy differs"; fi

out2=$(printf 'DEFINE CVTDIR "%s"\nSET DEFAULT CVTDIR:[000000]\nCONVERT/FDL=x.fdl in.txt fdl_out.txt\n' "$d" | $VMSDCL 2>&1)
echo "$out2"
if echo "$out2" | grep -q 'DCL-W-IVQUAL'; then echo CVT_FDL_REFUSED; else echo "CVT_FAIL fdl not refused"; fi
if [ ! -e fdl_out.txt ]; then echo CVT_FDL_NO_OUTPUT; else echo "CVT_FAIL fdl wrote an output"; fi
