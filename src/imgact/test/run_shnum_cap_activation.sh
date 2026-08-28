#!/bin/sh
# run_shnum_cap_activation.sh -- real IMGACT.EXE activation of a producer whose
# section-header table is LARGER than the old arbitrary 64-entry cap (vms-f60d /
# GCC-P1). ovmx_find_section() used to hard-reject any image with e_shnum > 64,
# so the genuine whole-archived alpha DECC$SHR.EXE (2083 section headers) never
# had its `.vms$sv` symbol vector located -> load_ovmx_producer() returned 0 ->
# %IMGACT-F-IMGNOTFND, before any user code ran. This harness reproduces that
# shape with a small LINK.EXE producer whose section-header table is rewritten
# to 2083 entries, and proves:
#
#   1. BASELINE: the unmodified small producer activates (consumer -> 42).
#   2. LARGE TABLE (the fix): the SAME producer, its section-header table padded
#      to 2083 entries, STILL has its `.vms$sv` found -> consumer -> 42. Under
#      the old `e_shnum > 64` cap this path returned %IMGACT-F-IMGNOTFND (44).
#   3. FAIL-HONEST BOUNDS (INV-6): a producer whose e_shnum claims a table that
#      runs PAST the readable image is rejected with %IMGACT-F-BADIMGHDR -- an
#      honest fatal, NOT a silent misread of bytes outside the image.
#
# Runs INSIDE an arm64 musl container (needs root to create /vms), exactly like
# run_symvec_activation.sh. Exit 0 only if all three hold.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/shnum-cap-act}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"
PROD="$SYSLIB/LIBBIG\$SHR.EXE"

echo "== build IMGACT.EXE =="
( cd "$IMGACT_DIR" && make CC="$CC" clean >/dev/null 2>&1 || true; make CC="$CC" )
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
echo "installed $SYSEXE/IMGACT.EXE"

echo "== build LINK.EXE =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c" 2>/dev/null

echo "== section-header-table rewriter =="
# Two modes:
#   pad <file> <target>   relocate the section-header table to end-of-file and
#                         grow it to <target> entries (extra entries are zeroed
#                         SHT_NULL -- name index 0, never matched; every real
#                         entry, incl. .vms$sv, keeps its index and contents).
#   setshnum <file> <n>   overwrite e_shnum with <n> in place (leaves e_shoff),
#                         so the claimed table runs past EOF -- the corrupt case.
cat > "$WORK/shrewrite.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
static unsigned char *slurp(const char *p, long *n){
    FILE *f=fopen(p,"rb"); if(!f){perror(p);exit(2);}
    fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *b=malloc(*n); if(fread(b,1,*n,f)!=(size_t)*n){exit(2);} fclose(f); return b;
}
static void spew(const char *p, const unsigned char *b, long n){
    FILE *f=fopen(p,"wb"); if(!f){perror(p);exit(2);}
    if(fwrite(b,1,n,f)!=(size_t)n){exit(2);} fclose(f);
}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: %s pad|setshnum FILE N\n",argv[0]);return 2;}
    long n; unsigned char *b=slurp(argv[2],&n);
    Elf64_Ehdr *eh=(Elf64_Ehdr*)b;
    unsigned long target=strtoul(argv[3],0,0);
    if(!strcmp(argv[1],"setshnum")){
        eh->e_shnum=(Elf64_Half)target; spew(argv[2],b,n); return 0;
    }
    if(strcmp(argv[1],"pad")){fprintf(stderr,"bad mode\n");return 2;}
    unsigned long old=eh->e_shnum, es=eh->e_shentsize, off=eh->e_shoff;
    if(es!=sizeof(Elf64_Shdr)){fprintf(stderr,"e_shentsize %lu != 64\n",es);return 2;}
    if(target<old){fprintf(stderr,"target %lu < current %lu\n",target,old);return 2;}
    /* new table at EOF: copy the existing `old` entries in order, then zeroes. */
    long newoff=n;
    long add=(long)(target*es);
    unsigned char *out=malloc(n+add); memcpy(out,b,n);
    memcpy(out+newoff,b+off,(size_t)(old*es));           /* keep real entries */
    memset(out+newoff+(long)(old*es),0,(size_t)((target-old)*es));
    Elf64_Ehdr *oh=(Elf64_Ehdr*)out;
    oh->e_shoff=(Elf64_Off)newoff; oh->e_shnum=(Elf64_Half)target;
    spew(argv[2],out,n+add);
    return 0;
}
EOF
$CC -O2 -o "$WORK/shrewrite" "$WORK/shrewrite.c"

build_producer() {   # $1 = output path, gsmatch fixed newest
    cat > "$WORK/big.c" <<'EOF'
int myadd(int a, int b) { return a + b; }
int mymul(int a, int b) { return a * b; }
EOF
    $CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/big.o" "$WORK/big.c"
    "$WORK/LINK.EXE" --shareable \
        --symbol-vector "myadd=PROCEDURE,mymul=PROCEDURE" \
        --gsmatch LEQUAL,1,1000 \
        -o "$1" "$WORK/big.o"
}

build_consumer() {   # $1 = producer to --use
    cat > "$WORK/bigcons.c" <<'EOF'
extern int myadd(int, int);
void _start(void) {
    int r = myadd(7, 35);                 /* == 42, resolved via IMGACT */
    register long x8 __asm__("x8") = 94;  /* exit_group */
    register long x0 __asm__("x0") = r;
    __asm__ volatile("svc 0" :: "r"(x8), "r"(x0) : "memory");
    __builtin_unreachable();
}
EOF
    $CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/bigcons.o" "$WORK/bigcons.c"
    "$WORK/LINK.EXE" --executable --use "$1" \
        -o "$WORK/BIGADDER.EXE" "$WORK/bigcons.o"
    chmod +x "$WORK/BIGADDER.EXE"
}

echo
echo "== 1. BASELINE: small unmodified producer activates =="
build_producer "$PROD"
build_consumer "$PROD"
echo "-- producer section-header count --"
readelf -h "$PROD" | grep -iE "Number of section"
set +e; "$WORK/BIGADDER.EXE"; RC=$?; set -e
echo "exit code = $RC (expect 42)"
[ "$RC" -eq 42 ] || { echo "FAIL: baseline activation did not yield 42"; exit 1; }

echo
echo "== 2. LARGE TABLE: pad producer to 2083 section headers, re-activate =="
"$WORK/shrewrite" pad "$PROD" 2083
echo "-- producer section-header count after pad --"
readelf -h "$PROD" | grep -iE "Number of section"
readelf -SW "$PROD" | grep -q '\.vms\$sv' || { echo "FAIL: .vms\$sv missing after pad"; exit 1; }
set +e; OUT=$("$WORK/BIGADDER.EXE" 2>&1); RC=$?; set -e
echo "output: $OUT"
echo "exit code = $RC (expect 42 -- old '>64' cap would give %IMGACT-F-IMGNOTFND/44)"
[ "$RC" -eq 42 ] || { echo "FAIL: 2083-section producer did not activate (got $RC)"; exit 1; }
echo "OK: .vms\$sv found in a 2083-section producer (cap removed)"

echo
echo "== 3. FAIL-HONEST: section table running past the image => %IMGACT-F-BADIMGHDR =="
build_producer "$PROD"                       # fresh small producer
"$WORK/shrewrite" setshnum "$PROD" 60000     # claims 60000*64 = 3.84MB of SHT
echo "-- producer now claims 60000 section headers (file is a few KB) --"
readelf -h "$PROD" 2>/dev/null | grep -iE "Number of section" || true
set +e; FOUT=$("$WORK/BIGADDER.EXE" 2>&1); FRC=$?; set -e
echo "output: $FOUT"
echo "exit code = $FRC"
case "$FOUT" in
    *"%IMGACT-F-BADIMGHDR"*) ;;
    *) echo "FAIL: over-long section table must fail honest with %IMGACT-F-BADIMGHDR"; exit 1 ;;
esac
[ "$FRC" -ne 0 ] || { echo "FAIL: fail-honest path must exit nonzero"; exit 1; }
echo "OK: bounds check fail-honests (BADIMGHDR), never over-reads past the image"

echo
echo "ALL IMGACT SECTION-COUNT-CAP ACTIVATION CHECKS PASSED"
