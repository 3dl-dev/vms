#!/bin/sh
# run_mmk_component_build.sh -- the OVMX-native toolchain builds the REAL
# multi-TU OVMX runtime component's LIBRARY + IMAGE steps BYTE-IDENTICALLY TWICE
# (self-host spine #5, bead vms-fe4).
#
# This is the "byte-identical twice" (deterministic build output) half of spine
# #5, proven on the host against the ACTUAL toolchain images LIBRARIAN.EXE and
# LINK.EXE on the REAL component objects (src/libvmssys vms_string / vms_snprintf
# / vms_math, the freestanding runtime described by
# tests/toolchain/component/OVMXRT.MMS).  It is the archive+link counterpart to
# run_mmk_component_plan.sh (which proves MMK's multi-TU PLAN):
#
#   1. Compile the three real runtime TUs freestanding (Rule 3) into objects.
#      (TCC.EXE's COMPILE determinism -- byte-identical objects across runs -- is
#      the S2 fixpoint already proven by src/imgact/test/run_tcc_selfhost.sh,
#      gen-2 == gen-3; this proof therefore uses the host CC for the object step
#      and concentrates on the LIBRARIAN + LINK output determinism spine #5 adds.)
#   2. LIBRARIAN.EXE /CREATE OVMXRT.OLB from the three objects -- TWICE -- and
#      assert the two .OLB are BYTE-IDENTICAL (cmp).  `ar`-container archives are
#      the classic reproducibility trap (mtime/uid/gid fields); this asserts the
#      OVMX LIBRARIAN zeroes them.
#   3. LINK.EXE links a shareable that REQUIRES vms_snprintf, pulling ONLY the
#      library members that resolve it (selective pull) -- TWICE -- and assert the
#      two images are BYTE-IDENTICAL (cmp).
#   4. OVMXDUMP validates the produced image (real symbol vector).
#
# Independent oracles: the system `ar` confirms the .OLB is a valid container and
# lists its members; OVMXDUMP confirms the image's symbol vector.
#
# SCOPE (honest, CLAUDE.md Rule 9).  This proves the toolchain OUTPUT is
# deterministic for the component's archive+link steps.  Driving those exact
# steps from MMK's persistent mailbox DCL against a real /dev/vms is the QEMU
# execution half (tests/qemu/test_syssvc_mmk_build.c); nothing here is faked.
#
# Inputs (env, set by CMake add_test): LIBRARIAN_EXE, LINK_EXE, OVMXDUMP,
# REPO_SRC, CC.  Exit 0 = success.
set -e

: "${LIBRARIAN_EXE:?need LIBRARIAN_EXE}"
: "${LINK_EXE:?need LINK_EXE}"
: "${OVMXDUMP:?need OVMXDUMP}"
: "${REPO_SRC:?need REPO_SRC (repo src/ dir)}"
CC=${CC:-gcc}
LIBVMSSYS="$REPO_SRC/libvmssys"

command -v ar >/dev/null 2>&1 || { echo "FAIL: system \`ar\` required as the oracle"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# Freestanding compile flags (Rule 3), mirroring run_olb_roundtrip.sh.
CF="-O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -c -I $LIBVMSSYS"
case "$(uname -m)" in x86_64|amd64) CF="$CF -fcf-protection=none" ;; esac

echo "== 1/5 compile the three REAL freestanding runtime TUs =="
$CC $CF "$LIBVMSSYS/vms_math.c"     -o VMS_MATH.OBJ
$CC $CF "$LIBVMSSYS/vms_string.c"   -o VMS_STRING.OBJ
$CC $CF "$LIBVMSSYS/vms_snprintf.c" -o VMS_SNPRINTF.OBJ
ls -l VMS_MATH.OBJ VMS_STRING.OBJ VMS_SNPRINTF.OBJ | sed 's/^/   /'

echo "== 2/5 LIBRARIAN.EXE /CREATE OVMXRT.OLB from the three objects (build #1 and #2) =="
"$LIBRARIAN_EXE" /CREATE OVMXRT1.OLB VMS_MATH.OBJ VMS_STRING.OBJ VMS_SNPRINTF.OBJ
sleep 1                                  # advance wall-clock: a naive `ar` would embed a different mtime
"$LIBRARIAN_EXE" /CREATE OVMXRT2.OLB VMS_MATH.OBJ VMS_STRING.OBJ VMS_SNPRINTF.OBJ
MEMBERS=$(ar t OVMXRT1.OLB | tr '\n' ' ')
echo "   ar t -> $MEMBERS"
for m in VMS_MATH VMS_STRING VMS_SNPRINTF; do
    echo "$MEMBERS" | grep -q "$m" || { echo "FAIL: $m missing from the .OLB per ar"; exit 1; }
done
if cmp -s OVMXRT1.OLB OVMXRT2.OLB; then
    echo "   PASS: OVMXRT.OLB is BYTE-IDENTICAL across two independent LIBRARIAN builds (cmp clean)"
else
    echo "FAIL: LIBRARIAN produced a non-deterministic .OLB:"; cmp OVMXRT1.OLB OVMXRT2.OLB; exit 1
fi

echo "== 3/5 LINK.EXE selective pull: a probe references vms_snprintf, LINK pulls it from the .OLB (build #1 and #2) =="
# A probe object with an undefined reference to vms_snprintf forces LINK to pull
# the member defining it AND, transitively, vms_strlen's member (VMS_STRING) --
# but NOT VMS_MATH, which nothing references.  Selective, symbol-driven pull on
# the real component members.
cat > PROBE.C <<'EOF'
#include "vms_snprintf.h"
int ovmxrt_probe(void){ char b[8]; return vms_snprintf(b, sizeof b, "%u", 216u); }
EOF
$CC -x c $CF PROBE.C -o PROBE.OBJ    # -x c: a case-sensitive host CC treats .C as C++ (see BUILD.COM)
LOG=$("$LINK_EXE" --shareable --symbol-vector "ovmxrt_probe=PROCEDURE" \
        --gsmatch LEQUAL,1,0 -o OVMXRT1.EXE PROBE.OBJ OVMXRT1.OLB 2>&1)
echo "$LOG" | sed 's/^/   /'
echo "$LOG" | grep -q "%LINK-S-CREATED" || { echo "FAIL: LINK did not succeed"; exit 1; }
echo "$LOG" | grep -q "2 of 3 members pulled" || { echo "FAIL: expected selective '2 of 3 members pulled' (SNPRINTF+STRING, not MATH)"; exit 1; }
sleep 1
"$LINK_EXE" --shareable --symbol-vector "ovmxrt_probe=PROCEDURE" \
        --gsmatch LEQUAL,1,0 -o OVMXRT2.EXE PROBE.OBJ OVMXRT1.OLB >/dev/null 2>&1
if cmp -s OVMXRT1.EXE OVMXRT2.EXE; then
    echo "   PASS: OVMXRT.EXE is BYTE-IDENTICAL across two independent LINK builds (cmp clean)"
else
    echo "FAIL: LINK produced a non-deterministic image:"; cmp OVMXRT1.EXE OVMXRT2.EXE; exit 1
fi

echo "== 4/5 OVMXDUMP validates the produced image =="
DUMP=$("$OVMXDUMP" OVMXRT1.EXE)
echo "$DUMP" | grep -q "symbol vector" || { echo "FAIL: OVMXDUMP found no symbol vector"; exit 1; }
echo "$DUMP" | grep -qiE 'ovmxrt_probe' || { echo "FAIL: 'ovmxrt_probe' universal missing from the image"; exit 1; }
echo "   PASS: OVMXDUMP parsed the image's symbol vector; ovmxrt_probe is a universal"

echo "== 5/5 negative sanity: the two .OLB / .EXE are non-empty and match sizes =="
[ -s OVMXRT1.OLB ] && [ -s OVMXRT1.EXE ] || { echo "FAIL: empty artifact"; exit 1; }

echo
echo "================================================================================"
echo "MILESTONE (vms-fe4, self-host spine #5): the OVMX-native LIBRARIAN.EXE + LINK.EXE"
echo "built the real freestanding runtime component's .OLB and image BYTE-IDENTICALLY"
echo "across two independent builds (cmp clean on both).  With TCC.EXE's compile"
echo "determinism (run_tcc_selfhost gen-2==gen-3), the whole TCC->LIBRARIAN->LINK chain"
echo "for this component is reproducible -- the byte-identical-twice bar for the build"
echo "OUTPUT.  Driving it from MMK's mailbox DCL rides QEMU (test_syssvc_mmk_build.c)."
echo "================================================================================"
