#!/bin/sh
# run_test_x86_64.sh — LINK.EXE x86_64 reloc proof harness (vms-8f5, vms-cd1).
#
# Ground-source done condition: hand-built x86_64 .o's carrying one of each of
# the "simple" static relocs the survey (docs/design-link-x86_64-relocs.md)
# confirmed are present in the real x86_64 object set — R_X86_64_64 (absolute
# data pointer), R_X86_64_PC32/PLT32 (same-image and cross-object function
# call) — link via LINK.EXE to a real ET_DYN x86_64 ELF (readelf -h shows
# EM_X86_64) and the produced image RUNS: CALLSLOT mmaps it and calls into it
# natively (this harness runs on an x86_64 host, so no emulation is needed for
# the execution half — only the byte-level relocation math is under test).
#
# vms-cd1 adds the GOT-relative pair R_X86_64_GOTPCREL / R_X86_64_REX_GOTPCRELX
# — a cross-image DATA import (mirrors DCL's real vms_months/vms_device_table
# imports from LIBVMS$SHR) proved via ACTIVATE (ovmx_activate.c), the same
# reference hosted-C SYS$IMGACT activation run_test.sh already uses for the
# aarch64 ADR_GOT_PAGE/LD64_GOT_LO12_NC cross-image DATA import: a REAL load
# (mmap, symbol-vector resolution, GOT-cell patch, control transfer), not just
# a readelf/byte check.
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

echo "== build LINK.EXE + CALLSLOT + ABSCHECK + ACTIVATE (host tools) =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE"    "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/CALLSLOT"    "$SRC/test/call_slot.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/ABSCHECK"    "$SRC/test/abs_check.c"
# ACTIVATE (ovmx_activate.c, vms-142) is the reference hosted-C SYS$IMGACT
# activation the aarch64 harness (run_test.sh) already uses to prove cross-image
# DATA imports round-trip through a real load (mmap the images, resolve
# .vms$imp via the producer's symbol vector, patch the consumer's GOT cell,
# transfer control) -- it is architecture-generic (pure ELF/mmap/syscall-ABI
# code, no AARCH64-specific instruction encoding), so the SAME binary proves
# the x86_64 GOTPCREL path below with no emulation needed on this native host.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/ACTIVATE"    "$SRC/test/ovmx_activate.c"

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
echo "== cross-image DATA import: R_X86_64_GOTPCREL / R_X86_64_REX_GOTPCRELX (vms-cd1) =="
# Mirrors DCL's real cross-image DATA imports (vms_months/vms_device_table from
# LIBVMS\$SHR): a producer shareable exports a DATA universal; the consumer
# executable reads it through 'mov sym@GOTPCREL(%rip), reg' -- a SINGLE
# relocation computed as GOT_entry_addr+A-P, unlike aarch64's ADR_GOT_PAGE/
# LD64_GOT_LO12_NC PAIR. LINK's --executable path turns the GOT reference into
# a .vms\$imp DATA binding + import-GOT cell, same machinery as the aarch64
# cross-image DATA import test in run_test.sh (vms-20b) -- reused here, not
# reimplemented, confirming the GOT-synthesis machinery link.c already had was
# already architecture-generic and needed no format change, only the new
# is_got_reloc()/patch_got() cases below.
#
# Ground-source proof: ACTIVATE performs a REAL load (mmap both images,
# resolve the import through the producer's actual symbol vector, patch the
# GOT cell, transfer control into real machine code) and the consumer exits
# with shared_counter+9 -- not a readelf-only check of the written bytes.
cat > "$WORK/datalib.c" <<'EOF'
int shared_counter = 90;                  /* exported DATA universal (.data) */
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/datalib.o" "$WORK/datalib.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "shared_counter=DATA" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBDATA\$SHR.EXE" "$WORK/datalib.o"

cat > "$WORK/datacons.c" <<'EOF'
extern int shared_counter;                /* imported via GOTPCREL(%rip) */
void _start(void) {
    int r = shared_counter + 9;           /* == 99, data resolved across images */
    __asm__ volatile("mov %0, %%edi\n\t"
                      "mov $60, %%eax\n\t" /* exit_group */
                      "syscall"
                      :: "r"(r) : "rdi", "rax", "memory");
    __builtin_unreachable();
}
EOF
# Default (REX_GOTPCRELX): -fPIC without hidden visibility can't prove
# shared_counter binds locally, so gcc/gas route the read through GOTPCREL --
# gas's default emits the REX-tagged relaxable variant.
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/datacons.o" "$WORK/datacons.c"
echo "-- consumer .text relocations (expect GOTPCREL/REX_GOTPCRELX to shared_counter) --"
readelf -rW "$WORK/datacons.o" | awk '/R_X86_64/{print $3}' | sort | uniq -c
readelf -rW "$WORK/datacons.o" | grep -qE "R_X86_64_(GOTPCREL|REX_GOTPCRELX)" \
    || { echo "FAIL: expected a GOTPCREL/REX_GOTPCRELX cross-image DATA reference"; exit 1; }
"$WORK/LINK.EXE" --executable --use "$WORK/LIBDATA\$SHR.EXE" \
    -o "$WORK/DATAPROG.EXE" "$WORK/datacons.o"
readelf -SW "$WORK/DATAPROG.EXE" | grep -E '\.got|\.vms\$imp' || true
set +e
"$WORK/ACTIVATE" "$WORK/DATAPROG.EXE" "$WORK/LIBDATA\$SHR.EXE"; RC=$?
set -e
echo "consumer exit code = $RC (expect 99 = shared_counter(90)+9 read across images)"
[ "$RC" -eq 99 ] || { echo "FAIL: R_X86_64_REX_GOTPCRELX cross-image DATA import did not yield 99 (got $RC)"; exit 1; }

# Also prove the plain (non-relaxable) R_X86_64_GOTPCREL form -- gas emits it
# with relaxation hints suppressed (-mrelax-relocations=no). Same is_got_reloc/
# patch_got code path in link.c handles both; this proves it against the OTHER
# survey-listed type, not just the one gcc happens to emit by default.
$CC -Wa,-mrelax-relocations=no -fPIC -O2 -ffreestanding -fno-stack-protector \
    -c -o "$WORK/datacons_norelax.o" "$WORK/datacons.c"
echo "-- norelax consumer .text relocations (expect plain GOTPCREL) --"
readelf -rW "$WORK/datacons_norelax.o" | awk '/R_X86_64/{print $3}' | sort | uniq -c
readelf -rW "$WORK/datacons_norelax.o" | grep -q "R_X86_64_GOTPCREL " \
    || { echo "FAIL: expected plain R_X86_64_GOTPCREL with relaxation suppressed"; exit 1; }
"$WORK/LINK.EXE" --executable --use "$WORK/LIBDATA\$SHR.EXE" \
    -o "$WORK/DATAPROG_NORELAX.EXE" "$WORK/datacons_norelax.o"
set +e
"$WORK/ACTIVATE" "$WORK/DATAPROG_NORELAX.EXE" "$WORK/LIBDATA\$SHR.EXE"; RC=$?
set -e
echo "norelax consumer exit code = $RC (expect 99)"
[ "$RC" -eq 99 ] || { echo "FAIL: R_X86_64_GOTPCREL cross-image DATA import did not yield 99 (got $RC)"; exit 1; }

echo
echo "== intra-image indirect call: R_X86_64_GOTPCRELX (type 41, vms-e5d) =="
# Grounded against real musl/gcc-toolchain objects (docs/design-link-x86_64-
# relocs.md's standing methodology + this bead's own survey): plain (non-REX)
# GOTPCRELX is what gas emits for a non-REX-prefixed GOT-indirect instruction
# -- observed byte-exactly as `call *sym@GOTPCREL(%rip)` / `jmp *sym@GOTPCREL
# (%rip)` (opcode ff /2 or ff /4, no REX prefix: near call/jmp defaults to a
# 64-bit operand without REX.W), which musl-gcc emits with -fno-plt (used
# empirically: 1521 real occurrences in Alpine x86_64 libgcc.a alone --
# GCC's runtime calling abort()/memcpy()/etc. through the GOT instead of a
# lazy PLT stub -- the actual gap vms-cb5f's whole-archive DECC$SHR build hit,
# not libc.a itself). Same flat-disp32-write shape as GOTPCREL/REX_GOTPCRELX
# (addend -4), confirmed via objdump -dr before this fix landed.
#
# UNLIKE the cross-image DATA import above, this is an INTRA-image reference:
# both caller and callee are defined in the SAME shareable, so the GOT cell is
# filled by LINK.EXE itself (not IMGACT's .vms$imp) and recorded in .vms$rel
# for load-bias. gotpcrelx_activate.c performs that bias step for real (mmaps
# the image at a genuine, non-zero, ASLR'd base and adds it to every .vms$rel
# slot) before calling in -- proving the GOT cell resolves to the correct
# address under a real load, not just a readelf/byte check.
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/GOTPCRELX_ACTIVATE" "$SRC/test/gotpcrelx_activate.c"
cat > "$WORK/gpx_callee.c" <<'EOF'
int gpx_callee(int x) { return x * 7; }
EOF
cat > "$WORK/gpx_caller.c" <<'EOF'
extern int gpx_callee(int);
int gpx_caller(int x, int unused) { (void)unused; return gpx_callee(x) + 1; }
EOF
$CC -fPIC -fno-plt -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/gpx_callee.o" "$WORK/gpx_callee.c"
$CC -fPIC -fno-plt -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/gpx_caller.o" "$WORK/gpx_caller.c"
echo "-- gpx_caller.o .text relocations (expect a plain GOTPCRELX to gpx_callee) --"
readelf -rW "$WORK/gpx_caller.o" | awk '/R_X86_64/{print $3}' | sort | uniq -c
readelf -rW "$WORK/gpx_caller.o" | grep -qE "R_X86_64_GOTPCRELX " \
    || { echo "FAIL: expected a plain R_X86_64_GOTPCRELX (type 41) call reloc"; exit 1; }
"$WORK/LINK.EXE" --shareable --symbol-vector "gpx_caller=PROCEDURE,gpx_callee=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBGPX\$SHR.EXE" "$WORK/gpx_caller.o" "$WORK/gpx_callee.o"
readelf -SW "$WORK/LIBGPX\$SHR.EXE" | grep -E '\.got|\.vms\$rel' || true
set +e
"$WORK/GOTPCRELX_ACTIVATE" "$WORK/LIBGPX\$SHR.EXE" gpx_caller 5 0; RC=$?
set -e
echo "gpx_caller(5) exit = $RC (expect 36 = gpx_callee(5)*7+1)"
[ "$RC" -eq 36 ] || { echo "FAIL: intra-image R_X86_64_GOTPCRELX indirect call did not yield 36 (got $RC)"; exit 1; }

echo
echo "== SYMBOL_VECTOR alias: universal/internal form (real DECC\$SHR decc\$<name>) (vms-c07 R1) =="
# Real OpenVMS DECC\$SHR exports `decc\$fprintf` (the universal a consumer imports)
# bound to the C-RTL's INTERNAL implementation symbol -- the VSI Linker
# SYMBOL_VECTOR=(universal/internal=PROCEDURE) alias form (Utility Manual S5.6).
# OVMX's musl libc.a defines `fprintf`, not `decc\$fprintf`, so R1's decc\$ vector
# needs exactly this: export the universal under a name that DIFFERS from the
# internal defining symbol. Proof: slot 0 (the alias) must resolve to real_impl's
# code, AND the exported .vms\$sv name blob must carry the ALIAS name, not the
# internal one. A plain `plain_export=PROCEDURE` shares the same vector to prove
# the slash-parsing leaves ordinary (internal==universal) entries untouched.
cat > "$WORK/aliaslib.c" <<'EOF'
int real_impl(int x, int y)   { (void)y; return x * 7; }   /* internal symbol */
int plain_export(int x, int y){ (void)y; return x + 5; }   /* plain (no alias) */
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/aliaslib.o" "$WORK/aliaslib.c"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "alias_export/real_impl=PROCEDURE,plain_export=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$WORK/LIBALIAS\$SHR.EXE" "$WORK/aliaslib.o"
set +e
"$WORK/CALLSLOT" "$WORK/LIBALIAS\$SHR.EXE" 0 6 0; RC=$?    # slot0=alias_export -> real_impl(6)=42
set -e
echo "alias slot0(6) exit = $RC (expect 42 = real_impl(6)*7)"
[ "$RC" -eq 42 ] || { echo "FAIL: aliased universal did not resolve to the internal impl (got $RC, want 42)"; exit 1; }
set +e
"$WORK/CALLSLOT" "$WORK/LIBALIAS\$SHR.EXE" 1 6 0; RC=$?    # slot1=plain_export(6)=11
set -e
echo "plain slot1(6) exit = $RC (expect 11 = plain_export(6)+5)"
[ "$RC" -eq 11 ] || { echo "FAIL: plain universal in a mixed vector broke (got $RC, want 11)"; exit 1; }
# The EXPORTED universal name is the ALIAS, not the internal defining symbol:
# isolate the .vms\$sv section (which carries only exported universal names) and
# assert alias_export is present while the internal `real_impl` is NOT exported.
objcopy -O binary --only-section='.vms$sv' "$WORK/LIBALIAS\$SHR.EXE" "$WORK/sv.bin"
grep -qa "alias_export" "$WORK/sv.bin" || { echo "FAIL: alias name 'alias_export' not exported in .vms\$sv"; exit 1; }
grep -qa "plain_export" "$WORK/sv.bin" || { echo "FAIL: 'plain_export' not exported in .vms\$sv"; exit 1; }
if grep -qa "real_impl" "$WORK/sv.bin"; then echo "FAIL: internal symbol 'real_impl' leaked as a universal name in .vms\$sv"; exit 1; fi
echo "alias OK: .vms\$sv exports alias_export + plain_export; internal 'real_impl' not exported"

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
