#!/bin/sh
#
# test_frozen_identity_tokens.sh - dual-identity rebrand tripwire
# (rd vms-700 / vms-296 / vms-3de)
#
# The OpenVMX product rebrand (ovmx_identity.h: OVMX_PRODUCT_NAME "OVMX" ->
# "OpenVMX") only touches the HUMAN-facing brand. A wide set of "OVMX"
# tokens are load-bearing as NON-BRAND identifiers -- VMS facility/status
# codes, the IMGACT ELF-note owner, the SCS/cluster wire OS-name field,
# nodename fallbacks, the kit/product-db vendor token -- and a naive
# find/replace across the tree would silently corrupt every one of them
# (wire incompatibility, status codes callers pattern-match, binary
# identity). This gate pins the specific byte values a blind rename is
# most likely to catch, so a future rebrand trips HERE instead of on the
# wire, on disk, or in a status code a caller pattern-matches.
#
# Spec: docs/design-authenticity-roadmap.md sec 4.5 INV-1; CLAUDE.md
# Project-Specific Rule 8 (clean-room wire fidelity).
#
# If you are here because this test failed and you meant to rebrand a
# non-brand token: don't. Read ovmx_identity.h's "DUAL-IDENTITY REBRAND"
# comment first -- these tokens are deliberately frozen.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

check_grep() {
    desc="$1"; file="$2"; pattern="$3"
    if [ ! -f "$file" ]; then
        echo "FAIL: $desc -- file not found: $file"
        status=1
        return
    fi
    if grep -qE "$pattern" "$file"; then
        echo "  OK: $desc"
    else
        echo "FAIL: $desc -- pattern not found in $file"
        echo "  looked for: $pattern"
        status=1
    fi
}

echo "Frozen identity tokens gate: dual-identity rebrand tripwire"

# 1. OVMX$_ facility name -- src/libvms/status.c's known_codes table renders
#    every OVMX$_* condition under the literal facility "OVMX", never a
#    rebranded token (a caller pattern-matching "%OVMX-" on the formatted
#    message would break silently otherwise).
check_grep "OVMX\$_ facility name literal (status.c known_codes)" \
    "$SRC_ROOT/src/libvms/status.c" \
    '"OVMX",[[:space:]]*"PRCLOST"'

# 2. dcl_error("OVMX", ...) call sites -- DCL's own MOUNT/DISMOUNT/SET
#    facility-error paths wear the same bare facility name.
check_grep 'dcl_error("OVMX", ...) in dcl_cmd_set.c' \
    "$SRC_ROOT/src/vmsdcl/dcl_cmd_set.c" \
    'dcl_error\("OVMX"'
check_grep 'dcl_error("OVMX", ...) in dcl_cmd_misc.c' \
    "$SRC_ROOT/src/vmsdcl/dcl_cmd_misc.c" \
    'dcl_error\("OVMX"'

# 3. IMGACT ELF-note owner -- the freestanding image activator's PT_NOTE
#    owner string, read by every activated image.
check_grep "IMGACT_NOTE_OWNER" \
    "$SRC_ROOT/src/libvms/include/imgact_activate.h" \
    '#define[[:space:]]+IMGACT_NOTE_OWNER[[:space:]]+"OVMX"'

# 4. SCS/cluster wire OS-name field -- the self-description scsd.c inserts
#    into its own SB (System Block) for cluster peers to read.
check_grep "SCS wire self os_name" \
    "$SRC_ROOT/src/vmsscs/scsd.c" \
    'self_info\.os_name = "OVMX";'

# 5. Kit/product-db vendor/producer token -- OVMX_VENDOR_TOKEN, deliberately
#    NOT OVMX_PRODUCT_NAME (which now carries the "OpenVMX" human brand).
check_grep "OVMX_VENDOR_TOKEN stays the pre-rebrand vendor token" \
    "$SRC_ROOT/src/libvms/include/ovmx_identity.h" \
    '#define[[:space:]]+OVMX_VENDOR_TOKEN[[:space:]]+"OVMX"'
check_grep "ovmx_kit_pack uses OVMX_VENDOR_TOKEN, not OVMX_PRODUCT_NAME" \
    "$SRC_ROOT/tools/ovmx_kit_pack.c" \
    'OVMX_VENDOR_TOKEN'

# 6. Nodename fallback -- the identity SSOT's compiled-in SCSNODE default.
check_grep "OVMX_DEFAULT_NODENAME stays OVMX" \
    "$SRC_ROOT/src/libvms/include/ovmx_identity.h" \
    '#define[[:space:]]+OVMX_DEFAULT_NODENAME[[:space:]]+"OVMX"'

# 7. INV-0 compat badge -- unaffected by the product-name rebrand.
check_grep "OVMX_COMPAT_BADGE stays OpenVMS-compatible" \
    "$SRC_ROOT/src/libvms/include/ovmx_identity.h" \
    '#define[[:space:]]+OVMX_COMPAT_BADGE[[:space:]]+"OpenVMS-compatible"'

# 8. The rebrand itself: the human product name must actually have moved to
#    "OpenVMX", or this whole gate is checking the wrong world.
check_grep "OVMX_PRODUCT_NAME is the rebranded OpenVMX product name" \
    "$SRC_ROOT/src/libvms/include/ovmx_identity.h" \
    '#define[[:space:]]+OVMX_PRODUCT_NAME[[:space:]]+"OpenVMX"'

if [ "$status" -eq 0 ]; then
    echo "Frozen identity tokens gate: PASS"
else
    echo "Frozen identity tokens gate: FAIL"
fi
exit "$status"
