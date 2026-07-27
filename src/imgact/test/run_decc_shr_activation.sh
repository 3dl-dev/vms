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
#      +  buf[0]-'0'=2  ==  14, and also free()s its buffer, mallocs+writes+frees a
#      second one, and exits 14. Without musl init the first malloc/snprintf faults
#      (no thread pointer / TCB); without the vms-36a LINK.EXE weak-override fix the
#      free() SIGSEGVs (malloc had bound to musl's WEAK __simple_malloc, so buf
#      carried no mallocng metadata). Exit 14 proves the C-RTL is live AND that
#      mallocng malloc/free are a matched pair.
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

echo "== consumer: malloc + snprintf(%d) + strtod (soft-float) + free through DECC\$SHR =="
# Format/number strings are built on the stack (immediate stores) so the consumer
# itself needs no .rodata/relocations — only the imported C-RTL calls.
cat > "$WORK/crtlcons.c" <<'EOF'
extern void  *malloc(unsigned long);
extern void   free(void *);
extern int    snprintf(char *, unsigned long, const char *, ...);
extern double strtod(const char *, char **);
void _start(void) {
    char fmt[6]; fmt[0]='%'; fmt[1]='d'; fmt[2]='+'; fmt[3]='%'; fmt[4]='d'; fmt[5]=0;
    char num[4]; num[0]='3'; num[1]='.'; num[2]='5'; num[3]=0;
    char *buf = malloc(32);               /* real musl mallocng arena         */
    int n = snprintf(buf, 32, fmt, 20, 22);   /* libc writes into malloc'd mem;
                                               * -> "20+22", returns 5         */
    double d = strtod(num, 0);            /* -> 3.5 via libgcc soft-float     */
    int first = buf[0] - '0';             /* '2' -> 2                          */
    free(buf);                            /* mallocng free — deallocates buf.  *
                                           * Before the vms-36a LINK.EXE weak- *
                                           * override fix this SIGSEGV'd: the  *
                                           * exported malloc bound to musl's   *
                                           * WEAK __simple_malloc bump alloc,  *
                                           * so buf carried no mallocng meta   *
                                           * and free's get_meta walked off    *
                                           * the mapping. Now malloc binds to  *
                                           * mallocng STRONG and free works.   */
    char *buf2 = malloc(64);              /* reuse the arena after free ...    */
    buf2[0] = 7; buf2[63] = 9;            /* ... write both ends ...           */
    int ok = (buf2[0] == 7 && buf2[63] == 9); /* ... arena healthy post-free  */
    free(buf2);                           /* free a second allocation          */
    int rc = n + (int)(d * 2.0) + first;  /* 5 + 7 + 2 == 14                   */
    if (!ok) rc = 99;                     /* fail marker if reuse-after-free bad */
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
echo "exit code = $RC (expect 14 = snprintf(5) + strtod*2(7) + buf[0](2); free()+reuse OK)"
[ "$RC" -eq 99 ] && { echo "FAIL: reuse-after-free returned bad data (arena corrupt)"; exit 1; }
[ "$RC" -eq 14 ] || { echo "FAIL: DECC\$SHR C-RTL did not init + run (got $RC, want 14; 139=free SIGSEGV)"; exit 1; }

echo
echo "MILESTONE: a consumer calls musl malloc + snprintf + strtod + free (with reuse"
echo "after free) through DECC\$SHR, VMS-native, after IMGACT.EXE drives musl runtime"
echo "init (vms-61f.2); mallocng free works via the vms-36a LINK.EXE weak-override fix."
