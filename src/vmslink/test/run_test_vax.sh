#!/bin/sh
# run_test_vax.sh — LINK.EXE elf32-vax .vms$sv shareable proof (vms-19b / vms-404
# P3a; docs/design/vax-symbol-vector-imgact.md §1c "R3").
#
# Ground-source done condition (readelf-shape, NOT runtime — P4 owns the SIMH
# activation proof): a SEPARATE compile of the same src/vmslink/link.c with
# -DOVMX_LINK_ELF32 (LINKVAX.EXE) consumes elf32-vax .o's from the cross-vax
# binutils/gcc toolchain and emits a valid elf32-vax ET_DYN OVMX shareable that
# carries a `.vms$sv` symbol vector + `.vms$imp` imports, with every R_VAX_*
# relocation (PC32 / PLT32 / GOT32 / R_VAX_32) APPLIED in-image (no leftover
# .rela.dyn — an OVMX shareable is self-contained, biased at activation via
# .vms$rel, NOT an ld.elf_so .so). Proven with vax--netbsdelf-readelf.
#
# Additive-gate regression: the DEFAULT (no-macro) build of the SAME source, run
# on an x86_64 object in the same script, still emits an EM_X86_64 ELF64 image —
# so the elf32-vax path is purely additive and the LP64 path is byte-unchanged.
#
# Runs inside the ovmx-cross-vax container (host gcc + vax--netbsdelf-* + generic
# binutils all present); mirrors src/vmslink/test/run_test_x86_64.sh in shape.
set -e
CC=${CC:-gcc}
VAXCC=${VAXCC:-vax--netbsdelf-gcc}
VAXREADELF=${VAXREADELF:-vax--netbsdelf-readelf}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/vmslink-test-vax}
rm -rf "$WORK"; mkdir -p "$WORK"

VAXCFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector"

echo "== build LINKVAX.EXE (-DOVMX_LINK_ELF32) + the default LINK.EXE (host tools) =="
$CC -std=gnu11 -O2 -Wall -Wextra -DOVMX_LINK_ELF32 -I"$SRC/include" \
    -o "$WORK/LINKVAX.EXE" "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/LINK.EXE"    "$SRC/link.c"

echo
echo "== compile elf32-vax producer + consumer objects (cross-vax toolchain) =="
# Producer: a PROCEDURE universal (prod_add) whose body reads a DATA universal
# (prod_counter) GOT-indirect -> exercises INTRA-image R_VAX_GOT32 + .vms$rel.
cat > "$WORK/vaxprod.c" <<'EOF'
int prod_counter = 90;                         /* exported DATA universal        */
int prod_add(int x) { return x + prod_counter; } /* PROCEDURE; GOT32 to prod_counter */
EOF
# Consumer: imports prod_add (call -> R_VAX_PLT32 -> .vms$imp proc + PLT stub) and
# prod_counter (R_VAX_GOT32 -> .vms$imp DATA import); has an intra-image absolute
# pointer initializer (R_VAX_32 -> .vms$rel), a local static call (R_VAX_PLT32/PC32
# intra-link, no stub), and a .rodata string ref (R_VAX_PC32).
cat > "$WORK/vaxcons.c" <<'EOF'
extern int prod_add(int);               /* imported PROCEDURE (R_VAX_PLT32)      */
extern int prod_counter;                /* imported DATA      (R_VAX_GOT32)      */
static int loc_helper(int x) { return x * 2; }
int (*loc_fp)(int) = loc_helper;        /* R_VAX_32 absolute pointer init        */
static const int loc_tbl[4] = {11, 22, 33, 44};
int cons_pick(int i) { return loc_tbl[i & 3]; }   /* R_VAX_PC32 to local .rodata  */
int cons_run(int x) {
    return prod_add(x) + prod_counter + loc_fp(x) + cons_pick(x);
}
EOF
$VAXCC $VAXCFLAGS -c -o "$WORK/vaxprod.o" "$WORK/vaxprod.c"
$VAXCC $VAXCFLAGS -c -o "$WORK/vaxcons.o" "$WORK/vaxcons.c"

echo "-- producer .o relocations (expect GOT32 to prod_counter) --"
$VAXREADELF -r "$WORK/vaxprod.o" | awk '/R_VAX/{print $3}' | sort | uniq -c
$VAXREADELF -r "$WORK/vaxprod.o" | grep -q "R_VAX_GOT32" \
    || { echo "FAIL: producer lacks the expected intra-image R_VAX_GOT32"; exit 1; }
echo "-- consumer .o relocations (expect PLT32 + GOT32 + R_VAX_32 + PC32) --"
$VAXREADELF -r "$WORK/vaxcons.o" | awk '/R_VAX/{print $3}' | sort | uniq -c
for rt in R_VAX_PLT32 R_VAX_GOT32 R_VAX_32 R_VAX_PC32; do
    $VAXREADELF -r "$WORK/vaxcons.o" | grep -q "$rt" \
        || { echo "FAIL: consumer .o lacks an expected $rt reloc"; exit 1; }
done

echo
echo "== LINKVAX.EXE --shareable : producer elf32-vax .vms\$sv shareable =="
"$WORK/LINKVAX.EXE" --shareable \
    --symbol-vector "prod_add=PROCEDURE,prod_counter=DATA" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBVAXPROD\$SHR.EXE" "$WORK/vaxprod.o"

echo
echo "== LINKVAX.EXE --shareable --use : consumer imports across the shareable boundary =="
"$WORK/LINKVAX.EXE" --shareable --use "$WORK/LIBVAXPROD\$SHR.EXE" \
    --symbol-vector "cons_run=PROCEDURE,cons_pick=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBVAXCONS\$SHR.EXE" "$WORK/vaxcons.o"

# ---- the P3a done-condition assertions (readelf shape) --------------------
assert_vax_etdyn() {
    img="$1"; label="$2"
    echo "-- $label header --"
    $VAXREADELF -h "$img" | grep -E "Class:|Data:|Type:|Machine:"
    $VAXREADELF -h "$img" | grep -q "Class:.*ELF32" \
        || { echo "FAIL: $label is not ELF32"; exit 1; }
    $VAXREADELF -h "$img" | grep -qi "Digital VAX" \
        || { echo "FAIL: $label is not Digital VAX (EM_VAX)"; exit 1; }
    $VAXREADELF -h "$img" | grep -qE "Type:.*DYN" \
        || { echo "FAIL: $label is not ET_DYN"; exit 1; }
    # An OVMX shareable is self-contained: it carries NO ELF dynamic relocations
    # (its load-bias fixups live in .vms$rel, applied by IMGACT, not ld.elf_so).
    # A stray R_VAX_* here would mean LINK left a reloc UNAPPLIED.
    if $VAXREADELF -r "$img" 2>/dev/null | grep -q "R_VAX"; then
        echo "FAIL: $label carries UNAPPLIED R_VAX_* dynamic relocations"; exit 1
    fi
    echo "   OK: $label is a self-contained elf32-vax ET_DYN, relocs applied"
}

echo
echo "== ASSERT: producer shareable shape =="
assert_vax_etdyn "$WORK/LIBVAXPROD\$SHR.EXE" "LIBVAXPROD\$SHR.EXE"
$VAXREADELF -SW "$WORK/LIBVAXPROD\$SHR.EXE" | grep -qE '\.vms\$sv' \
    || { echo "FAIL: producer missing .vms\$sv"; exit 1; }
# The exported universal names live only in .vms$sv.
$VAXREADELF -x '.vms$sv' "$WORK/LIBVAXPROD\$SHR.EXE" | grep -qa "prod_add" \
    || { echo "FAIL: producer .vms\$sv does not export prod_add"; exit 1; }
echo "   OK: producer exports prod_add/prod_counter via .vms\$sv (+ .got/.vms\$rel for the intra GOT32)"

echo
echo "== ASSERT: consumer shareable shape (.vms\$sv + .vms\$imp imports) =="
assert_vax_etdyn "$WORK/LIBVAXCONS\$SHR.EXE" "LIBVAXCONS\$SHR.EXE"
for sec in '.vms$sv' '.vms$imp' '.vms$rel' '.plt' '.igot'; do
    $VAXREADELF -SW "$WORK/LIBVAXCONS\$SHR.EXE" | grep -qF "$sec" \
        || { echo "FAIL: consumer missing section $sec"; exit 1; }
done
# .vms$imp names the producer this consumer binds to (LIBVAXPROD$SHR.EXE).
$VAXREADELF -x '.vms$imp' "$WORK/LIBVAXCONS\$SHR.EXE" | grep -qa "LIBVAXPROD" \
    || { echo "FAIL: consumer .vms\$imp does not reference the producer LIBVAXPROD\$SHR.EXE"; exit 1; }
$VAXREADELF -x '.vms$sv' "$WORK/LIBVAXCONS\$SHR.EXE" | grep -qa "cons_run" \
    || { echo "FAIL: consumer .vms\$sv does not export cons_run"; exit 1; }
echo "   OK: consumer is elf32-vax ET_DYN with .vms\$sv (cons_run) + .vms\$imp (-> LIBVAXPROD\$SHR.EXE) + .plt/.igot/.vms\$rel"

echo
echo "== additive-gate regression: the DEFAULT build still emits EM_X86_64 ELF64 =="
# Prove the elf32-vax path is ADDITIVE, not a replacement: the same link.c
# compiled WITHOUT the macro links an x86_64 object to an unchanged ELF64 image.
cat > "$WORK/x86leaf.c" <<'EOF'
int leaf_add(int a, int b) { return a + b; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/x86leaf.o" "$WORK/x86leaf.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "leaf_add=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBX86\$SHR.EXE" "$WORK/x86leaf.o"
readelf -h "$WORK/LIBX86\$SHR.EXE" | grep -E "Class:|Machine:"
readelf -h "$WORK/LIBX86\$SHR.EXE" | grep -q "ELF64" \
    || { echo "FAIL: default build no longer emits ELF64"; exit 1; }
readelf -h "$WORK/LIBX86\$SHR.EXE" | grep -q "X86-64" \
    || { echo "FAIL: default build no longer emits EM_X86_64"; exit 1; }
echo "   OK: default (no-macro) build unchanged — EM_X86_64 / ELF64"

echo
echo "== negative control: LINKVAX.EXE must REJECT an ELF64 (x86_64) object =="
# The elf32-vax build must refuse a foreign-class object rather than mis-parse it.
set +e
"$WORK/LINKVAX.EXE" --shareable --symbol-vector "leaf_add=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/BADVAX\$SHR.EXE" "$WORK/x86leaf.o" 2>"$WORK/reject.err"
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "FAIL: LINKVAX.EXE accepted an ELF64 object"; exit 1; }
grep -qi "ELF class" "$WORK/reject.err" \
    || { echo "FAIL: wrong rejection reason:"; cat "$WORK/reject.err"; exit 1; }
echo "   OK: LINKVAX.EXE rejected the ELF64 object ($(cat "$WORK/reject.err"))"

echo
echo "ALL LINK.EXE elf32-vax .vms\$sv SHAREABLE CHECKS PASSED"
