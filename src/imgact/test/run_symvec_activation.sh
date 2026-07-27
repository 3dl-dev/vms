#!/bin/sh
# run_symvec_activation.sh — real IMGACT.EXE activation of a VMS-native image
# (bead vms-714). Builds IMGACT.EXE (with .vms$imp support) + LINK.EXE, installs
# IMGACT at its PT_INTERP path, links a producer shareable image and a consumer
# executable with LINK.EXE, and runs the consumer FOR REAL: the kernel loads
# IMGACT.EXE as the interpreter, which resolves the symbol vector and transfers
# control. The cross-image call must yield exit 42; a GSMATCH-older producer
# must be refused.
#
# Runs INSIDE an arm64 musl container (needs root to create /vms). Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/symvec-act}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

echo "== build IMGACT.EXE (with symbol-vector activation) =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" )
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
echo "installed $SYSEXE/IMGACT.EXE"

echo "== build LINK.EXE =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== LINK.EXE: producer LIBMATH\$SHR.EXE (into SYS\$SHARE) =="
cat > "$WORK/math.c" <<'EOF'
int myadd(int a, int b) { return a + b; }
int mymul(int a, int b) { return a * b; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/math.o" "$WORK/math.c"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "myadd=PROCEDURE,mymul=PROCEDURE" \
    --gsmatch LEQUAL,1,1000 \
    -o "$SYSLIB/LIBMATH\$SHR.EXE" "$WORK/math.o"

echo "== LINK.EXE: consumer ADDER.EXE (PT_INTERP=IMGACT.EXE) =="
cat > "$WORK/consumer.c" <<'EOF'
extern int myadd(int, int);
void _start(void) {
    int r = myadd(7, 35);                 /* == 42, resolved via IMGACT */
    register long x8 __asm__("x8") = 94;  /* exit_group */
    register long x0 __asm__("x0") = r;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/consumer.o" "$WORK/consumer.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBMATH\$SHR.EXE" \
    -o "$WORK/ADDER.EXE" "$WORK/consumer.o"
chmod +x "$WORK/ADDER.EXE"

echo
echo "== RUN ./ADDER.EXE FOR REAL (kernel -> PT_INTERP=IMGACT.EXE -> activate) =="
set +e
"$WORK/ADDER.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 42 = myadd(7,35) via symbol vector)"
[ "$RC" -eq 42 ] || { echo "FAIL: real IMGACT activation did not yield 42"; exit 1; }

echo
echo "== GSMATCH reject: install an OLDER producer, re-run =="
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "myadd=PROCEDURE,mymul=PROCEDURE" \
    --gsmatch LEQUAL,1,500 \
    -o "$SYSLIB/LIBMATH\$SHR.EXE" "$WORK/math.o"
set +e
"$WORK/ADDER.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 44 = %IMGACT-F-GSMATCH clean fatal, NOT a crash)"
[ "$RC" -eq 44 ] || { echo "FAIL: GSMATCH reject must exit 44 cleanly (got $RC)"; exit 1; }

echo
echo "ALL REAL IMGACT SYMBOL-VECTOR ACTIVATION CHECKS PASSED"
