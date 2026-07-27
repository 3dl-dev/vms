#!/bin/sh
# run_test.sh — LINK.EXE MVP proof harness (bead vms-9dd).
#
# Builds LINK.EXE + OVMXDUMP, compiles a leaf object with gcc, links it into an
# OVMX shareable image with a symbol vector, and asserts the image is a valid
# ET_DYN whose .vms$sv exports the declared universal symbols with the right
# kinds and GSMATCH. Proves LINK.EXE produces OVMX images WITHOUT ld.
#
# Runs in any gcc environment (host or musl container). Exit 0 only on success.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/vmslink-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE + OVMXDUMP =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE"     "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/OVMXDUMP"     "$SRC/dump_image.c"

echo "== compile a leaf object (gcc -fPIC) =="
cat > "$WORK/math.c" <<'EOF'
int myadd(int a, int b) { return a + b; }
int mymul(int a, int b) { return a * b; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/math.o" "$WORK/math.c"

echo "== LINK.EXE: math.o -> LIBMATH\$SHR.EXE (VMS-native, no ld) =="
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "myadd=PROCEDURE,mymul=PROCEDURE" \
    --gsmatch LEQUAL,1,1000 \
    -o "$WORK/LIBMATH\$SHR.EXE" "$WORK/math.o"

echo
echo "== readelf: must be ET_DYN with a .vms\$sv section =="
readelf -hS "$WORK/LIBMATH\$SHR.EXE" | grep -E "Type:|\.vms" || true
readelf -h "$WORK/LIBMATH\$SHR.EXE" | grep -q "DYN" || { echo "FAIL: not ET_DYN"; exit 1; }
readelf -S "$WORK/LIBMATH\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: no .vms\$sv"; exit 1; }

echo
echo "== OVMXDUMP: symbol vector contents =="
OUT=$("$WORK/OVMXDUMP" "$WORK/LIBMATH\$SHR.EXE")
echo "$OUT"

echo
echo "== assertions =="
echo "$OUT" | grep -q "GSMATCH        : LEQUAL,1,1000" || { echo "FAIL: GSMATCH"; exit 1; }
echo "$OUT" | grep -q "symbol vector  : 2 entries"     || { echo "FAIL: count"; exit 1; }
echo "$OUT" | grep -qE '\[  0\] PROCEDURE .* myadd'    || { echo "FAIL: slot 0 myadd"; exit 1; }
echo "$OUT" | grep -qE '\[  1\] PROCEDURE .* mymul'    || { echo "FAIL: slot 1 mymul"; exit 1; }
# Universal-symbol values must be nonzero image-relative addresses.
echo "$OUT" | grep -qE 'value=0x0{16}' && { echo "FAIL: zero symbol value"; exit 1; } || true

echo
echo "== non-leaf producer: .rodata + PC-relative data relocations (vms-20b) =="
echo "   (ADR_PREL_PG_HI21 + ADD_ABS_LO12_NC; same-section calls are assembler-"
echo "    resolved, so CALL26/JUMP26 relocs are exercised by the multi-object step)"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/CALLSLOT" "$SRC/test/call_slot.c"
cat > "$WORK/nonleaf.c" <<'EOF'
static const int TABLE[4] = { 10, 20, 30, 40 };   /* .rodata */
/* noinline forces a real local BL -> exercises R_AARCH64_CALL26 */
static __attribute__((noinline)) int scale(int x) { return x * 3; }
/* reads .rodata (ADRP/ADD) AND calls a local function (CALL26) */
int lookup(int i, int unused) { (void)unused; return scale(TABLE[i & 3]); }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/nonleaf.o" "$WORK/nonleaf.c"
echo "-- relocations gcc emitted against .text --"
readelf -rW "$WORK/nonleaf.o" | awk '/R_AARCH64/{print $3}' | sort | uniq -c
"$WORK/LINK.EXE" --shareable --symbol-vector "lookup=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBLOOK\$SHR.EXE" "$WORK/nonleaf.o"
set +e
"$WORK/CALLSLOT" "$WORK/LIBLOOK\$SHR.EXE" 0 2 0; RC=$?    # lookup(2)=scale(TABLE[2])=30*3=90
set -e
echo "lookup(2) exit = $RC (expect 90 = scale(TABLE[2]=30))"
[ "$RC" -eq 90 ] || { echo "FAIL: non-leaf producer relocations wrong (got $RC, want 90)"; exit 1; }

echo
echo "== multi-object producer: cross-object call (R_AARCH64_CALL26) (vms-20b) =="
cat > "$WORK/a.c" <<'EOF'
extern int helper(int);                 /* defined in b.o -> cross-object CALL26 */
int dispatch(int x, int y) { (void)y; return helper(x) + 1; }
EOF
cat > "$WORK/b.c" <<'EOF'
int helper(int x) { return x * 10; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/a.o" "$WORK/a.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/b.o" "$WORK/b.c"
echo "-- a.o .text relocations (expect a CALL26 to helper) --"
readelf -rW "$WORK/a.o" | awk '/R_AARCH64/{print $3}' | sort | uniq -c
"$WORK/LINK.EXE" --shareable --symbol-vector "dispatch=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIB2\$SHR.EXE" "$WORK/a.o" "$WORK/b.o"
set +e
"$WORK/CALLSLOT" "$WORK/LIB2\$SHR.EXE" 0 5 0; RC=$?    # dispatch(5)=helper(5)+1=51
set -e
echo "dispatch(5) exit = $RC (expect 51 = helper(5)*... 5*10+1)"
[ "$RC" -eq 51 ] || { echo "FAIL: cross-object CALL26 wrong (got $RC, want 51)"; exit 1; }

echo
echo "== GOT producer: global-via-GOT synthesizes .got + .data/.bss + .vms\$rel (vms-20b) =="
echo "   (ADR_GOT_PAGE/LD64_GOT_LO12_NC -> a synthesized GOT cell; each cell holds an"
echo "    image-relative address recorded in .vms\$rel for +load_bias at activation.)"
cat > "$WORK/gotvar.c" <<'EOF'
int g_base = 100;   /* .data — read via GOT */
int g_zero;         /* .bss  — read via GOT */
int addbase(int a, int b) { return g_base + g_zero + a + b; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/gotvar.o" "$WORK/gotvar.c"
echo "-- GOT relocations gcc emitted against .text --"
readelf -rW "$WORK/gotvar.o" | awk '/GOT/{print $3}' | sort | uniq -c
"$WORK/LINK.EXE" --shareable --symbol-vector "addbase=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBGOT\$SHR.EXE" "$WORK/gotvar.o"
echo "-- producer sections (expect .got, .data, .bss, .vms\$rel) --"
readelf -SW "$WORK/LIBGOT\$SHR.EXE" | grep -E '\.got|\.data|\.bss|\.vms' || true
readelf -SW "$WORK/LIBGOT\$SHR.EXE" | grep -q '\.got'      || { echo "FAIL: no .got synthesized"; exit 1; }
readelf -SW "$WORK/LIBGOT\$SHR.EXE" | grep -q '\.data'     || { echo "FAIL: no .data merged"; exit 1; }
readelf -SW "$WORK/LIBGOT\$SHR.EXE" | grep -q '\.bss'      || { echo "FAIL: no .bss merged"; exit 1; }
readelf -SW "$WORK/LIBGOT\$SHR.EXE" | grep -q '\.vms\$rel' || { echo "FAIL: no .vms\$rel emitted"; exit 1; }
# The PT_LOAD carrying a writable GOT/.data must be RWX.
readelf -lW "$WORK/LIBGOT\$SHR.EXE" | grep -E 'LOAD .* RWE' >/dev/null || { echo "FAIL: writable image PT_LOAD is not RWX"; exit 1; }

echo
echo "== TLS producer: __thread synthesizes PT_TLS + .tlsdesc + .vms\$tls (vms-99c) =="
echo "   (TLSDESC_ADR_PAGE21/LD64_LO12/ADD_LO12 -> a 2-word TLSDESC entry; PT_TLS"
echo "    carries the .tdata init image; IMGACT completes the entries at activation.)"
cat > "$WORK/tlsvar.c" <<'EOF'
__thread int t_val = 100;   /* thread-local -> .tdata + TLSDESC access */
int read_tls(int a, int b) { return t_val + a + b; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/tlsvar.o" "$WORK/tlsvar.c"
echo "-- TLSDESC relocations gcc emitted --"
readelf -rW "$WORK/tlsvar.o" | awk '/TLSDESC/{print $3}' | sort | uniq -c
"$WORK/LINK.EXE" --shareable --symbol-vector "read_tls=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBTLS\$SHR.EXE" "$WORK/tlsvar.o"
readelf -lW "$WORK/LIBTLS\$SHR.EXE" | grep -E "TLS" || true
readelf -SW "$WORK/LIBTLS\$SHR.EXE" | grep -E '\.tdata|\.tlsdesc|\.vms' || true
readelf -lW "$WORK/LIBTLS\$SHR.EXE" | grep -q "TLS"        || { echo "FAIL: no PT_TLS"; exit 1; }
readelf -SW "$WORK/LIBTLS\$SHR.EXE" | grep -q '\.tlsdesc'  || { echo "FAIL: no .tlsdesc table"; exit 1; }
readelf -SW "$WORK/LIBTLS\$SHR.EXE" | grep -q '\.vms\$tls' || { echo "FAIL: no .vms\$tls"; exit 1; }

echo
echo "== resolve + CALL a universal symbol via the vector (IMGACT resolver, vms-8d5) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/RESOLVE" "$SRC/test/resolve_call.c"
"$WORK/RESOLVE" "$WORK/LIBMATH\$SHR.EXE"

echo
echo "== 2-image link + activate: consumer imports myadd via symbol vector (vms-142) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/ACTIVATE" "$SRC/test/ovmx_activate.c"
cat > "$WORK/consumer.c" <<'EOF'
extern int myadd(int, int);
void _start(void) {
    int r = myadd(7, 35);                 /* == 42, resolved across images */
    register long x8 __asm__("x8") = 94;  /* exit_group */
    register long x0 __asm__("x0") = r;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/consumer.o" "$WORK/consumer.c"

echo "-- LINK.EXE --executable: bind myadd to LIBMATH\$SHR.EXE's vector --"
"$WORK/LINK.EXE" --executable --use "$WORK/LIBMATH\$SHR.EXE" \
    -o "$WORK/ADDER.EXE" "$WORK/consumer.o"
readelf -lW "$WORK/ADDER.EXE" | grep -iE "INTERP|IMGACT" || true
readelf -SW "$WORK/ADDER.EXE" | grep -E '\.plt|\.got|\.vms' || true

echo "-- activate ADDER.EXE (SYS\$IMGACT resolves .vms\$imp via symbol vector) --"
set +e
"$WORK/ACTIVATE" "$WORK/ADDER.EXE" "$WORK/LIBMATH\$SHR.EXE"; RC=$?
set -e
echo "consumer exit code = $RC (expect 42 = myadd(7,35))"
[ "$RC" -eq 42 ] || { echo "FAIL: cross-image symbol-vector call did not yield 42"; exit 1; }

echo
echo "== GSMATCH reject: same consumer vs an OLDER producer must NOT activate =="
mkdir -p "$WORK/old"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "myadd=PROCEDURE,mymul=PROCEDURE" \
    --gsmatch LEQUAL,1,500 \
    -o "$WORK/old/LIBMATH\$SHR.EXE" "$WORK/math.o"
set +e
"$WORK/ACTIVATE" "$WORK/ADDER.EXE" "$WORK/old/LIBMATH\$SHR.EXE"; RC=$?
set -e
echo "activation against older (minor 500 < linked 1000) exit = $RC (expect nonzero, not 42)"
[ "$RC" -ne 42 ] || { echo "FAIL: GSMATCH reject did not block activation"; exit 1; }

echo
echo "== cross-image DATA import: consumer reads an exported DATA universal (vms-20b) =="
# The producer exports a variable as a DATA universal; the consumer reads it via
# a GOT pair (ADR_GOT_PAGE/LD64_GOT_LO12_NC) against the undefined symbol. LINK's
# --executable path turns that into a .vms$imp DATA binding + GOT cell; the
# activator resolves the producer's DATA universal (its biased .data address) and
# writes it into the cell, so the consumer's load dereferences the real variable.
cat > "$WORK/datalib.c" <<'EOF'
int shared_counter = 90;                  /* exported DATA universal (.data) */
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/datalib.o" "$WORK/datalib.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "shared_counter=DATA" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBDATA\$SHR.EXE" "$WORK/datalib.o"
cat > "$WORK/datacons.c" <<'EOF'
extern int shared_counter;                /* imported via GOT (ADR_GOT_PAGE/LD64) */
void _start(void) {
    int r = shared_counter + 9;           /* == 99, data resolved across images */
    register long x8 __asm__("x8") = 94;  /* exit_group */
    register long x0 __asm__("x0") = r;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/datacons.o" "$WORK/datacons.c"
echo "-- consumer .text relocations (expect a GOT pair to shared_counter) --"
readelf -rW "$WORK/datacons.o" | awk '/R_AARCH64/{print $3}' | sort | uniq -c
"$WORK/LINK.EXE" --executable --use "$WORK/LIBDATA\$SHR.EXE" \
    -o "$WORK/DATAPROG.EXE" "$WORK/datacons.o"
readelf -SW "$WORK/DATAPROG.EXE" | grep -E '\.got|\.vms\$imp' || true
set +e
"$WORK/ACTIVATE" "$WORK/DATAPROG.EXE" "$WORK/LIBDATA\$SHR.EXE"; RC=$?
set -e
echo "consumer exit code = $RC (expect 99 = shared_counter(90)+9 read across images)"
[ "$RC" -eq 99 ] || { echo "FAIL: cross-image DATA import did not yield 99 (got $RC)"; exit 1; }

echo
echo "ALL LINK.EXE MVP CHECKS PASSED"
