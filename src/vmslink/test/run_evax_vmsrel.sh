#!/bin/sh
# run_evax_vmsrel.sh — integration test for LINK.EXE's EVAX/Alpha .vms$rel
# load-bias fixup table (bead vms-b5a0).
#
# THE GAP: a cc1-compiled program with an initialized global pointer
#   const char *g_ptr = greeting;      /* REFQUAD in $DATA$ -> greeting */
# links via LINK.EXE's EVAX path. Before this fix the image carried NO .vms$rel,
# so g_ptr held greeting's IMAGE-RELATIVE address UNBIASED — correct only at load
# bias 0. Under a non-zero IMGACT load bias B, greeting moves to B+off but g_ptr
# still held off -> a wrong pointer. The ELF path already emits .vms$rel; this
# proves the EVAX/Alpha path now does too, in the format IMGACT's generic
# apply_vms_rel (imgact.c) re-biases.
#
# VERIFY-FIRST (emit AND consume — a .vms$rel IMGACT would ignore must not pass):
#   PART A  gdata.obj (has a global pointer):
#     - EMIT:    the image gains a .vms$rel (magic 'REL1', count>=1) whose offset
#                list includes the slot holding g_ptr ($DATA$+0).
#     - CONSUME: replicate IMGACT apply_vms_rel's exact `*(base+off)+=base` walk
#                at a non-zero bias B and assert g_ptr resolves to B+greeting_off
#                (before bias == greeting_off; after == B+greeting_off).
#   PART B  gvalfold_ref.obj (`.quad C$_EXIT1`, a GLOBALVALUE folded to an
#           absolute link-time constant): the image gets NO .vms$rel entry — an
#           absolute constant is not image-relative, so it must not be biased.
#   PART C  link_main.obj --use a PROCEDURE producer: the cross-image import
#           patch_off slots (.vms$imp) are ABSENT from .vms$rel (IMGACT fills
#           those; biasing would double-count), while the section-relative slot
#           IS recorded.
#
# Architecture-independent: LINK.EXE's EVAX path + the verifier are pure byte
# manipulation, so this runs on the CI host with no Alpha toolchain. The EVAX
# .obj fixtures are checked in; gdata.obj was produced by the alpha-dec-vms
# binutils `as` from gdata.s (checked in beside it), itself cc1 output for
# gdata.c. Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
FIX="$HERE/evax-fixtures"
WORK=${WORK:-/tmp/evax-vmsrel-test}
BIAS=0x40000000
GVAL=0x0035A009
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE (host tool, ELF + EVAX paths) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"

echo "== build the .vms\$rel verifier =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/evax_vmsrel_verify" "$HERE/evax_vmsrel_verify.c"

echo
echo "== PART A: link gdata.obj (a global initialized pointer) =="
"$WORK/LINK.EXE" --transfer main -o "$WORK/gdata.exe" "$FIX/gdata.obj"
echo
echo "== verify EMIT + CONSUME: g_ptr survives load bias $BIAS =="
"$WORK/evax_vmsrel_verify" emit-consume "$WORK/gdata.exe" "$BIAS"

echo
echo "== PART B: a GLOBALVALUE fold must NOT be recorded in .vms\$rel =="
cat > "$WORK/dummy.c" <<'EOF'
int dummy(void) { return 0; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/dummy.o" "$WORK/dummy.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "C\$_EXIT1=GLOBALVALUE:$GVAL" \
    --gsmatch LEQUAL,1,0 -o "$WORK/CEXIT\$SHR.EXE" "$WORK/dummy.o"
"$WORK/LINK.EXE" --transfer MAIN --use "$WORK/CEXIT\$SHR.EXE" \
    -o "$WORK/ref_gvalfold.exe" "$FIX/gvalfold_ref.obj"
"$WORK/evax_vmsrel_verify" no-rel "$WORK/ref_gvalfold.exe"

echo
echo "== PART C: cross-image import slots must NOT be in .vms\$rel =="
cat > "$WORK/helper.c" <<'EOF'
int HELPER_PROC(void) { return 42; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/helper.o" "$WORK/helper.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "HELPER_PROC=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$WORK/HELPER\$SHR.EXE" "$WORK/helper.o"
"$WORK/LINK.EXE" --transfer MAIN_PROC --use "$WORK/HELPER\$SHR.EXE" \
    -o "$WORK/main_ximport.exe" "$FIX/link_main.obj"
"$WORK/evax_vmsrel_verify" import-exclude "$WORK/main_ximport.exe"

echo
echo "ALL EVAX .vms\$rel (vms-b5a0: emit + consume + exclusions) CHECKS PASSED"
