#!/bin/bash
# overlay-ovmx-drivers.sh - overlay the OVMX kernel modules IN-TREE.
#
# vms-934 (parent vms-19e "owns-kernel"); generalized into a reusable module
# home by vms-bae. Copies the canonical OVMX module source into a from-source
# kernel tree as drivers/ovmx/<mod>/, with in-tree Kbuild + Kconfig scaffolding,
# and wires drivers/ovmx/ into the kernel's drivers/Kconfig + drivers/Makefile.
# The subsequent `make modules` then builds each <mod>.ko as part of OUR kernel
# tree -- modpost stamps them modinfo intree=Y, so loading them at boot does NOT
# set TAINT_OUT_OF_TREE, and CONFIG_MODULE_SIG_ALL / the Dockerfile sign step
# sign them. "In-tree" means OUR tree; no mainline/Linus acceptance is implied.
#
# THE REUSABLE-MODULE-HOME PATTERN (vms-bae)
# ------------------------------------------
# drivers/ovmx/ is a MENU, not a hard-coded pair. Each module is a self-
# describing subdir of distro/kernel/drivers-ovmx/:
#
#     drivers-ovmx/<mod>/
#         Kconfig       -- declares exactly one `config OVMX_<MOD>` tristate
#         Kbuild        -- the in-tree object list (obj-$(CONFIG_OVMX_<MOD>) ...)
#         sources.conf  -- globs (repo-relative) to FLATTEN into the module dir
#
# This script DISCOVERS every such subdir (any dir containing a Kbuild), copies
# its scaffolding, flattens the sources its sources.conf names, and GENERATES
# the drivers/ovmx/Kconfig wrapper + drivers/ovmx/Makefile from what it found.
# Adding a new OVMX kernel module is therefore exactly:
#     (1) a new drivers-ovmx/<mod>/ subdir (Kconfig + Kbuild + sources.conf),
#     (2) a `config OVMX_<MOD>` stanza (its subdir Kconfig), and
#     (3) a CONFIG_OVMX_<MOD>=m flag in distro/kernel/ovmx-<arch>.config.
# Nothing in THIS script, the generated Makefile/Kconfig, or the Dockerfile
# harvest/sign/verify loops needs editing -- the new module inherits intree=Y +
# signing for free. See docs/adding-an-ovmx-kernel-module.md.
#
# This is a build-time OVERLAY, not a fork: src/kernel* stay the single source
# of record and still drive the standalone (out-of-tree) build used by the QEMU
# test harness (tests/qemu/, src/kernel/Dockerfile). The core sources are
# FLATTENED into each module dir because in-tree Kbuild builds objects under the
# module's own directory; nearly all local #include "..." directives are
# basename-only, so a flat dir + -I$(src) resolves every header.
#
# SUBDIR-PRESERVING STAGING (rd vms-4a8): a source that #includes a header by a
# SUBDIR path -- e.g. the genuine ODS-2 codec's #include "vmsfs/ods2.h" -- needs
# that ONE header to keep its path component even in the flat module dir. A
# sources.conf line of the form
#     <glob> -> <subdir>/
# stages the matched files under $OUT/<subdir>/ instead of at $OUT/, so the flat
# dir still resolves the subdir'd include via -I$(src). Lines without '->'
# flatten to basenames exactly as before; the convention is purely additive.
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

echo "== overlay OVMX drivers: $SCAFFOLD -> $DST =="
rm -rf "$DST"
mkdir -p "$DST"

# --- discover modules: every drivers-ovmx/<mod>/ that carries a Kbuild --------
MODS=()
for kb in "$SCAFFOLD"/*/Kbuild; do
    [ -e "$kb" ] || continue
    d=$(dirname "$kb")
    mod=$(basename "$d")
    [ -f "$d/Kconfig" ]     || { echo "FAIL: module '$mod' has no Kconfig";     exit 1; }
    [ -f "$d/sources.conf" ] || { echo "FAIL: module '$mod' has no sources.conf"; exit 1; }
    MODS+=("$mod")
done
[ "${#MODS[@]}" -gt 0 ] || { echo "FAIL: no OVMX modules found under $SCAFFOLD"; exit 1; }
echo "  discovered modules: ${MODS[*]}"

# --- per-module: scaffold + flatten sources -----------------------------------
for mod in "${MODS[@]}"; do
    SRCDIR="$SCAFFOLD/$mod"
    OUT="$DST/$mod"
    mkdir -p "$OUT"
    cp "$SRCDIR/Kbuild"  "$OUT/Kbuild"
    cp "$SRCDIR/Kconfig" "$OUT/Kconfig"

    # Flatten every glob named in sources.conf (repo-relative). Comments/blanks
    # ignored. At least one file must land, or the module source is missing.
    n=0
    while IFS= read -r line; do
        line="${line%%#*}"                       # strip inline comment
        line="$(echo "$line" | xargs 2>/dev/null || true)"  # trim whitespace
        [ -n "$line" ] || continue
        # Optional 'glob -> subdir/' form (rd vms-4a8, additive): stage matched
        # files under $OUT/<subdir>/ so a subdir'd #include keeps its path
        # component. No '->' => flatten to $OUT/ exactly as before.
        glob="$line"
        subdir=""
        if [[ "$line" == *"->"* ]]; then
            glob="$(echo "${line%%->*}" | xargs 2>/dev/null || true)"
            subdir="$(echo "${line##*->}" | xargs 2>/dev/null || true)"
        fi
        destdir="$OUT${subdir:+/$subdir}"
        mkdir -p "$destdir"
        matched=0
        for f in $REPO/$glob; do                 # word-split glob on purpose
            [ -e "$f" ] || continue
            cp "$f" "$destdir/"
            matched=$((matched + 1))
            n=$((n + 1))
        done
        # A sources.conf glob that stages ZERO files is fatal, not a warning: it
        # silently drops objects the module's Kbuild names, so the kernel build
        # later dies on "No rule to make target <obj>.o" far from the cause
        # (rd vms-4a8 -- src/vmsfs was absent from the Dockerfile build context,
        # so the codec globs matched nothing and the WARN shipped the break).
        [ "$matched" -gt 0 ] || {
            echo "FAIL: sources.conf glob '$glob' for module '$mod' matched no files under $REPO"
            echo "      -- source missing from the build context; the Kbuild would"
            echo "      later fail 'No rule to make target' for its objects. Ensure the"
            echo "      Dockerfile/build stage COPYs the tree this glob names."
            exit 1
        }
    done < "$SRCDIR/sources.conf"
    [ "$n" -gt 0 ] || { echo "FAIL: module '$mod' flattened 0 source files"; exit 1; }
    echo "  $mod: staged $n source files + Kbuild + Kconfig"
done

# --- generate drivers/ovmx/Kconfig (the menuconfig OVMX wrapper) --------------
# GENERATED: the menu shell + one `source` per discovered module. Each module's
# own Kconfig declares its config OVMX_<MOD> tristate.
{
    cat <<'EOF'
# SPDX-License-Identifier: GPL-2.0
#
# GENERATED by distro/kernel/overlay-ovmx-drivers.sh (vms-bae) -- do not edit
# in-tree. Edit the per-module drivers-ovmx/<mod>/Kconfig fragments instead.
#
# The in-tree home for OVMX's kernel modules. Module SOURCE is canonical in the
# OVMX repo (src/kernel* + each module's sources.conf); the overlay copies it
# here so `make modules` builds each <mod>.ko as part of OUR kernel tree
# (modinfo intree=Y, no TAINT_OUT_OF_TREE). "In-tree" means OUR tree -- no
# mainline/Linus acceptance is implied or required.

menuconfig OVMX
	bool "OVMX VMS-compatibility drivers"
	default n
	help
	  In-tree home for the OVMX VMS-compatibility kernel modules. Say Y
	  here to expose the individual OVMX drivers below; each is an
	  independent tristate. If unsure, say N.

if OVMX

EOF
    for mod in "${MODS[@]}"; do
        echo "source \"drivers/ovmx/$mod/Kconfig\""
    done
    echo ""
    echo "endif # OVMX"
} > "$DST/Kconfig"

# --- generate drivers/ovmx/Makefile -------------------------------------------
# GENERATED: one `obj-$(CONFIG_OVMX_<MOD>) += <mod>/` per module. The config
# symbol is read from the module's own Kconfig (its single `config OVMX_*`),
# so the Makefile and the Kconfig can never disagree.
{
    cat <<'EOF'
# SPDX-License-Identifier: GPL-2.0
#
# GENERATED by distro/kernel/overlay-ovmx-drivers.sh (vms-bae) -- do not edit.
# One subdir per module; each subdir carries its own Kbuild. See Kconfig.

EOF
    for mod in "${MODS[@]}"; do
        sym=$(awk '/^config OVMX_/ {print $2; exit}' "$SCAFFOLD/$mod/Kconfig")
        [ -n "$sym" ] || { echo "FAIL: module '$mod' Kconfig declares no 'config OVMX_*' symbol" >&2; exit 1; }
        printf 'obj-$(CONFIG_%s)\t+= %s/\n' "$sym" "$mod"
    done
} > "$DST/Makefile"

# --- wire drivers/ovmx into the kernel's drivers/ Kconfig + Makefile ----------
# Kconfig: add a `source "drivers/ovmx/Kconfig"` before the final endmenu.
KCONFIG="$KTREE/drivers/Kconfig"
if ! grep -q 'drivers/ovmx/Kconfig' "$KCONFIG"; then
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

echo "== overlay complete: ${#MODS[@]} module(s) [${MODS[*]}], $(find "$DST" -name '*.c' | wc -l) source files staged in-tree =="
