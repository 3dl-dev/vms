#!/bin/sh
# run_test_vax_exec.sh — LINKVAX.EXE elf32-vax --executable proof (vms-099,
# vms-404 P3b follow-on; the CONSUMER half P3a/P3b deferred).
#
# P3a (run_test_vax.sh) proved the SHAREABLE side: LINKVAX.EXE (src/vmslink/
# link.c -DOVMX_LINK_ELF32) emits elf32-vax `.vms$sv` shareables. It stopped at
# the executable — `--executable` failed honestly ("%LINK-F-ERROR, elf32-vax
# --executable emit is vms-404 P3b/P4, not P3a") — so there was no genuine
# elf32-vax PT_INTERP=IMGACT.EXE CONSUMER that imports a `.vms$sv` producer.
# This script proves that gap is closed.
#
# Ground-source done condition (readelf-shape, NOT runtime — the SIMH
# activation proof is P4/vms-d4a): a SEPARATE compile of link.c with
# -DOVMX_LINK_ELF32 (LINKVAX.EXE) links an elf32-vax `main()` object against a
# `.vms$sv` producer shareable and emits a valid elf32-vax EXECUTABLE that
# readelf-verifies as:
#   * ELF32 / Digital VAX / ET_DYN (static-PIE-style, biased at activation);
#   * a real entry point whose bytes are the synthesized VAX crt0 (recovers
#     argc/argv/envp off the initial process stack, `calls $3, main`, then
#     `calls $1, exit`);
#   * PT_INTERP == IMGACT.EXE  (NEVER ld.elf_so — the rejected LARP);
#   * a `.vms$imp` that references the producer shareable (LIBVAXPROD$SHR.EXE);
#   * every R_VAX_* relocation APPLIED in-image (no leftover .rela.dyn / dynamic
#     R_VAX_* — an OVMX image is self-contained, biased via .vms$rel by IMGACT).
#
# Additive-gate regression: the DEFAULT (no-macro) build of the SAME source
# still emits an unchanged x86_64 ELF64 --executable — so the elf32-vax
# executable path is purely additive and the LP64 path is byte-unchanged.
#
# Runs inside the ovmx-cross-vax container (host gcc + vax--netbsdelf-* present);
# mirrors src/vmslink/test/run_test_vax.sh in shape.
set -e
CC=${CC:-gcc}
VAXCC=${VAXCC:-vax--netbsdelf-gcc}
VAXREADELF=${VAXREADELF:-vax--netbsdelf-readelf}
VAXOBJDUMP=${VAXOBJDUMP:-vax--netbsdelf-objdump}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/vmslink-test-vax-exec}
rm -rf "$WORK"; mkdir -p "$WORK"

VAXCFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector"

echo "== build LINKVAX.EXE (-DOVMX_LINK_ELF32) + the default LINK.EXE (host tools) =="
$CC -std=gnu11 -O2 -Wall -Wextra -DOVMX_LINK_ELF32 -I"$SRC/include" \
    -o "$WORK/LINKVAX.EXE" "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" \
    -o "$WORK/LINK.EXE"    "$SRC/link.c"

echo
echo "== compile elf32-vax producer + consumer-executable objects (cross-vax toolchain) =="
# Producer shareable: exports prod_add (PROCEDURE the consumer's main() imports),
# prod_counter (DATA), and exit (PROCEDURE — the crt0's terminator, bound as an
# import exactly as the x86_64/aarch64 crt0 binds exit() from DECC$SHR).
cat > "$WORK/vaxprod.c" <<'EOF'
int prod_counter = 90;                          /* exported DATA universal        */
void exit(int code) { (void)code; }             /* exported PROCEDURE (crt0 tail)  */
int prod_add(int x) { return x + prod_counter; }/* PROCEDURE; GOT32 to prod_counter*/
EOF
# Consumer EXECUTABLE: a real main() -> synthesized crt0. main() calls the
# imported prod_add (R_VAX_PLT32 -> PLT stub -> .vms$imp) and the crt0 tail-calls
# the imported exit. Both imports bind to the producer via .vms$imp.
cat > "$WORK/vaxmain.c" <<'EOF'
extern int prod_add(int);                       /* imported PROCEDURE (R_VAX_PLT32)*/
int main(int argc, char **argv) {
    (void)argv;
    return prod_add(argc);                      /* cross-image call -> PLT/.vms$imp*/
}
EOF
$VAXCC $VAXCFLAGS -c -o "$WORK/vaxprod.o" "$WORK/vaxprod.c"
$VAXCC $VAXCFLAGS -c -o "$WORK/vaxmain.o" "$WORK/vaxmain.c"

echo "-- consumer main() .o relocations (expect an R_VAX_PLT32 call to prod_add) --"
$VAXREADELF -r "$WORK/vaxmain.o" | awk '/R_VAX/{print $3}' | sort | uniq -c
$VAXREADELF -r "$WORK/vaxmain.o" | grep -q "R_VAX_PLT32" \
    || { echo "FAIL: consumer main() lacks the expected R_VAX_PLT32 call reloc"; exit 1; }

echo
echo "== LINKVAX.EXE --shareable : producer elf32-vax .vms\$sv shareable =="
"$WORK/LINKVAX.EXE" --shareable \
    --symbol-vector "prod_add=PROCEDURE,prod_counter=DATA,exit=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBVAXPROD\$SHR.EXE" "$WORK/vaxprod.o"

echo
echo "== LINKVAX.EXE --executable --use : elf32-vax CONSUMER image (crt0 + PT_INTERP) =="
"$WORK/LINKVAX.EXE" --executable --use "$WORK/LIBVAXPROD\$SHR.EXE" \
    -o "$WORK/VAXCONS.EXE" "$WORK/vaxmain.o"

# ---- the vms-099 done-condition assertions (readelf shape) ----------------
IMG="$WORK/VAXCONS.EXE"
echo
echo "== ASSERT: elf32-vax ET_DYN executable header =="
$VAXREADELF -h "$IMG" | grep -E "Class:|Data:|Type:|Machine:|Entry"
$VAXREADELF -h "$IMG" | grep -q "Class:.*ELF32" \
    || { echo "FAIL: consumer is not ELF32"; exit 1; }
$VAXREADELF -h "$IMG" | grep -qi "Digital VAX" \
    || { echo "FAIL: consumer is not Digital VAX (EM_VAX)"; exit 1; }
$VAXREADELF -h "$IMG" | grep -qE "Type:.*DYN" \
    || { echo "FAIL: consumer is not ET_DYN (static-PIE-style)"; exit 1; }
# A real, nonzero entry point (the synthesized crt0).
ENTRY=$($VAXREADELF -h "$IMG" | awk '/Entry point/{print $NF}')
[ -n "$ENTRY" ] && [ "$ENTRY" != "0x0" ] \
    || { echo "FAIL: consumer has no entry point"; exit 1; }
echo "   OK: ELF32 / Digital VAX / ET_DYN, entry=$ENTRY"

echo
echo "== ASSERT: PT_INTERP == IMGACT.EXE (NOT ld.elf_so) =="
$VAXREADELF -lW "$IMG" | grep -iE "INTERP|interpreter"
INTERP=$($VAXREADELF -lW "$IMG" | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p')
echo "   interp = $INTERP"
echo "$INTERP" | grep -q "IMGACT.EXE" \
    || { echo "FAIL: PT_INTERP is not IMGACT.EXE (got '$INTERP')"; exit 1; }
if echo "$INTERP" | grep -q "ld.elf_so"; then
    echo "FAIL: PT_INTERP is ld.elf_so — the rejected LARP, not the OVMX activator"; exit 1
fi
echo "   OK: PT_INTERP=IMGACT.EXE (the OVMX image activator, not ld.elf_so)"

echo
echo "== ASSERT: PT_NOTE carries the NetBSD OS-version note (rd vms-d4a) =="
# The NetBSD kernel execs an ELF executable only when a PT_NOTE carries this
# note (sys/kern/exec_elf.c netbsd_elf_signature); without it execve() is
# ENOEXEC and IMGACT.EXE is never reached.
$VAXREADELF -lW "$IMG" | grep -q "NOTE" \
    || { echo "FAIL: no PT_NOTE program header"; exit 1; }
$VAXREADELF -nW "$IMG" | grep -E "NetBSD.*IDENT 1001000000" \
    || { echo "FAIL: no NetBSD IDENT note (namesz 7, type 1, version 1001000000)"; $VAXREADELF -nW "$IMG"; exit 1; }
echo "   OK: PT_NOTE NetBSD IDENT 10.1"

echo
echo "== ASSERT: crt0 at the entry point (VAX argc/argv recovery + calls main + calls exit) =="
# The entry bytes must be the synthesized VAX crt0: it opens with `movl (sp),r0`
# (opcode 0xd0, the argc load) and issues two `calls` (opcode 0xfb) — one to
# main, one to the exit import's PLT stub. Read the raw entry bytes via readelf
# -x on .text (objdump mis-decodes the leading entry-mask words as data).
$VAXOBJDUMP -d --start-address="$ENTRY" \
    --stop-address="$(printf '0x%x' $(( ENTRY + 40 )))" "$IMG" 2>/dev/null \
    | grep -E "movl \(sp\),r0|calls|pushl|movab|moval" | tee "$WORK/crt0.dis"
grep -q "movl (sp),r0" "$WORK/crt0.dis" \
    || { echo "FAIL: entry does not begin with the crt0 argc load (movl (sp),r0)"; exit 1; }
[ "$(grep -c "calls" "$WORK/crt0.dis")" -ge 2 ] \
    || { echo "FAIL: crt0 lacks the two calls (main + exit)"; exit 1; }
echo "   OK: entry point is the synthesized VAX crt0 (argc/argv recovery + calls main + calls exit)"

echo
echo "== ASSERT: .vms\$imp references the producer shareable + PLT/.igot present =="
for sec in '.vms$imp' '.plt' '.igot'; do
    $VAXREADELF -SW "$IMG" | grep -qF "$sec" \
        || { echo "FAIL: consumer executable missing section $sec"; exit 1; }
done
$VAXREADELF -x '.vms$imp' "$IMG" | grep -qa "LIBVAXPROD" \
    || { echo "FAIL: .vms\$imp does not reference the producer LIBVAXPROD\$SHR.EXE"; exit 1; }
echo "   OK: .vms\$imp binds to LIBVAXPROD\$SHR.EXE; .plt + .igot present (IMGACT fills at activation)"

echo
echo "== ASSERT: every call-import PLT stub enters the callee PAST its entry mask (rd vms-d4a) =="
# The import cell IMGACT fills holds the producer's procedure address (its
# entry-mask word). CALLS consumes the STUB's mask, so the stub must load the
# cell and jump to cell+2; a `jmp *cell` executes the callee's mask word as an
# instruction (the SIGSEGV the SIMH gate saw). Each 12-byte slot must be:
#   fc 0f | d0 ef <disp32> 50 | 17 a0 02
# with disp32 landing inside .igot (PC after the field = slot + 8).
PLT_LINE=$($VAXREADELF -SW "$IMG" | sed -n 's/.*\] \.plt  *PROGBITS  *\([0-9a-f]*\) [0-9a-f]* \([0-9a-f]*\).*/\1 \2/p')
IGOT_LINE=$($VAXREADELF -SW "$IMG" | sed -n 's/.*\] \.igot  *PROGBITS  *\([0-9a-f]*\) [0-9a-f]* \([0-9a-f]*\).*/\1 \2/p')
[ -n "$PLT_LINE" ] && [ -n "$IGOT_LINE" ] \
    || { echo "FAIL: could not read .plt/.igot addresses"; $VAXREADELF -SW "$IMG"; exit 1; }
PLT_VA=$((0x${PLT_LINE% *})); PLT_SZ=$((0x${PLT_LINE#* }))
IGOT_VA=$((0x${IGOT_LINE% *})); IGOT_SZ=$((0x${IGOT_LINE#* }))
[ "$PLT_SZ" -gt 0 ] && [ $((PLT_SZ % 12)) -eq 0 ] \
    || { echo "FAIL: .plt size $PLT_SZ is not a whole number of 12-byte slots"; exit 1; }
"${VAXOBJCOPY:-vax--netbsdelf-objcopy}" -O binary --only-section=.plt "$IMG" "$WORK/plt.bin"
NSLOT=0
off=0
while [ "$off" -lt "$PLT_SZ" ]; do
    hex=$(od -An -tx1 -v -j "$off" -N 12 "$WORK/plt.bin" | tr -d ' \n')
    case "$hex" in
        fc0fd0ef????????5017a002) ;;
        *) echo "FAIL: PLT slot at 0x$(printf %x $((PLT_VA + off))) is '$hex', want fc0fd0ef<disp32>5017a002"; exit 1 ;;
    esac
    # disp32, little-endian, signed: bytes 4..7 of the slot.
    d=$(echo "$hex" | cut -c9-16 | sed 's/\(..\)\(..\)\(..\)\(..\)/\4\3\2\1/')
    d=$((0x$d)); [ "$d" -ge 2147483648 ] && d=$((d - 4294967296))
    cell=$((PLT_VA + off + 8 + d))
    [ "$cell" -ge "$IGOT_VA" ] && [ "$cell" -lt $((IGOT_VA + IGOT_SZ)) ] \
        || { echo "FAIL: PLT slot at 0x$(printf %x $((PLT_VA + off))) loads 0x$(printf %x $cell), outside .igot"; exit 1; }
    NSLOT=$((NSLOT + 1)); off=$((off + 12))
done
echo "   OK: $NSLOT PLT stub(s) = .word 0x0ffc; movl <.igot cell>,r0; jmp 2(r0)"

echo
echo "== ASSERT: self-contained — NO unapplied dynamic R_VAX_* relocations =="
# An OVMX image carries its load-bias fixups in .vms$rel (applied by IMGACT), not
# ELF dynamic relocations. A stray R_VAX_* here would mean LINK left a reloc
# UNAPPLIED (an ld.elf_so-style .so, the rejected shape).
if $VAXREADELF -r "$IMG" 2>/dev/null | grep -q "R_VAX"; then
    echo "FAIL: consumer executable carries UNAPPLIED R_VAX_* dynamic relocations"; exit 1
fi
echo "   OK: no dynamic R_VAX_* — all R_VAX_* relocs applied in-image"

echo
echo "== additive-gate regression: the DEFAULT build still emits an EM_X86_64 ELF64 --executable =="
# Prove the elf32-vax executable path is ADDITIVE: the same link.c compiled
# WITHOUT the macro links an x86_64 main() to an unchanged ELF64 executable.
cat > "$WORK/x86prod.c" <<'EOF'
int x_counter = 5;
void exit(int code) { (void)code; }
int x_add(int a) { return a + x_counter; }
EOF
cat > "$WORK/x86main.c" <<'EOF'
extern int x_add(int);
int main(int argc, char **argv) { (void)argv; return x_add(argc); }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/x86prod.o" "$WORK/x86prod.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/x86main.o" "$WORK/x86main.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "x_add=PROCEDURE,x_counter=DATA,exit=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBX86\$SHR.EXE" "$WORK/x86prod.o"
"$WORK/LINK.EXE" --executable --use "$WORK/LIBX86\$SHR.EXE" \
    -o "$WORK/X86CONS.EXE" "$WORK/x86main.o"
readelf -h "$WORK/X86CONS.EXE" | grep -E "Class:|Machine:|Type:"
readelf -h "$WORK/X86CONS.EXE" | grep -q "ELF64" \
    || { echo "FAIL: default build no longer emits an ELF64 executable"; exit 1; }
readelf -h "$WORK/X86CONS.EXE" | grep -q "X86-64" \
    || { echo "FAIL: default build no longer emits EM_X86_64"; exit 1; }
readelf -lW "$WORK/X86CONS.EXE" | grep -qi "IMGACT.EXE" \
    || { echo "FAIL: default x86_64 executable lost PT_INTERP=IMGACT.EXE"; exit 1; }
echo "   OK: default (no-macro) build unchanged — EM_X86_64 / ELF64 executable, PT_INTERP=IMGACT.EXE"

echo
echo "ALL LINKVAX.EXE elf32-vax --executable CHECKS PASSED"
