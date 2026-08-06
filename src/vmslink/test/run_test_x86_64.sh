#!/bin/sh
# run_test_x86_64.sh — LINK.EXE x86_64 "simple" reloc proof harness (vms-8f5).
#
# Ground-source done condition: hand-built x86_64 .o's carrying one of each of
# the three "simple" static relocs the survey (docs/design-link-x86_64-relocs.md)
# confirmed are present in the real x86_64 object set — R_X86_64_64 (absolute
# data pointer), R_X86_64_PC32/PLT32 (same-image and cross-object function
# call) — link via LINK.EXE to a real ET_DYN x86_64 ELF (readelf -h shows
# EM_X86_64) and the produced image RUNS: CALLSLOT mmaps it and calls into it
# natively (this harness runs on an x86_64 host, so no emulation is needed for
# the execution half — only the byte-level relocation math is under test).
#
# Also proves the gate is ADDITIVE, not a replacement: a matching aarch64
# object built in the same run still yields EM_AARCH64 (regression check).
#
# Companion to run_test.sh (the aarch64 MVP harness, still the CI job of
# record — see .github/workflows/ci.yml vmslink-mvp). This script targets the
# x86_64-native dev host directly; no arm64 QEMU/container needed.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/vmslink-test-x86_64}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE + CALLSLOT + ABSCHECK (host tools) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE"    "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/CALLSLOT"    "$SRC/test/call_slot.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/ABSCHECK"    "$SRC/test/abs_check.c"

echo
echo "== same-image function call: R_X86_64_PC32/PLT32 to a local function =="
cat > "$WORK/nonleaf.c" <<'EOF'
static __attribute__((noinline)) int scale(int x) { return x * 3; }
int lookup(int i, int unused) { (void)unused; return scale(i) + 1; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/nonleaf.o" "$WORK/nonleaf.c"
echo "-- relocations gcc emitted against .text --"
readelf -rW "$WORK/nonleaf.o" | awk '/R_X86_64/{print $3}' | sort | uniq -c
readelf -rW "$WORK/nonleaf.o" | grep -qE "R_X86_64_(PC32|PLT32)" \
    || { echo "FAIL: expected a PC32/PLT32 same-image call reloc"; exit 1; }
"$WORK/LINK.EXE" --shareable --symbol-vector "lookup=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBLOOK\$SHR.EXE" "$WORK/nonleaf.o"
set +e
"$WORK/CALLSLOT" "$WORK/LIBLOOK\$SHR.EXE" 0 4 0; RC=$?    # lookup(4)=scale(4)+1=13
set -e
echo "lookup(4) exit = $RC (expect 13 = scale(4)*3+1)"
[ "$RC" -eq 13 ] || { echo "FAIL: same-image PC32/PLT32 call wrong (got $RC, want 13)"; exit 1; }

echo
echo "== PLT-style cross-object call: R_X86_64_PLT32 across a.o -> b.o =="
cat > "$WORK/a.c" <<'EOF'
extern int helper(int);                 /* defined in b.o -> cross-object PLT32 */
int dispatch(int x, int y) { (void)y; return helper(x) + 1; }
EOF
cat > "$WORK/b.c" <<'EOF'
int helper(int x) { return x * 10; }
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/a.o" "$WORK/a.c"
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/b.o" "$WORK/b.c"
echo "-- a.o .text relocations (expect PLT32 to helper) --"
readelf -rW "$WORK/a.o" | awk '/R_X86_64/{print $3}' | sort | uniq -c
readelf -rW "$WORK/a.o" | grep -qE "R_X86_64_(PC32|PLT32)" \
    || { echo "FAIL: expected a PC32/PLT32 cross-object call reloc"; exit 1; }
"$WORK/LINK.EXE" --shareable --symbol-vector "dispatch=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIB2\$SHR.EXE" "$WORK/a.o" "$WORK/b.o"
set +e
"$WORK/CALLSLOT" "$WORK/LIB2\$SHR.EXE" 0 5 0; RC=$?    # dispatch(5)=helper(5)+1=51
set -e
echo "dispatch(5) exit = $RC (expect 51 = helper(5)*10+1)"
[ "$RC" -eq 51 ] || { echo "FAIL: cross-object PLT32 call wrong (got $RC, want 51)"; exit 1; }

echo
echo "== absolute data pointer: R_X86_64_64 .rela.data pointer initializers =="
cat > "$WORK/ptr.c" <<'EOF'
int f(int x) { return x + 1; }
int g(int x) { return x + 2; }
int (*tbl[2])(int) = { f, g };            /* .data: two R_X86_64_64 pointer inits */
int usetbl(int i, int x) { return tbl[i & 1](x); }
EOF
# -fvisibility=hidden: without it gcc routes the `tbl` access through a GOT-
# indirect load (R_X86_64_REX_GOTPCRELX) since -fPIC alone can't prove `tbl`
# binds locally. GOT-class x86_64 relocs are explicitly out of scope for this
# bead (vms-cd1/vms-2e4) -- hidden visibility keeps the access a plain PC32
# RIP-relative load instead, so this test stays inside the "simple" reloc set.
$CC -fPIC -fvisibility=hidden -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/ptr.o" "$WORK/ptr.c"
echo "-- R_X86_64_64 relocs gcc emitted against .data --"
readelf -rW "$WORK/ptr.o" | awk '/R_X86_64_64[^0-9]/{print $3}' | sort | uniq -c
readelf -rW "$WORK/ptr.o" | grep -qE "R_X86_64_64 " || { echo "FAIL: expected R_X86_64_64 pointer inits"; exit 1; }
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "f=PROCEDURE,g=PROCEDURE,tbl=DATA,usetbl=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBPTR\$SHR.EXE" "$WORK/ptr.o"
# ABSCHECK proves the R_X86_64_64 write is correct: the pointer table entries
# resolved to the real image-relative addresses of f/g and were recorded in
# .vms$rel for load-bias at activation. (Do NOT execute usetbl() through
# CALLSLOT here: CALLSLOT calls symbol-vector entries through ovmx_sv_resolve,
# which bias-adjusts by the real mmap base -- but the .data pointer table's
# raw bytes are the UNBIASED image-relative addresses LINK.EXE wrote; only the
# real activator applies .vms$rel at load time. Jumping through the raw table
# value without that bias step segfaults for an honest reason -- it is not
# what any real IMGACT-activated image does -- so this harness checks the
# reloc's written bytes directly instead, exactly as the aarch64 ABS64 test
# in run_test.sh does for R_AARCH64_ABS64.)
"$WORK/ABSCHECK" "$WORK/LIBPTR\$SHR.EXE" tbl 0 f || { echo "FAIL: tbl[0] != &f"; exit 1; }
"$WORK/ABSCHECK" "$WORK/LIBPTR\$SHR.EXE" tbl 8 g || { echo "FAIL: tbl[1] != &g"; exit 1; }

echo
echo "== output header: e_machine must be EM_X86_64 =="
readelf -h "$WORK/LIBPTR\$SHR.EXE" | grep -E "Machine:" || true
readelf -h "$WORK/LIBPTR\$SHR.EXE" | grep -q "X86-64" \
    || { echo "FAIL: emitted image is not EM_X86_64"; exit 1; }

echo
echo "== regression: an aarch64 input in the SAME LINK.EXE build must still =="
echo "   yield EM_AARCH64 (the gate is additive, not a replacement) =="
# Cross-compile a real aarch64 .o via the same arm64/alpine container CI uses
# for the vmslink-mvp job (.github/workflows/ci.yml) -- no native aarch64
# cross-compiler is assumed on this host. Only the COMPILE step runs under
# arm64 emulation; the produced .o is then fed to the NATIVE x86_64 LINK.EXE
# built above, so the gate/emission code under test runs unemulated.
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    mkdir -p "$WORK/aarch64-cc"
    cat > "$WORK/aarch64-cc/aarch64math.c" <<'EOF'
int myadd(int a, int b) { return a + b; }
EOF
    docker run --rm --platform linux/arm64 -v "$WORK/aarch64-cc:/out" docker.io/library/alpine:3.20 sh -c '
        set -e
        apk add --no-cache gcc musl-dev >/dev/null
        gcc -fPIC -O2 -ffreestanding -fno-stack-protector -c -o /out/aarch64math.o /out/aarch64math.c
    '
    cp "$WORK/aarch64-cc/aarch64math.o" "$WORK/aarch64math.o"
    "$WORK/LINK.EXE" --shareable --symbol-vector "myadd=PROCEDURE" \
        --gsmatch EQUAL,1,0 -o "$WORK/LIBAARCH64\$SHR.EXE" "$WORK/aarch64math.o"
    readelf -h "$WORK/LIBAARCH64\$SHR.EXE" | grep -E "Machine:" || true
    readelf -h "$WORK/LIBAARCH64\$SHR.EXE" | grep -q "AArch64" \
        || { echo "FAIL: aarch64 input did not still emit EM_AARCH64"; exit 1; }
    echo "regression OK: aarch64 input -> EM_AARCH64 in the SAME native x86_64 LINK.EXE"

    echo
    echo "== mixed-architecture link must be rejected (gate stays additive, not permissive) =="
    set +e
    "$WORK/LINK.EXE" --shareable --symbol-vector "lookup=PROCEDURE" \
        --gsmatch EQUAL,1,0 -o "$WORK/BAD\$SHR.EXE" "$WORK/nonleaf.o" "$WORK/aarch64math.o" 2>"$WORK/mix.err"
    RC=$?
    set -e
    [ "$RC" -ne 0 ] || { echo "FAIL: mixed aarch64+x86_64 link should have been rejected"; exit 1; }
    grep -qi "mixed-architecture" "$WORK/mix.err" || { echo "FAIL: wrong rejection reason"; cat "$WORK/mix.err"; exit 1; }
    echo "mixed-architecture link correctly rejected: $(cat "$WORK/mix.err")"
else
    echo "SKIP: no usable docker on this host to cross-compile the aarch64"
    echo "      regression object -- covered instead by run_test.sh under arm64"
    echo "      QEMU emulation (same LINK.EXE source, same gate code path)."
fi

echo
echo "ALL LINK.EXE x86_64 SIMPLE-RELOC CHECKS PASSED"
