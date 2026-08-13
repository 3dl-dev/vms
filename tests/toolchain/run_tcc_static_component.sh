#!/bin/sh
# run_tcc_static_component.sh -- the STATIC TCC.EXE (the plain fork+execve
# compiler DCL activates as a foreign command) compiles the REAL OVMX runtime
# component's translation units to VALID, BYTE-IDENTICAL objects (self-host
# spine #6, bead vms-d1b).
#
# This is the host-provable prerequisite the QEMU MMK-driven build
# (tests/qemu/test_syssvc_mmk_build.c) rests on: BEFORE asserting that MMK can
# drive TCC over its mailbox DCL in-guest, prove the static TCC.EXE itself is a
# real, deterministic compiler of the actual component sources
# (src/libvmssys/vms_string.c, vms_snprintf.c + tests/toolchain/component/
# OVMXRTDRV.C -- the freestanding string/format runtime and its driver).
#
# WHAT THIS PROVES (all real, nothing mocked):
#   1. mk_tcc_static.sh builds a genuinely STATIC TCC.EXE from the vendored
#      tinycc (the binary DCL fork+execve activates -- no dynamic linker, no
#      IMGACT, matching the staged MMK.EXE/DCL.EXE).
#   2. That TCC.EXE compiles each real runtime TU freestanding (Rule 3:
#      -ffreestanding -fno-builtin) to a VALID ELF relocatable object carrying
#      the expected global symbol (readelf oracle).
#   3. Each object is BYTE-IDENTICAL across two independent compiles (cmp) --
#      the compile-step determinism the byte-identical-twice build bar needs.
#
# HONEST SCOPE (Rule 9 / the residual spine #6 gap). This runs on the host, NOT
# in QEMU, and covers the COMPILE stage only. vms_math.c is deliberately NOT in
# the set: on x86_64 its SSE `"x"`-constraint inline asm and __builtin_fabs are
# not accepted by tinycc (documented below), so the full three-TU library +
# LINK-to-image is out of the tcc-in-guest scope this spine closes -- see
# docs/design-self-host-spine5-mmk-component.md. The MMK-DRIVEN half (TCC run
# from MMK's persistent mailbox DCL against a real /dev/vms) rides QEMU.
#
# Inputs (env, set by CMake add_test): TCC_SRC (vendored tinycc src dir),
# REPO_SRC (repo src/ dir), COMPONENT (tests/toolchain/component), CC.
# Exit 0 = success.
set -e

: "${REPO_SRC:?need REPO_SRC (repo src/ dir)}"
: "${COMPONENT:?need COMPONENT (tests/toolchain/component dir)}"
HERE=$(cd "$(dirname "$0")" && pwd)
TCC_SRC=${TCC_SRC:-$REPO_SRC/../third-party/tcc/src}
CC=${CC:-gcc}
LIBVMSSYS="$REPO_SRC/libvmssys"
TCC_INC="$TCC_SRC/include"

command -v readelf >/dev/null 2>&1 || { echo "FAIL: readelf required as the object oracle"; exit 1; }
[ -d "$TCC_INC" ] || { echo "FAIL: vendored tinycc include dir not found: $TCC_INC"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== 1/4 build the STATIC TCC.EXE (mk_tcc_static.sh, CC=$CC) =="
CC="$CC" WORK="$WORK/tccbuild" sh "$HERE/mk_tcc_static.sh" "$WORK/TCC.EXE" "$TCC_SRC"
[ -x "$WORK/TCC.EXE" ] || { echo "FAIL: TCC.EXE not produced"; exit 1; }

# Stage the REAL component sources (VMS-style upper-case .C, as the drive uses).
cp "$LIBVMSSYS/vms_string.c"   "$WORK/VMS_STRING.C"
cp "$LIBVMSSYS/vms_snprintf.c" "$WORK/VMS_SNPRINTF.C"
cp "$COMPONENT/OVMXRTDRV.C"    "$WORK/OVMXRTDRV.C"
# The component's own freestanding headers (its only includes besides tinycc's
# stddef.h/stdint.h).
cp "$LIBVMSSYS"/*.h "$WORK/"

cd "$WORK"

# The compile command line is exactly the shape MMK resolves from a descrip.mms
# TCC action -- freestanding, the component headers on -I, tinycc's own headers
# on -I for stddef.h/stdint.h. (In-guest the same line runs, with these two -I
# pointing at the staged component + tinycc include dirs.)
compile() { # <src> <obj>
    "$WORK/TCC.EXE" -x c -c -ffreestanding -fno-builtin -I "$TCC_INC" -I "$WORK" "$1" -o "$2"
}

# TU -> the global symbol readelf must find (the object's reason to exist).
check_tu() { # <STEM> <symbol>
    stem=$1; sym=$2
    echo "== compile $stem.C -> $stem.OBJ (static TCC.EXE), twice, assert valid + byte-identical =="
    compile "$stem.C" "$stem.A.OBJ" || { echo "FAIL: TCC.EXE could not compile $stem.C"; return 1; }
    compile "$stem.C" "$stem.B.OBJ" || { echo "FAIL: TCC.EXE could not compile $stem.C (2nd)"; return 1; }
    # (a) valid ELF relocatable
    readelf -h "$stem.A.OBJ" 2>/dev/null | grep -q "REL (Relocatable file)" \
        || { echo "FAIL: $stem.OBJ is not a valid ELF relocatable object"; return 1; }
    # (b) carries the expected defined global symbol
    readelf -sW "$stem.A.OBJ" 2>/dev/null | grep -qw "$sym" \
        || { echo "FAIL: $stem.OBJ does not define expected symbol $sym"; return 1; }
    # (c) byte-identical across two compiles
    cmp -s "$stem.A.OBJ" "$stem.B.OBJ" \
        || { echo "FAIL: TCC.EXE produced a non-deterministic $stem.OBJ:"; cmp "$stem.A.OBJ" "$stem.B.OBJ"; return 1; }
    echo "  PASS: $stem.OBJ valid ELF, defines $sym, byte-identical twice ($(wc -c < "$stem.A.OBJ") bytes)"
}

check_tu VMS_STRING   vms_strlen
check_tu VMS_SNPRINTF vms_snprintf
check_tu OVMXRTDRV    main

echo "== 4/4 confirm vms_math.c is the documented x86 tcc-blocked TU (honest scope) =="
# Not a silent omission: assert the block is REAL so a future tinycc bump that
# starts accepting it flips this to a visible signal to widen the in-guest set.
cp "$LIBVMSSYS/vms_math.c" "$WORK/VMS_MATH.C"
if compile "$WORK/VMS_MATH.C" "$WORK/VMS_MATH.OBJ" 2>"$WORK/math.err"; then
    case "$(uname -m)" in
        x86_64|amd64)
            echo "NOTE: tinycc now compiles vms_math.c on $(uname -m) -- the in-guest scope can widen to the full 3-TU library + LINK. Update docs/design-self-host-spine5-mmk-component.md and this test." ;;
        *)
            echo "  (vms_math.c compiled on $(uname -m); the x86 SSE-asm block is x86-only)" ;;
    esac
else
    echo "  PASS: vms_math.c is NOT tcc-compilable here (expected on x86_64: SSE \"x\"-constraint inline asm / __builtin_fabs) -- documented scope boundary:"
    grep -iE "constraint|builtin|asm" "$WORK/math.err" | head -3 | sed 's/^/     /' || true
fi

echo
echo "================================================================================"
echo "MILESTONE (vms-d1b, self-host spine #6 prerequisite): the STATIC TCC.EXE -- the"
echo "plain fork+execve compiler DCL activates as a foreign command -- compiled the REAL"
echo "OVMX runtime component TUs (VMS_STRING/VMS_SNPRINTF/OVMXRTDRV) to valid, byte-"
echo "identical objects. This is the compiler the QEMU test_syssvc_mmk_build.c drives"
echo "from MMK's persistent mailbox DCL against a real /dev/vms."
echo "================================================================================"
