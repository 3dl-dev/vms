#!/bin/sh
# run_test_x86_64_tls.sh — LINK.EXE x86_64 TLSDESC proof (bead vms-2e4).
#
# The LAST x86_64 relocation class in the vms-bdf chain: gnu2-dialect TLSDESC.
# Grounded by docs/design-link-x86_64-relocs.md, which tallied
# R_X86_64_GOTPC32_TLSDESC (21), R_X86_64_TLSDESC_CALL (22) and
# R_X86_64_DTPOFF32 (8) in the real OVMX x86_64 object set.
#
# Why this is NOT the aarch64 logic generalized. aarch64 TLSDESC is FOUR relocs
# bit-patched into an ADRP+LDR+ADD+BLR quartet. x86_64 gnu2 is:
#
#     48 8d 05 <disp32>   lea  sym@TLSDESC(%rip), %rax   R_X86_64_GOTPC32_TLSDESC (A=-4)
#     ff 10               call *(%rax)                   R_X86_64_TLSDESC_CALL    (marker)
#     64 03 38            add  %fs:(%rax), %edi          TP + returned offset
#
# — a single flat disp32 plus a two-byte marker that must be left ALONE. And for
# `static _Thread_local` (local-dynamic) gcc emits ONE TLSDESC pair against the
# synthetic UND symbol `_TLS_MODULE_BASE_` plus a separate R_X86_64_DTPOFF32
# per variable carrying its module-relative offset. Both models are exercised.
#
# GROUND-SOURCE DONE CONDITION (not a readelf-only check): the linked image is
# ACTIVATED FOR REAL — the kernel execs it, its PT_INTERP runs the real x86_64
# IMGACT.EXE (src/imgact/arch/x86_64, bead vms-913.11), IMGACT lays out the TLS
# block and completes every descriptor ([0]=__tlsdesc_static, [1]+=tls_offset),
# and the program then reads and writes its thread-locals through the real gnu2
# access sequence. That is the only way LINK.EXE's STATIC TLSDESC output and
# IMGACT's LOAD-TIME resolution can be shown to AGREE rather than merely each
# compiling.
#
# Two threads: the program clones a second REAL thread (CLONE_VM|CLONE_THREAD|
# CLONE_SETTLS, so both run in one address space with distinct %fs bases) and
# gives it its own TLS block — a copy of the parent's, sized from the variable
# addresses the descriptors actually returned, never from a hardcoded layout.
# The child writes every thread-local; the parent then proves its OWN copies are
# untouched. That is what "thread-local" has to mean, and it can only hold if
# every descriptor returned a TP-RELATIVE offset. (The block hand-off is what a
# thread library does; IMGACT itself only ever sets up the initial thread, and
# growing musl's DTV for spawned threads is the separately-tracked vms-616
# follow-up — deliberately not depended on here.)
#
# Runs natively on an x86_64 host: no emulation, no container needed.
# Needs /vms/SYS0/SYSCOMMON/{SYSEXE,SYSLIB} to be creatable (PT_INTERP path).
# Exit 0 on success.

set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)                  # src/vmslink
IMGACT_DIR=$(cd "$SRC/../imgact" && pwd)     # src/imgact
WORK=${WORK:-/tmp/vmslink-test-x86_64-tls}
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
# A producer shareable exporting one DATA universal. Its only job is to give the
# executable a .vms$imp table: IMGACT's symbol-vector activation path requires
# one (activate_symbol_vector dies with %IMGACT-F-IMGFMTERR without it). Reuses
# the cross-image DATA import path vms-cd1 already proved on x86_64.
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
# The subject: a freestanding x86_64 executable carrying BOTH TLSDESC models.
# ---------------------------------------------------------------------------
echo
echo "== compile the TLS test program (-mtls-dialect=gnu2, per vms-be5) =="
cat > "$WORK/tlsprog.c" <<'EOF'
/*
 * x86_64 gnu2-TLSDESC test image for LINK.EXE (vms-2e4). Freestanding: it owns
 * _start and talks to the kernel with raw syscalls, so activation exercises
 * LINK.EXE's TLSDESC output and IMGACT's TLS setup with no C-RTL in between.
 *
 * GLOBAL-DYNAMIC model (external-linkage __thread): one
 * R_X86_64_GOTPC32_TLSDESC + R_X86_64_TLSDESC_CALL pair per variable.
 */
__thread int t_val = 100;      /* .tdata: nonzero init image IMGACT must copy */
__thread int t_acc;            /* .tbss:  IMGACT must present it zeroed       */

/*
 * LOCAL-DYNAMIC model (static _Thread_local — what src/libvms/rtl/lib_signal.c
 * has): ONE TLSDESC pair against the synthetic `_TLS_MODULE_BASE_`, plus one
 * R_X86_64_DTPOFF32 per variable holding its module-relative offset.
 */
static _Thread_local int s_a = 7;   /* .tdata */
static _Thread_local int s_b;       /* .tbss  */

extern int shared_base;             /* cross-image DATA import (GOTPCREL)     */

/* Shared (non-TLS) state: how the child reports back to the parent. */
volatile int g_child_done;
volatile int g_child_t_val, g_child_t_acc, g_child_s_a, g_child_s_b;

#define SYS_write       1
#define SYS_mmap        9
#define SYS_clone      56
#define SYS_exit       60
#define SYS_sched_yield 24
#define SYS_exit_group 231

#define PROT_RW        0x3
#define MAP_PRIVANON   0x22

#define CLONE_VM      0x00000100
#define CLONE_FS      0x00000200
#define CLONE_FILES   0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD  0x00010000
#define CLONE_SETTLS  0x00080000

static long sys3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static void *sys_mmap(unsigned long len)
{
    long r;
    register long r10 __asm__("r10") = MAP_PRIVANON;
    register long r8  __asm__("r8")  = -1;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"((long)SYS_mmap), "D"(0L), "S"((long)len),
                       "d"((long)PROT_RW), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return (void *)r;
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

static unsigned long get_tp(void)
{
    unsigned long tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

/* The child thread body. Reached with %fs already pointing at its OWN TLS block
 * (CLONE_SETTLS), so every TLSDESC access below lands in that block. */
void child_entry(void);
void child_entry(void)
{
    g_child_t_val = t_val;   /* inherited copy of the parent's block */
    g_child_t_acc = t_acc;
    g_child_s_a   = s_a;
    g_child_s_b   = s_b;
    t_val = 555;             /* write through the GD descriptors     */
    t_acc = 777;
    s_a   = 66;              /* write through the LD DTPOFF32 path   */
    s_b   = 88;
    /* Read back through the same sequences: proves the writes landed where the
     * reads look, in this thread's block. */
    if (t_val != 555 || t_acc != 777 || s_a != 66 || s_b != 88)
        die(31);
    g_child_done = 1;
    sys3(SYS_exit, 0, 0, 0);   /* thread exit, not exit_group */
    __builtin_unreachable();
}

static long spawn(void *child_stack, void *newtls)
{
    long ret;
    register long r10 __asm__("r10") = 0;             /* ctid  */
    register long r8  __asm__("r8")  = (long)newtls;  /* tls   */
    __asm__ volatile(
        "syscall\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "call child_entry\n\t"
        "hlt\n"
        "1:"
        : "=a"(ret)
        : "a"((long)SYS_clone),
          "D"((long)(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                     CLONE_THREAD | CLONE_SETTLS)),
          "S"((long)child_stack), "d"(0L), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

void tls_main(void);
void tls_main(void)
{
    /* ---- 1. Initialization, as IMGACT presented it to the initial thread. */
    if (t_val != 100) { report("t_val", t_val); die(11); }
    if (s_a   != 7)   { report("s_a",   s_a);   die(12); }
    if (t_acc != 0)   { report("t_acc", t_acc); die(13); }
    if (s_b   != 0)   { report("s_b",   s_b);   die(14); }
    if (shared_base != 90) { report("shared_base", shared_base); die(15); }

    /* ---- 2. This thread's own read/write. */
    t_acc += 5;
    s_b   += 3;
    if (t_acc != 5 || s_b != 3) { report("rw", t_acc * 100 + s_b); die(16); }

    /* ---- 3. Build the child's TLS block. Its size is DERIVED from where the
     * descriptors actually put the variables (lowest address below TP), never
     * from a hardcoded module layout. */
    unsigned long tp = get_tp();
    long lo = 0;
    long d;
    d = (long)((char *)&t_val - (char *)tp); if (d < lo) lo = d;
    d = (long)((char *)&t_acc - (char *)tp); if (d < lo) lo = d;
    d = (long)((char *)&s_a   - (char *)tp); if (d < lo) lo = d;
    d = (long)((char *)&s_b   - (char *)tp); if (d < lo) lo = d;
    if (lo >= 0 || lo < -4096) { report("tlswindow", lo); die(17); }

    char *blk = (char *)sys_mmap(8192);
    if ((unsigned long)blk >= (unsigned long)-4095L) die(18);
    char *ctp = blk + 4096;
    {   /* volatile byte copy: never let gcc turn this into a memcpy call */
        volatile unsigned char *dst = (volatile unsigned char *)(ctp + lo);
        const volatile unsigned char *srcb =
            (const volatile unsigned char *)((char *)tp + lo);
        for (long i = 0; i < -lo; i++) dst[i] = srcb[i];
    }
    *(void **)ctp = (void *)ctp;    /* Variant II TCB self-pointer at %fs:0 */

    char *cstk = (char *)sys_mmap(65536);
    if ((unsigned long)cstk >= (unsigned long)-4095L) die(19);
    void *csp = (void *)((unsigned long)(cstk + 65536) & ~15UL);

    long tid = spawn(csp, ctp);
    if (tid <= 0) { report("clone", tid); die(20); }

    for (long spin = 0; spin < 200000000L && !g_child_done; spin++)
        if ((spin & 0xFFFF) == 0) sys3(SYS_sched_yield, 0, 0, 0);
    if (!g_child_done) die(21);

    /* ---- 4. The child saw the values it inherited... */
    if (g_child_t_val != 100) { report("c_t_val", g_child_t_val); die(22); }
    if (g_child_s_a   != 7)   { report("c_s_a",   g_child_s_a);   die(23); }
    if (g_child_t_acc != 5)   { report("c_t_acc", g_child_t_acc); die(24); }
    if (g_child_s_b   != 3)   { report("c_s_b",   g_child_s_b);   die(25); }

    /* ---- 5. ...and every write it made is INVISIBLE here. This is the whole
     * point: it can only hold if each descriptor returned a TP-relative offset
     * rather than a fixed address. */
    if (t_val != 100) { report("p_t_val", t_val); die(26); }
    if (s_a   != 7)   { report("p_s_a",   s_a);   die(27); }
    if (t_acc != 5)   { report("p_t_acc", t_acc); die(28); }
    if (s_b   != 3)   { report("p_s_b",   s_b);   die(29); }

    /* 100 + 7 + 5 + 3 + (90 - 90) = 115 */
    die(t_val + s_a + t_acc + s_b + (shared_base - 90));
}

__asm__(".globl _start\n"
        ".type _start,@function\n"
        "_start:\n"
        "  xorl %ebp, %ebp\n"
        "  andq $-16, %rsp\n"
        "  call tls_main\n"
        "  hlt\n");
EOF

# -mtls-dialect=gnu2 is the standing x86_64 choice (bead vms-be5); -fPIC keeps
# the compiler in the GD/LD models rather than initial-exec.
$CC -fPIC -O2 -mtls-dialect=gnu2 -ffreestanding -fno-builtin -fno-stack-protector \
    -c -o "$WORK/tlsprog.o" "$WORK/tlsprog.c"

echo "-- TLS relocations gcc emitted (both models must be present) --"
readelf -rW "$WORK/tlsprog.o" | awk '/R_X86_64_(GOTPC32_TLSDESC|TLSDESC_CALL|DTPOFF32)/{print $3}' \
    | sort | uniq -c
readelf -rW "$WORK/tlsprog.o" | grep -q "R_X86_64_GOTPC32_TLSDESC" \
    || { echo "FAIL: no R_X86_64_GOTPC32_TLSDESC (compiler did not use the gnu2 TLSDESC model)"; exit 1; }
readelf -rW "$WORK/tlsprog.o" | grep -q "R_X86_64_TLSDESC_CALL" \
    || { echo "FAIL: no R_X86_64_TLSDESC_CALL"; exit 1; }
readelf -rW "$WORK/tlsprog.o" | grep -q "R_X86_64_DTPOFF32" \
    || { echo "FAIL: no R_X86_64_DTPOFF32 (local-dynamic half absent)"; exit 1; }
readelf -rW "$WORK/tlsprog.o" | grep -q "_TLS_MODULE_BASE_" \
    || { echo "FAIL: no _TLS_MODULE_BASE_ reference (local-dynamic half absent)"; exit 1; }

echo
echo "== LINK.EXE --executable --use TLSDATA\$SHR -> TLSX86.EXE =="
"$WORK/LINK.EXE" --executable --use "$SYSLIB/TLSDATA\$SHR.EXE" \
    -o "$WORK/TLSX86.EXE" "$WORK/tlsprog.o"
chmod +x "$WORK/TLSX86.EXE"

echo "-- image layout --"
readelf -lW "$WORK/TLSX86.EXE" | grep -E '\bINTERP\b|\bPHDR\b|\bTLS\b' || true
readelf -SW "$WORK/TLSX86.EXE" | grep -E '\.tdata|\.tbss|\.tlsdesc|\.vms\$tls|\.vms\$imp' || true
readelf -lW "$WORK/TLSX86.EXE" | grep -q 'INTERP'     || { echo "FAIL: no PT_INTERP"; exit 1; }
readelf -lW "$WORK/TLSX86.EXE" | grep -q '\bTLS\b'    || { echo "FAIL: no PT_TLS"; exit 1; }
readelf -SW "$WORK/TLSX86.EXE" | grep -q '\.tlsdesc'  || { echo "FAIL: no .tlsdesc table"; exit 1; }
readelf -SW "$WORK/TLSX86.EXE" | grep -q '\.vms\$tls' || { echo "FAIL: no .vms\$tls"; exit 1; }
readelf -hW "$WORK/TLSX86.EXE" | grep -q 'X86-64'     || { echo "FAIL: image is not EM_X86_64"; exit 1; }

# -------------------------------------------------------------------------
# Byte-level checks on the PATCH ITSELF, independent of the run below. These
# catch the exact way the aarch64 logic would have gone wrong if generalized.
# -------------------------------------------------------------------------
echo
echo '== patch check 1: every TLSDESC lea ...(%rip),%rax resolves INTO .tlsdesc =='
objdump -d --section=.text "$WORK/TLSX86.EXE" > "$WORK/text.dis"
# readelf prints the index as "[ 4]" (two fields) or "[10]" (one), so locate the
# name column by value rather than by a fixed position: name, type, addr, off, size.
sec_field() { readelf -SW "$1" | awk -v n="$2" -v k="$3" \
    '{for(i=1;i<=NF;i++) if($i==n){print $(i+k); exit}}'; }
TD_ADDR=$(sec_field "$WORK/TLSX86.EXE" .tlsdesc 2)
TD_SIZE=$(sec_field "$WORK/TLSX86.EXE" .tlsdesc 4)
[ -n "$TD_ADDR" ] && [ -n "$TD_SIZE" ] || { echo "FAIL: cannot read .tlsdesc geometry"; exit 1; }
TD_BEG=$((0x$TD_ADDR)); TD_END=$((TD_BEG + 0x$TD_SIZE))
NDESC=$((TD_END - TD_BEG))
NDESC=$((NDESC / 16))
echo ".tlsdesc = [$TD_BEG, $TD_END), $NDESC descriptors"
[ "$NDESC" -eq 3 ] \
    || { echo "FAIL: expected 3 descriptors (t_val, t_acc, _TLS_MODULE_BASE_), got $NDESC"; exit 1; }
# A gnu2 TLSDESC site is `lea <disp>(%rip),<reg>` whose resolved target is the
# descriptor; objdump annotates that target in the trailing comment. (gcc hoists
# and re-uses the lea across several `call *(%rax)` sites, so the pairing is NOT
# one-lea-per-call -- collect the targets directly instead.)
sed -n 's/.*lea .*(%rip),.*#[ ]*0x\([0-9a-f]*\).*/\1/p' "$WORK/text.dis" \
    | sort -u > "$WORK/riptargets"
: > "$WORK/hit"
while read -r a; do
    [ -n "$a" ] || continue
    v=$((0x$a))
    [ "$v" -ge "$TD_BEG" ] && [ "$v" -lt "$TD_END" ] || continue
    [ $((v % 16)) -eq 0 ] \
        || { echo "FAIL: a lea resolves to 0x$a, inside .tlsdesc but NOT on a 16-byte descriptor boundary"; exit 1; }
    echo "$v" >> "$WORK/hit"
done < "$WORK/riptargets"
NHIT=$(sort -u "$WORK/hit" | wc -l)
[ "$NHIT" -eq "$NDESC" ] \
    || { echo "FAIL: $NHIT of $NDESC descriptors are actually reached by a patched lea"; exit 1; }
echo "  ok: all $NDESC descriptors reached, every TLSDESC lea on a descriptor boundary"

echo
echo '== patch check 1b: descriptor contents + .vms$tls agree with IMGACT contract =='
# LINK.EXE's half of the contract IMGACT's symvec_tls_place()/absorb_tls_over_crtl()
# completes: entry[0] = 0 (IMGACT stores __tlsdesc_static), entry[1] = the
# MODULE-relative offset (IMGACT adds the module's assigned tls_offset), and
# .vms$tls lists each descriptor's image-relative address so IMGACT can find them.
TLS_MEMSZ=$((0x$(readelf -lW "$WORK/TLSX86.EXE" | awk '$1=="TLS"{sub(/^0x/,"",$6); print $6}')))
echo "PT_TLS memsz = $TLS_MEMSZ"
TD_OFF=$((0x$(sec_field "$WORK/TLSX86.EXE" .tlsdesc 3)))
od -A n -t x8 -j "$TD_OFF" -N $((TD_END - TD_BEG)) "$WORK/TLSX86.EXE" | tr -s ' ' '\n' \
    | grep -v '^$' > "$WORK/descwords"
i=0
while read -r w; do
    if [ $((i % 2)) -eq 0 ]; then
        [ "$((0x$w))" -eq 0 ] \
            || { echo "FAIL: descriptor $((i/2)) word0 = 0x$w, expected 0 (IMGACT fills the resolver)"; exit 1; }
    else
        [ "$((0x$w))" -lt $((TLS_MEMSZ)) ] \
            || { echo "FAIL: descriptor $((i/2)) word1 = 0x$w, outside the ${TLS_MEMSZ}-byte TLS module"; exit 1; }
    fi
    i=$((i + 1))
done < "$WORK/descwords"
echo "  ok: $((i/2)) descriptors: word0=0, word1 inside the TLS module"

VT_OFF=$((0x$(sec_field "$WORK/TLSX86.EXE" '.vms$tls' 3)))
# .vms$tls = { u32 magic, u32 count, u64 entry_off[count] }
VT_COUNT=$(od -A n -t u4 -j $((VT_OFF + 4)) -N 4 "$WORK/TLSX86.EXE" | tr -d ' ')
[ "$VT_COUNT" -eq "$NDESC" ] \
    || { echo "FAIL: .vms\$tls count=$VT_COUNT, .tlsdesc has $NDESC descriptors"; exit 1; }
k=0
while [ "$k" -lt "$NDESC" ]; do
    e=$(od -A n -t u8 -j $((VT_OFF + 8 + k * 8)) -N 8 "$WORK/TLSX86.EXE" | tr -d ' ')
    [ "$e" -eq $((TD_BEG + k * 16)) ] \
        || { echo "FAIL: .vms\$tls[$k] = $e, expected $((TD_BEG + k * 16))"; exit 1; }
    k=$((k + 1))
done
echo "  ok: .vms\$tls lists all $NDESC descriptor addresses -- IMGACT can find every one"

echo
echo '== patch check 2: the TLSDESC_CALL marker was NOT written over =='
# Its site is the two-byte ff 10. The aarch64 path patches a 32-bit instruction
# field at the reloc site; doing that here would overwrite the two bytes that
# follow and the call would no longer disassemble.
NCALL=$(grep -c 'call[[:space:]]*\*(%rax)' "$WORK/text.dis" || true)
[ "$NCALL" -ge 2 ] \
    || { echo "FAIL: TLSDESC call *(%rax) markers clobbered (found $NCALL, expected >=2)"; exit 1; }
echo "  ok: $NCALL intact call *(%rax) TLSDESC markers"

echo
echo '== patch check 3: read the patched fields AT their relocation sites =='
# Read the exact 4-byte fields LINK.EXE wrote, located by the relocation offsets
# in the ORIGINAL object rather than by pattern-matching a disassembly (the same
# `lea 0xN(%rax)` shape also appears in ordinary non-TLS code). The mapping
# object-.text-offset -> image address is SELF-VALIDATING: the same mapping is
# used for the GOTPC32_TLSDESC fields, and those cannot land on a descriptor
# boundary inside .tlsdesc if the mapping is wrong.
TEXT_ADDR=$((0x$(sec_field "$WORK/TLSX86.EXE" .text 2)))
TEXT_OFF=$((0x$(sec_field "$WORK/TLSX86.EXE" .text 3)))
i32() { od -A n -t d4 -j "$1" -N 4 "$WORK/TLSX86.EXE" | tr -d ' '; }

NG=0
for o in $(readelf -rW "$WORK/tlsprog.o" | awk '$3=="R_X86_64_GOTPC32_TLSDESC"{print $1}'); do
    site=$((0x$o))
    disp=$(i32 $((TEXT_OFF + site)))
    # the written value is descriptor + A - P with A = -4, so descriptor = disp + P + 4
    target=$((disp + TEXT_ADDR + site + 4))
    { [ "$target" -ge "$TD_BEG" ] && [ "$target" -lt "$TD_END" ] && [ $((target % 16)) -eq 0 ]; } \
        || { echo "FAIL: GOTPC32_TLSDESC at .text+0x$o resolves to $target, not a descriptor in [$TD_BEG,$TD_END)"; exit 1; }
    NG=$((NG + 1))
done
[ "$NG" -ge 2 ] || { echo "FAIL: expected >=2 GOTPC32_TLSDESC sites, found $NG"; exit 1; }
echo "  ok: all $NG GOTPC32_TLSDESC fields resolve to a 16-byte-aligned descriptor"

ND=0; NNZ=0
for o in $(readelf -rW "$WORK/tlsprog.o" | awk '$3=="R_X86_64_DTPOFF32"{print $1}'); do
    v=$(i32 $((TEXT_OFF + 0x$o)))
    { [ "$v" -ge 0 ] && [ "$v" -lt "$TLS_MEMSZ" ]; } \
        || { echo "FAIL: DTPOFF32 at .text+0x$o = $v, outside the ${TLS_MEMSZ}-byte TLS module"; exit 1; }
    ND=$((ND + 1)); [ "$v" -ne 0 ] && NNZ=$((NNZ + 1))
done
[ "$ND"  -ge 2 ] || { echo "FAIL: expected >=2 DTPOFF32 sites, found $ND"; exit 1; }
# All-zero would be the signature of the relocation never being applied.
[ "$NNZ" -ge 1 ] || { echo "FAIL: every DTPOFF32 field is 0 -- the relocation was not applied"; exit 1; }
echo "  ok: $ND DTPOFF32 fields inside the ${TLS_MEMSZ}-byte TLS module, $NNZ nonzero"

# -------------------------------------------------------------------------
# The real thing: kernel -> PT_INTERP -> IMGACT.EXE -> TLS setup -> program.
# -------------------------------------------------------------------------
echo
echo "== ACTIVATE FOR REAL: kernel execs TLSX86.EXE, PT_INTERP runs IMGACT.EXE =="
set +e
"$WORK/TLSX86.EXE"; RC=$?
set -e
echo "TLSX86.EXE exit = $RC (expect 115 = t_val(100)+s_a(7)+t_acc(5)+s_b(3))"
case "$RC" in
    115) ;;
    11|12|13|14) echo "FAIL: TLS initialization wrong (IMGACT/LINK disagree on the init image)"; exit 1 ;;
    15) echo "FAIL: cross-image DATA import did not resolve"; exit 1 ;;
    16) echo "FAIL: initial thread could not read back its own TLS write"; exit 1 ;;
    17|18|19|20|21) echo "FAIL: second thread could not be created ($RC)"; exit 1 ;;
    22|23|24|25) echo "FAIL: second thread did not inherit the parent's TLS values"; exit 1 ;;
    26|27|28|29) echo "FAIL: second thread's writes LEAKED into the parent -- TLSDESC returned a non-TP-relative offset"; exit 1 ;;
    31) echo "FAIL: second thread could not read back its own TLS write"; exit 1 ;;
    *) echo "FAIL: unexpected exit $RC"; exit 1 ;;
esac

echo
echo "ALL LINK.EXE x86_64 TLSDESC CHECKS PASSED"
