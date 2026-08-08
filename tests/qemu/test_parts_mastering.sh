#!/bin/bash
# test_parts_mastering.sh - proves the bootable image (distro/Dockerfile.bootable)
# masters PARTS.EXE + PARTS_SETUP.COM into SYS$UPDATE: (bead vms-cde, parent
# vms-2579, docs/release-plan-0.2-to-0.5.md §4/§5).
#
# WHAT THIS PROVES: given an already-built bootable image, the FAT initramfs
# it ships (the one STARTUP.EXE installs onto a blank system disk, and the
# same tree an overlay-mode boot serves directly) contains, under
# vms/SYS0/SYSCOMMON/SYSUPD/ (SYS$UPDATE:):
#
#   1. PARTS_SETUP.COM  - the pre-PCSI install procedure (vms-977/#183).
#   2. PARTS.EXE         - the PARTS kit image itself (vms-e97/f20), built by
#                           the VMS-native toolchain: ET_DYN, EM_X86_64, a
#                           PT_INTERP (IMGACT.EXE activates it), and NO
#                           DT_NEEDED/DT_HASH (LINK.EXE never emits ld/ld.so
#                           metadata - see docs/design-link-native-toolchain.md).
#
# This is an INDEPENDENT re-check of the shipped bytes, not a restatement of
# distro/Dockerfile.bootable's own build-time ground-source gate: that gate
# would already fail the BUILD if mastering were wrong, so a green build is
# already one proof. This test extracts the artifact the build produced
# (docker create + docker cp, no boot needed - fast, no QEMU) and inspects it
# from the outside, the same "recheck the actual shipped bytes" discipline
# the Dockerfile's own DCL.EXE/LOGINOUT.EXE re-check documents.
#
# Deliberately does NOT boot QEMU or drive `@SYS$UPDATE:PARTS_SETUP.COM` end
# to end - that is the next item, vms-5dd. This test only proves the image is
# correctly mastered.
#
# Usage:
#   docker build -f distro/Dockerfile.bootable -t ovmx-boot .
#   tests/qemu/test_parts_mastering.sh [image_tag]
#
# Exit code 0 = pass, 1 = failure, 0 (SKIP) if docker/cpio/readelf are absent.
# Runs on the HOST (or CI runner) - no qemu required.

set -uo pipefail

IMAGE="${1:-ovmx-boot:latest}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for tool in docker cpio readelf gzip; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIP: $tool not available"; exit 0; }
done

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "FATAL: image '$IMAGE' not found - run:"
    echo "  docker build -f distro/Dockerfile.bootable -t $IMAGE ."
    exit 1
fi

PASS=0
FAIL=0
TOTAL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
check() { TOTAL=$((TOTAL + 1)); if [ "$2" -eq 0 ]; then pass "$1"; else fail "$1"; fi; }

echo "=== PARTS mastering: SYS\$UPDATE: contents of the bootable image (vms-cde) ==="
echo "image=$IMAGE"

CID=$(docker create "$IMAGE") || { echo "FATAL: docker create failed for $IMAGE"; exit 1; }
docker cp "$CID:/boot/initramfs-ovmx.cpio.gz" "$WORK/initramfs.cpio.gz" 2>/dev/null
CP_RC=$?
docker rm "$CID" >/dev/null 2>&1

if [ "$CP_RC" -ne 0 ] || [ ! -f "$WORK/initramfs.cpio.gz" ]; then
    echo "FATAL: could not extract /boot/initramfs-ovmx.cpio.gz from $IMAGE"
    exit 1
fi

mkdir -p "$WORK/extract"
( cd "$WORK/extract" && gzip -dc ../initramfs.cpio.gz | cpio -idm --quiet 2>/dev/null )

SYSUPD="$WORK/extract/vms/SYS0/SYSCOMMON/SYSUPD"
PARTS_EXE="$SYSUPD/PARTS.EXE"
SETUP_COM="$SYSUPD/PARTS_SETUP.COM"

echo ""
echo "--- SYS\$UPDATE: (vms/SYS0/SYSCOMMON/SYSUPD/) directory listing ---"
ls -la "$SYSUPD" 2>/dev/null || echo "(directory missing)"
echo ""

test -f "$PARTS_EXE"; check "PARTS.EXE present under SYS\$UPDATE:" $?
test -f "$SETUP_COM"; check "PARTS_SETUP.COM present under SYS\$UPDATE:" $?

if [ -f "$PARTS_EXE" ]; then
    readelf -h "$PARTS_EXE" 2>/dev/null | grep -q "Machine:.*X86-64"
    check "PARTS.EXE is EM_X86_64" $?

    N=$(readelf -d "$PARTS_EXE" 2>/dev/null | grep -c NEEDED || true)
    [ "${N:-1}" -eq 0 ]
    check "PARTS.EXE has zero DT_NEEDED entries (VMS-native, not ld-linked)" $?

    H=$(readelf -d "$PARTS_EXE" 2>/dev/null | grep -c HASH || true)
    [ "${H:-1}" -eq 0 ]
    check "PARTS.EXE has zero DT_HASH entries" $?

    readelf -l "$PARTS_EXE" 2>/dev/null | grep -q INTERP
    check "PARTS.EXE carries PT_INTERP (IMGACT-activated)" $?
fi

if [ -f "$SETUP_COM" ]; then
    grep -q 'COPY SYS\$UPDATE:PARTS.EXE' "$SETUP_COM"
    check "PARTS_SETUP.COM stages the expected COPY of PARTS.EXE" $?

    grep -q 'PARTS :== \$SYS\$SYSTEM:PARTS.EXE' "$SETUP_COM"
    check "PARTS_SETUP.COM defines the PARTS foreign command" $?
fi

echo ""
echo "=========================================="
echo "RESULTS: $PASS/$TOTAL checks passed, $FAIL failed"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo "SOME CHECKS FAILED"
    exit 1
fi
echo "ALL PARTS MASTERING CHECKS PASSED"
exit 0
