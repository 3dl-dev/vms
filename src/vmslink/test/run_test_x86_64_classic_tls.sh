#!/bin/sh
# run_test_x86_64_classic_tls.sh — LINK.EXE x86_64 CLASSIC GD/LD -> Local-Exec
# relaxation proof (bead vms-83e8, epic vms-da0 F2b).
#
# The SIBLING of run_test_x86_64_tls.sh. That harness proves the gnu2-dialect
# TLSDESC access model (R_X86_64_GOTPC32_TLSDESC + R_X86_64_TLSDESC_CALL). This
# one proves the OTHER x86_64 TLS access model — the CLASSIC general-/local-
# dynamic sequence that calls __tls_get_addr:
#
#   GD:  66 48 8d 3d <d32>   data16 lea x@tlsgd(%rip),%rdi   R_X86_64_TLSGD
#        66 48 ff 15 <d32>   data16 call *__tls_get_addr(%rip)  (GOTPCRELX, -fno-plt)
#   LD:  48 8d 3d <d32>      lea x@tlsld(%rip),%rdi          R_X86_64_TLSLD
#        ff 15 <d32>         call *__tls_get_addr(%rip)      (GOTPCRELX, -fno-plt)
#        ... 8b 80 <d32>     mov x@dtpoff(%rax),reg          R_X86_64_DTPOFF32
#
# WHY THIS MATTERS (vms-83e8). The upstream libstdc++.a / libgcc.a whole-archived
# into every C++ OVMX image (cpptest, and eventually cc1plus) are built with the
# DEFAULT (classic) TLS dialect and -fno-plt, so EVERY thread-local access in
# libsupc++'s eh_globals.o (__cxa_get_globals, LD form) and libgcc's decimal-float
# bid*.o (__bid_IDEC_glbround, GD form) is one of the two sequences above, NOT a
# TLSDESC pair. LINK.EXE relaxes each to Local-Exec (patch_tls_le / the DTPOFF32
# arm in link.c, bead vms-76a) so the resulting single static image never calls
# __tls_get_addr at all. Before vms-76a a residual __tls_get_addr call SIGSEGV'd
# a C++ image during libstdc++ static-init; this harness is the standing guard
# that it stays relaxed. run_test_x86_64_tls.sh's gnu2 program does NOT exercise
# this path (a gnu2 object carries no TLSGD/TLSLD/__tls_get_addr at all), so
# without this test the classic GD/LD relaxation has no end-to-end coverage.
#
# GROUND-SOURCE DONE CONDITION (not a readelf-only check): the linked image is
# ACTIVATED FOR REAL — the kernel execs it, its PT_INTERP runs the real x86_64
# IMGACT.EXE, IMGACT lays out the .tdata/.tbss TLS block and sets TP (%fs), and
# the program reads and writes both a GD (external __thread) and an LD (function-
# local static _Thread_local) variable through the RELAXED Local-Exec sequences.
# The embedded tpoff LINK.EXE wrote is TP-relative (moff - ALIGN_UP(tls_memsz,
# tls_align)); it can only read back the right values if that offset AGREES with
# where IMGACT's assign_tls_offsets() placed the block — the same LINK/IMGACT
# contract the gnu2 harness proves, exercised here for the classic model.
#
# Runs natively on an x86_64 host: no emulation, no container needed.
# Needs /vms/SYS0/SYSCOMMON/{SYSEXE,SYSLIB} to be creatable (PT_INTERP path).
# Exit 0 on success.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)                  # src/vmslink
IMGACT_DIR=$(cd "$SRC/../imgact" && pwd)     # src/imgact
WORK=${WORK:-/tmp/vmslink-test-x86_64-classic-tls}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    x86_64|amd64) ;;
    *) echo "FAIL: this harness must run on an x86_64 host (got $(uname -m))"; exit 1 ;;
esac

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB" 2>/dev/null || {
    sudo mkdir -p "$SYSEXE" "$SYSLIB"
    sudo chown "$(id -u):$(id -g)" "$SYSEXE" "$SYSLIB"
}

echo "== build LINK.EXE (host tool) + the real x86_64 IMGACT.EXE =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE" "$SRC/link.c"
( cd "$IMGACT_DIR" && make CC="$CC" ARCH=x86_64 clean >/dev/null 2>&1 || true
  cd "$IMGACT_DIR" && make CC="$CC" ARCH=x86_64 >/dev/null )
readelf -hW "$IMGACT_DIR/IMGACT.EXE" | grep -q "X86-64" \
    || { echo "FAIL: IMGACT.EXE is not an x86_64 build"; exit 1; }
cp "$IMGACT_DIR/IMGACT.EXE" "$SYSEXE/IMGACT.EXE"
chmod +x "$SYSEXE/IMGACT.EXE"

# ---------------------------------------------------------------------------
# A producer shareable exporting one DATA universal, so the executable gets a
# .vms$imp table (IMGACT's symbol-vector activation path requires one). Same
# device run_test_x86_64_tls.sh uses.
# ---------------------------------------------------------------------------
echo
echo "== producer shareable (one DATA universal, so the exe has a .vms\$imp) =="
cat > "$WORK/tlslib.c" <<'EOF'
int shared_base = 90;
EOF
$CC -fPIC -O2 -ffreestanding -fno-stack-protector -c -o "$WORK/tlslib.o" "$WORK/tlslib.c"
"$WORK/LINK.EXE" --shareable --symbol-vector "shared_base=DATA" \
    --gsmatch EQUAL,1,0 -o "$SYSLIB/TLSDATA\$SHR.EXE" "$WORK/tlslib.o"

# ---------------------------------------------------------------------------
# The GD subject lives in its OWN TU so the reference from the main TU is a
# genuine cross-TU __thread access — that is what makes gcc pick the general-
# dynamic model (an intra-TU __thread under -fPIC could be lowered to LE by the
# compiler and never emit a TLSGD at all).
# ---------------------------------------------------------------------------
echo
echo "== compile the classic-dialect TLS test program (DEFAULT dialect, -fno-plt) =="
cat > "$WORK/tlsdef.c" <<'EOF'
/* GD subject: an external-linkage __thread with a nonzero .tdata initializer
 * IMGACT must copy. Referenced from the other TU -> R_X86_64_TLSGD there. */
__thread int g_ext = 100;
EOF

cat > "$WORK/classic_tls.c" <<'EOF'
/*
 * x86_64 CLASSIC GD/LD test image for LINK.EXE (vms-83e8). Freestanding: it owns
 * _start and talks to the kernel with raw syscalls, so activation exercises
 * LINK.EXE's GD/LD->LE relaxation and IMGACT's TLS setup with no C-RTL between.
 *
 * GENERAL-DYNAMIC (external-linkage __thread g_ext, defined in tlsdef.c):
 *   66 48 8d 3d ... (data16 lea x@tlsgd) ; 66 48 ff 15 ... (data16 call
 *   *__tls_get_addr, because -fno-plt). LINK relaxes the 16-byte window to
 *   `mov %fs:0,%rax ; lea tpoff(%rax),%rax`.
 *
 * LOCAL-DYNAMIC (function-local static _Thread_local s_a/s_b): ONE
 *   48 8d 3d ... (lea x@tlsld) ; ff 15 ... (call *__tls_get_addr) grabs the
 *   block base; each variable is then reached with an R_X86_64_DTPOFF32
 *   operand. LINK relaxes the lea+call to `mov %fs:0,%rax` and writes each
 *   DTPOFF32 as a TP-relative offset. This is the exact shape of libsupc++'s
 *   eh_globals.o __cxa_get_globals — the object whose residual __tls_get_addr
 *   call once SIGSEGV'd a C++ image.
 */
extern __thread int g_ext;      /* GD, .tdata, init 100 */

/* Kept in a separate noinline function so gcc emits the local-dynamic model
 * (one TLSLD + per-variable DTPOFF32) rather than folding the statics away. */
__attribute__((noinline))
static int ld_accumulate(int add_a, int add_b)
{
    static _Thread_local int s_a = 7;   /* .tdata */
    static _Thread_local int s_b;       /* .tbss  */
    s_a += add_a;
    s_b += add_b;
    return s_a * 1000 + s_b;            /* reads back through DTPOFF32 too */
}

extern int shared_base;             /* cross-image DATA import (GOTPCREL) */

#define SYS_write       1
#define SYS_exit       60
#define SYS_exit_group 231

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static void die(int code)
{
    sys3(SYS_exit_group, code, 0, 0);
    __builtin_unreachable();
}

/* Diagnostic: "<tag>=<value>\n" on stderr, so a failure says WHICH value was
 * wrong instead of only an exit code. */
static void report(const char *tag, long v)
{
    char buf[64];
    int n = 0, neg = 0;
    while (tag[n] && n < 24) { buf[n] = tag[n]; n++; }
    buf[n++] = '=';
    if (v < 0) { neg = 1; v = -v; }
    char d[24]; int k = 0;
    do { d[k++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    if (neg) buf[n++] = '-';
    while (k) buf[n++] = d[--k];
    buf[n++] = '\n';
    sys3(SYS_write, 2, (long)buf, n);
}

void tls_main(void);
void tls_main(void)
{
    /* ---- 1. Initialization, as IMGACT presented it to the initial thread.
     * g_ext (GD) reads its .tdata init image; s_a (LD .tdata) reads 7; s_b
     * (LD .tbss) reads 0. Each read goes through a RELAXED Local-Exec sequence;
     * a wrong tpoff (LINK/IMGACT disagreement) or a surviving __tls_get_addr
     * call would fault or return garbage here. */
    if (g_ext != 100) { report("g_ext", g_ext); die(11); }
    if (shared_base != 90) { report("shared_base", shared_base); die(12); }

    /* first LD touch: s_a 7->8, s_b 0->3 -> 8003 */
    int r1 = ld_accumulate(1, 3);
    if (r1 != 8003) { report("ld1", r1); die(13); }

    /* ---- 2. Writes through the same relaxed sequences, read back. */
    g_ext = 205;                        /* GD write */
    if (g_ext != 205) { report("g_ext_w", g_ext); die(14); }

    /* second LD touch: s_a 8->10, s_b 3->8 -> 10008 (proves the DTPOFF32
     * stores landed at the same TP-relative slots the loads read). */
    int r2 = ld_accumulate(2, 5);
    if (r2 != 10008) { report("ld2", r2); die(15); }

    /* 100 + 7 + 3 = 110 (GD init + the two LD .tdata/.tbss starting values) */
    die(g_ext - 205 + 100 + 7 + 3 + (shared_base - 90));
}

__asm__(".globl _start\n"
        ".type _start,@function\n"
        "_start:\n"
        "  xorl %ebp, %ebp\n"
        "  andq $-16, %rsp\n"
        "  call tls_main\n"
        "  hlt\n");
EOF

# DEFAULT dialect (NO -mtls-dialect=gnu2) so gcc emits the classic GD/LD model;
# -fno-plt so the __tls_get_addr call is the indirect GOTPCRELX `ff 15` form
# (exactly what Alpine's libstdc++/libgcc emit). -fPIC keeps it out of initial-
# exec/local-exec compiler lowering so a real TLSGD/TLSLD is emitted.
$CC -fPIC -O2 -fno-plt -ffreestanding -fno-builtin -fno-stack-protector \
    -c -o "$WORK/tlsdef.o" "$WORK/tlsdef.c"
$CC -fPIC -O2 -fno-plt -ffreestanding -fno-builtin -fno-stack-protector \
    -c -o "$WORK/classic_tls.o" "$WORK/classic_tls.c"

echo "-- classic TLS relocations gcc emitted (both models + the __tls_get_addr call) --"
readelf -rW "$WORK/classic_tls.o" | awk '/R_X86_64_(TLSGD|TLSLD|DTPOFF32|GOTPCRELX)/{print $3}' \
    | sort | uniq -c
readelf -rW "$WORK/classic_tls.o" | grep -q "R_X86_64_TLSGD" \
    || { echo "FAIL: no R_X86_64_TLSGD (compiler did not use the classic GD model)"; exit 1; }
readelf -rW "$WORK/classic_tls.o" | grep -q "R_X86_64_TLSLD" \
    || { echo "FAIL: no R_X86_64_TLSLD (local-dynamic half absent)"; exit 1; }
readelf -rW "$WORK/classic_tls.o" | grep -q "R_X86_64_DTPOFF32" \
    || { echo "FAIL: no R_X86_64_DTPOFF32 (local-dynamic operand half absent)"; exit 1; }
readelf -rW "$WORK/classic_tls.o" | grep -q "__tls_get_addr" \
    || { echo "FAIL: no __tls_get_addr call reloc (the whole point of the classic model)"; exit 1; }

echo
echo "== LINK.EXE --executable --use TLSDATA\$SHR -> CLASSICTLS.EXE =="
"$WORK/LINK.EXE" --executable --use "$SYSLIB/TLSDATA\$SHR.EXE" \
    -o "$WORK/CLASSICTLS.EXE" "$WORK/classic_tls.o" "$WORK/tlsdef.o"
chmod +x "$WORK/CLASSICTLS.EXE"

echo "-- image layout --"
readelf -lW "$WORK/CLASSICTLS.EXE" | grep -E '\bINTERP\b|\bTLS\b' || true
readelf -SW "$WORK/CLASSICTLS.EXE" | grep -E '\.tdata|\.tbss|\.vms\$imp' || true
readelf -lW "$WORK/CLASSICTLS.EXE" | grep -q 'INTERP'  || { echo "FAIL: no PT_INTERP"; exit 1; }
readelf -lW "$WORK/CLASSICTLS.EXE" | grep -q '\bTLS\b' || { echo "FAIL: no PT_TLS"; exit 1; }
readelf -hW "$WORK/CLASSICTLS.EXE" | grep -q 'X86-64'  || { echo "FAIL: image is not EM_X86_64"; exit 1; }

# -------------------------------------------------------------------------
# RESIDUAL CHECK, done at BYTE level on the linked image (independent of the
# run). A single surviving classic GD/LD sequence is the exact vms-83e8 bug.
# -------------------------------------------------------------------------
echo
echo "== residual check: NO classic GD/LD sequence survives in the linked image =="
# 1) No __tls_get_addr may be referenced at all — not as a symbol, not deferred.
if readelf -sW "$WORK/CLASSICTLS.EXE" 2>/dev/null | grep -q "__tls_get_addr"; then
    echo "FAIL: __tls_get_addr survives as a symbol in the linked image"; exit 1
fi
# 2) The distinctive input byte patterns must be gone from the loaded image. A
# GD lea/call carries the data16 (0x66) prefix; scan the raw file for them and
# for the relaxed `mov %fs:0` landing that must replace them. Match against a
# flat hex stream of the whole image so the search is byte-exact.
HEX="$WORK/flat.hex"
od -An -tx1 -v "$WORK/CLASSICTLS.EXE" | tr -d ' \n' > "$HEX"
count() { grep -o "$1" "$HEX" | wc -l | tr -d ' '; }
GD_LEA=$(count '66488d3d')      # data16 lea x@tlsgd(%rip),%rdi
GD_CALL=$(count '6648ff15')     # data16 call *__tls_get_addr(%rip)
GD_DIR=$(count '666648e8')      # data16 direct call __tls_get_addr@plt (if -fplt)
MOVFS=$(count '64488b0425')     # mov %fs:0,%rax LE landing
echo "  data16 GD lea   (66 48 8d 3d) remaining : $GD_LEA (want 0)"
echo "  data16 GD call  (66 48 ff 15) remaining : $GD_CALL (want 0)"
echo "  direct  GD call (66 66 48 e8) remaining : $GD_DIR (want 0)"
echo "  mov %fs:0,%rax  (64 48 8b 04 25) LE hits : $MOVFS (want > 0)"
[ "$GD_LEA"  = 0 ] || { echo "FAIL: an unrelaxed data16 TLSGD lea survives"; exit 1; }
[ "$GD_CALL" = 0 ] || { echo "FAIL: an unrelaxed data16 __tls_get_addr call survives"; exit 1; }
[ "$GD_DIR"  = 0 ] || { echo "FAIL: an unrelaxed direct __tls_get_addr call survives"; exit 1; }
[ "$MOVFS" -gt 0 ] || { echo "FAIL: no relaxed 'mov %fs:0,%rax' Local-Exec landing found"; exit 1; }
echo "  ok: every classic GD/LD sequence relaxed to Local-Exec; no __tls_get_addr survives"

# -------------------------------------------------------------------------
# The real thing: kernel -> PT_INTERP -> IMGACT.EXE -> TLS setup -> program.
# -------------------------------------------------------------------------
echo
echo "== ACTIVATE FOR REAL: kernel execs CLASSICTLS.EXE, PT_INTERP runs IMGACT.EXE =="
set +e
"$WORK/CLASSICTLS.EXE"; RC=$?
set -e
echo "CLASSICTLS.EXE exit = $RC (expect 110 = g_ext(100)+s_a(7)+s_b(3))"
case "$RC" in
    110) ;;
    11) echo "FAIL: GD __thread init wrong (IMGACT/LINK disagree on the .tdata image or the GD tpoff)"; exit 1 ;;
    12) echo "FAIL: cross-image DATA import did not resolve"; exit 1 ;;
    13) echo "FAIL: first local-dynamic read/write wrong (TLSLD relax or DTPOFF32 offset wrong)"; exit 1 ;;
    14) echo "FAIL: GD write not read back (general-dynamic tpoff wrong)"; exit 1 ;;
    15) echo "FAIL: second local-dynamic accumulate wrong (DTPOFF32 store/load slots disagree)"; exit 1 ;;
    132) echo "FAIL: SIGILL/SIGSEGV — a residual __tls_get_addr call or a bad TLS offset (the vms-83e8 bug)"; exit 1 ;;
    139) echo "FAIL: SIGSEGV — a residual __tls_get_addr call or a bad TLS offset (the vms-83e8 bug)"; exit 1 ;;
    *) echo "FAIL: unexpected exit $RC"; exit 1 ;;
esac

echo
echo "ALL LINK.EXE x86_64 CLASSIC GD/LD RELAXATION CHECKS PASSED"
