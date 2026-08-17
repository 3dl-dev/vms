#!/bin/bash
# test_ovmx_module_home.sh - standing gate for the OVMX reusable kernel-module
#                            home (rd vms-bae, epic vms-19e "owns-kernel").
#
# WHAT THIS PROVES
# ----------------
# drivers/ovmx/ is a MENU, not a hard-coded {vms,vmsfs} pair: adding an OVMX
# kernel module is exactly {a new drivers-ovmx/<mod>/ subdir + a Kconfig stanza
# + a CONFIG_OVMX_<MOD>=m flag}, and the new module then inherits the in-tree
# build (intree=Y) + auto-signing WITHOUT any edit to the overlay script, the
# generated Kconfig/Makefile, or the Dockerfile harvest/sign loops.
#
# This gate runs the REAL overlay-ovmx-drivers.sh against a fabricated kernel
# tree, TWICE: once with the two real modules, and once with a hypothetical
# third module `ovmxdemo` dropped in as {subdir + Kconfig + Kbuild + sources.
# conf} only. It asserts the demonstrator is discovered and fully wired (Kconfig
# sourced, Makefile obj- line, sources flattened) with the overlay script byte-
# for-byte unchanged -- i.e. the pattern is generic, not two special cases.
#
# It is a hermetic source/wiring proof: NO kernel build, NO Docker, NO QEMU
# (the real intree=Y + signed proof on the shipped modules is the boot-time
# taint gate tests/qemu/test_kernel_taint.sh). Fast enough for the Debug ctest
# lane.
#
# Usage: test_ovmx_module_home.sh [SRC_ROOT]     (defaults to repo root)
# Exit 0 = pass, 1 = fail.

set -uo pipefail

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
OVERLAY="$SRC_ROOT/distro/kernel/overlay-ovmx-drivers.sh"
SCAFFOLD="$SRC_ROOT/distro/kernel/drivers-ovmx"

PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
chk()  { if [ "$2" -eq 0 ]; then ok "$1"; else bad "$1"; fi; }

echo "=== OVMX reusable kernel-module home gate (vms-bae) ==="
echo "SRC_ROOT: $SRC_ROOT"

[ -f "$OVERLAY" ] || { echo "FATAL: overlay script not found: $OVERLAY"; exit 1; }
[ -d "$SCAFFOLD" ] || { echo "FATAL: scaffold dir not found: $SCAFFOLD"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# --- self-describing-subdir invariant on the REAL scaffold -------------------
# Every module subdir must carry the three self-describing files, and NO stray
# hand-maintained top-level Kconfig/Makefile may linger in the scaffold (the
# overlay GENERATES those -- a checked-in copy would be a stale second source).
echo "--- scaffold structure ---"
[ -f "$SCAFFOLD/Kconfig" ]  && bad "no stale top-level Kconfig in scaffold (overlay generates it)"  || ok "no stale top-level Kconfig in scaffold (overlay generates it)"
[ -f "$SCAFFOLD/Makefile" ] && bad "no stale top-level Makefile in scaffold (overlay generates it)" || ok "no stale top-level Makefile in scaffold (overlay generates it)"
real_mods=0
for kb in "$SCAFFOLD"/*/Kbuild; do
    [ -e "$kb" ] || continue
    d=$(dirname "$kb"); mod=$(basename "$d")
    [ -f "$d/Kconfig" ];     chk "module '$mod' carries Kconfig" $?
    [ -f "$d/sources.conf" ]; chk "module '$mod' carries sources.conf" $?
    sym=$(awk '/^config OVMX_/ {print $2; exit}' "$d/Kconfig")
    [ -n "$sym" ];           chk "module '$mod' Kconfig declares a config OVMX_* symbol ($sym)" $?
    real_mods=$((real_mods + 1))
done
[ "$real_mods" -ge 2 ]; chk "scaffold has the two baseline modules (vms, vmsfs)" $?

# --- build a fabricated kernel tree the overlay will accept ------------------
make_ktree() {
    local kt="$1"
    mkdir -p "$kt/drivers"
    : > "$kt/Makefile"
    printf 'menu "Device Drivers"\n\nsource "drivers/other/Kconfig"\n\nendmenu\n' > "$kt/drivers/Kconfig"
    printf 'obj-$(CONFIG_OTHER) += other/\n' > "$kt/drivers/Makefile"
}

# --- (1) BASELINE: overlay the real scaffold, unmodified ---------------------
echo "--- baseline overlay (real two modules) ---"
KT1="$WORK/kt-baseline"
make_ktree "$KT1"
if bash "$OVERLAY" "$KT1" "$SRC_ROOT" > "$WORK/ov1.log" 2>&1; then
    ok "overlay runs clean on the real scaffold"
else
    bad "overlay runs clean on the real scaffold"; cat "$WORK/ov1.log"
fi
[ -f "$KT1/drivers/ovmx/Kconfig" ];  chk "baseline: drivers/ovmx/Kconfig generated" $?
[ -f "$KT1/drivers/ovmx/Makefile" ]; chk "baseline: drivers/ovmx/Makefile generated" $?
grep -q 'source "drivers/ovmx/vms/Kconfig"'   "$KT1/drivers/ovmx/Kconfig"; chk "baseline: vms sourced in menu"   $?
grep -q 'source "drivers/ovmx/vmsfs/Kconfig"' "$KT1/drivers/ovmx/Kconfig"; chk "baseline: vmsfs sourced in menu" $?
grep -q 'obj-$(CONFIG_OVMX_VMS)'   "$KT1/drivers/ovmx/Makefile"; chk "baseline: vms obj- line generated"   $?
grep -q 'obj-$(CONFIG_OVMX_VMSFS)' "$KT1/drivers/ovmx/Makefile"; chk "baseline: vmsfs obj- line generated" $?
grep -q 'drivers/ovmx/Kconfig' "$KT1/drivers/Kconfig"; chk "baseline: drivers/ovmx wired into kernel drivers/Kconfig" $?
grep -q 'obj-$(CONFIG_OVMX)'    "$KT1/drivers/Makefile"; chk "baseline: drivers/ovmx wired into kernel drivers/Makefile" $?
# Sources actually flattened (a .c that must exist for each real module).
[ -f "$KT1/drivers/ovmx/vms/vms_module.c" ];   chk "baseline: vms sources flattened (vms_module.c)"     $?
[ -f "$KT1/drivers/ovmx/vmsfs/vmsfs_super.c" ]; chk "baseline: vmsfs sources flattened (vmsfs_super.c)"  $?
# Byte-identical hash of the overlay script, captured for the tamper check below.
OVERLAY_SHA_BEFORE=$(sha256sum "$OVERLAY" | awk '{print $1}')

# --- (2) DEMONSTRATOR: a hypothetical module added as {subdir + stanza + flag}
# We copy the WHOLE scaffold to a scratch dir and add ONLY a new subdir. We do
# NOT touch the overlay script, and there is no top-level Makefile/Kconfig to
# touch. This is the exact "adding a module" motion from the docs.
echo "--- demonstrator overlay (hypothetical ovmxdemo added, overlay untouched) ---"
SCAFFOLD2="$WORK/drivers-ovmx"
cp -r "$SCAFFOLD" "$SCAFFOLD2"
# Fabricate a minimal but real source file the demo will flatten.
mkdir -p "$WORK/repo/src/ovmxdemo" "$WORK/repo/distro/kernel"
cat > "$WORK/repo/src/ovmxdemo/ovmxdemo_main.c" <<'EOF'
/* hypothetical OVMX demo module source (test fixture, vms-bae) */
int ovmxdemo_answer(void) { return 42; }
EOF
# Point the scratch repo's scaffold + overlay at the real overlay logic but the
# scratch scaffold. The overlay resolves the scaffold as <repo>/distro/kernel/
# drivers-ovmx and sources as <repo>/<glob>, so lay the scratch repo out to match.
cp "$OVERLAY" "$WORK/repo/distro/kernel/overlay-ovmx-drivers.sh"
cp -r "$SCAFFOLD2" "$WORK/repo/distro/kernel/drivers-ovmx"
# The scratch repo carries the REAL vms/vmsfs sources so those two modules still
# flatten (the overlay fails a module whose sources.conf glob matches 0 files);
# the demo adds its own src/ovmxdemo/ alongside. src/vmsfs is required because
# vmsfs's sources.conf stages the genuine ODS-2 codec from src/vmsfs/ods2/ + the
# public header src/vmsfs/include/vmsfs/ods2.h (rd vms-4a8) -- the SAME trees the
# real distro/Dockerfile.bootable kernel-build stage now COPYs into its overlay
# context. Without it the codec glob matches nothing and the overlay's
# zero-match-glob guard (correctly) aborts.
cp -r "$SRC_ROOT/src/kernel"      "$WORK/repo/src/kernel"
cp -r "$SRC_ROOT/src/kernel-core" "$WORK/repo/src/kernel-core"
cp -r "$SRC_ROOT/src/vmsfs"       "$WORK/repo/src/vmsfs"
# The new module subdir: Kconfig stanza + Kbuild + sources.conf. NOTHING ELSE.
DEMO="$WORK/repo/distro/kernel/drivers-ovmx/ovmxdemo"
mkdir -p "$DEMO"
cat > "$DEMO/Kconfig" <<'EOF'
config OVMX_OVMXDEMO
	tristate "OVMX demonstrator module (test fixture)"
	default n
	help
	  Hypothetical OVMX kernel module used by the module-home gate to prove
	  a new module is picked up from {subdir + Kconfig stanza + config flag}.
EOF
cat > "$DEMO/Kbuild" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_OVMX_OVMXDEMO) += ovmxdemo.o
ovmxdemo-y := ovmxdemo_main.o
ccflags-y := -I $(src)
EOF
cat > "$DEMO/sources.conf" <<'EOF'
src/ovmxdemo/*.c
EOF

KT2="$WORK/kt-demo"
make_ktree "$KT2"
if bash "$WORK/repo/distro/kernel/overlay-ovmx-drivers.sh" "$KT2" "$WORK/repo" > "$WORK/ov2.log" 2>&1; then
    ok "overlay runs clean with the demonstrator module added"
else
    bad "overlay runs clean with the demonstrator module added"; cat "$WORK/ov2.log"
fi

# The overlay script the demo run used is byte-identical to the real one.
OVERLAY_SHA_AFTER=$(sha256sum "$WORK/repo/distro/kernel/overlay-ovmx-drivers.sh" | awk '{print $1}')
[ "$OVERLAY_SHA_BEFORE" = "$OVERLAY_SHA_AFTER" ]; chk "overlay script UNCHANGED (no harvest-logic edit needed for a new module)" $?

# The demonstrator is fully wired, purely from its subdir + flag:
grep -q 'source "drivers/ovmx/ovmxdemo/Kconfig"' "$KT2/drivers/ovmx/Kconfig"; chk "demo: ovmxdemo Kconfig sourced in generated menu" $?
grep -q 'obj-$(CONFIG_OVMX_OVMXDEMO)' "$KT2/drivers/ovmx/Makefile";           chk "demo: ovmxdemo obj- line generated in Makefile" $?
[ -f "$KT2/drivers/ovmx/ovmxdemo/ovmxdemo_main.c" ];                          chk "demo: ovmxdemo sources flattened into module dir" $?
[ -f "$KT2/drivers/ovmx/ovmxdemo/Kbuild" ];                                   chk "demo: ovmxdemo Kbuild staged" $?
# The two real modules are STILL wired alongside it (additive, not replaced).
grep -q 'source "drivers/ovmx/vms/Kconfig"'   "$KT2/drivers/ovmx/Kconfig"; chk "demo: vms still wired alongside" $?
grep -q 'source "drivers/ovmx/vmsfs/Kconfig"' "$KT2/drivers/ovmx/Kconfig"; chk "demo: vmsfs still wired alongside" $?

# --- negative: a subdir with NO Kbuild is not a module (isn't picked up) ------
echo "--- negative: a non-module subdir is ignored ---"
mkdir -p "$WORK/repo/distro/kernel/drivers-ovmx/notamodule"
echo "# just a note" > "$WORK/repo/distro/kernel/drivers-ovmx/notamodule/README"
KT3="$WORK/kt-neg"
make_ktree "$KT3"
bash "$WORK/repo/distro/kernel/overlay-ovmx-drivers.sh" "$KT3" "$WORK/repo" > "$WORK/ov3.log" 2>&1
if grep -q 'notamodule' "$KT3/drivers/ovmx/Makefile" 2>/dev/null; then
    bad "a subdir without a Kbuild is NOT treated as a module"
else
    ok "a subdir without a Kbuild is NOT treated as a module"
fi

echo ""
echo "=== module-home gate: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] || exit 1
exit 0
