#!/bin/sh
# run_mmk_component_plan.sh -- MMK.EXE builds the PLAN for a REAL multi-TU OVMX
# component end to end (self-host spine #5, bead vms-fe4).
#
# Spine #4 (run_mmk_parse.sh) proved MMK plans a SINGLE-TU descrip.mms
# (one .C -> one .OBJ -> one .EXE).  This proves MMK plans a real MULTI-
# translation-unit component with a LIBRARY step: the OVMX freestanding runtime
# (the actual src/libvmssys vms_string.c / vms_snprintf.c / vms_math.c, Rule 3
# freestanding) plus a driver, described by tests/toolchain/component/OVMXRT.MMS.
#
# Veracity (Q1/Q2): a REAL end-to-end through the actual files MMK reads.
#   1. The committed OVMXRT.MMS + OVMXRTDRV.C + the three real runtime TUs are
#      staged into an isolated work dir (uppercased .C names, VMS style).
#   2. MMK.EXE reads OVMXRT.MMS through OVMX RMS, parses it with the real
#      lib$table_parse engine + the vms-486 PARSE_TABLES grammar + MMK's own
#      parse_store, expands the MMS macros ($(CFLAGS)/$(OBJS)/$(MMS$TARGET)/
#      $(MMS$SOURCE)), builds the target/dependency graph and, in /NOACTION,
#      emits the exact DCL command lines it WOULD drive -- in dependency order.
#   3. This test asserts the WHOLE plan:  all FOUR TCC compiles, THEN the
#      LIBRARIAN archive, THEN the LINK -- proving MMK really ordered a real
#      multi-TU + library build, not that a script faked it.  Compile-before-
#      archive-before-link is the plan a self-hosting `make` must produce.
#   4. DETERMINISM: MMK is run twice and the two plans are asserted
#      byte-identical (cmp) -- the plan a reproducible build depends on is stable.
#
# The independent oracle is OVMXRT.MMS itself: the expected command bodies are
# the TCC/LIBR/LNK lines the description file specifies, in the order its
# dependency graph dictates.
#
# SCOPE (honest, CLAUDE.md Rule 9 / INV-6).  This is the PARSE+PLAN half, run on
# the host with NO /dev/vms.  Turning the plan into a byte-identical built image
# rides MMK's persistent mailbox-driven DCL subprocess (ovmx_mmk_sp.c), which
# requires a real executive and is exercised in QEMU by
# tests/qemu/test_syssvc_mmk_build.c.  This test does NOT fake execution: in
# /NOACTION MMK executes nothing (its own documented dry-run mode).
#
# Inputs (env, set by CMake add_test): MMK_EXE, REPO_SRC (repo src/ dir).
# Exit 0 = success.
set -e

: "${MMK_EXE:?need MMK_EXE (built MMK.EXE)}"
[ -x "$MMK_EXE" ] || { echo "FAIL: MMK_EXE not executable: $MMK_EXE"; exit 1; }
: "${REPO_SRC:?need REPO_SRC (repo src/ dir)}"

HERE=$(cd "$(dirname "$0")" && pwd)
COMPONENT="$HERE/component"
LIBVMSSYS="$REPO_SRC/libvmssys"

for f in "$COMPONENT/OVMXRT.MMS" "$COMPONENT/OVMXRTDRV.C" \
         "$LIBVMSSYS/vms_string.c" "$LIBVMSSYS/vms_snprintf.c" "$LIBVMSSYS/vms_math.c"; do
    [ -f "$f" ] || { echo "FAIL: missing input $f"; exit 1; }
done

# MMK opens the description file through OVMX RMS, which resolves a bare filespec
# against the process default directory (cwd).  Work in an isolated temp dir.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# Stage the descrip.mms and the REAL component sources (VMS-style upper-case .C).
cp "$COMPONENT/OVMXRT.MMS" OVMXRT.MMS
cp "$COMPONENT/OVMXRTDRV.C" OVMXRTDRV.C
cp "$LIBVMSSYS/vms_string.c"   VMS_STRING.C
cp "$LIBVMSSYS/vms_snprintf.c" VMS_SNPRINTF.C
cp "$LIBVMSSYS/vms_math.c"     VMS_MATH.C
# /RULES defaults to MMS$RULES; provide an empty one so MMK does not warn about a
# missing default-rules file (keeps the final status clean), exactly as the
# spine #4 parse proof does.
printf '! empty default rules (vms-fe4 component plan proof)\n' > "MMS\$RULES"

echo "== OVMXRT.MMS (the committed descrip.mms) =="
sed 's/^/   /' OVMXRT.MMS

run_plan() {
    VMS_FOREIGN_CMD="/DESCRIPTION=OVMXRT.MMS /NOACTION OVMXRT.EXE" \
        "$MMK_EXE" < /dev/null 2>/dev/null
}

echo
echo "== MMK.EXE /DESCRIPTION=OVMXRT.MMS /NOACTION OVMXRT.EXE (build 1) =="
set +e
run_plan > plan1.txt
RC=$?
set -e
echo "-- MMK exit=$RC (odd VMS status = success); resolved plan: --"
sed 's/^/   /' plan1.txt

# VMS success status is ODD.
if [ "$RC" != "1" ] && [ "$RC" != "0" ]; then
    if [ $((RC & 1)) -eq 0 ]; then echo "FAIL: MMK exited with an even (failure) status $RC"; exit 1; fi
fi

# ---- assert the full multi-TU + library plan, in dependency order ----
line() { grep -nxF "$1" plan1.txt | head -1 | cut -d: -f1; }

C_DRV=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: OVMXRTDRV.C -o OVMXRTDRV.OBJ')
C_MTH=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: VMS_MATH.C -o VMS_MATH.OBJ')
C_STR=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: VMS_STRING.C -o VMS_STRING.OBJ')
C_SNP=$(line 'TCC -x c -c -ffreestanding -fno-builtin -I OVMX$INCLUDE: VMS_SNPRINTF.C -o VMS_SNPRINTF.OBJ')
L_LIB=$(grep -nE '^LIBR /CREATE OVMXRT.OLB VMS_MATH.OBJ VMS_STRING.OBJ VMS_SNPRINTF.OBJ$' plan1.txt | head -1 | cut -d: -f1)
L_LNK=$(grep -nE '^LNK --executable .* -o OVMXRT.EXE OVMXRTDRV.OBJ OVMXRT.OLB$' plan1.txt | head -1 | cut -d: -f1)

fails=0
need() { if [ -z "$2" ]; then echo "FAIL: $1"; fails=$((fails+1)); else echo "  PASS: $1 (plan line $2)"; fi; }
need "MMK resolved the TCC compile of the driver TU (OVMXRTDRV.C -> .OBJ, macros expanded)" "$C_DRV"
need "MMK resolved the TCC compile of VMS_MATH.C   -> VMS_MATH.OBJ"     "$C_MTH"
need "MMK resolved the TCC compile of VMS_STRING.C -> VMS_STRING.OBJ"   "$C_STR"
need "MMK resolved the TCC compile of VMS_SNPRINTF.C -> VMS_SNPRINTF.OBJ" "$C_SNP"
need "MMK resolved the LIBRARIAN archive of the three runtime objects -> OVMXRT.OLB" "$L_LIB"
need "MMK resolved the LINK of the driver against the .OLB -> OVMXRT.EXE (RTLIBS macro expanded)" "$L_LNK"
[ "$fails" -eq 0 ] || { echo "FAIL: MMK did not resolve the full component plan"; exit 1; }

# Dependency order: every compile precedes the archive; the archive precedes the
# link.  (The four compiles may appear in any order among themselves.)
maxc=0
for v in "$C_DRV" "$C_MTH" "$C_STR" "$C_SNP"; do [ "$v" -gt "$maxc" ] && maxc=$v; done
[ "$maxc" -lt "$L_LIB" ] || { echo "FAIL: plan order -- a compile did not precede the LIBRARIAN archive"; exit 1; }
[ "$L_LIB" -lt "$L_LNK" ] || { echo "FAIL: plan order -- the archive did not precede the LINK"; exit 1; }
echo "  PASS: dependency order -- all 4 compiles < LIBRARIAN archive < LINK"

# ---- determinism: a second run yields a byte-identical plan ----
echo
echo "== MMK.EXE (build 2): assert the plan is byte-identical (determinism) =="
set +e
run_plan > plan2.txt
set -e
if cmp -s plan1.txt plan2.txt; then
    echo "  PASS: the resolved plan is BYTE-IDENTICAL across two independent MMK runs (cmp clean)"
else
    echo "FAIL: MMK produced a non-deterministic plan across two runs:"; diff plan1.txt plan2.txt || true; exit 1
fi

echo
echo "================================================================================"
echo "MILESTONE (vms-fe4, self-host spine #5): MMK.EXE parsed a REAL multi-TU OVMX"
echo "component descrip.mms (OVMXRT.MMS -- the freestanding string/format/math runtime),"
echo "expanded its MMS macros, built the target/dependency graph and emitted the correct"
echo "build plan -- four TCC.EXE compiles, then a LIBRARIAN.EXE archive, then a LINK.EXE"
echo "link -- in dependency order, byte-identical across two runs.  ZERO bash in the plan"
echo "(MMK drives it).  Executing the plan into a byte-identical image rides the QEMU"
echo "mailbox-driven DCL (tests/qemu/test_syssvc_mmk_build.c)."
echo "================================================================================"
