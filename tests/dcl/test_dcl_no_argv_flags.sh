#!/bin/bash
# test_dcl_no_argv_flags.sh - DCL.EXE's main() parses no "--" argv flags
# (rd vms-838b).
#
# On VMS, LOGINOUT builds the process and DCL learns from PROCESS STATE that it
# is starting a login, the account's CAPTIVE/DISCTLY flags and its login command
# file. OVMX's DCL once took --login, --captive and --lgicmd <file> on its
# command line instead -- a Unix convention in a shipped VMS image. That state
# now lives in the executive (VMS_IOCTL_LGI): LOGINOUT stores it and DCL reads
# it. This gate fails the build if DCL's main() names any "--" string again, and
# if LOGINOUT hands DCL any.
#
# Self-proving: before checking the tree it runs the same check over a copy of
# main() with "--login" put back and requires the check to FAIL there.
#
# Usage: test_dcl_no_argv_flags.sh [repo-root]
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
DCL_MAIN="$ROOT/src/vmsdcl/dcl_main.c"
LOGINOUT="$ROOT/tools/vms_login.c"
fail=0

# The body of `int main(...)`: from its definition to the first column-0 '}'.
main_body() {
    awk '/^int main\(/ { on = 1 } on { print } on && /^}/ { exit }' "$1"
}

# Exit 1 if the text on stdin has a C string literal starting with "--".
# Comments are stripped first so prose may still name the retired flags.
has_dashdash_literal() {
    sed -e 's://.*$::' | perl -0777 -pe 's{/\*.*?\*/}{}gs' | grep -n '"--' && return 1
    return 0
}

[ -f "$DCL_MAIN" ] || { echo "FAIL: $DCL_MAIN not found"; exit 1; }
[ -f "$LOGINOUT" ] || { echo "FAIL: $LOGINOUT not found"; exit 1; }

body=$(main_body "$DCL_MAIN")
if [ -z "$body" ]; then
    echo "FAIL: could not find int main( in $DCL_MAIN"
    exit 1
fi

# Negative control: the check must catch a flag put back.
mutant=$(printf '%s\n' "$body" | sed '0,/{/s//{\n    if (argc > 1 \&\& strcmp(argv[1], "--login") == 0) login_mode = 1;/')
if printf '%s\n' "$mutant" | has_dashdash_literal >/dev/null; then
    echo "FAIL: negative control: the check did not catch a \"--login\" put back into main()"
    fail=1
else
    echo "ok:   negative control: a \"--login\" in main() is caught"
fi

if printf '%s\n' "$body" | has_dashdash_literal; then
    echo "ok:   DCL main() has no \"--\" argv flag"
else
    echo "FAIL: DCL main() names a \"--\" string (lines above): DCL takes its login state from the executive (vms_kif_lgi), not argv"
    fail=1
fi

# LOGINOUT's exec of DCL passes no flags.
if grep -nE 'exec[lv]p?e?\([^;]*"--(login|captive|lgicmd)' "$LOGINOUT"; then
    echo "FAIL: LOGINOUT passes a \"--\" flag to DCL"
    fail=1
else
    echo "ok:   LOGINOUT passes DCL no \"--\" flag"
fi

# DCL reads the login state from the executive.
if printf '%s\n' "$body" | grep -q 'vms_kif_lgi(VMS_LGI_OP_GET'; then
    echo "ok:   DCL main() reads its login data from the executive (VMS_LGI_OP_GET)"
else
    echo "FAIL: DCL main() does not read its login data from the executive"
    fail=1
fi
if grep -q 'vms_kif_lgi(VMS_LGI_OP_SET' "$LOGINOUT"; then
    echo "ok:   LOGINOUT stores the login data in the executive (VMS_LGI_OP_SET)"
else
    echo "FAIL: LOGINOUT does not store the login data in the executive"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "PASS: test_dcl_no_argv_flags" || echo "FAILED: test_dcl_no_argv_flags"
exit "$fail"
