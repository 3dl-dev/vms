#!/bin/sh
# run_multiobj_exec_x86_64.sh — x86_64 counterpart of run_multiobj_exec.sh
# (bead vms-206, following on vms-ba1/vms-8f5).
#
# vms-2e4's implementer proving x86_64 TLSDESC found that emit_shareable() in
# LINK.EXE (src/vmslink/link.c) had TWO pieces hardcoded to AArch64 machine
# code regardless of g_out_machine: (1) the synthesized crt0 entry stub for a
# main()-based --executable, and (2) the cross-image CALL PLT stub + the
# import-CALL detection gated on R_AARCH64_CALL26/JUMP26 only. An x86_64
# main()-based program, or any x86_64 image with a cross-image CALL import,
# linked to garbage. DCL.EXE (main()-based, calls into every shareable it
# links against) is exactly this shape — vms-cb5f (the real x86_64 DCL.EXE
# link proof) cannot succeed without this fixed.
#
# This is the x86_64 mirror of run_multiobj_exec.sh's ground-source proof,
# unchanged in what it proves, changed only in target machine:
#   1. crt0            — LINK synthesizes a REAL x86_64 entry stub (e_entry):
#      mov rdi,[rsp] / lea rsi,[rsp+8] / lea rdx,[rsi+rdi*8+8] recovers
#      argc/argv/envp off the kernel-built initial process stack per the
#      SysV ABI, `call main` then `mov edi,eax ; call exit` tail the return
#      value into exit(). Two runs (0 args, 2 args) with DIFFERENT expected
#      exit codes prove argc is really read off the stack, not a constant.
#   2. cross-image CALL — mprog.c calls put_str/str_len/exit, defined in NO
#      input object, exported by a --use'd producer shareable
#      (LIBRT$SHR.EXE): R_X86_64_PLT32 references to them get routed through
#      the import table into a REAL `jmp *disp32(%rip)` PLT stub (link.c's
#      x86_64 analogue of aarch64's adrp/ldr/br), which at activation IMGACT
#      points at the producer's real code. The printed banner + computed
#      line + process exit code are ground-source proof the PLT stub
#      actually reaches and executes the producer's function and returns
#      correctly — not just a readelf/byte check of the stub bytes.
#
# The producer (LIBRT$SHR.EXE) is a tiny hand-written runtime (raw write(2)/
# exit_group(2) syscalls, no libc) rather than a whole-archive musl ingest:
# vms-206 is about the crt0/PLT machine-code gap, not musl compatibility
# (already proven separately by vms-004/vms-8f5's whole-archive tests), and a
# minimal producer keeps this harness independent of which musl package
# happens to be installed on the host.
#
# Runs natively: the project's dev host and CI runners are both x86_64 (see
# CLAUDE.md, workshop-dev-host), so — unlike the aarch64 harness, which needs
# arm64 QEMU emulation via a container — this test builds with the host's
# native gcc and executes MULTIPROG.EXE directly: the real Linux kernel ELF
# loader invokes IMGACT.EXE via PT_INTERP exactly as it would for any
# process, giving a REAL kernel-built argc/argv/envp stack. No docker, no
# qemu-user, no emulation in the path under test.
#
# Exit code is argc + str_len("beta")(=4): 1+4=5 with no args, 3+4=7 with two
# args. Exit 0 on success.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
WORK=${WORK:-/tmp/multiobj-exec-x86_64}
rm -rf "$WORK"; mkdir -p "$WORK"

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

echo "== build IMGACT.EXE (x86_64) + LINK.EXE (host gcc) =="
( cd "$IMGACT_DIR" && make ARCH=x86_64 CC="$CC" clean >/dev/null 2>&1 || true; make ARCH=x86_64 CC="$CC" ) >/dev/null 2>&1
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$LINK_DIR/include" -o "$WORK/LINK.EXE" "$LINK_DIR/link.c"
readelf -h "$SYSEXE/IMGACT.EXE" | grep -q "X86-64" || { echo "FAIL: IMGACT.EXE is not x86_64"; exit 1; }

echo "== compile a minimal producer runtime: put_str/str_len/exit (raw syscalls, no libc) =="
cat > "$WORK/librt.c" <<'EOF'
static long sys_write(int fd, const void *buf, unsigned long n) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                      : "a"(1), "D"(fd), "S"(buf), "d"(n)
                      : "rcx", "r11", "memory");
    return ret;
}
void put_str(const char *s) {                 /* PROCEDURE universal (write(1,s,strlen(s))) */
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
    sys_write(1, "\n", 1);
}
int str_len(const char *s) {                   /* PROCEDURE universal */
    int n = 0;
    while (s[n]) n++;
    return n;
}
void exit(int code) {                          /* PROCEDURE universal, tail-called by crt0 */
    __asm__ volatile("syscall" :: "a"(60), "D"(code) : "memory");
    __builtin_unreachable();
}
EOF
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector"
$CC $CFLAGS -c -o "$WORK/librt.o" "$WORK/librt.c"
"$WORK/LINK.EXE" --shareable \
    --symbol-vector "put_str=PROCEDURE,str_len=PROCEDURE,exit=PROCEDURE" \
    --gsmatch LEQUAL,1,0 -o "$SYSLIB/LIBRT\$SHR.EXE" "$WORK/librt.o"
readelf -h "$SYSLIB/LIBRT\$SHR.EXE" | grep -q "X86-64" || { echo "FAIL: LIBRT\$SHR.EXE is not x86_64"; exit 1; }
readelf -SW "$SYSLIB/LIBRT\$SHR.EXE" | grep -q '\.vms\$sv' || { echo "FAIL: LIBRT\$SHR no symbol vector"; exit 1; }

echo "== compile a TWO-object main() C program (gcc -fPIC, no libc) =="
cat > "$WORK/mhelp.c" <<'EOF'
/* A const pointer TABLE -> .data.rel.ro: 4 ABS64 pointer initializers the linker
 * must resolve intra-image and record in .vms$rel (biased at activation). */
const char *const g_words[] = { "alpha", "beta", "gamma", "delta" };
extern int str_len(const char *s);            /* imported from LIBRT$SHR (cross-image CALL) */
int word_len(int i) { return str_len(g_words[i & 3]); }
const char *banner(void) { return "OVMX-MULTIOBJ-X86"; }
EOF
cat > "$WORK/mprog.c" <<'EOF'
extern const char *banner(void);      /* defined in mhelp.c (intra-image PLT32) */
extern int word_len(int i);           /* defined in mhelp.c (intra-image PLT32) */
extern void put_str(const char *s);   /* imported from LIBRT$SHR (cross-image CALL) */

static char buf[64];
static void put_int(char *dst, int v) {
    char tmp[16]; int n = 0;
    if (v == 0) { dst[0] = '0'; dst[1] = 0; return; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) dst[i] = tmp[n - 1 - i];
    dst[n] = 0;
}

int main(int argc, char **argv)
{
    /* argv[0] is read off the kernel-built initial stack (proves crt0 argv/argc). */
    __builtin_memcpy(buf, "argc=", 5);
    put_int(buf + 5, argc);
    put_str(banner());
    put_str(buf);
    put_str(argv[0]);
    int code = argc + word_len(1);    /* argc + str_len("beta")(=4) */
    __builtin_memcpy(buf, "computed=", 9);
    put_int(buf + 9, code);
    put_str(buf);
    return code;                      /* crt0 tail-calls exit(code) */
}
EOF
CFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector"
$CC $CFLAGS -c -o "$WORK/mhelp.o" "$WORK/mhelp.c"
$CC $CFLAGS -c -o "$WORK/mprog.o" "$WORK/mprog.c"
echo "-- mprog.o .text relocations (expect PLT32 to banner/word_len/put_str) --"
readelf -rW "$WORK/mprog.o" | awk '/R_X86_64/{print $3}' | sort | uniq -c
readelf -rW "$WORK/mprog.o" | grep -q "R_X86_64_PLT32" || { echo "FAIL: expected R_X86_64_PLT32 call relocs"; exit 1; }

echo "== LINK.EXE --executable --use LIBRT\$SHR -> MULTIPROG.EXE (multi-object) =="
"$WORK/LINK.EXE" --executable --use "$SYSLIB/LIBRT\$SHR.EXE" \
    -o "$WORK/MULTIPROG.EXE" "$WORK/mprog.o" "$WORK/mhelp.o"
chmod +x "$WORK/MULTIPROG.EXE"

echo "-- MULTIPROG.EXE: PT_INTERP=IMGACT, PT_PHDR, .vms\$imp (put_str/str_len/exit), EM_X86_64 --"
readelf -h  "$WORK/MULTIPROG.EXE" | grep -E "Machine:" || true
readelf -lW "$WORK/MULTIPROG.EXE" | grep -E '\bINTERP\b|\bPHDR\b' || true
readelf -SW "$WORK/MULTIPROG.EXE" | grep -E '\.vms\$imp|\.vms\$rel|\.plt|\.igot' || true
readelf -h  "$WORK/MULTIPROG.EXE" | grep -q "X86-64"  || { echo "FAIL: MULTIPROG.EXE is not x86_64"; exit 1; }
readelf -lW "$WORK/MULTIPROG.EXE" | grep -q 'INTERP'  || { echo "FAIL: no PT_INTERP"; exit 1; }
readelf -lW "$WORK/MULTIPROG.EXE" | grep -q 'PHDR'    || { echo "FAIL: no PT_PHDR"; exit 1; }
readelf -SW "$WORK/MULTIPROG.EXE" | grep -q '\.vms\$imp' || { echo "FAIL: no .vms\$imp"; exit 1; }
readelf -SW "$WORK/MULTIPROG.EXE" | grep -q '\.vms\$rel' || { echo "FAIL: no .vms\$rel (ABS64 g_words[] not recorded)"; exit 1; }
readelf -SW "$WORK/MULTIPROG.EXE" | grep -q '\.plt'      || { echo "FAIL: no .plt (cross-image CALL stub) synthesized"; exit 1; }

echo
echo "== RUN ./MULTIPROG.EXE FOR REAL, no args (kernel -> IMGACT -> LIBRT\$SHR, native x86_64) =="
set +e
OUT=$("$WORK/MULTIPROG.EXE"); RC=$?
set -e
echo "$OUT"
echo "exit code = $RC (expect 5 = argc(1) + str_len(\"beta\")(4))"
echo "$OUT" | grep -q 'OVMX-MULTIOBJ-X86' || { echo "FAIL: banner line missing (cross-image PLT call to put_str never happened)"; exit 1; }
echo "$OUT" | grep -q 'argc=1' || { echo "FAIL: argc line wrong (crt0 argc read wrong)"; exit 1; }
echo "$OUT" | grep -q 'computed=5' || { echo "FAIL: computed line wrong (cross-image PLT call to str_len wrong)"; exit 1; }
[ "$RC" -eq 5 ] || { echo "FAIL: multi-object exec did not run correctly (got $RC, want 5 -- crt0 exit() call wrong)"; exit 1; }

echo
echo "== RUN ./MULTIPROG.EXE FOR REAL, TWO args (proves argc read from stack, not constant) =="
set +e
OUT2=$("$WORK/MULTIPROG.EXE" alpha beta); RC2=$?
set -e
echo "$OUT2"
echo "exit code = $RC2 (expect 7 = argc(3) + str_len(\"beta\")(4))"
echo "$OUT2" | grep -q 'argc=3' || { echo "FAIL: argc not 3 with two args (argv/argc off stack wrong)"; exit 1; }
[ "$RC2" -eq 7 ] || { echo "FAIL: argc-dependent exit wrong (got $RC2, want 7)"; exit 1; }

echo
echo "MILESTONE (vms-206): a REAL multi-object main() C program links VMS-native"
echo "via LINK.EXE --executable --use LIBRT\$SHR on x86_64, activates through a"
echo "native x86_64 IMGACT.EXE, reads argc/argv off the process stack via LINK's"
echo "synthesized x86_64 crt0 stub, calls put_str/str_len through a REAL x86_64"
echo "PLT stub (jmp *disp32(%rip)) into LIBRT\$SHR, and exits with an"
echo "argc-computed code via a cross-image exit() call — no ld / no ld.so, no"
echo "emulation. Unblocks vms-cb5f (real x86_64 DCL.EXE link proof)."
