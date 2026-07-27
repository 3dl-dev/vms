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
echo "ALL LINK.EXE MVP CHECKS PASSED"
