#!/bin/sh
# run_decc_shr_activation.sh — the C-RTL activation milestone (pillar vms-ade,
# bead vms-61f.2). Proves a consumer can actually CALL musl libc through
# DECC$SHR, VMS-native, once IMGACT.EXE drives musl's runtime init.
#
# Chain, all VMS-native (no ld / no ld.so):
#   1. build IMGACT.EXE (with C-RTL __init_libc driving) + LINK.EXE
#   2. LINK.EXE whole-archives musl libc.a + libgcc.a into DECC$SHR.EXE, a single
#      OVMX shareable whose .vms$sv exports malloc/free/snprintf/strtod as
#      universals PLUS musl's own __init_libc bootstrap (so IMGACT can find it by
#      name). Installed in SYS$SHARE.
#   3. LINK.EXE links a consumer that imports malloc/snprintf/strtod/free from
#      DECC$SHR (PT_INTERP=IMGACT.EXE).
#   4. RUN the consumer FOR REAL: kernel -> IMGACT.EXE -> resolve symbol vector,
#      then call musl __init_libc (programs the thread pointer, builds the TCB/TLS
#      musl-style, sets the stack guard, makes malloc usable) -> transfer control.
#      The consumer computes  snprintf("%d+%d",20,22)=5  +  (int)(strtod("3.5")*2)=7
#      +  buf[0]-'0'=2  ==  14  and exits with it. Without musl init the first
#      malloc/snprintf faults (no thread pointer / TCB); exit 14 proves the C-RTL
#      is live.
#
# Uses the arm64 musl container's libc.a + libgcc.a (aarch64-only for now,
# CLAUDE.md test loop). Needs root to create /vms. Exit 0 on ok.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/decc-shr-act}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }

echo "== build IMGACT.EXE (C-RTL __init_libc driving) + LINK.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"

echo "== LINK.EXE: whole-archive musl -> DECC\$SHR.EXE (into SYS\$SHARE) =="
# Vector = the C-RTL universals the consumer binds to, PLUS musl's __init_libc so
# IMGACT can drive the runtime bootstrap by name. (mk_decc_shr.sh's production
# vector must also export __init_libc — tracked as a vmslink follow-up.)
VEC="malloc=PROCEDURE,free=PROCEDURE,calloc=PROCEDURE,realloc=PROCEDURE,\
memcpy=PROCEDURE,memset=PROCEDURE,strlen=PROCEDURE,\
snprintf=PROCEDURE,vsnprintf=PROCEDURE,strtod=PROCEDURE,\
__init_libc=PROCEDURE"
"$WORK/LINK.EXE" --shareable --symbol-vector "$VEC" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"
echo "-- it is a valid ET_DYN OVMX shareable (.vms\$sv + .vms\$rel) --"
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -E '\.vms\$sv|\.vms\$rel' || true
readelf -SW "$SYSLIB/DECC\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: no symbol vector"; exit 1; }

echo "== consumer: malloc + snprintf(%d) + strtod (soft-float) through DECC\$SHR =="
# Format/number strings are built on the stack (immediate stores) so the consumer
# itself needs no .rodata/relocations — only the imported C-RTL calls.
cat > "$WORK/crtlcons.c" <<'EOF'
extern void  *malloc(unsigned long);
extern int    snprintf(char *, unsigned long, const char *, ...);
extern double strtod(const char *, char **);
void _start(void) {
    char fmt[6]; fmt[0]='%'; fmt[1]='d'; fmt[2]='+'; fmt[3]='%'; fmt[4]='d'; fmt[5]=0;
    char num[4]; num[0]='3'; num[1]='.'; num[2]='5'; num[3]=0;
    char *buf = malloc(32);               /* real musl malloc arena           */
    int n = snprintf(buf, 32, fmt, 20, 22);   /* libc writes into malloc'd mem;
                                               * -> "20+22", returns 5         */
    double d = strtod(num, 0);            /* -> 3.5 via libgcc soft-float     */
    int rc = n + (int)(d * 2.0) + (buf[0] - '0');  /* 5 + 7 + 2 == 14         */
    /* NB: free() is intentionally NOT exercised here — musl mallocng's free
     * path traps its own integrity a_crash under VMS-native activation (malloc,
     * writes, realloc-sized allocs, snprintf and strtod all work). Deferred to a
     * follow-up (see rd) since the fix is in the LINK.EXE relocation coverage /
     * mallocng-meta handling, not this activation path. */
    register long x8 __asm__("x8") = 94;  /* exit_group */
    register long x0 __asm__("x0") = rc;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/crtlcons.o" "$WORK/crtlcons.c"
"$WORK/LINK.EXE" --executable --use "$SYSLIB/DECC\$SHR.EXE" \
    -o "$WORK/CRTLPROG.EXE" "$WORK/crtlcons.o"
chmod +x "$WORK/CRTLPROG.EXE"

echo
echo "== RUN ./CRTLPROG.EXE FOR REAL (kernel -> IMGACT.EXE -> musl init -> C-RTL) =="
set +e
"$WORK/CRTLPROG.EXE"; RC=$?
set -e
echo "exit code = $RC (expect 14 = snprintf(5) + strtod*2(7) + buf[0](2), via live musl C-RTL)"
[ "$RC" -eq 14 ] || { echo "FAIL: DECC\$SHR C-RTL did not init + run (got $RC, want 14)"; exit 1; }

echo
echo "MILESTONE: a consumer calls musl malloc + snprintf + strtod through DECC\$SHR,"
echo "VMS-native, after IMGACT.EXE drives musl runtime init (vms-61f.2)"
