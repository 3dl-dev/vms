#!/bin/bash
# build-vax-modular-kernel.sh - cross-build a custom NetBSD/vax kernel that
# carries the loadable-module framework, so the OVMX executive `vms' module can
# be modload'ed to bring /dev/vms up on real NetBSD/vax under SIMH (rd vms-f78bb,
# epic vms-8e8; design-p4 §4.2 risk 2 -- the compile-into-kernel fallback).
#
# WHY A CUSTOM KERNEL. NetBSD/vax GENERIC is the ONE vax GENERIC (of every port)
# built WITHOUT `options MODULAR' -- so `modctl' returns ENOSYS and no module can
# load. Worse, the vax port's module support was never fully WIRED: the loader
# glue arch/vax/vax/kobj_machdep.c (kobj_reloc/kobj_machdep) has existed since
# 2018 but is NOT listed in files.vax, and module_init_md() (an empty per-arch
# hook every MODULAR port defines in machdep.c) is missing for vax -- so plain
# `options MODULAR' fails to LINK. This script patches those two gaps and builds
# a GENERIC+MODULAR kernel (config `OVMX'). It does NOT statically compile the
# driver in; it re-enables the platform's own module loader and modload's the
# unchanged OVMX `vms' module -- the least-invasive form of the documented
# fallback, and the one that keeps a single module source across substrates.
#
# CLEAN-ROOM: the two patches are OVMX build glue over OPEN NetBSD source; this
# is not VMS reverse-engineering (CLAUDE.md Rule 8 is about VSI/VMS source, not
# about building/patching NetBSD). No VSI/HPE source is touched.
#
# Runs INSIDE the ovmx-cross-vax container (Debian + build tools). Mounts:
#   /nbsrc  the FULL NetBSD/vax source tree (usr/src: sys + share/mk + tools +
#           external ...), pinned to the same NetBSD version as the lab + the
#           cross module (10.1). The caller (run-devvms.sh) downloads+extracts it.
#   /obj /tools /dest  build.sh work dirs (kept between runs to cache the tools).
#   /out    receives the built kernel as netbsd-OVMX.
#
# The `tools' build (nbmake, config, the vax cross gcc/binutils) is the long pole
# and is BUILT ONCE -- a warm /tools makes reruns fast (kernel only). Every step
# is deterministic; nothing touches the host.
set -euo pipefail
export MKREPRO=no DEBIAN_FRONTEND=noninteractive

# build.sh tools/compat needs zlib dev headers on the host toolchain.
apt-get update -qq && apt-get install -y -qq zlib1g-dev >/dev/null 2>&1 || true

SRC="${SRC:-/nbsrc/usr/src}"
OUT="${OUT:-/out}"
: "${JOBS:=$(nproc)}"
cd "$SRC"

[ -f "$SRC/build.sh" ] || { echo "FAIL: full NetBSD src tree not at $SRC (need src+syssrc+sharesrc+gnusrc)"; exit 2; }

# --- patch the vax module-loader wiring (idempotent) -------------------------
FILESVAX="$SRC/sys/arch/vax/conf/files.vax"
MACHDEP="$SRC/sys/arch/vax/vax/machdep.c"
if ! grep -q 'kobj_machdep.c' "$FILESVAX"; then
  printf '\n# rd vms-f78bb: wire the vax module-loader support. arch/vax/vax/kobj_machdep.c\n# (kobj_reloc/kobj_machdep) has existed since 2018 but was never added to\n# files.vax, so `options MODULAR'"'"' failed to link on vax. Add it (modular attr).\nfile\tarch/vax/vax/kobj_machdep.c\tmodular\n' >> "$FILESVAX"
  echo "patched files.vax (+kobj_machdep.c modular)"
fi
if ! grep -q 'module_init_md' "$MACHDEP"; then
  cat >> "$MACHDEP" <<'MEOF'

#ifdef MODULAR
#include <sys/module.h>
/*
 * rd vms-f78bb: vax never provided module_init_md() (alpha/sparc/i386 all define
 * an empty one in machdep.c). With options MODULAR it is an undefined reference
 * at kernel link. Provide the same trivial hook so the VAX port links MODULAR
 * and can modload the OVMX vms executive module to bring /dev/vms up.
 */
void
module_init_md(void)
{
}
#endif /* MODULAR */
MEOF
  echo "patched machdep.c (+module_init_md)"
fi

# --- rd vms-603: faithful boot console -- suppress the NetBSD banner under -----
# AB_QUIET. The autoconf device probes + root/swap mount lines already go quiet
# under AB_QUIET (subr_prf.c: aprint_normal drops TOCONS), but banner() emits the
# copyright + version + total/avail-memory lines with printf (always TOCONS), so
# no boothowto bit stock-suppresses them. Gate banner()'s body on AB_QUIET so
# that a QUIET boot (R5 = RB_SINGLE|AB_QUIET, drive_boot_vax.py) shows the VMS
# boot sequence, not a Unix dmesg. A non-quiet boot is untouched (reversible).
# Clean-room (CLAUDE.md Rule 8): NetBSD kernel source, not VMS -- unaffected.
INITMAIN="$SRC/sys/kern/init_main.c"
if ! grep -q 'OVMX vms-603 quiet banner' "$INITMAIN"; then
  # Anchor on the unique `if ((boothowto & AB_SILENT) != 0)' -- banner()'s first
  # STATEMENT -- so the early return lands AFTER the locals (no C89
  # declaration-after-statement issue) and only in banner() (that AB_SILENT test
  # occurs exactly once in init_main.c).
  perl -0pi -e 's/(\n\tif \(\(boothowto & AB_SILENT\) != 0\) \{)/\n\tif \(boothowto & AB_QUIET\)\t\/* OVMX vms-603 quiet banner *\/\n\t\treturn;$1/' "$INITMAIN"
  grep -q 'OVMX vms-603 quiet banner' "$INITMAIN" \
    || { echo "FAIL: could not gate banner() on AB_QUIET in $INITMAIN"; exit 3; }
  echo "patched init_main.c (banner() quiet under AB_QUIET -- vms-603)"
fi

# --- custom kernel config: GENERIC + the module framework (idempotent) -------
CONF="$SRC/sys/arch/vax/conf/OVMX"
if [ ! -f "$CONF" ]; then
  cat > "$CONF" <<'EOF'
# OVMX - GENERIC plus the loadable-module framework (rd vms-f78bb, epic vms-8e8).
# vax GENERIC omits `options MODULAR' (memory-size reasons on the VAX); this
# re-adds it (the wiring gaps are patched by build-vax-modular-kernel.sh) so the
# OVMX `vms' pseudo-device (a real loadable module) can modload to bring /dev/vms
# up on real NetBSD/vax under SIMH. Everything else is GENERIC.
include "arch/vax/conf/GENERIC"
options MODULAR
options MODULAR_DEFAULT_VERBOSE
EOF
  echo "wrote sys/arch/vax/conf/OVMX"
fi

echo "=== host toolchain ==="; cc --version | head -1
if [ ! -x /tools/bin/nbmake ]; then
  echo "=== build.sh -m vax tools (JOBS=$JOBS; the long pole, built once) ==="
  sh build.sh -m vax -U -j"$JOBS" -O /obj -T /tools -D /dest tools
else
  echo "=== tools already built (/tools/bin/nbmake present) -- skipping ==="
fi

echo "=== build.sh -m vax kernel=OVMX ==="
sh build.sh -m vax -U -j"$JOBS" -O /obj -T /tools kernel=OVMX

K="/obj/sys/arch/vax/compile/OVMX/netbsd"
[ -f "$K" ] || K="$(find /obj -path '*compile/OVMX/netbsd' -type f | head -1)"
[ -f "$K" ] || { echo "FAIL: kernel not found under /obj"; exit 1; }
mkdir -p "$OUT"; cp "$K" "$OUT/netbsd-OVMX"
ls -lh "$OUT/netbsd-OVMX"
echo "KERNEL_BUILD_OK: NetBSD/vax MODULAR kernel built ($OUT/netbsd-OVMX)"
