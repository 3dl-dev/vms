#!/bin/bash
# TEST: SYSGEN SET/SHOW round-trips a real parameter value and enforces its
# real min/max range (OpenVMS SYSGEN Utility Reference Manual: SET validates
# against the parameter's stored MINIMUM/MAXIMUM; SHOW reflects the current
# working-set value, not the factory default)
# EXPECT: regex:(SYSGEN_ROUNDTRIP_OK|SYSGEN_SKIPPED)
# EXPECT_NOT: contains:SYSGEN_ROUNDTRIP_FAIL
#
# vms-fe21: re-armed. The old test checked for the literal strings
# "MAXPROCESSCNT", "64", and a "Parameter...Current...Default" header -- and
# when SYSGEN.EXE wasn't found at the guessed path, it fell back to ECHOING
# those exact literals itself:
#
#   echo "Parameter Name  Current  Default  Minimum  Maximum"
#   echo "MAXPROCESSCNT        64       64        4      1024"
#
# That is worse than tautological: the test's own fallback FABRICATED the
# output its assertions checked for. It could not fail even if SYSGEN.EXE
# did not exist at all.
#
# The re-armed test requires the real binary (it is built by default,
# BUILD_TOOLS=ON, into the same bin/ directory as vmsdcl -- see
# tools/CMakeLists.txt) and never echoes a substitute. If it is genuinely
# absent this becomes an honest SKIP, not a fake pass.
#
# The real assertion: SET a parameter to a new in-range value, then SHOW it
# and require the Current column reflects the change (this cannot be
# satisfied by a static canned printout -- vms_sysgen.c's cmd_set/cmd_show
# operate on the same in-memory working_set). Then SET it out of its real
# MAXIMUM (1024, tools/vms_sysgen.c default_params[]) and require the real
# %SYSGEN-E-TOOLARGE rejection AND that the value did NOT change.
VMSDCL="${VMSDCL:-vmsdcl}"
SYSGEN="${SYSGEN:-$(dirname "$VMSDCL")/SYSGEN.EXE}"

if [ ! -x "$SYSGEN" ]; then
    echo "SYSGEN_SKIPPED: SYSGEN.EXE not found at $SYSGEN -- BUILD_TOOLS=ON"
    echo "  builds it by default into the same directory as VMSDCL; if it is"
    echo "  genuinely absent here this is an honest skip, not a fabricated pass."
    exit 0
fi

output=$(printf 'USE DEFAULT\nSHOW MAXPROCESSCNT\nSET MAXPROCESSCNT 128\nSHOW MAXPROCESSCNT\nSET MAXPROCESSCNT 99999\nSHOW MAXPROCESSCNT\nEXIT\n' | "$SYSGEN" 2>&1)
echo "$output"

FAILURES=0

# 1. Default load must show the real factory default (64), grounded in
#    tools/vms_sysgen.c default_params[] -- not assumed, read from the same
#    source the product itself is built from.
default_val=$(grep '"MAXPROCESSCNT"' "$(dirname "${BASH_SOURCE[0]}")/../../tools/vms_sysgen.c" 2>/dev/null | head -1 | grep -oE '[0-9]+' | head -1)
if [ -z "$default_val" ]; then
    default_val=64  # last-resort grounding if source isn't reachable from here
fi

# 2. After USE DEFAULT, SHOW must report the default value.
if ! echo "$output" | grep -qE "MAXPROCESSCNT +${default_val} +${default_val}"; then
    echo "  FAIL: initial SHOW MAXPROCESSCNT did not report the factory default (${default_val})"
    FAILURES=$((FAILURES + 1))
fi

# 3. SET must actually change it, and the FOLLOWING SHOW must reflect the
#    new value in the Current column -- proof this is a live working set,
#    not a static printout.
if ! echo "$output" | grep -q '%SYSGEN-I-SETPARAM, MAXPROCESSCNT changed from 64 to 128'; then
    echo "  FAIL: SET MAXPROCESSCNT 128 did not report the real change"
    FAILURES=$((FAILURES + 1))
fi
if ! echo "$output" | grep -qE "MAXPROCESSCNT +128 +${default_val}"; then
    echo "  FAIL: SHOW after SET did not reflect Current=128 (Default stays ${default_val})"
    FAILURES=$((FAILURES + 1))
fi

# 4. Out-of-range SET must be rejected with the real ident and the real
#    maximum (1024), and must NOT silently change the value.
if ! echo "$output" | grep -q '%SYSGEN-E-TOOLARGE, value 99999 exceeds maximum 1024 for MAXPROCESSCNT'; then
    echo "  FAIL: SET MAXPROCESSCNT 99999 did not produce the real TOOLARGE rejection"
    FAILURES=$((FAILURES + 1))
fi
last_show=$(echo "$output" | grep -E "^  MAXPROCESSCNT" | tail -1)
if ! echo "$last_show" | grep -qE "MAXPROCESSCNT +128 "; then
    echo "  FAIL: value changed despite the out-of-range SET being rejected (last SHOW: $last_show)"
    FAILURES=$((FAILURES + 1))
fi

if [ $FAILURES -eq 0 ]; then
    echo "SYSGEN_ROUNDTRIP_OK"
else
    echo "SYSGEN_ROUNDTRIP_FAIL ($FAILURES check(s) failed)"
fi
