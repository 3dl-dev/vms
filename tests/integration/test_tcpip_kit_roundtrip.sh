#!/bin/bash
# TEST (rd vms-f394): the TCP/IP Services layered product packs into a DISTINCT,
# self-consistent kit (ovmx-tcpip.kit) that CARRIES the TCP/IP components and
# round-trips BYTE-EXACT -- the same pack->list->extract->diff proof the base-OS
# ovmx-os.kit gets (distro/Dockerfile.bootable), applied to the layered product.
#
# Proves the kit ARTIFACT + the tools/build-tcpip-kit.sh build (the PRODUCT half of
# vms-f394). The release-stream wiring (a first-class deliverable line) and
# PRODUCT-INSTALL onto a booted system are a separate rung + the layered-product-vs-
# appliance posture gate -- NOT asserted here.
#
# Arg 1 = the build bin dir (ovmx_kit_pack + the TCP/IP binaries); defaults to build/bin.
set -u
BIN="${1:-build/bin}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
ROOTFS="$REPO/distro/rootfs"
PACK="$BIN/ovmx_kit_pack"
FAIL=0
ok(){  echo "  PASS: $*"; }
bad(){ echo "  FAIL: $*"; FAIL=1; }

echo "=== test_tcpip_kit_roundtrip (TCP/IP Services distinct layered-product kit, rd vms-f394) ==="

# Skip HONESTLY (never a silent no-op pass) if the factory tool / components are not
# in this build configuration (e.g. BUILD_TOOLS=OFF). CI's Build&Test runs BUILD_TOOLS=ON,
# so this gate genuinely runs there.
[ -x "$PACK" ] || { echo "SKIP: ovmx_kit_pack not built at $PACK (BUILD_TOOLS off?)"; exit 77; }
for b in 'TCPIP$INETD.EXE' 'TCPIP$DAYTIME.EXE' 'TCPIP$CONFIG.EXE'; do
    [ -f "$BIN/$b" ] || { echo "SKIP: TCP/IP component $b not built at $BIN (BUILD_TOOLS off?)"; exit 77; }
done

KIT="$(mktemp -u "${TMPDIR:-/tmp}/ovmx-tcpip.XXXXXX.kit")"
"$REPO/tools/build-tcpip-kit.sh" "$KIT" "$BIN" "$ROOTFS" >/tmp/tcpip-kit-build.log 2>&1 \
    || { echo "FAIL: tools/build-tcpip-kit.sh failed"; cat /tmp/tcpip-kit-build.log; exit 1; }
trap 'rm -f "$KIT"' EXIT

# (1) Manifest names every TCP/IP Services component (the kit is COMPLETE).
LISTING="$("$PACK" list "$KIT" 2>&1)"
for name in 'TCPIP$INETD.EXE' 'TCPIP$DAYTIME.EXE' 'TCPIP$CONFIG.EXE' \
            'TCPIP$SERVICE.DAT' 'TCPIP$CONFIG.COM' 'TCPIP$STARTUP.COM'; do
    if printf '%s\n' "$LISTING" | grep -qF "$name"; then ok "kit manifest names $name"
    else bad "kit manifest MISSING $name"; fi
done

# (2) The kit carries the BYTES, not a manifest of promises: extract + cmp each
# component against its ORIGINAL source (path-format-agnostic via find-by-basename).
OUT="$(mktemp -d "${TMPDIR:-/tmp}/ovmx-tcpip-verify.XXXXXX")"
trap 'rm -f "$KIT"; rm -rf "$OUT"' EXIT
if ! "$PACK" extract "$KIT" "$OUT" >/dev/null 2>&1; then bad "kit extract failed"; fi
for src in "$BIN/TCPIP\$INETD.EXE" "$BIN/TCPIP\$DAYTIME.EXE" "$BIN/TCPIP\$CONFIG.EXE" \
           "$ROOTFS/vms/SYS0/SYSCOMMON/SYSEXE/TCPIP\$SERVICE.DAT" \
           "$ROOTFS/vms/SYS0/SYSCOMMON/SYSMGR/TCPIP\$CONFIG.COM" \
           "$ROOTFS/vms/SYS0/SYSCOMMON/SYS\$STARTUP/TCPIP\$STARTUP.COM"; do
    base="$(basename "$src")"
    found="$(find "$OUT" -type f -name "$base" 2>/dev/null | head -1)"
    if [ -n "$found" ] && cmp -s "$src" "$found"; then ok "round-trip byte-exact: $base"
    else bad "round-trip MISMATCH/MISSING: $base"; fi
done

echo "=== test_tcpip_kit_roundtrip: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL) ==="
exit "$FAIL"
