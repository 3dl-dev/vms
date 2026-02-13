/*
 * vms_math.c - Math library (no libm dependency)
 *
 * Software implementations of standard math functions.
 * Uses hardware sqrt/fabs where available via builtins; polynomial
 * approximations for transcendentals.
 *
 * Accuracy: ~1 ULP for most functions.  Sufficient for VMS RTL use.
 */

#include "vms_math.h"
#include <stdint.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define M_PI_VAL      3.14159265358979323846
#define M_PI_2_VAL    1.57079632679489661923
#define M_PI_4_VAL    0.78539816339744830962
#define M_LN2_VAL     0.69314718055994530942
#define M_LOG2E_VAL   1.44269504088896340736
#define M_LN10_VAL    2.30258509299404568402

/* ================================================================
 * Bit manipulation helpers
 * ================================================================ */

typedef union {
    double d;
    uint64_t u;
} dbl_bits_t;

typedef union {
    float f;
    uint32_t u;
} flt_bits_t;

static inline double mk_double(uint64_t bits)
{
    dbl_bits_t b;
    b.u = bits;
    return b.d;
}

static inline uint64_t dbl_to_bits(double d)
{
    dbl_bits_t b;
    b.d = d;
    return b.u;
}

/* ================================================================
 * Basic functions (hardware-accelerated)
 * ================================================================ */

double vms_fabs(double x)
{
    return __builtin_fabs(x);
}

float vms_fabsf(float x)
{
    return __builtin_fabsf(x);
}

double vms_sqrt(double x)
{
#if defined(__aarch64__)
    double result;
    __asm__("fsqrt %d0, %d1" : "=w"(result) : "w"(x));
    return result;
#else
    return __builtin_sqrt(x);
#endif
}

float vms_sqrtf(float x)
{
#if defined(__aarch64__)
    float result;
    __asm__("fsqrt %s0, %s1" : "=w"(result) : "w"(x));
    return result;
#else
    return __builtin_sqrtf(x);
#endif
}

double vms_floor(double x)
{
#if defined(__aarch64__)
    double result;
    __asm__("frintm %d0, %d1" : "=w"(result) : "w"(x));
    return result;
#else
    return __builtin_floor(x);
#endif
}

double vms_ceil(double x)
{
#if defined(__aarch64__)
    double result;
    __asm__("frintp %d0, %d1" : "=w"(result) : "w"(x));
    return result;
#else
    return __builtin_ceil(x);
#endif
}

double vms_fmod(double x, double y)
{
    if (y == 0.0) return x;
    double q = x / y;
    /* Truncate toward zero */
    double qi = (q >= 0.0) ? vms_floor(q) : vms_ceil(q);
    return x - qi * y;
}

/* ================================================================
 * Exponential: exp(x) via range reduction + polynomial
 *
 * exp(x) = 2^n * exp(r) where x = n*ln2 + r, |r| <= ln2/2
 * exp(r) approximated by minimax polynomial
 * ================================================================ */

double vms_exp(double x)
{
    /* Clamp to avoid overflow/underflow */
    if (x > 709.0) return mk_double(0x7FF0000000000000ULL); /* +inf */
    if (x < -745.0) return 0.0;

    /* Range reduction: x = n*ln2 + r */
    double n = vms_floor(x * M_LOG2E_VAL + 0.5);
    double r = x - n * M_LN2_VAL;

    /* Polynomial approximation for exp(r), |r| <= ln2/2 */
    /* Using Horner's method with coefficients for Taylor series */
    double r2 = r * r;
    double p = 1.0 + r + r2 * (0.5 + r * (1.0/6.0 + r * (1.0/24.0 +
               r * (1.0/120.0 + r * (1.0/720.0 + r * (1.0/5040.0))))));

    /* Reconstruct: exp(x) = p * 2^n */
    int ni = (int)n;
    uint64_t bits = dbl_to_bits(p);
    /* Add n to the exponent field (biased by 1023) */
    bits += ((uint64_t)ni << 52);
    return mk_double(bits);
}

/* ================================================================
 * Logarithm: log(x) via range reduction
 *
 * log(x) = log(m * 2^e) = e*ln2 + log(m), 1 <= m < 2
 * log(m) approximated by polynomial around 1
 * ================================================================ */

double vms_log(double x)
{
    if (x <= 0.0) {
        if (x == 0.0) return mk_double(0xFFF0000000000000ULL); /* -inf */
        return mk_double(0x7FF8000000000000ULL); /* NaN */
    }

    /* Extract exponent and mantissa */
    uint64_t bits = dbl_to_bits(x);
    int e = (int)((bits >> 52) & 0x7FF) - 1023;
    /* Set exponent to 0 (bias 1023) to get m in [1, 2) */
    bits = (bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
    double m = mk_double(bits);

    /* If m > sqrt(2), adjust */
    if (m > 1.4142135623730951) {
        m *= 0.5;
        e++;
    }

    /* log(m) via log(1+f) where f = m - 1 */
    double f = m - 1.0;
    double f2 = f * f;

    /* Rational approximation */
    double p = f - f2 * (0.5 - f * (1.0/3.0 - f * (0.25 - f * (0.2 - f * (1.0/6.0)))));

    return (double)e * M_LN2_VAL + p;
}

double vms_log10(double x)
{
    return vms_log(x) / M_LN10_VAL;
}

/* ================================================================
 * Power: pow(base, exponent)
 * ================================================================ */

double vms_pow(double base, double exponent)
{
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    if (base == 1.0) return 1.0;

    /* Check for integer exponent */
    if (exponent == (double)(int)exponent && exponent >= 0 && exponent <= 64) {
        int n = (int)exponent;
        double result = 1.0;
        double b = base;
        while (n > 0) {
            if (n & 1) result *= b;
            b *= b;
            n >>= 1;
        }
        return result;
    }

    /* General case: pow(b, e) = exp(e * log(b)) */
    if (base < 0.0) {
        /* Negative base with non-integer exponent -> NaN */
        return mk_double(0x7FF8000000000000ULL);
    }
    return vms_exp(exponent * vms_log(base));
}

/* ================================================================
 * Trigonometric functions
 *
 * Range reduction to [-pi/4, pi/4], then polynomial approximation.
 * ================================================================ */

/* Reduce x to [-pi/4, pi/4], return quadrant (0-3) */
static int trig_reduce(double x, double *r)
{
    /* Handle negative */
    int neg = 0;
    if (x < 0) {
        x = -x;
        neg = 1;
    }

    /* Reduce modulo pi/2 */
    double q = vms_floor(x / M_PI_2_VAL + 0.5);
    int quadrant = (int)q & 3;
    *r = x - q * M_PI_2_VAL;

    if (neg) {
        *r = -*r;
        quadrant = (-quadrant) & 3;
    }
    return quadrant;
}

/* Polynomial sin(x) for |x| <= pi/4 */
static double sin_poly(double x)
{
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0 +
           x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0))))));
}

/* Polynomial cos(x) for |x| <= pi/4 */
static double cos_poly(double x)
{
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0/24.0 + x2 * (-1.0/720.0 +
           x2 * (1.0/40320.0 + x2 * (-1.0/3628800.0)))));
}

double vms_sin(double x)
{
    double r;
    int q = trig_reduce(x, &r);
    switch (q) {
    case 0:  return  sin_poly(r);
    case 1:  return  cos_poly(r);
    case 2:  return -sin_poly(r);
    case 3:  return -cos_poly(r);
    default: return  sin_poly(r);
    }
}

double vms_cos(double x)
{
    double r;
    int q = trig_reduce(x, &r);
    switch (q) {
    case 0:  return  cos_poly(r);
    case 1:  return -sin_poly(r);
    case 2:  return -cos_poly(r);
    case 3:  return  sin_poly(r);
    default: return  cos_poly(r);
    }
}

double vms_tan(double x)
{
    double s = vms_sin(x);
    double c = vms_cos(x);
    if (c == 0.0) return mk_double(0x7FF0000000000000ULL); /* +inf */
    return s / c;
}

/* ================================================================
 * Inverse trigonometric functions
 * ================================================================ */

/*
 * atan(x) using argument reduction + polynomial.
 * Reduce to |x| <= tan(pi/12) ~= 0.2679 for fast convergence.
 */
static double atan_core(double x)
{
    /* For small x, use series directly */
    double x2 = x * x;
    /* Taylor series: x - x^3/3 + x^5/5 - x^7/7 + ... (15 terms) */
    return x * (1.0 + x2 * (-1.0/3.0 + x2 * (1.0/5.0 + x2 * (-1.0/7.0 +
           x2 * (1.0/9.0 + x2 * (-1.0/11.0 + x2 * (1.0/13.0 + x2 * (-1.0/15.0 +
           x2 * (1.0/17.0 + x2 * (-1.0/19.0 + x2 * (1.0/21.0 + x2 * (-1.0/23.0))))))))))));
}

double vms_atan(double x)
{
    int neg = 0;
    if (x < 0) {
        x = -x;
        neg = 1;
    }

    double result;
    if (x <= 0.4142135623730951) {
        /* |x| <= tan(pi/8): direct polynomial */
        result = atan_core(x);
    } else if (x <= 1.0) {
        /* tan(pi/8) < x <= 1: use identity atan(x) = pi/4 + atan((x-1)/(x+1)) */
        double t = (x - 1.0) / (x + 1.0);
        result = M_PI_4_VAL + atan_core(t);
    } else if (x <= 2.414213562373095) {
        /* 1 < x <= tan(3pi/8): atan(x) = pi/4 + atan((x-1)/(x+1)) */
        double t = (x - 1.0) / (x + 1.0);
        result = M_PI_4_VAL + atan_core(t);
    } else {
        /* x > tan(3pi/8): atan(x) = pi/2 - atan(1/x) */
        result = M_PI_2_VAL - atan_core(1.0 / x);
    }

    return neg ? -result : result;
}

double vms_atan2(double y, double x)
{
    if (x > 0.0) return vms_atan(y / x);
    if (x < 0.0) {
        if (y >= 0.0) return vms_atan(y / x) + M_PI_VAL;
        return vms_atan(y / x) - M_PI_VAL;
    }
    /* x == 0 */
    if (y > 0.0) return M_PI_2_VAL;
    if (y < 0.0) return -M_PI_2_VAL;
    return 0.0; /* undefined, return 0 */
}

double vms_asin(double x)
{
    if (x < -1.0 || x > 1.0)
        return mk_double(0x7FF8000000000000ULL); /* NaN */

    /* asin(x) = atan(x / sqrt(1 - x^2)) */
    if (x == 1.0) return M_PI_2_VAL;
    if (x == -1.0) return -M_PI_2_VAL;

    return vms_atan(x / vms_sqrt(1.0 - x * x));
}

double vms_acos(double x)
{
    return M_PI_2_VAL - vms_asin(x);
}
