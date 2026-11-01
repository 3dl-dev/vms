#!/bin/sh
# rd vms-83d / vms-4029 / vms-97c2: the boot staging directory is a tmpfs, i.e.
# RAM, on a 16 MB VAX. Staging every SYS$SYSTEM utility there at boot took ~40%
# of the free memory and starved the node (ENOSPC, a get_pventry panic, a console
# that never offers Username:). This test pins the footprint rule at the source:
#   1. stage_boot_images() stages ONLY the utilities boot and NETACP need;
#   2. every other utility is staged on first use by dcl_exec_utility().
# It runs the check on the real tree (must pass) and on a mutated copy (must
# fail), so it is a control and not a tautology.
set -u
SRC="${1:?usage: test_stage_footprint.sh <repo-root>}"
INIT="$SRC/src/ovmx_init/ovmx_init.c"
MISC="$SRC/src/vmsdcl/dcl_cmd_misc.c"
HEAVY="MAIL.EXE MONITOR.EXE INITIALIZE.EXE PRODUCT.EXE LIBRARIAN.EXE HELP.EXE TCC.EXE LINK.EXE"

check() {   # check <init.c> <dcl_cmd_misc.c>
    # the utils[] initialiser, comments stripped
    body=$(awk '/static const char \*const utils\[\] = \{/{f=1;next} f&&/^    \};/{exit} f' "$1" |
           sed -e 's|/\*.*\*/||g' | awk '/\/\*/{c=1} !c{print} /\*\//{c=0}')
    for h in $HEAVY; do
        if printf '%s\n' "$body" | grep -q "\"$h\""; then
            echo "FAIL: $h is staged eagerly at boot (RAM on a 16 MB VAX)"; return 1
        fi
    done
    grep -q 'dcl_resolve_activatable(ctx, vms_spec' "$2" || {
        echo "FAIL: dcl_exec_utility has no lazy-staging fallback"; return 1; }
    return 0
}

check "$INIT" "$MISC" || exit 1
echo "ok: heavy utilities are not staged at boot; dcl_exec_utility stages on first use"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
sed 's|"INSTALL.EXE", "SYSGEN.EXE",|"INSTALL.EXE", "TCC.EXE", "SYSGEN.EXE",|' "$INIT" > "$T/init.c"
if check "$T/init.c" "$MISC" >/dev/null; then
    echo "FAIL: negative control did not fire (an eager TCC.EXE was accepted)"; exit 1
fi
echo "ok: negative control fires (an eager TCC.EXE is refused)"
