#!/bin/bash
# test_no_decnetd_image.sh - rd vms-0a7 S4 retirement gate: DECNETD.EXE is gone.
#
# DECNETD.EXE was every DECnet role behind argv flags. rd vms-0a7 split it into
# VMS images: NETACP.EXE (NCP SET EXECUTOR STATE ON creates it, no command
# line), RTPAD.EXE ($ SET HOST), FAL.EXE (object 17, created by $CREPRC) and
# RMS's own remote access on _NET:. S4 deleted the image, its source and its
# staging. A retired image that can come back through one unreviewed `cp` line
# or image-list entry is not retired, so this asserts the removal on every
# path that builds or stages a system volume, and that the replacements are
# staged on the same paths.
set -u
SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0
fail() { echo "FAIL: $1"; shift; for l in "$@"; do echo "  -> $l"; done; status=1; }

echo "vms-0a7 S4 retirement gate: no DECNETD.EXE in the source, the build or any image list"

for f in src/vmsdecnet/engine/decnetd.c src/vmsdecnet/engine/decnetd_retired.c \
         src/vmsdecnet/engine/decnetd_rms_bind.c; do
    if [ -e "$SRC_ROOT/$f" ]; then fail "$f exists" "the DECnet daemon source is retired"; fi
done

# Every place that builds or stages the x86_64, Alpha or VAX system volume.
STAGERS="CMakeLists.txt src/vmsdecnet/engine/CMakeLists.txt distro/Dockerfile.bootable
tools/cross-alpha/build-alpha-bootimage.sh tools/cross-vax/build-ovmx-images-vax-cmake.sh
tests/lab-vax/run-boot.sh tests/lab-vax/stage_sysvol.sh"
for f in $STAGERS; do
    p="$SRC_ROOT/$f"
    if [ ! -f "$p" ]; then
        fail "$f not found" "this gate cannot verify the staging it exists to check"
        continue
    fi
    if grep -nE '^[^#]*(DECNETD\.EXE|decnetd_exe)' "$p" >/dev/null; then
        fail "$f still builds or stages DECNETD.EXE" \
             "$(grep -nE '^[^#]*(DECNETD\.EXE|decnetd_exe)' "$p" | sed 's/^/       | /')"
    else
        echo "  OK: $f builds/stages no DECNETD.EXE"
    fi
done

# The replacements are on the same paths (a gate that passes because the whole
# network product vanished would be proving nothing).
# (the VAX cmake rung checks only the images it verifies, NETACP and RTPAD.)
for spec in "distro/Dockerfile.bootable:NETACP.EXE RTPAD.EXE FAL.EXE NCP.EXE" \
            "tools/cross-alpha/build-alpha-bootimage.sh:NETACP.EXE RTPAD.EXE FAL.EXE NCP.EXE" \
            "tests/lab-vax/stage_sysvol.sh:NETACP.EXE RTPAD.EXE FAL.EXE NCP.EXE" \
            "tools/cross-vax/build-ovmx-images-vax-cmake.sh:NETACP.EXE RTPAD.EXE"; do
    f="${spec%%:*}"; p="$SRC_ROOT/$f"
    [ -f "$p" ] || continue
    for img in ${spec#*:}; do
        if ! grep -qE "^[^#]*$img" "$p"; then
            fail "$f does not stage $img" "the S1-S3 replacement for DECNETD.EXE must ship"
        fi
    done
done
for t in netacp_exe rtpad_exe fal_exe; do
    grep -qE "^[[:space:]]*$t[[:space:]]+#" "$SRC_ROOT/CMakeLists.txt" ||
        fail "the image aggregate does not list $t" "the replacement image must be built on every arch"
done

# Nothing activates it: no DCL procedure or NCP object entry names the file.
if grep -arnE '^[^!]*DECNETD' "$SRC_ROOT/distro/rootfs" >/dev/null 2>&1; then
    fail "a shipped procedure still names DECNETD" \
         "$(grep -arnE '^[^!]*DECNETD' "$SRC_ROOT/distro/rootfs" | sed 's/^/       | /')"
else
    echo "  OK: no shipped procedure names DECNETD"
fi

[ "$status" -eq 0 ] && echo "PASS: DECNETD.EXE is retired; NETACP/RTPAD/FAL/NCP are staged in its place"
exit "$status"
