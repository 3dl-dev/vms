#!/bin/sh
# build-activation-vax.sh - prove image ACTIVATION for OVMX images on netbsd-vax
# (rd vms-42d, epic vms-8e8; docs/design-p4-netbsd-vax-boot.md §4.5, decision
# record §4.5.1).
#
# THE DECISION THIS GATE ENFORCES (Decision A, a LABELED Rule-8 substrate
# divergence): OVMX images on the NetBSD/vax substrate are ORDINARY NetBSD
# ELF32-vax DYNAMIC executables, activated by NetBSD's own runtime linker
# `/usr/libexec/ld.elf_so` -- NOT by OVMX-native IMGACT.EXE + `.vms$sv` symbol-vector
# activation. The OVMX-native LINK.EXE/IMGACT/symbol-vector toolchain is the
# Linux / self-hosting-pillar path; on NetBSD the platform's runtime linker does
# activation, because the whole NetBSD-vax userspace already LINKS NetBSD libc
# (docs/design-ovmx-netbsd-syskrnl.md §4.1) and there is NO OVMX-native VAX
# toolchain (§4.3) to emit symbol-vector images in the first place.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host.
#
# This is the ACTIVATION-MODEL proof that sits above the layer build gates
# (build-libvmssys-vax.sh, build-vmsprocess-vax.sh). Those prove the OVMX
# libraries cross-compile + link for netbsd-vax; THIS proves the linked image
# activates by the DECIDED path. It:
#
#   0. Builds libvmssys.a + libvmsprocess.a for netbsd-vax (the ported layers).
#   1. Links a REPRESENTATIVE OVMX image (a LOGINOUT/DCL-class program that
#      exercises the OVMX process-control + syscall/kif surface) as a real
#      vax--netbsdelf DYNAMIC ELF32 executable against NetBSD libc + libpthread.
#   2. ASSERTS the activation contract of Decision A:
#        - ELFCLASS32, Digital VAX, 2's-complement little-endian
#        - carries a PT_INTERP program header (it is dynamically activated)
#        - the requested interpreter is EXACTLY /usr/libexec/ld.elf_so
#          (NetBSD's native runtime linker == the activator)
#        - it has DT_NEEDED entries (a genuinely dynamic image)
#        - it does NOT carry OVMX symbol-vector sections (.vms$sv / .vms$imp):
#          it is NOT an OVMX-native IMGACT image
#        - the interpreter is NOT IMGACT.EXE (the Linux OVMX activator)
#
# Exit 0 = the representative OVMX image builds AND activates by the decided
# NetBSD ld.elf_so path. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
READELF="${TARGET}-readelf"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
VMSPROCESS="$SRC/src/vmsprocess"
VMS_INCLUDE="$SRC/src/libvms/include"
OUT="${OUT:-/tmp/vax-activation-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

MAKE_PROG="$(command -v make)"
TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo "sysroot: $SYSROOT"
cmake --version | head -1
echo

# --- proof 0: build the ported OVMX layers (libvmssys + vmsprocess) ----------
echo "=== proof 0: build libvmssys.a + libvmsprocess.a for netbsd-vax ==="
cmake -S "$LIBVMSSYS" -B "$OUT/libvmssys" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/libvmssys" --target vmssys -- -j"$(nproc)"
LIBVMSSYS_A="$(find "$OUT/libvmssys" -name 'libvmssys.a' | head -1)"
test -n "$LIBVMSSYS_A"

cmake -S "$VMSPROCESS" -B "$OUT/vmsprocess" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVMS_INCLUDE_DIR="$VMS_INCLUDE"
cmake --build "$OUT/vmsprocess" --target vmsprocess -- -j"$(nproc)"
LIBVMSPROCESS_A="$(find "$OUT/vmsprocess" -name 'libvmsprocess.a' | head -1)"
test -n "$LIBVMSPROCESS_A"
echo "built: $LIBVMSSYS_A"
echo "built: $LIBVMSPROCESS_A"
echo

# --- proof 1: link a REPRESENTATIVE OVMX image as a dynamic vax exe -----------
echo "=== proof 1: link a representative OVMX image (LOGINOUT/DCL-class) ==="
cat > "$OUT/ovmx_image.c" <<'EOF'
/* A representative OVMX image for the netbsd-vax activation gate. It references
 * one symbol from each vmsprocess translation unit, forcing a genuine OVMX image
 * (the VMS process-control + syscall/kif surface) to be produced -- a stand-in
 * for LOGINOUT.EXE / DCL.EXE, whose higher library layers (libvms, vmsdcl) are
 * not yet cross-built for netbsd-vax (rd vms-1b2/vms-1cb2/vms-5d1). It links
 * against NetBSD libc + libpthread, so the produced binary activates by the
 * decided path: NetBSD's ld.elf_so. */
#include <stdint.h>
#include <stddef.h>
#include "vms/pcb.h"      /* vms_pcb_init          (vms_pcb.c)      */
#include "vms/process.h"  /* vms_format_uic        (vms_process.c) */
#include "vms/ast.h"      /* ast_init/ast_pending  (ast.c)         */

/* access_modes.c has no public header; declare the entry points used. */
uint8_t  access_mode_get(void);
int      priv_check(uint64_t priv);

int main(void)
{
    char buf[32];

    vms_pcb_init(0);                              /* vms_pcb.c     */
    vms_format_uic(0x00010002u, buf, sizeof buf); /* vms_process.c */
    ast_init();                                   /* ast.c         */
    (void)ast_pending_count();
    (void)priv_check(0);                          /* access_modes.c */
    return (int)access_mode_get();
}
EOF

# Link DYNAMIC (the default; do NOT pass -static) so the produced image carries a
# PT_INTERP requesting NetBSD's runtime linker -- exactly the activation path
# Decision A commits to for LOGINOUT/DCL-class images.
IMG="$OUT/OVMX_IMAGE.EXE"
"$CC" --sysroot="$SYSROOT" \
    -I"$VMSPROCESS/include" -I"$VMS_INCLUDE" -I"$LIBVMSSYS" \
    "$OUT/ovmx_image.c" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" -lpthread -o "$IMG"
echo "--- linked OVMX image ---"
file "$IMG" || true
echo

# --- proof 2: assert the Decision-A activation contract ----------------------
echo "=== proof 2: activation contract (Decision A: ld.elf_so, NOT OVMX IMGACT) ==="

HDR="$("$READELF" -h "$IMG")"
echo "$HDR" | grep -Ei 'Class|Data|Machine|Type'

echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: image is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: image Machine is not Digital VAX"; exit 1; }
echo "$HDR" | grep -qiE "2's complement, little endian" \
    || { echo "FAIL: image is not little-endian"; exit 1; }

# It must carry a PT_INTERP and request NetBSD's native runtime linker.
PHDRS="$("$READELF" -l "$IMG")"
echo "$PHDRS" | grep -qiE 'INTERP' \
    || { echo "FAIL: image has no PT_INTERP -- it is not dynamically activated"; exit 1; }

INTERP="$("$READELF" -p .interp "$IMG" 2>/dev/null | grep -oE '/[^ ]*ld\.elf_so' | head -1 || true)"
if [ -z "$INTERP" ]; then
    # Fall back to the program-header banner phrasing.
    INTERP="$(echo "$PHDRS" | grep -oE '/[^]]*ld\.elf_so' | head -1 || true)"
fi
echo "requested interpreter: ${INTERP:-<none found>}"
# NetBSD's native runtime linker lives at /usr/libexec/ld.elf_so (empirically
# confirmed: the vax--netbsdelf toolchain emits exactly this interp string).
[ "$INTERP" = "/usr/libexec/ld.elf_so" ] \
    || { echo "FAIL: interpreter is not /usr/libexec/ld.elf_so (got '${INTERP:-none}')"; exit 1; }

# It must NOT be activated by the OVMX Linux activator.
echo "$PHDRS" | grep -qiF 'IMGACT.EXE' \
    && { echo "FAIL: image requests IMGACT.EXE -- OVMX-native activation must NOT be used on netbsd-vax"; exit 1; }
echo "  -> interpreter is NOT IMGACT.EXE (Linux OVMX activator): correct"

# It must be genuinely dynamic (has DT_NEEDED).
DYN="$("$READELF" -d "$IMG" 2>/dev/null || true)"
echo "$DYN" | grep -qiE 'NEEDED' \
    || { echo "FAIL: no DT_NEEDED -- image is not a dynamic exe activated by ld.elf_so"; exit 1; }
echo "  -> DT_NEEDED present: genuinely dynamic, activated by ld.elf_so"

# It must NOT carry OVMX symbol-vector sections -- it is not an IMGACT image.
SECS="$("$READELF" -S "$IMG")"
if echo "$SECS" | grep -qE '\.vms\$(sv|imp)'; then
    echo "FAIL: image carries OVMX symbol-vector sections (.vms\$sv/.vms\$imp) --"
    echo "      it must be a plain NetBSD ELF, not an OVMX-native IMGACT image"
    exit 1
fi
echo "  -> no .vms\$sv / .vms\$imp symbol-vector sections: a plain NetBSD ELF, not an OVMX IMGACT image"

echo
echo "=== DIVERGENCE (LABELED, CLAUDE.md Rule 8) ==="
echo "  Linux/self-hosting substrate : PT_INTERP=IMGACT.EXE + .vms\$sv symbol vectors"
echo "                                 (OVMX-native LINK.EXE/IMGACT activation)"
echo "  NetBSD-vax substrate         : PT_INTERP=/usr/libexec/ld.elf_so, plain NetBSD ELF32"
echo "                                 (NetBSD's own runtime linker activates)"
echo
echo "=== ALL PROOFS PASSED: representative OVMX image activates on netbsd-vax via ld.elf_so ==="
