#!/bin/sh
# run_libvmssys_native.sh — the VMS-native toolchain milestone (pillar vms-ade,
# bead vms-80a). Compiles the REAL src/libvmssys, links it into an OVMX shareable
# image with LINK.EXE (no ld), installs it in SYS$SHARE, links a consumer that
# imports one of its universals, and runs the consumer FOR REAL: the kernel loads
# IMGACT.EXE as PT_INTERP, which activates the symbol vector (GOT/.vms$rel bias +
# TLS setup) and transfers control. The consumer must compute vms_strlen("hello")
# == 5 by calling into the VMS-native-linked libvmssys.
#
# This proves the whole toolchain end to end: gcc .o -> LINK.EXE -> IMGACT.EXE,
# with zero dependence on the GNU linker or ld.so.
#
# libvmssys uses atomics (LSE outline-atomics by default) — built here with
# -mno-outline-atomics so gcc inlines them instead of calling libgcc helpers
# (__aarch64_*_acq_rel), which no OVMX RTL provides yet.
#
# Runs INSIDE an arm64 musl container (needs root to create /vms). Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SYSLIB_SRC=$(cd "$IMGACT_DIR/../libvmssys" && pwd)
WORK=${WORK:-/tmp/libvmssys-native}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

CFLAGS="-fPIC -O2 -ffreestanding -fno-stack-protector -fno-builtin -mno-outline-atomics -I$SYSLIB_SRC"

echo "== build IMGACT.EXE + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== compile REAL src/libvmssys (7 C objects + syscall.S) =="
OBJS=""
for c in vms_string vms_snprintf vms_futex vms_stdio vms_math vms_runtime_init vms_kif kif_transport_linux; do
    $CC $CFLAGS -c -o "$WORK/$c.o" "$SYSLIB_SRC/$c.c"
    OBJS="$OBJS $WORK/$c.o"
done
$CC -fPIC -mno-outline-atomics -c -o "$WORK/syscall.o" "$SYSLIB_SRC/arch/aarch64/syscall.S"
OBJS="$OBJS $WORK/syscall.o"

echo "== LINK.EXE: libvmssys -> LIBVMSSYS\$SHR.EXE (VMS-native, no ld) =="
"$WORK/LINK.EXE" --shareable --symbol-vector "vms_strlen=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBVMSSYS\$SHR.EXE" $OBJS
echo "-- it is a valid ET_DYN OVMX shareable (PT_LOAD + PT_TLS + .vms\$sv) --"
readelf -lW "$SYSLIB/LIBVMSSYS\$SHR.EXE" | grep -E "LOAD|TLS" || true
readelf -SW "$SYSLIB/LIBVMSSYS\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: no symbol vector"; exit 1; }

echo "== consumer: call the REAL vms_strlen through the symbol vector =="
# The string is built on the stack (immediate stores) so the consumer itself
# needs no .rodata/relocations — only the imported call to vms_strlen.
cat > "$WORK/strcons.c" <<'EOF'
extern unsigned long vms_strlen(const char *);
void _start(void) {
    char b[6];
    b[0]='h'; b[1]='e'; b[2]='l'; b[3]='l'; b[4]='o'; b[5]=0;
    int r = (int)vms_strlen(b);           /* == 5, computed inside libvmssys */
    register long x8 __asm__("x8") = 94;  /* exit_group */
    register long x0 __asm__("x0") = r;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/strcons.o" "$WORK/strcons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    -o "$WORK/STRLEN.EXE" "$WORK/strcons.o"
chmod +x "$WORK/STRLEN.EXE"

echo
echo "== RUN ./STRLEN.EXE FOR REAL (kernel -> IMGACT.EXE -> activate libvmssys) =="
set +e
"$WORK/STRLEN.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 5 = vms_strlen(\"hello\") via VMS-native activation)"
[ "$RC" -eq 5 ] || { echo "FAIL: real libvmssys did not activate + run (got $RC, want 5)"; exit 1; }

echo
echo "MILESTONE: real src/libvmssys links + activates + runs VMS-native (no ld/ld.so)"
