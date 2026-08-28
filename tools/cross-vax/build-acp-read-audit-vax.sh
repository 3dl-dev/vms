#!/bin/sh
# build-acp-read-audit-vax.sh - STANDING elf32-vax (ILP32) COMPILE AUDIT of the
# whole VMS ACP-read code path (rd vms-8e8f, piece A of the VAX ACP flip epic
# vms-c45; the VAX leg of the conductor's 3-way convergence gate
# {x86_64 CI + VAX ILP32 + Alpha LP64}).
#
# WHAT THIS IS, AND WHAT IT IS NOT.
#
# This is a COMPILE AUDIT, not a runtime build -- exactly the relationship
# build-vms-module-vax.sh has to the shipped vms.kmod. It deliberately compiles
# a configuration NO SHIPPED VAX RECIPE USES YET: the ACP-read arms with
# OVMX_HAVE_ACP + OVMX_BOOT_ACP_BRIDGE defined. The shipped VAX runtime
# (tools/cross-vax/build-ovmx-init-vax.sh for STARTUP.EXE, and the standalone
# cross vmsrms) defines NEITHER, so on the running VAX every one of these arms
# is still #if'd OUT and the boot behaviour is unchanged. Turning them on at
# runtime is the COUPLED cutover, rd vms-329 -- NOT this script's job, and this
# script must never be used to justify skipping that item's e2e proof.
#
# WHY IT EXISTS. Those arms had NEVER been compiled for elf32-vax at all: they
# are OVMX_HAVE_ACP-gated and only the x86_64/Linux build defines that macro
# (the standalone netbsd-vax vmsrms cross skips the define at
# src/vmsrms/CMakeLists.txt's VMSRMS_STANDALONE branch, and
# build-ovmx-init-vax.sh defines neither macro). So the coupled cutover was
# facing an unknown pile of ILP32 width errors ON TOP of a hard, boot-bricking
# wiring change. This audit takes the compile risk off that item and then keeps
# it off: it is standing, so the arms can never silently rot back to
# LP64-only-correct while nothing on the VAX side builds them.
#
# WHAT IT PROVES (each one is a hard failure):
#
#   1. toolchain: a real vax--netbsdelf cross compiler is in front of us.
#   2. the FULL ACP-read closure compiles for elf32-vax with the ACP arms ON:
#        src/vmsrms/rms_core.c    (the RMS ACP arm: $ASSIGN/IO$_ACCESS/DID walk)
#        src/vmsrms/rms_io.c      (READVBLK/WRITEVBLK/truncate over the ACP)
#        src/vmsrms/rms_search.c  (ACP wildcard search / ACPCONTROL)
#        src/imgact/imgact_acp.c  (the shared ACP file-access walk)
#        src/ovmx_init/ovmx_boot_acp_read.c    (PID 1's staging bridge)
#        src/ovmx_init/ovmx_boot_sysgen_acp.c  (OVMXVMSSYS.PAR over the ACP)
#        src/ovmx_init/ovmx_boot_netbsd.c      (the NetBSD boot backend, incl.
#                                               the vms-8e8f staging twin)
#        src/ovmx_init/ovmx_init.c             (stage_boot_images, ACP-active)
#        src/ovmx_init/sysboot.c               (SYSBOOT>, same link)
#      ...under an ILP32 WIDTH ERROR SET (-Werror=, below) that turns the
#      classic LP64-only-correct constructs into hard errors on this target.
#   3. every object really is ELF32 / Digital VAX (not a host object that
#      wandered in).
#   4. TEETH -- the ACP arms are genuinely COMPILED IN, not #if'd out. A gate
#      that "compiles the ACP path" by compiling an empty preprocessor arm
#      proves nothing (this is the INV-6 fake-success class in gate form), so
#      every object is checked for the ACP symbols it must define and the
#      executive ACP entry points it must reference.
#   5. NEGCTL -- the width check has teeth: a deliberately LP64 translation
#      unit MUST be rejected by this compiler. If it compiles, proof 2's
#      -Werror width set is meaningless and the whole audit is void.
#   6. closure consistency: after a partial link of the whole closure, the only
#      ACP symbols still undefined are the vms_kif_acp_* executive wrappers
#      that live in libvmssys -- i.e. the ACP-read code is self-consistent and
#      nothing inside it is missing a definition (the static-link weak-seam
#      class: ovmx_sysgen_acp_read must resolve STRONG, not stay weak/NULL).
#
# Clean-room (CLAUDE.md Rule 8): compiles OVMX's own sources with a public
# cross toolchain. No VSI/HPE or NetBSD source is copied.
#
# Exit 0 = every proof passed. Any failure is fatal.

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${CC:-${TARGET}-gcc}"
NM="${NM:-${TARGET}-nm}"
LD="${LD:-${TARGET}-ld}"
OBJDUMP="${OBJDUMP:-${TARGET}-objdump}"
SRC="$(pwd)"
OUT="${OUT:-/tmp/vax-acp-read-audit}"
rm -rf "$OUT"; mkdir -p "$OUT"

fail() { echo "FAIL: $*"; exit 1; }

# ---------------------------------------------------------------------------
# proof 1: toolchain
# ---------------------------------------------------------------------------
echo "=== proof 1: vax--netbsdelf cross toolchain ==="
"$CC" --version | head -1
DUMPMACHINE="$("$CC" -dumpmachine)"
echo "dumpmachine: $DUMPMACHINE"
case "$DUMPMACHINE" in
    vax*) : ;;
    *) fail "compiler $CC does not target vax (dumpmachine=$DUMPMACHINE)" ;;
esac
echo "sysroot: $SYSROOT"
echo "OK: elf32-vax cross compiler present"
echo

# ---------------------------------------------------------------------------
# The ILP32 width error set.
#
# These are the diagnostic classes that separate LP64-only-correct C from
# width-clean C, promoted from warnings to ERRORS for this audit. The point of
# naming them individually (rather than a blanket -Werror) is that the audit
# must fail on WIDTH defects and not on the tree's pre-existing, target-neutral
# style warnings (-Wstringop-truncation, -Wformat-truncation, the fab$b_bln
# -Woverflow) which are identical on x86_64 and say nothing about ILP32.
#
#   shift-count-overflow            "x << 32" on a 32-bit long -- the single
#                                   most common LP64-only construct, and the
#                                   negctl this audit uses to prove it bites.
#   int-conversion / int-to-pointer-cast / pointer-to-int-cast
#                                   pointer<->integer traffic through a type
#                                   that is only wide enough on LP64.
#   incompatible-pointer-types      a 16-bit DID field addressed as a wider
#                                   type (the "DID-uint16 class").
#   implicit-function-declaration   an implicit decl returns int; on a target
#                                   where that is narrower than the real return
#                                   type it silently truncates.
#   format                          %ld/%zu/%p against a differently-sized
#                                   argument -- width-dependent by definition.
#                                   -Wformat-truncation / -Wformat-overflow are
#                                   explicitly de-escalated back to warnings:
#                                   GCC files them under the same "format"
#                                   -Werror= prefix, but they are buffer-size
#                                   style findings, identical on x86_64, and
#                                   say nothing about ILP32.
#   sizeof-pointer-memaccess / array-bounds / return-type
#                                   the memcpy/array-width defects that follow.
# ---------------------------------------------------------------------------
WIDTH_WERROR="-Werror=shift-count-overflow \
    -Werror=int-conversion \
    -Werror=int-to-pointer-cast \
    -Werror=pointer-to-int-cast \
    -Werror=incompatible-pointer-types \
    -Werror=implicit-function-declaration \
    -Werror=implicit-int \
    -Werror=format \
    -Wno-error=format-truncation \
    -Wno-error=format-overflow \
    -Werror=sizeof-pointer-memaccess \
    -Werror=array-bounds \
    -Werror=return-type"

INC="-I$SRC/src/ovmx_init \
    -I$SRC/src/libvms/include \
    -I$SRC/src/vmsprocess/include \
    -I$SRC/src/vmslnm/include \
    -I$SRC/src/vmsfs/include \
    -I$SRC/src/vmsrms/include \
    -I$SRC/src/vmsrms \
    -I$SRC/src/libvmssys \
    -I$SRC/src/imgact \
    -I$SRC/src/kernel"

# _NETBSD_SOURCE is what src/ovmx_init/CMakeLists.txt selects for the NetBSD
# substrate (defining _POSIX_C_SOURCE would turn the needed surface OFF).
#
# OVMX_HAVE_ACP        -- the RMS/boot ACP arms (this is the whole point).
# OVMX_BOOT_ACP_BRIDGE -- "the ACP-read bridge TUs are in this link", so
#                         ovmx_init.c compiles stage_boot_images() for real.
# NEITHER is defined by any shipped VAX runtime recipe; see the header.
CFLAGS="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -DOVMX_HAVE_ACP -DOVMX_BOOT_ACP_BRIDGE=1 \
    $WIDTH_WERROR"

# ---------------------------------------------------------------------------
# proof 2: compile the closure
# ---------------------------------------------------------------------------
CLOSURE="src/vmsrms/rms_core.c
src/vmsrms/rms_io.c
src/vmsrms/rms_search.c
src/imgact/imgact_acp.c
src/ovmx_init/ovmx_boot_acp_read.c
src/ovmx_init/ovmx_boot_sysgen_acp.c
src/ovmx_init/ovmx_boot_netbsd.c
src/ovmx_init/ovmx_init.c
src/ovmx_init/sysboot.c"

echo "=== proof 2: cross-compile the ACP-read closure for $TARGET (ACP arms ON) ==="
echo "    width error set: $(echo "$WIDTH_WERROR" | tr -s ' ')"
echo
for f in $CLOSURE; do
    obj="$OUT/$(basename "$f" .c).o"
    printf '    %-40s -> %s\n' "$f" "$(basename "$obj")"
    # shellcheck disable=SC2086
    "$CC" $CFLAGS $INC -c "$SRC/$f" -o "$obj" \
        || fail "$f does not compile ILP32-clean for $TARGET with the ACP arms on"
done
echo "OK: all 9 ACP-read closure translation units compile for $TARGET"
echo

# ---------------------------------------------------------------------------
# proof 3: every object is really ELF32 / Digital VAX
# ---------------------------------------------------------------------------
echo "=== proof 3: every object is ELF32 Digital VAX ==="
for f in $CLOSURE; do
    obj="$OUT/$(basename "$f" .c).o"
    fmt="$("$OBJDUMP" -f "$obj" | grep -i 'file format' | head -1)"
    echo "    $(basename "$obj"): $fmt"
    echo "$fmt" | grep -qi 'elf32-vax' \
        || fail "$(basename "$obj") is not elf32-vax ($fmt)"
done
echo "OK: 9/9 objects are elf32-vax"
echo

# ---------------------------------------------------------------------------
# proof 4: TEETH -- the ACP arms are genuinely compiled IN
#
# Without this, proof 2 would still pass if every `#if defined(OVMX_HAVE_ACP)`
# arm were deleted or the macro silently stopped reaching the compiler: an
# empty preprocessor arm compiles beautifully. Each check below names a symbol
# that EXISTS ONLY INSIDE an ACP arm, so its absence means the audit compiled
# the POSIX path and called it a VAX ACP proof (the INV-6 fake-success class).
# ---------------------------------------------------------------------------
echo "=== proof 4 (TEETH): the ACP arms are compiled IN, not #if'd out ==="

# defines_sym <object> <symbol>  -- defined here (T or t: extern or static)
defines_sym() {
    "$NM" "$1" | grep -qE "^[0-9a-f]+ [Tt] $2(\..*)?\$"
}
# refs_sym <object> <symbol>  -- referenced as undefined (the executive edge)
refs_sym() {
    "$NM" "$1" | grep -qE "^ +U $2\$"
}

check_defines() {
    if defines_sym "$OUT/$1" "$2"; then
        echo "    OK   $1 defines $2"
    else
        fail "$1 does not define $2 -- the ACP arm was NOT compiled in"
    fi
}
check_refs() {
    if refs_sym "$OUT/$1" "$2"; then
        echo "    OK   $1 references the executive entry $2"
    else
        fail "$1 does not reference $2 -- the ACP arm was NOT compiled in"
    fi
}

# The RMS ACP arm: the DID walk, the file open, the staging copy, and the five
# executive ACP entry points it must reach through /dev/vms.
check_defines rms_core.o        rms_acp_resolve_did
check_defines rms_core.o        rms_acp_open_file
check_defines rms_core.o        rms_stage_over_acp
check_refs    rms_core.o        vms_kif_acp_assign
check_refs    rms_core.o        vms_kif_acp_access
check_refs    rms_core.o        vms_kif_acp_deaccess
check_refs    rms_core.o        vms_kif_acp_fileop
check_refs    rms_core.o        vms_kif_acp_readvb
check_defines rms_io.o          rms_io_write_acp
check_defines rms_io.o          rms_io_ftruncate_acp
check_refs    rms_io.o          vms_kif_acp_readvb
check_refs    rms_io.o          vms_kif_acp_writevb
check_defines rms_search.o      rms_acp_search
check_refs    rms_search.o      vms_kif_acp_acpcontrol

# The shared ACP file-access walk IMGACT.EXE and PID 1 both use.
check_defines imgact_acp.o      imgact_acp_open
check_defines imgact_acp.o      imgact_acp_pread
check_defines imgact_acp.o      imgact_acp_close

# PID 1's staging bridge + its libc-backed host primitives.
check_defines ovmx_boot_acp_read.o  ovmx_boot_acp_present
check_defines ovmx_boot_acp_read.o  ovmx_boot_acp_stage
check_defines ovmx_boot_acp_read.o  imgact_acp_dev_open
check_defines ovmx_boot_acp_read.o  imgact_acp_dev_ioctl

# OVMXVMSSYS.PAR over the ACP -- the STRONG side of the weak seam.
check_defines ovmx_boot_sysgen_acp.o ovmx_sysgen_acp_read
check_defines ovmx_boot_sysgen_acp.o ovmx_sysgen_acp_write

# The NetBSD boot backend, including the vms-8e8f staging twin.
check_defines ovmx_boot_netbsd.o ovmx_boot_prepare_stage_dir
check_defines ovmx_boot_netbsd.o ovmx_boot_acp_mount_system_disk
check_refs    ovmx_boot_netbsd.o vms_kif_acp_mount

# PID 1 itself: stage_boot_images() must exist and must call the ACP bridge.
check_defines ovmx_init.o       stage_boot_images
check_refs    ovmx_init.o       ovmx_boot_acp_stage
check_refs    ovmx_init.o       ovmx_boot_acp_present
check_refs    ovmx_init.o       ovmx_boot_prepare_stage_dir
echo "OK: every ACP arm in the closure is genuinely compiled in"
echo

# ---------------------------------------------------------------------------
# proof 5: NEGCTL -- prove the width check bites
# ---------------------------------------------------------------------------
echo "=== proof 5 (NEGCTL): an LP64 TU must be REJECTED by this compiler ==="
cat > "$OUT/lp64_negctl.c" <<'NEGCTL'
/* Deliberately WRONG for the VAX (ILP32): asserts the LP64 widths, and does
 * the single most common LP64-only-correct thing -- a 32-bit shift of a long.
 * On a real vax--netbsdelf compile long and void* are 32-bit, so all three
 * MUST be hard errors. If this compiles, proof 2's width set has no teeth. */
#include <stdint.h>
_Static_assert(sizeof(long)  == 8, "NEGCTL: pretend VAX long is 64-bit (LP64)");
_Static_assert(sizeof(void*) == 8, "NEGCTL: pretend VAX pointer is 64-bit");
unsigned long ovmx_negctl_pack(unsigned short a, unsigned short b,
                               unsigned short c)
{
    return (unsigned long)a
         | ((unsigned long)b << 16)
         | ((unsigned long)c << 32);   /* LP64-only: 32 >= width of long here */
}
NEGCTL
set +e
# shellcheck disable=SC2086
NEG_OUT="$("$CC" $CFLAGS $INC -c "$OUT/lp64_negctl.c" -o "$OUT/lp64_negctl.o" 2>&1)"
NEG_RC=$?
set -e
echo "$NEG_OUT" | sed 's/^/    | /'
if [ "$NEG_RC" -eq 0 ]; then
    fail "the LP64 negctl COMPILED for $TARGET -- the ILP32 width gate has no teeth"
fi
echo "$NEG_OUT" | grep -qi 'shift count' \
    || fail "the LP64 negctl failed, but NOT on the 32-bit shift -- the width error set does not cover shift-count-overflow"
echo "PASS (negctl): the LP64 translation unit is correctly rejected for $TARGET"
echo

# ---------------------------------------------------------------------------
# proof 6: closure consistency (the static-link weak-seam class)
# ---------------------------------------------------------------------------
echo "=== proof 6: ACP-read closure is self-consistent (partial link) ==="
"$LD" -r -o "$OUT/acp_read_closure.o" \
    "$OUT/rms_core.o" "$OUT/rms_io.o" "$OUT/rms_search.o" \
    "$OUT/imgact_acp.o" "$OUT/ovmx_boot_acp_read.o" \
    "$OUT/ovmx_boot_sysgen_acp.o" "$OUT/ovmx_boot_netbsd.o" \
    "$OUT/ovmx_init.o" "$OUT/sysboot.o" \
    || fail "the ACP-read closure does not partially link for $TARGET"

# ovmx_sysgen_acp_read must be resolved STRONG here. This is the exact
# static-link weak-seam defect class that has bitten the VAX/Alpha ports
# before: the weak default in sysgen_params.h gives NO link error when the
# strong TU is missing -- PID 1 just silently reads nothing.
if "$NM" "$OUT/acp_read_closure.o" | grep -qE '^ +[wW] ovmx_sysgen_acp_read$'; then
    fail "ovmx_sysgen_acp_read is still WEAK/undefined after the closure link -- the strong ACP reader did not land (static-link weak-seam class)"
fi
defines_sym "$OUT/acp_read_closure.o" ovmx_sysgen_acp_read \
    || fail "ovmx_sysgen_acp_read is not defined by the closure"
echo "    OK   ovmx_sysgen_acp_read resolves STRONG in the closure"

# The only ACP symbols still undefined must be the vms_kif_acp_* executive
# wrappers, which live in libvmssys (linked in by the real image build).
STRAY="$("$NM" "$OUT/acp_read_closure.o" \
    | grep -E '^ +U ' \
    | grep -iE 'acp' \
    | grep -vE ' vms_kif_acp_' || true)"
if [ -n "$STRAY" ]; then
    echo "$STRAY" | sed 's/^/    | /'
    fail "the closure leaves ACP symbols undefined that are not the libvmssys vms_kif_acp_* executive wrappers"
fi
echo "    OK   the only undefined ACP symbols are the libvmssys vms_kif_acp_* wrappers:"
"$NM" "$OUT/acp_read_closure.o" | grep -E '^ +U vms_kif_acp_' | sed 's/^/         /'
echo

echo "=== ALL PROOFS PASSED: the VMS ACP-read code path compiles ILP32-clean for $TARGET with OVMX_HAVE_ACP + OVMX_BOOT_ACP_BRIDGE, every ACP arm is genuinely compiled in, and the LP64 negctl is rejected ==="
