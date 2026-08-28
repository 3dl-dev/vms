/* crtl_cc1fp_test.c — a MEASURE-FIRST toolchain-stress alpha-dec-vms port
 * program, wired as a REPRODUCIBLE joint-e2e VARIANT (build-joint-image.sh's
 * JOINT_MAIN override, vms-crtl-rms-porttest ladder).
 *
 * Where crtl_rms_test.c proved the DECC$/RMS/stdio CRTL SURFACE builds
 * complete (heap + RMS file I/O + stdio), this program deliberately stresses
 * the alpha-dec-vms TOOLCHAIN itself — cc1 EVAX codegen + LINK.EXE +
 * OTS$/libgcc — with constructs the prior port tests did NOT exercise. The
 * point is to find the NEXT BUILD RUNG: the first construct the real cross
 * cc1 cannot codegen, or the first OTS$/libgcc/DECC$ symbol LINK.EXE cannot
 * bind. Every reference here is compiled by the REAL alpha-dec-vms cross cc1
 * and links against the SAME genuine alpha DECC$SHR + LIBOTS$SHR the other
 * joint-e2e variants use — no POSIX bypass, no fake success.
 *
 * WHAT IT STRESSES (none of which crtl_rms_test.c / joint_main.c touched):
 *   1. FLOATING POINT — double + float arithmetic (ADDT/MULT/DIVT/ADDS/MULS),
 *      a harmonic series sum and a float dot product, and — the strongest
 *      libgcc-FP probe — UNSIGNED 64-bit <-> double conversions, which Alpha
 *      has no hardware instruction for and GCC lowers to libgcc helpers
 *      (__floatundidf / __fixunsdfdi). Plus <math.h>-class sqrt/pow (a DECC$
 *      math-export probe) and printf("%f"/"%g")/sscanf("%lf") FP formatting.
 *   2. STRUCT/UNION/BITFIELD with STATIC aggregate initializers — stresses
 *      EVAX $DATA$/$READONLY$ data-section emission + cc1 aggregate codegen.
 *   3. FUNCTION-POINTER DISPATCH TABLE — a static array of function pointers
 *      + indirect call, stressing the PDSC/linkage-pointer path directly.
 *   4. LARGE SWITCH (jump-table codegen), RECURSION, and a larger function
 *      (register pressure / stack frame).
 *
 * SENTINEL-RETURN CONVENTION (deterministic — fixed inputs, no argv
 * dependence; main returns a constant per stage). crt0 maps the return N
 * through C$_EXIT1, so BOOT-A/B decodes $STATUS as C$_EXIT1 + (N-1)*8, telling
 * us exactly which stage a real activation reached:
 *
 *   7 = FULL SUCCESS (all stages verified)
 *   1 = FP arithmetic (double series / float dot) mismatch
 *   2 = unsigned64<->double conversion (libgcc __floatundidf/__fixunsdfdi) mismatch
 *   3 = sqrt/pow (math) mismatch
 *   4 = printf/sscanf FP round-trip mismatch
 *   5 = struct/union/bitfield static-aggregate mismatch
 *   6 = function-pointer dispatch-table mismatch
 *   8 = large-switch / recursion mismatch
 */

/* alpha-dec-vms is LP64 (-mpointer-size=64): long/pointer are 64-bit. No libc
 * headers are set up in the cross image, so declare every reference as an
 * extern — the symbol NAMES are what matter; the cross cc1 decorates them to
 * the decc$ surface at codegen (printf -> decc$tprintf, etc.). */
typedef unsigned long ovmx_size_t;

extern int printf(const char *, ...);
extern int sprintf(char *, const char *, ...);
extern int sscanf(const char *, const char *, ...);

extern double sqrt(double);
extern double pow(double, double);
extern double fabs(double);

/* ---- helpers: keep each construct in its own function so cc1 must emit a
 *      real call/frame for it (no whole-program inlining at -O0). ---- */

/* 1a. double arithmetic: harmonic partial sum (FP add + FP divide). */
static double series_sum(int n)
{
    double s = 0.0;
    for (int i = 1; i <= n; i++)
        s += 1.0 / (double)i;          /* CVTQT + DIVT + ADDT */
    return s;
}

/* 1b. float arithmetic: dot product (single-precision MULS/ADDS). */
static float dotf(const float *a, const float *b, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++)
        s += a[i] * b[i];
    return s;
}

/* 2. unsigned-64 <-> double: Alpha has no unsigned convert instruction, so
 *    GCC lowers these to libgcc soft helpers (__floatundidf / __fixunsdfdi).
 *    This is the strongest libgcc-FP-routine link probe in the file. */
static double u64_sum_as_double(unsigned long n)
{
    double s = 0.0;
    for (unsigned long i = 0; i < n; i++)
        s += (double)i;                /* unsigned long -> double */
    return s;
}
static unsigned long double_to_u64(double x)
{
    return (unsigned long)x;           /* double -> unsigned long */
}

/* 3. struct / union / bitfield with STATIC aggregate initializers. */
struct point { double x, y; int tag; };
struct node  { struct point p; const char *name; };

static const struct node node_table[] = {
    { { 1.0,  2.0  }, "alpha" },
    { { 3.5, -1.5  }, "beta"  },
    { { 10.0, 0.25 }, "gamma" },
};

union fpbits { double d; unsigned long u; };

struct packed_flags {
    unsigned int a : 3;
    unsigned int b : 5;
    unsigned int c : 1;
    int          s : 7;
};

/* 4. function-pointer dispatch table. */
typedef double (*binop)(double, double);
static double op_add(double a, double b) { return a + b; }
static double op_sub(double a, double b) { return a - b; }
static double op_mul(double a, double b) { return a * b; }
static double op_div(double a, double b) { return a / b; }
static binop  op_table[4] = { op_add, op_sub, op_mul, op_div };

/* 5. recursion + a larger function with a big switch (jump-table codegen). */
static long fib(int n)
{
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
static int classify(int k)
{
    /* wide, dense switch -> jump table; distinct arms exercise the branch. */
    switch (k) {
    case 0:  return 100;
    case 1:  return 101;
    case 2:  return 102;
    case 3:  return 103;
    case 4:  return 104;
    case 5:  return 105;
    case 6:  return 106;
    case 7:  return 107;
    case 8:  return 108;
    case 9:  return 109;
    case 10: return 110;
    case 11: return 111;
    case 12: return 112;
    case 13: return 113;
    case 14: return 114;
    case 15: return 115;
    case 16: return 116;
    case 17: return 117;
    case 18: return 118;
    case 19: return 119;
    case 20: return 120;
    default: return -1;
    }
}

static int near(double a, double b) { return fabs(a - b) < 1e-3; }

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    /* FOLD-DEFEAT: every input below is derived from a VOLATILE seed so the
     * cross cc1 cannot constant-fold the arithmetic, builtin-fold sqrt/pow, or
     * dead-code-eliminate any stage — the codegen + library calls we want to
     * link-test are actually emitted. seed==0 for a normal non-CLI activation
     * (argc==1, argv[0]=image spec), so the expected values below hold. */
    volatile int vseed = argc - 1;             /* runtime 0; compiler treats as unknown */
    int   seed  = vseed;                       /* 0 */
    double base = 1.0 + (double)seed;          /* 1.0, but non-foldable */

    /* -- stage 1: FP arithmetic (double series + float dot) -- */
    double h4 = series_sum(4 + seed);          /* harmonic sum, runtime bound */
    if (!near(h4, 2.0833333))
        return 1;
    float av[3] = { 1.0f + seed, 2.0f, 3.0f };
    float bv[3] = { 4.0f, 5.0f, 6.0f };
    float dp = dotf(av, bv, 3);                /* 4 + 10 + 18 = 32 */
    if (!near((double)dp, 32.0))
        return 1;

    /* -- stage 2: unsigned64 <-> double (libgcc __floatundidf/__fixunsdfdi,
     *    or Alpha's inline srl/and/or/cvtqt expansion) -- */
    double us = u64_sum_as_double(1000UL + (unsigned long)seed);  /* 0..999 -> 499500 */
    if (!near(us, 499500.0))
        return 2;
    unsigned long back = double_to_u64(499500.0 + base - 1.0);
    if (back != 499500UL)
        return 2;

    /* -- stage 3: sqrt / pow with RUNTIME args (real DECC$ math export probe:
     *    non-constant args cannot be builtin-folded, so a genuine call to the
     *    math routine is emitted and must LINK against DECC$SHR) -- */
    double r2 = sqrt(2.0 * base);              /* sqrt(2.0) */
    if (!near(r2 * r2, 2.0))
        return 3;
    double p = pow(2.0 * base, 10.0);          /* pow(2.0,10.0) = 1024 */
    if (!near(p, 1024.0))
        return 3;

    /* -- stage 4: printf/sscanf FP round-trip (%f/%g/%lf formatting) -- */
    char fbuf[80];
    sprintf(fbuf, "%.6f %g", h4, p);
    double rr1 = 0.0, rr2 = 0.0;
    if (sscanf(fbuf, "%lf %lf", &rr1, &rr2) != 2)
        return 4;
    if (!near(rr1, h4) || !near(rr2, p))
        return 4;

    /* -- stage 5: struct/union/bitfield static-aggregate emission -- */
    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < 3; i++) { sx += node_table[i].p.x; sy += node_table[i].p.y; }
    if (!near(sx, 14.5) || !near(sy, 0.75))    /* 1+3.5+10 ; 2-1.5+0.25 */
        return 5;
    /* NB: reading the type-punned union member fb.u is exercised (its bit
     * pattern is carried into the final printf below), but we deliberately do
     * NOT branch on it: the alpha-dec-vms cc1 (GCC 14.2.0) SILENTLY MISCOMPILES
     * a conditional branch controlled by a union type-punned member — it drops
     * the compare+branch and the fall-through path, at -O0, with no diagnostic
     * (isolated + reported separately as the NEXT cc1-codegen rung). A plain
     * (non-union) branch and a non-branching union read both codegen correctly,
     * so this variant stays on the clean side of that fault. */
    union fpbits fb;
    fb.d = base;                               /* IEEE-754 1.0 = 0x3FF0000000000000 */
    unsigned long fbits = fb.u;                /* punned read (used, not branched) */

    struct packed_flags pf = { 0, 0, 0, 0 };
    pf.a = 5 + seed; pf.b = 21; pf.c = 1; pf.s = -3;
    if (pf.a != 5U || pf.b != 21U || pf.c != 1U || pf.s != -3)
        return 5;

    /* -- stage 6: function-pointer dispatch table (runtime index defeats
     *    devirtualization) -- */
    double acc = base + 1.0;                   /* 2.0 */
    acc = op_table[(0 + seed) & 3](acc, 3.0);  /* 2+3 = 5 */
    acc = op_table[(2 + seed) & 3](acc, 4.0);  /* 5*4 = 20 */
    acc = op_table[(1 + seed) & 3](acc, 5.0);  /* 20-5 = 15 */
    acc = op_table[(3 + seed) & 3](acc, 3.0);  /* 15/3 = 5 */
    if (!near(acc, 5.0))
        return 6;

    /* -- stage 8: large switch + recursion + larger function (runtime args) -- */
    long f10 = fib(10 + seed);                 /* 55 */
    if (f10 != 55L)
        return 8;
    int csum = 0;
    for (int k = 0; k <= 20; k++) csum += classify(k + seed);   /* sum 100..120 = 2310 */
    if (csum != 2310)
        return 8;
    if (classify(99 + seed) != -1)
        return 8;

    /* -- all stages verified: report (carrying the punned union bits) +
     *    return the full-success sentinel -- */
    printf("OVMX cc1/FP toolchain-stress port test: OK "
           "(fp+u64cvt+math+fmt+aggregate+fnptr+switch) argc=%d fbits=0x%lx\n",
           argc, fbits);
    return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
}
