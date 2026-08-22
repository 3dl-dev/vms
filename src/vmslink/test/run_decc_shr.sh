#!/bin/sh
# run_decc_shr.sh — producer proof for DECC$SHR.EXE (bead vms-61f.1, pillar
# vms-ade). Builds LINK.EXE + OVMXDUMP, runs the mk_decc_shr.sh recipe to link
# the WHOLE musl libc.a + libgcc.a into an OVMX C-RTL shareable, and asserts the
# done-condition:
#
#   1. LINK.EXE links it CLEAN (no --allow-undefined, zero deferred externals) —
#      libgcc.a satisfies the soft-float/long-double builtins libc.a defers, and
#      the weak linker-defined boundary symbols (__init_array_start/end,
#      _DYNAMIC, ...) resolve to 0 (musl carries no static constructors).
#   2. The image is a valid ELF ET_DYN carrying .vms$sv and .vms$rel.
#   3. OVMXDUMP lists the C-RTL universals (malloc/free/memcpy/memset/strlen/
#      snprintf/printf/...) as PROCEDURE universals at nonzero image-relative
#      addresses.
#
# This item PRODUCES the shareable only; runtime init (__libc_start_main, TCB/
# thread-pointer setup, running constructors) is vms-61f.2. Runs in the arm64
# musl container (the CI decc-shr job / CLAUDE.md test loop). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/decc-shr-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE + OVMXDUMP =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE"   "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/OVMXDUMP"   "$SRC/dump_image.c"

if [ ! -f /usr/lib/libc.a ]; then
    echo "FAIL: /usr/lib/libc.a not found — this harness must run in the arm64 musl"
    echo "      container (the CI decc-shr job; see CLAUDE.md test loop)."
    exit 1
fi

echo
echo "== recipe: whole musl libc.a + libgcc.a -> DECC\$SHR.EXE (strict, no ld) =="
# Strict on purpose: mk_decc_shr.sh does NOT pass --allow-undefined, so any
# unresolved external that a C-RTL consumer would need aborts the link.
sh "$SRC/mk_decc_shr.sh" "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE"

echo
echo "== readelf: must be ET_DYN with .vms\$sv + .vms\$rel =="
readelf -hSW "$WORK/DECC\$SHR.EXE" | grep -E "Type:|\.vms\\\$sv|\.vms\\\$rel|\.got|\.data" || true
readelf -h "$WORK/DECC\$SHR.EXE" | grep -q "DYN"        || { echo "FAIL: not ET_DYN"; exit 1; }
readelf -S "$WORK/DECC\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: no .vms\$sv"; exit 1; }
readelf -S "$WORK/DECC\$SHR.EXE" | grep -q '\.vms\$rel' || { echo "FAIL: no .vms\$rel"; exit 1; }

echo
echo "== OVMXDUMP: C-RTL universals =="
OUT=$("$WORK/OVMXDUMP" "$WORK/DECC\$SHR.EXE")
echo "$OUT" | head -20
echo "  ..."

echo
echo "== assertions: core libc universals are PROCEDURE at nonzero addresses =="
for s in malloc free calloc realloc memcpy memmove memset memcmp \
         strlen strcmp strncpy strchr strstr snprintf vsnprintf printf \
         fprintf puts fwrite fread fopen fclose qsort getenv exit; do
    echo "$OUT" | grep -qE "PROCEDURE .* $s\$" \
        || { echo "FAIL: $s missing / not a PROCEDURE universal"; exit 1; }
done
# No universal may have a zero image-relative value (would mean unresolved).
echo "$OUT" | grep -E 'PROCEDURE +value=0x0{16}' \
    && { echo "FAIL: a universal resolved to address 0"; exit 1; } || true

echo
echo "== unneeded compiler-runtime builtins stay INTERNAL, not exported =="
# Most of libgcc.a's builtins are linked in but never appear in the symbol
# vector — a C-RTL consumer never calls them directly. EXCEPTION (vms-4ba.4):
# __addtf3/__trunctfdf2 and 16 other IEEE-quad ("tf", 128-bit long double)
# helpers WERE promoted to real universals for tcc-as-an-OVMX-image (TCC.EXE
# is a genuine cross-image CONSUMER of them — its own long-double constant
# folding calls them, unlike every prior consumer) — see mk_decc_shr.sh's
# vms-4ba.4 comment block for the full list. __multc3 (complex multiply) and
# __fixtfsi (another tf conversion, unused by tcc) are NOT in that list and
# must stay internal-only, so they remain the regression check here.
for b in __multc3 __fixtfsi; do
    echo "$OUT" | grep -qE "PROCEDURE .* $b\$" \
        && { echo "FAIL: compiler-runtime builtin $b leaked into .vms\$sv"; exit 1; } \
        || true
done

echo
echo "== decc\$-prefixed CRTL alias vector (vms-3e4 R1b-1): the port imports decc\$<name> =="
# The alpha-dec-vms GCC port references every C-RTL entry as decc$<name>. R1b-1
# exports each NO-DECORATION-FLAG entry as a universal aliased to the musl impl
# (SYMBOL_VECTOR universal/internal). Assert representative aliases are PROCEDURE
# universals AND resolve to the SAME image-relative address as their bare-name
# sibling (same code; append-only, the bare name stays exported too). Names used
# here are all flag-free in the crtlmap (strlen/fopen/... — malloc/fprintf/memcpy
# carry 64/FLOAT flags and are R1b-2, deliberately NOT generated yet).
for s in decc\$strlen decc\$fopen decc\$fclose decc\$fread decc\$strcmp decc\$getenv; do
    echo "$OUT" | awk -v s="$s" '/PROCEDURE/ && $NF==s {f=1} END{exit !f}' \
        || { echo "FAIL: $s missing / not a PROCEDURE universal (R1b-1 decc\$ vector)"; exit 1; }
done
# decc$<name> must resolve to the SAME value= as the bare <name> (same impl):
for pair in strlen fopen strcmp getenv; do
    bare=$(echo "$OUT" | awk -v s="$pair"       '$NF==s {print $(NF-1); exit}')
    deco=$(echo "$OUT" | awk -v s="decc\$$pair" '$NF==s {print $(NF-1); exit}')
    [ -n "$bare" ] && [ "$bare" = "$deco" ] \
        || { echo "FAIL: decc\$$pair ($deco) != $pair ($bare) — alias not bound to the impl"; exit 1; }
done
echo "decc\$ aliases present and bound to their musl impls (decc\$strlen==strlen, etc.)"

echo
echo "== proxy: an object importing decc\$<name> links CLEAN against DECC\$SHR (strict) =="
# Stand in for a real alpha-dec-vms object: reference no-flag decc$ CRTL entries as
# undefined externals (asm-label so the `$` is legal) and link --executable --use
# DECC$SHR with NO --allow-undefined. If the decc$ imports bind to DECC$SHR's
# .vms$sv, the link succeeds — the R1b-1 outcome ("the port's decc$ refs resolve").
cat > "$WORK/decc_proxy.c" <<'EOF'
extern void *d_fopen(const char *, const char *)  __asm__("decc$fopen");
extern int   d_fclose(void *)                     __asm__("decc$fclose");
extern unsigned long d_fread(void *, unsigned long, unsigned long, void *) __asm__("decc$fread");
extern unsigned long d_strlen(const char *)       __asm__("decc$strlen");
extern int   d_strcmp(const char *, const char *) __asm__("decc$strcmp");
extern char *d_getenv(const char *)               __asm__("decc$getenv");
void _start(void) {
    void *fp = d_fopen("x", "r");
    char buf[8];
    volatile unsigned long n = d_fread(buf, 1, sizeof buf, fp);
    (void)n; (void)d_fclose(fp);
    volatile unsigned long m = d_strlen(d_getenv("PATH") ? d_getenv("PATH") : "");
    volatile int c = d_strcmp("a", "b");
    (void)m; (void)c;
    __asm__ volatile("mov $60,%%eax\n\txor %%edi,%%edi\n\tsyscall" ::: "eax","edi","memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/decc_proxy.o" "$WORK/decc_proxy.c"
echo "-- proxy .o undefined decc\$ references --"
nm -u "$WORK/decc_proxy.o" | grep 'decc\$' || { echo "FAIL: proxy has no decc\$ imports"; exit 1; }
"$WORK/LINK.EXE" --executable --use "$WORK/DECC\$SHR.EXE" \
    -o "$WORK/DECCPROXY.EXE" "$WORK/decc_proxy.o" \
    || { echo "FAIL: proxy did not link against DECC\$SHR — a decc\$ import went unresolved (R1b-1 incomplete)"; exit 1; }
echo "proxy linked clean: every decc\$ import bound to DECC\$SHR's alias vector"

echo
echo "== DEC C RTL special routines (vms-3e4 R1b-2a): errno accessors + dual-pointer malloc =="
# Real impls (ovmx_decc_crtl.c), not musl aliases: get_errno_addr /
# get_vms_errno_addr (per-thread errno cells) + _malloc32 / _malloc64 (the port
# crt0's dual-pointer allocators). Assert each is a PROCEDURE universal, then link
# a proxy that imports them.
for s in get_errno_addr get_vms_errno_addr _malloc32 _malloc64; do
    echo "$OUT" | awk -v s="$s" '/PROCEDURE/ && $NF==s {f=1} END{exit !f}' \
        || { echo "FAIL: $s missing / not a PROCEDURE universal (R1b-2a special routines)"; exit 1; }
done
cat > "$WORK/decc_crtl_proxy.c" <<'EOF'
extern int  *get_errno_addr(void);
extern int  *get_vms_errno_addr(void);
extern int   _malloc32(int);
extern void *_malloc64(unsigned long);
void _start(void) {
    volatile int  *e  = get_errno_addr();
    volatile int  *ve = get_vms_errno_addr();
    volatile int   p32 = _malloc32(32);
    volatile void *p64 = _malloc64(64);
    (void)e; (void)ve; (void)p32; (void)p64;
    __asm__ volatile("mov $60,%%eax\n\txor %%edi,%%edi\n\tsyscall" ::: "eax","edi","memory");
    __builtin_unreachable();
}
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/decc_crtl_proxy.o" "$WORK/decc_crtl_proxy.c"
"$WORK/LINK.EXE" --executable --use "$WORK/DECC\$SHR.EXE" \
    -o "$WORK/DECCCRTLPROXY.EXE" "$WORK/decc_crtl_proxy.o" \
    || { echo "FAIL: special-routine proxy did not link against DECC\$SHR (R1b-2a incomplete)"; exit 1; }
echo "R1b-2a OK: errno accessors + _malloc32/_malloc64 exported and bound (real impls, not stubs)"

echo
echo "ALL DECC\$SHR PRODUCER CHECKS PASSED"
