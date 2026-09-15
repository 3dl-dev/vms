#!/bin/sh
# run_mmk_native.sh — close the self-host spine's residual MMK gap (bead
# vms-ec70, epic vms-16d "self-host S3: MMK+LIBRARIAN+assembler as OVMX-
# native"): build the MadGoat MMK ("make" for VMS) AS a VMS-native EXECUTABLE
# image (MMK.EXE, via src/vmslink/mk_mmk.sh — previously DEAD CODE, invoked by
# nothing), activate it through IMGACT.EXE (NO ld / NO ld.so), and have the
# ACTIVATED MMK read + resolve a real multi-TU descrip.mms. Mirrors
# run_tcc_native.sh's shape exactly: build-to-/tmp, hard FAIL on any DONE-
# condition miss, an *_EXPECT_LINK soft-skip flag, milestone banner on success.
#
# Unlike run_tcc_native.sh (which inlines its own six-shareable producer
# build), this harness sources lib_build_graph.sh — the SAME shared recipe
# run_dcl_native.sh uses — because mk_mmk.sh needs the FULL SEVEN-producer
# graph (it also --uses LIBVMSSYS$SHR.EXE, which run_tcc_native.sh's six-
# shareable inline copy does not build). Reusing the shared builder avoids
# adding a THIRD independently-drifting copy of "how do you build the
# producer graph in the arm64 musl container" (see lib_build_graph.sh's own
# header for why it was factored out of run_dcl_native.sh in the first place).
#
# DONE conditions (bead vms-ec70), proven for REAL below, nothing mocked:
#   1. MMK.EXE is built AS a VMS-native EXECUTABLE image (mk_mmk.sh's 19
#      objects — 13 vendored MadGoat corpus TUs + the vms-486 grammar +
#      5 OVMX companions — via LINK.EXE --executable --use {DECC$SHR + the
#      six OVMX shareables}), carrying PT_INTERP=IMGACT.EXE.
#   2. IMGACT.EXE activates MMK.EXE, transitively pulling the seven-producer
#      graph from .vms$imp, and the ACTIVATED MMK runs: it opens a REAL
#      multi-TU descrip.mms (OVMXRT.MMS, the same fixture
#      run_mmk_component_plan.sh already proves on the host against the
#      plain-exec'd mmk_native target) through OVMX RMS, parses it with the
#      real lib$table_parse engine + the vms-486 PARSE_TABLES grammar, expands
#      its MMS macros, builds the target/dependency graph, and in /NOACTION
#      emits the correct DCL command lines in dependency order — four TCC
#      compiles, then a LIBRARIAN archive, then a LINK — byte-identical
#      across two independent runs. This is the CORE NEW proof vs. the plain
#      mmk_native ctest: the resolving image here is the genuinely
#      OVMX-native, IMGACT-activated one, not a bare host fork+exec target.
#
# CRUX (vms-16d): conditions 3-6 (MMK's persistent mailbox-driven DCL
# subprocess actually DRIVING those TCC/LIBRARIAN/LINK command lines to a
# built, running image) are OUT OF SCOPE for this harness and are NOT
# attempted here. tests/corpus/tier3-mmk/ovmx/ovmx_mmk_sp.c's own header is
# explicit: reaching a mailbox goes through the executive, and with no
# /dev/vms $CREMBX fails SS$_NOSUCHDEV — MMK signals it and stops (Rule 9 /
# INV-6 honest failure, the same contract tests/qemu/test_syssvc_mmk_drive.c
# and test_syssvc_mmk_build.c hold every test_syssvc_* suite to: EXIT_SKIP
# when run before vms.ko is loaded). This arm64 musl Alpine container's /vms
# is a HOST-MODE passthrough directory (no vms.ko loaded, no /dev/vms
# character device) — the same reason RMS file I/O (opening OVMXRT.MMS) works
# fine here while $CREMBX/lib$spawn/write-attention-AST cannot. Proving
# conditions 3-6 for real requires the REAL /dev/vms QEMU executive; that is
# tests/qemu/test_syssvc_mmk_build.c's job today (against the STATIC
# `mmk_native` CMake target, fork+exec'd, no PT_INTERP — see its own
# "MMK.EXE is a bare static image" comment). Swapping that QEMU test's
# subject from `mmk_native` to THIS harness's OVMX-native, IMGACT-activated
# MMK.EXE (built the same way, staged into the QEMU initramfs by
# tests/qemu/Dockerfile / inject_and_run.sh) is a follow-on item, not
# attempted here — this harness does not fake the exec-drive.
#
# VMS_FOREIGN_CMD: MMK.EXE is activated directly (no DCL parent to dispatch a
# foreign command), so — exactly as run_mmk_component_plan.sh already does
# against mmk_native — this harness sets VMS_FOREIGN_CMD itself to hand MMK
# its command tail. This is the SAME no-executive fallback
# src/vmsdcl/dcl_cmd_process.c documents (vms_kif_setcli fails without
# /dev/vms, so DCL falls back to this env channel for LIB$GET_FOREIGN); it is
# not a new shortcut invented here, and it activates only because /dev/vms is
# genuinely absent in this container (INV-6 honest, not a bypass).
#
# MMK_EXPECT_LINK (default 1, mirrors TCC_EXPECT_LINK / DCL_EXPECT_LINK in the
# sibling native harnesses): mk_mmk.sh's own LINK.EXE --executable step is
# closed toolchain (vms-ba1/vms-9c1), so a failure there is a real regression
# (hard FAIL) by default. Set MMK_EXPECT_LINK=0 to soften that ONE step (and
# the downstream activation it gates) to a SKIP (exit 2) while iterating on a
# new finding — every other assertion here stays a hard FAIL regardless.
#
# link.c and imgact.c are the complete toolchain and are OUT of the Systems-
# Engineer file-domain — do NOT edit them here.
#
# arm64 musl Alpine container only (CLAUDE.md test loop). Needs root to create
# /vms. Run natively on an aarch64 host, or under arm64 emulation (binfmt/QEMU)
# on x86_64 (see .github/workflows/ci.yml).
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
LIBVMSSYS_DIR="$SRC/libvmssys"
# shellcheck disable=SC2034  # consumed by build_producer_graph() in the sourced lib_build_graph.sh, not directly here
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
# shellcheck disable=SC2034  # consumed by build_producer_graph() in the sourced lib_build_graph.sh, not directly here
VMSRMS_DIR="$SRC/vmsrms"
# shellcheck disable=SC2034  # consumed by build_producer_graph() in the sourced lib_build_graph.sh, not directly here
LIBVMS_INC="$LIBVMS_DIR/include"
# shellcheck disable=SC2034
LNM_INC="$VMSLNM_DIR/include"
# shellcheck disable=SC2034
VMSFS_INC="$VMSFS_DIR/include"
COMPONENT="$REPO/tests/toolchain/component"
WORK=${WORK:-/tmp/mmk-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ;;
    *) echo "SKIP-FAIL: run_mmk_native.sh needs a native aarch64 host (got $(uname -m)) — mk_mmk.sh's default CFLAGS target aarch64 musl and LINK.EXE/IMGACT.EXE are aarch64-only today. Run in the arm64 musl Alpine container per CLAUDE.md."; exit 1 ;;
esac

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }
[ -d "$REPO/tests/corpus/tier3-mmk" ] || { echo "FAIL: tests/corpus/tier3-mmk (vendored MadGoat MMK) not found"; exit 1; }
for f in "$COMPONENT/OVMXRT.MMS" "$COMPONENT/OVMXRTDRV.C" \
         "$LIBVMSSYS_DIR/vms_string.c" "$LIBVMSSYS_DIR/vms_snprintf.c" "$LIBVMSSYS_DIR/vms_math.c"; do
    [ -f "$f" ] || { echo "FAIL: missing component fixture $f"; exit 1; }
done

echo "== build the seven-producer VMS-native shareable graph (shared recipe, lib_build_graph.sh) =="
. "$HERE/lib_build_graph.sh"
build_producer_graph
echo "-- full seven-library producer graph linked VMS-native (incl. LIBVMSSYS\$SHR for MMK) --"

echo
echo "== build MMK.EXE VMS-native (mk_mmk.sh: 19 objects, LINK.EXE --executable) =="
set +e
CC="$CC" WORK="$WORK/mk-mmk" sh "$LINK_DIR/mk_mmk.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/MMK.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SRC" 2>"$WORK/mmk-link.err"
LRC=$?
set -e
echo "-- mk_mmk.sh exit=$LRC; message: --"
tail -6 "$WORK/mmk-link.err" | sed 's/^/   /'

if [ "$LRC" -ne 0 ]; then
    if [ "${MMK_EXPECT_LINK:-1}" = "1" ]; then
        echo "FAIL: MMK.EXE build failed (regression — the producer graph +"
        echo "  emit_executable toolchain is complete; MMK must build+link). See mmk-link.err above."
        exit 1
    fi
    echo "SKIP (MMK_EXPECT_LINK=0): MMK.EXE build failed but the assertion is disabled."
    exit 2
fi
readelf -lW "$SYSEXE/MMK.EXE" | grep -q 'INTERP' || { echo "FAIL: MMK.EXE has no PT_INTERP (IMGACT)"; exit 1; }
chmod +x "$SYSEXE/MMK.EXE"
echo "-- confirmed: MMK.EXE built VMS-native (LINK.EXE --executable, PT_INTERP=IMGACT.EXE) — DONE condition 1 --"

echo
echo "== stage the real OVMXRT.MMS multi-TU component fixture =="
# MMK opens the description file through OVMX RMS, which resolves a bare
# filespec against the process default directory (cwd). Work in an isolated
# subdirectory of WORK, VMS-style upper-case .C names (same staging
# run_mmk_component_plan.sh already does against mmk_native on the host).
PLANDIR="$WORK/plan"
rm -rf "$PLANDIR"; mkdir -p "$PLANDIR"
cp "$COMPONENT/OVMXRT.MMS" "$PLANDIR/OVMXRT.MMS"
cp "$COMPONENT/OVMXRTDRV.C" "$PLANDIR/OVMXRTDRV.C"
cp "$LIBVMSSYS_DIR/vms_string.c"   "$PLANDIR/VMS_STRING.C"
cp "$LIBVMSSYS_DIR/vms_snprintf.c" "$PLANDIR/VMS_SNPRINTF.C"
cp "$LIBVMSSYS_DIR/vms_math.c"     "$PLANDIR/VMS_MATH.C"
printf '! empty default rules (vms-16d MMK-native activation proof)\n' > "$PLANDIR/MMS\$RULES"

echo
echo "== MMK.EXE linked — activate through IMGACT.EXE, resolve OVMXRT.MMS in /NOACTION — DONE condition 2 =="
run_plan() {
    ( cd "$PLANDIR" && VMS_FOREIGN_CMD="/DESCRIPTION=OVMXRT.MMS /NOACTION OVMXRT.EXE" \
        "$SYSEXE/MMK.EXE" < /dev/null 2>/dev/null )
}
set +e
run_plan > "$WORK/plan1.txt"
PRC=$?
set -e
echo "-- MMK exit=$PRC (odd VMS status = success); resolved plan: --"
sed 's/^/   /' "$WORK/plan1.txt"
# VMS success status is ODD.
if [ "$PRC" != "1" ] && [ "$PRC" != "0" ]; then
    if [ $((PRC & 1)) -eq 0 ]; then echo "FAIL: activated MMK.EXE exited with an even (failure) status $PRC"; exit 1; fi
fi

line() { grep -nxF "$1" "$WORK/plan1.txt" | head -1 | cut -d: -f1; }
C_DRV=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: OVMXRTDRV.C -o OVMXRTDRV.OBJ')
C_MTH=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: VMS_MATH.C -o VMS_MATH.OBJ')
C_STR=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: VMS_STRING.C -o VMS_STRING.OBJ')
C_SNP=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: VMS_SNPRINTF.C -o VMS_SNPRINTF.OBJ')
L_LIB=$(grep -nE '^LIBR /CREATE OVMXRT.OLB VMS_MATH.OBJ VMS_STRING.OBJ VMS_SNPRINTF.OBJ$' "$WORK/plan1.txt" | head -1 | cut -d: -f1)
L_LNK=$(grep -nE '^LNK --executable .* -o OVMXRT.EXE OVMXRTDRV.OBJ OVMXRT.OLB$' "$WORK/plan1.txt" | head -1 | cut -d: -f1)

fails=0
need() { if [ -z "$2" ]; then echo "FAIL: $1"; fails=$((fails+1)); else echo "  PASS: $1 (plan line $2)"; fi; }
need "activated MMK resolved the TCC compile of the driver TU (OVMXRTDRV.C -> .OBJ)" "$C_DRV"
need "activated MMK resolved the TCC compile of VMS_MATH.C   -> VMS_MATH.OBJ"     "$C_MTH"
need "activated MMK resolved the TCC compile of VMS_STRING.C -> VMS_STRING.OBJ"   "$C_STR"
need "activated MMK resolved the TCC compile of VMS_SNPRINTF.C -> VMS_SNPRINTF.OBJ" "$C_SNP"
need "activated MMK resolved the LIBRARIAN archive of the three runtime objects -> OVMXRT.OLB" "$L_LIB"
need "activated MMK resolved the LINK of the driver against the .OLB -> OVMXRT.EXE" "$L_LNK"
[ "$fails" -eq 0 ] || { echo "FAIL: activated MMK.EXE did not resolve the full component plan"; exit 1; }

maxc=0
for v in "$C_DRV" "$C_MTH" "$C_STR" "$C_SNP"; do [ "$v" -gt "$maxc" ] && maxc=$v; done
[ "$maxc" -lt "$L_LIB" ] || { echo "FAIL: plan order — a compile did not precede the LIBRARIAN archive"; exit 1; }
[ "$L_LIB" -lt "$L_LNK" ] || { echo "FAIL: plan order — the archive did not precede the LINK"; exit 1; }
echo "  PASS: dependency order — all 4 compiles < LIBRARIAN archive < LINK"

echo
echo "== activated MMK.EXE (run 2): assert the plan is byte-identical (determinism) =="
set +e
run_plan > "$WORK/plan2.txt"
set -e
if cmp -s "$WORK/plan1.txt" "$WORK/plan2.txt"; then
    echo "  PASS: the resolved plan is BYTE-IDENTICAL across two independent activated-MMK runs (cmp clean)"
else
    echo "FAIL: activated MMK.EXE produced a non-deterministic plan across two runs:"; diff "$WORK/plan1.txt" "$WORK/plan2.txt" || true; exit 1
fi
echo "-- confirmed: IMGACT.EXE activates MMK.EXE VMS-native, no ld / no ld.so, and the ACTIVATED image really parses + resolves a real multi-TU descrip.mms, deterministically --"

echo
echo "================================================================================"
echo "MILESTONE (vms-16d, self-host spine #4 residual): the MadGoat MMK build engine"
echo "(tests/corpus/tier3-mmk, 13 vendored TUs + the vms-486 grammar + 5 OVMX"
echo "companions) now builds AS a VMS-native EXECUTABLE image via mk_mmk.sh (LINK.EXE"
echo "--executable --use {DECC\$SHR + the six OVMX shareables}) and activates through"
echo "IMGACT.EXE with NO ld / NO ld.so — MMK joins TCC.EXE and LINK.EXE as a genuinely"
echo "activatable OVMX-native spine tool. The ACTIVATED image opens a real multi-TU"
echo "descrip.mms via OVMX RMS, parses it with the real lib\$table_parse engine, and"
echo "resolves the correct 4-compile -> archive -> link plan, byte-identical twice."
echo "MMK's persistent mailbox-driven DCL exec-drive (conditions 3-6: actually RUNNING"
echo "that plan) requires a real /dev/vms executive and is NOT attempted in this"
echo "container harness (see this file's CRUX comment above) — that rides the QEMU"
echo "test_syssvc_mmk_build.c suite, a follow-on item to point at THIS OVMX-native"
echo "MMK.EXE instead of the static mmk_native target it drives today."
echo "================================================================================"
