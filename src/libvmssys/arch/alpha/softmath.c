/*
 * softmath.c - Freestanding libm leaf routines for Alpha.
 *
 * Alpha's FPU has hardware divide (divt) but no hardware sqrt/floor/ceil,
 * and GCC's alpha backend has no inline expansion for __builtin_sqrt/
 * floor/ceil the way it does on x86_64/aarch64 -- it always emits a
 * libcall (confirmed by inspecting the generated .s: `jsr $26,($27),sqrt`
 * etc. even with -mieee). vms_math.c's vms_sqrt/vms_floor/vms_ceil/
 * vms_sqrtf wrap __builtin_* calls that resolve to these names on every
 * arch; only Alpha needs a definition supplied locally instead of by the
 * hosted C library (libvmssys is -nostdlib/-nodefaultlibs and must not
 * depend on glibc, project Rule 3).
 *
 * (The OTHER freestanding-toolchain gap discovered alongside this one --
 * Alpha has no hardware integer divide/remainder either -- is __divqu/
 * __remqu in softdiv.S, not here: those use a non-standard Alpha-specific
 * calling convention that can't be expressed as a plain C function, so
 * they're hand-written assembly. See that file's header for the ABI.)
 *
 * Newton-Raphson square root from a bit-halved exponent guess and
 * IEEE-754 mantissa-mask floor/ceil are ordinary, publicly-documented
 * numerical algorithms -- no VMS/DEC-specific reverse engineering
 * involved (see CLAUDE.md Rule 8 -- clean-room RE applies to VMS
 * wire/link formats, not generic Linux/Alpha ABI helpers). Test
 * tolerance is documented in tests/libvmssys/test_math.c (0.0001); these
 * are not claimed bit-exact against glibc.
 */

#include "vms_types.h"

/* ================================================================
 * floor / ceil -- IEEE-754 double, mask out the fractional mantissa bits
 * ================================================================ */

double floor(double x)
{
    union { double d; unsigned long u; } v;
    v.d = x;

    int biased_exp = (int)((v.u >> 52) & 0x7FF);
    int exp = biased_exp - 1023;
    int sign = (int)(v.u >> 63);

    if (biased_exp == 0x7FF || exp >= 52) {
        return x; /* NaN, +/-Inf, or already an integer */
    }
    if (exp < 0) {
        if (x == 0.0) {
            return x; /* preserve +0.0 / -0.0 */
        }
        return sign ? -1.0 : 0.0;
    }

    unsigned long frac_mask = (1UL << (52 - exp)) - 1;
    if ((v.u & frac_mask) == 0) {
        return x; /* already integral */
    }

    union { double d; unsigned long u; } t;
    t.u = v.u & ~frac_mask;
    double truncated = t.d;
    return sign ? (truncated - 1.0) : truncated;
}

double ceil(double x)
{
    return -floor(-x);
}

/* ================================================================
 * sqrt / sqrtf -- Newton-Raphson from an exponent-halved initial guess
 * ================================================================ */

double sqrt(double x)
{
    if (x != x) {
        return x; /* NaN in, NaN out */
    }
    if (x <= 0.0) {
        return x; /* sqrt(0)=0, sqrt(-0)=-0; negative input -- no NaN
                      constant available here, return input like most
                      freestanding minimal libms do */
    }

    union { double d; unsigned long u; } v;
    v.d = x;

    /* Halve the (unbiased) exponent for an initial guess within roughly a
     * factor of sqrt(2) of the true root -- keeps Newton-Raphson stable
     * and fast across the whole double range instead of seeding from x
     * itself (which would take far more iterations, or misconverge, for
     * very large/small x). */
    long exp = (long)((v.u >> 52) & 0x7FF) - 1023;
    long half_exp = exp >> 1; /* arithmetic shift: floor(exp / 2) */
    unsigned long guess_bits = ((unsigned long)(half_exp + 1023) << 52) |
                                (v.u & 0x000FFFFFFFFFFFFFUL);
    union { double d; unsigned long u; } gv;
    gv.u = guess_bits;
    double g = gv.d;
    if (g <= 0.0) {
        g = x; /* degenerate guess (e.g. subnormal input) -- fall back */
    }

    for (int i = 0; i < 20; i++) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

float sqrtf(float x)
{
    return (float)sqrt((double)x);
}
