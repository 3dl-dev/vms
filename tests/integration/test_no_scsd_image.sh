#!/bin/sh
#
# test_no_scsd_image.sh - the RETIREMENT, asserted (FC-P3.9's own
# done-condition: "no scsd binary in the image").
#
# WHAT WAS RETIRED, AND WHY THIS GATE EXISTS. Until FC-P3.9 a booted OVMX node
# ran SCSD.EXE, a userspace daemon that spoke SCS on a raw socket beside the
# executive: SYS$STARTUP:VMS$VMS.DAT registered SYS$STARTUP:SCS_STARTUP.COM at
# the CONFIG phase, that procedure RUN/DETACHED'd the image, and the image was
# staged into the system disk by distro/Dockerfile.bootable. The operator's
# 2026-09-02 clustering reset ruled that stack a non-portable strawman: the
# cluster port, SCS and the connection manager are EXECUTIVE-RESIDENT
# (src/kernel-core/vms_pe.c, vms_scs.c, vms_cnxman.c, in vms.ko and the NetBSD
# vms.kmod), and STARTUP.EXE forms or joins through VMS_IOCTL_CLUSTER_START
# before the system disk is mounted.
#
# A retirement that can be undone by one unreviewed `cp` line, one DAT line or
# one restored directory is not a retirement. This gate is cheap, hermetic
# (reads the tree; boots nothing) and specific: it names each way the daemon
# could come back and refuses each one.
#
# WHAT IT DOES NOT CLAIM. It reads the SOURCES that BUILD the image, not a
# built image -- there is no image to open on a plain host. That is honest and
# sufficient for its purpose: every path by which SCSD.EXE could enter the
# system disk goes through one of the files checked below.
#
# Usage: test_no_scsd_image.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

fail() {
    echo "FAIL: $1"
    shift
    for l in "$@"; do echo "  -> $l"; done
    status=1
}

echo "FC-P3.9 retirement gate: no SCS daemon in the source, the startup database or the image"

# --- 1. the source tree ------------------------------------------------------
if [ -d "$SRC_ROOT/src/vmsscs" ]; then
    fail "src/vmsscs exists" \
         "the userspace SCS stack is retired (operator ruling 2026-09-02);" \
         "the cluster stack lives in src/kernel-core/ and ships inside vms.ko."
else
    echo "  OK: src/vmsscs is gone"
fi

# --- 2. the image staging ----------------------------------------------------
DOCKERFILE="$SRC_ROOT/distro/Dockerfile.bootable"
if [ ! -f "$DOCKERFILE" ]; then
    fail "distro/Dockerfile.bootable not found at $DOCKERFILE" \
         "this gate cannot verify the staging it exists to check"
elif grep -qE '^[^#]*SCSD\.EXE' "$DOCKERFILE"; then
    fail "distro/Dockerfile.bootable still stages SCSD.EXE into the system disk" \
         "$(grep -nE '^[^#]*SCSD\.EXE' "$DOCKERFILE" | sed 's/^/       | /')"
else
    echo "  OK: the bootable image stages no SCSD.EXE"
fi

# --- 3. the STDRV component database ----------------------------------------
VMSDAT="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/VMS\$VMS.DAT"
if [ ! -f "$VMSDAT" ]; then
    fail "VMS\$VMS.DAT not found at $VMSDAT" \
         "this gate cannot verify the component registration it exists to check"
elif grep -qE '^[^!]*SCS_STARTUP\.COM' "$VMSDAT"; then
    fail "VMS\$VMS.DAT still registers a cluster startup component" \
         "a cluster that starts from a DCL phase is a different operating" \
         "system: SYSINIT joins BEFORE the system disk is mounted." \
         "$(grep -nE '^[^!]*SCS_STARTUP\.COM' "$VMSDAT" | sed 's/^/       | /')"
else
    echo "  OK: VMS\$VMS.DAT registers no cluster startup component"
fi

# --- 4. the startup procedure itself -----------------------------------------
SCSCOM="$SRC_ROOT/distro/rootfs/vms/SYS0/SYSCOMMON/SYS\$STARTUP/SCS_STARTUP.COM"
if [ -f "$SCSCOM" ]; then
    fail "SYS\$STARTUP:SCS_STARTUP.COM exists" \
         "there is no daemon for it to start."
else
    echo "  OK: SYS\$STARTUP:SCS_STARTUP.COM is gone"
fi

# --- 5. the build ------------------------------------------------------------
if grep -qE '^[^#]*(add_subdirectory\(src/vmsscs\)|scsd_exe)' "$SRC_ROOT/CMakeLists.txt"; then
    fail "the top-level CMakeLists still builds the SCS daemon" \
         "$(grep -nE '^[^#]*(add_subdirectory\(src/vmsscs\)|scsd_exe)' "$SRC_ROOT/CMakeLists.txt" | sed 's/^/       | /')"
else
    echo "  OK: nothing builds SCSD.EXE"
fi

# --- 6. and the executive really does carry the stack instead ----------------
# The teeth on the other side: asserting an absence alone would pass on a tree
# that had simply deleted clustering. These are the TUs that replace it, on the
# object list that becomes vms.ko.
KMAKE="$SRC_ROOT/src/kernel/Makefile"
missing=""
for tu in vms_pe vms_scs vms_cnxman; do
    grep -qE "$tu\.o" "$KMAKE" || missing="$missing $tu.o"
done
if [ -n "$missing" ]; then
    fail "the executive does not carry the cluster stack that replaces the daemon" \
         "missing from src/kernel/Makefile:$missing" \
         "an absent daemon with no executive-resident replacement is not a" \
         "retirement, it is a removed feature."
else
    echo "  OK: vms.ko carries vms_pe.o, vms_scs.o and vms_cnxman.o"
fi

if [ "$status" -eq 0 ]; then
    echo "FC-P3.9 retirement gate: PASS"
else
    echo "FC-P3.9 retirement gate: FAIL"
fi
exit $status
