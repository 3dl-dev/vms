#!/bin/bash
# overlay-ovmx-drivers.sh - overlay the OVMX VMS kernel modules IN-TREE.
#
# vms-934 (parent vms-19e "owns-kernel"). Copies the canonical OVMX module
# source (src/kernel/ + src/kernel-core/) into a from-source kernel tree as
# drivers/ovmx/{vms,vmsfs}/, with the in-tree Kbuild + Kconfig scaffolding, and
# wires drivers/ovmx/ into the kernel's drivers/Kconfig + drivers/Makefile. The
# subsequent `make modules` then builds vms.ko / vmsfs.ko as part of OUR kernel
# tree -- modpost stamps them modinfo intree=Y, so loading them at boot does NOT
# set TAINT_OUT_OF_TREE.
#
# This is a build-time OVERLAY, not a fork: src/kernel/ + src/kernel-core/ stay
# the single source of record and still drive the standalone (out-of-tree) build
# used by the QEMU test harness (tests/qemu/, src/kernel/Dockerfile). "In-tree"
# means OUR tree; no mainline/Linus acceptance is implied.
#
# The core sources are FLATTENED into each module dir (the executive core and
# the ODS-2 core .c files sit beside the Linux glue), because in-tree Kbuild
# builds objects under the module's own directory. All local #include "..."
# directives in the sources are basename-only, so a flat dir + -I$(src)
# resolves every header.
#
# Usage: overlay-ovmx-drivers.sh <kernel-source-tree> <ovmx-repo-root>
#
# Idempotent: safe to re-run against an already-overlaid tree.

set -euo pipefail

KTREE="${1:?usage: overlay-ovmx-drivers.sh <kernel-source-tree> <ovmx-repo-root>}"
REPO="${2:?usage: overlay-ovmx-drivers.sh <kernel-source-tree> <ovmx-repo-root>}"

[ -f "$KTREE/Makefile" ] && [ -d "$KTREE/drivers" ] || {
    echo "FAIL: '$KTREE' is not a kernel source tree (no ./Makefile or ./drivers)"; exit 1; }
[ -d "$REPO/src/kernel" ] && [ -d "$REPO/src/kernel-core" ] || {
    echo "FAIL: '$REPO' is not the OVMX repo (no src/kernel or src/kernel-core)"; exit 1; }

SCAFFOLD="$REPO/distro/kernel/drivers-ovmx"
DST="$KTREE/drivers/ovmx"

echo "== overlay OVMX drivers: $REPO/src/{kernel,kernel-core} -> $DST =="
rm -rf "$DST"
mkdir -p "$DST/vms" "$DST/vmsfs"

# --- in-tree scaffolding (Kconfig / Makefile / per-module Kbuild) ----------
cp "$SCAFFOLD/Kconfig"        "$DST/Kconfig"
cp "$SCAFFOLD/Makefile"       "$DST/Makefile"
cp "$SCAFFOLD/vms/Kbuild"     "$DST/vms/Kbuild"
cp "$SCAFFOLD/vmsfs/Kbuild"   "$DST/vmsfs/Kbuild"

# --- vms.ko sources: Linux glue (src/kernel) + executive core --------------
# src/kernel/*.c        : vms_module.c, vms_p0.c, vms_p1.c (Linux glue)
# src/kernel/*.h        : vms_internal.h, vms_ioctl.h, vms_lnm.h, vms_mbx.h,
#                         exec_*_linux.h (Linux backend shims + shared struct hdr)
# src/kernel-core/*.c   : vms_access/ast/devtab/eflag/lnm/lock/mbx/proctab.c
# src/kernel-core/*.h   : exec_hash/kbackend/list/rbtree.h (shim contracts)
cp "$REPO"/src/kernel/*.c        "$DST/vms/"
cp "$REPO"/src/kernel/*.h        "$DST/vms/"
cp "$REPO"/src/kernel-core/*.c   "$DST/vms/"
cp "$REPO"/src/kernel-core/*.h   "$DST/vms/"

# --- vmsfs.ko sources: thin VFS/bio backend + ODS-2 core -------------------
cp "$REPO"/src/kernel/vmsfs/*.c       "$DST/vmsfs/"
cp "$REPO"/src/kernel/vmsfs/*.h       "$DST/vmsfs/"
cp "$REPO"/src/kernel-core/vmsfs/*.c  "$DST/vmsfs/"
cp "$REPO"/src/kernel-core/vmsfs/*.h  "$DST/vmsfs/"

# --- wire drivers/ovmx into the kernel's drivers/ Kconfig + Makefile -------
# Kconfig: add a `source "drivers/ovmx/Kconfig"` before the final endmenu.
KCONFIG="$KTREE/drivers/Kconfig"
if ! grep -q 'drivers/ovmx/Kconfig' "$KCONFIG"; then
    # Insert before the LAST endmenu line (the "Device Drivers" menu close).
    last_endmenu=$(grep -n '^endmenu' "$KCONFIG" | tail -1 | cut -d: -f1)
    [ -n "$last_endmenu" ] || { echo "FAIL: no endmenu in $KCONFIG"; exit 1; }
    sed -i "${last_endmenu}i source \"drivers/ovmx/Kconfig\"" "$KCONFIG"
    echo "  wired drivers/ovmx/Kconfig into $KCONFIG (before line $last_endmenu)"
else
    echo "  drivers/ovmx/Kconfig already sourced in $KCONFIG"
fi

# Makefile: descend into drivers/ovmx/ when CONFIG_OVMX is set.
KMAKE="$KTREE/drivers/Makefile"
if ! grep -q 'obj-\$(CONFIG_OVMX)' "$KMAKE"; then
    printf 'obj-$(CONFIG_OVMX)\t\t+= ovmx/\n' >> "$KMAKE"
    echo "  wired obj-\$(CONFIG_OVMX) += ovmx/ into $KMAKE"
else
    echo "  obj-\$(CONFIG_OVMX) already present in $KMAKE"
fi

echo "== overlay complete: $(ls "$DST"/vms/*.c "$DST"/vmsfs/*.c | wc -l) source files staged in-tree =="
