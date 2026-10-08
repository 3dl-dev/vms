#!/bin/sh
# run_lib_initialize_x86_64.sh - LIB$INITIALIZE through the native toolchain on
# x86_64 (rd vms-db7): tcc -> LINK.EXE -> IMGACT.EXE.
#
# A VMS C program registers image-initialization routines by placing their
# addresses in the psect LIB$INITIALIZE with DEC C's
#     #pragma extern_model strict_refdef "LIB$INITIALIZE" nowrt, long
# and references the symbol LIB$INITIALIZE; image activation calls each routine,
# in psect (link) order, before main. This proves the three halves:
#   - OVMX's tcc honours #pragma extern_model: the array lands in an ELF section
#     named LIB$INITIALIZE in each object;
#   - LINK.EXE concatenates the contributions of every object in link order into
#     ONE output section LIB$INITIALIZE, and resolves the symbol LIB$INITIALIZE;
#   - IMGACT.EXE calls the routines (skipping a zero entry) after the image is
#     relocated and before main -- main sees them already run, in order 1,2,3.
# A program that references LIB$INITIALIZE with no contribution links and runs.
#
# Needs root (stages IMGACT.EXE at its PT_INTERP path /vms/SYS0/SYSCOMMON/SYSEXE),
# like run_multiobj_exec_x86_64.sh: CI runs it in an Alpine container.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
IMGACT_DIR=$(cd "$HERE/.." && pwd)
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
TCC_SRC=$(cd "$IMGACT_DIR/../../third-party/tcc" && pwd)
WORK=${WORK:-/tmp/lib-initialize-x86_64}
rm -rf "$WORK"; mkdir -p "$WORK"
SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

echo "== build IMGACT.EXE, LINK.EXE and tcc (x86_64) =="
( cd "$IMGACT_DIR" && make ARCH=x86_64 CC="$CC" clean >/dev/null 2>&1 || true; make ARCH=x86_64 CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"
make -C "$TCC_SRC" CC="$CC" BUILD="$WORK/tcc-build" >/dev/null 2>&1 || { echo "FAIL: tcc did not build"; exit 1; }
TCC=$(find "$WORK/tcc-build" -maxdepth 2 -name tcc -type f | head -1)
[ -x "$TCC" ] || { echo "FAIL: no tcc binary under $WORK/tcc-build"; exit 1; }
TCCDIR=$(dirname "$TCC")

echo "== a minimal producer runtime: put_str / exit (raw syscalls) =="
cat > "$WORK/librt.c" <<'EOT'
static long sys_write(int fd, const void *buf, unsigned long n) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(1), "D"(fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return ret;
}
void put_str(const char *s) { unsigned long n = 0; while (s[n]) n++; sys_write(1, s, n); sys_write(1, "\n", 1); }
void exit(int code) { __asm__ volatile("syscall" :: "a"(60), "D"(code) : "memory"); __builtin_unreachable(); }
EOT
$CC -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -c -o "$WORK/librt.o" "$WORK/librt.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "put_str=PROCEDURE,exit=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBRT\$SHR.EXE" "$WORK/librt.o"

echo "== the program: two objects, each contributing to LIB\$INITIALIZE (DEC C pragma) =="
cat > "$WORK/main.c" <<'EOT'
extern void put_str(const char *s);
int seen[8];
int nseen;
void record(int v) { if (nseen < 8) seen[nseen++] = v; }
static void init_one(void) { record(1); }

#pragma extern_model save
#pragma extern_model strict_refdef "LIB$INITIALIZE" nowrt, long
void (* const iniarray[])() = { init_one, };
#pragma extern_model restore

int LIB$INITIALIZE();
int (*lib_init_ref)() = LIB$INITIALIZE;

int main(void)
{
    char line[] = "LIBINIT seen=0 order=---";
    int i;
    line[13] = (char)('0' + nseen);
    for (i = 0; i < nseen && i < 3; i++)
        line[21 + i] = (char)('0' + seen[i]);
    put_str(line);
    return nseen == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3 ? 42 : 1;
}
EOT
cat > "$WORK/more.c" <<'EOT'
extern void record(int v);
static void init_two(void) { record(2); }
static void init_three(void) { record(3); }

#pragma extern_model save
#pragma extern_model strict_refdef "LIB$INITIALIZE" nowrt, long
void (* const iniarray_more[])() = { init_two, 0, init_three, };
#pragma extern_model restore
EOT
cat > "$WORK/noinit.c" <<'EOT'
extern void put_str(const char *s);
int LIB$INITIALIZE();
int (*lib_init_ref)() = LIB$INITIALIZE;
int main(void) { put_str("NOINIT ran"); return 9; }
EOT
for f in main more noinit; do
    "$TCC" -B"$TCCDIR" -c -o "$WORK/$f.o" "$WORK/$f.c" || { echo "FAIL: tcc did not compile $f.c"; exit 1; }
done
readelf -SW "$WORK/main.o" | grep -q 'LIB\$INITIALIZE' \
    || { echo "FAIL: tcc ignored #pragma extern_model (no LIB\$INITIALIZE section in main.o)"; exit 1; }
readelf -SW "$WORK/more.o" | grep -q 'LIB\$INITIALIZE' \
    || { echo "FAIL: no LIB\$INITIALIZE section in more.o"; exit 1; }

"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBRT\$SHR.EXE" -o "$WORK/LIBINIT.EXE" "$WORK/main.o" "$WORK/more.o"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBRT\$SHR.EXE" -o "$WORK/NOINIT.EXE" "$WORK/noinit.o"
chmod +x "$WORK/LIBINIT.EXE" "$WORK/NOINIT.EXE"
readelf -SW "$WORK/LIBINIT.EXE" | grep 'LIB\$INITIALIZE' || { echo "FAIL: LINK.EXE dropped the LIB\$INITIALIZE psect"; exit 1; }
SZ=$(readelf -SW "$WORK/LIBINIT.EXE" | awk '/LIB\$INITIALIZE/{for(i=1;i<=NF;i++) if($i=="PROGBITS"){print $(i+3); exit}}')
[ "$SZ" = "000020" ] || { echo "FAIL: LIB\$INITIALIZE is $SZ bytes, want 0x20 (both objects' 4 entries concatenated)"; exit 1; }

echo "== RUN LIBINIT.EXE (kernel -> IMGACT -> LIB\$INITIALIZE routines -> main) =="
set +e
OUT=$("$WORK/LIBINIT.EXE"); RC=$?
OUT2=$("$WORK/NOINIT.EXE"); RC2=$?
set -e
echo "$OUT (exit $RC)"
echo "$OUT2 (exit $RC2)"
echo "$OUT" | grep -q 'LIBINIT seen=3 order=123' \
    || { echo "FAIL: main did not see the three LIB\$INITIALIZE routines run in link order"; exit 1; }
[ "$RC" -eq 42 ] || { echo "FAIL: LIBINIT.EXE exit $RC, want 42"; exit 1; }
[ "$RC2" -eq 9 ] || { echo "FAIL: a program with an empty LIB\$INITIALIZE did not run (exit $RC2)"; exit 1; }
echo "PASS: LIB\$INITIALIZE routines from two objects ran in link order before main (vms-db7)"
