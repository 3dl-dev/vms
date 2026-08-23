#!/bin/sh
#
# test_identity_ssot.sh - INV-1 standing gate (rd vms-e652)
#
# INV-1 says ONE module owns system identity and every surface reads it.
# A one-time cleanup does not hold that: the next hardcoded "V7.3" walks
# straight back in. This gate fails the build when a VMS/OVMX version
# literal appears in a string outside the identity SSOT.
#
# Before INV-1 the tree carried FOUR different hardcoded versions at once
# (V7.3 in DCL/login/MONITOR/AUTHORIZE, V1.0 in STARTUP.COM, V0.1 in the
# TCP/IP banner, plus a separate F$GETSYI VERSION constant) -- which is
# precisely the tell INV-1 exists to prevent.
#
# Spec: docs/design-authenticity-roadmap.md sec 4.5 INV-1.
#
# If you are here because this test failed: do not add an allowlist entry.
# Read the version from ovmx_identity.h instead -- that is the whole point.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

# The SSOT itself is the one place a version literal is legal.
SSOT="src/libvms/include/ovmx_identity.h"

# src/vmsscs/** is exempt: version tokens there are OBSERVED CLUSTER WIRE
# DATA (e.g. the software-version field of a captured SCS START packet),
# not OVMX's own identity. They must match the specimen, not our version.
#
# distro/**/STARTUP.COM and SYLOGICALS.COM are scanned as text below.
# (SYLOGICALS.CONF -- the Unix-config-file LARP -- was deleted by vms-21a;
# SYLOGICALS.COM, a real DCL site procedure, is where a banner override now
# lives, so it is the file this gate checks instead.)

echo "INV-1 identity SSOT gate: scanning for hardcoded version literals"

# --- 1. C sources: version literals inside string literals ----------
# Comment lines are excluded: documentation legitimately quotes the old
# values ("this used to answer V7.3"). A real hardcode is on a code line --
#   const char *ver = "V0.1";
# -- which does not start with a comment marker, so it is still caught.
hits=$(grep -rnE '"[^"]*(OpenVMS V|VMS V[0-9]|V[0-9]+\.[0-9]+(-[0-9]+)?)[^"]*"' \
        --include=*.c --include=*.h \
        "$SRC_ROOT/src" "$SRC_ROOT/tools" 2>/dev/null \
        | grep -v "$SSOT" \
        | grep -v "/src/vmsscs/" \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' \
        || true)

if [ -n "$hits" ]; then
    echo "FAIL: version literal(s) outside the identity SSOT ($SSOT):"
    echo "$hits" | sed 's/^/  /'
    echo "  -> read the version from ovmx_identity.h instead."
    status=1
else
    echo "  OK: no hardcoded version literals in src/ or tools/"
fi

# --- 2. Boot-time DCL/config must not bake in a banner version ------
for f in "$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/STARTUP.COM" \
         "$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYLOGICALS.COM" \
         "$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/SYCONFIG.COM"; do
    [ -f "$f" ] || continue
    bad=$(grep -nE '(OpenVMS V|OVMX V)[0-9]' "$f" | grep -v '^[0-9]*:[[:space:]]*#' || true)
    if [ -n "$bad" ]; then
        echo "FAIL: hardcoded version in $(basename "$f"):"
        echo "$bad" | sed 's/^/  /'
        echo "  -> the banner comes from the identity module / SYS\$WELCOME."
        status=1
    else
        echo "  OK: $(basename "$f") carries no hardcoded version"
    fi
done

# --- 3. The login banner must stay logical-driven -------------------
# LOGINOUT and the SSH daemon must resolve SYS$WELCOME, not printf a
# greeting. This is the regression that prompted the gate.
for f in "$SRC_ROOT/tools/vms_login.c" "$SRC_ROOT/src/vmsssh/vmssshd.c"; do
    [ -f "$f" ] || continue
    if grep -qE 'ovmx_banner_welcome' "$f"; then
        echo "  OK: $(basename "$f") resolves SYS\$WELCOME"
    else
        echo "FAIL: $(basename "$f") no longer resolves SYS\$WELCOME"
        echo "  -> the login banner is a boot-defined logical, not a printf."
        status=1
    fi
    if grep -qE 'printf\("[^"]*Welcome to' "$f"; then
        echo "FAIL: $(basename "$f") hardcodes a welcome banner in printf()"
        status=1
    fi
done

# --- 4. SYSKRNL os-release must track the product version -----------
# distro/rootfs/etc/os-release is a hand-maintained static file (it is a
# Linux SYSKRNL-layer surface copied verbatim by distro/Dockerfile.bootable,
# not generated from the SSOT). Its own header requires VERSION_ID be kept
# "in step with OVMX_PRODUCT_VERSION" -- but that by-hand step silently
# lapsed: os-release sat at 0.3 across the whole V0.4/V0.5 line while the
# product shipped V0.5-2 (vms-8328). Because no gate scanned this file, the
# drift was invisible. This check closes that gap: VERSION_ID must equal
# OVMX_PRODUCT_VERSION with the leading "V" stripped.
OSREL="$SRC_ROOT/distro/rootfs/etc/os-release"
if [ -f "$OSREL" ] && [ -f "$SRC_ROOT/$SSOT" ]; then
    prod_ver=$(sed -n 's/^#define[[:space:]]\+OVMX_PRODUCT_VERSION[[:space:]]\+"\([^"]*\)".*/\1/p' \
        "$SRC_ROOT/$SSOT")
    want_id="${prod_ver#V}"          # strip the leading "V": V0.5-2 -> 0.5-2
    got_id=$(sed -n 's/^VERSION_ID="\([^"]*\)".*/\1/p' "$OSREL")
    if [ -z "$prod_ver" ]; then
        echo "FAIL: could not read OVMX_PRODUCT_VERSION from $SSOT"
        status=1
    elif [ "$got_id" = "$want_id" ]; then
        echo "  OK: os-release VERSION_ID ($got_id) tracks OVMX_PRODUCT_VERSION ($prod_ver)"
    else
        echo "FAIL: os-release VERSION_ID drifted from the identity SSOT"
        echo "  os-release VERSION_ID=\"$got_id\"  (from $OSREL)"
        echo "  expected \"$want_id\"  (OVMX_PRODUCT_VERSION=\"$prod_ver\", leading V stripped)"
        echo "  -> bump VERSION/VERSION_ID in distro/rootfs/etc/os-release to match."
        status=1
    fi
fi

if [ "$status" -eq 0 ]; then
    echo "INV-1 identity SSOT gate: PASS"
else
    echo "INV-1 identity SSOT gate: FAIL"
fi
exit "$status"
