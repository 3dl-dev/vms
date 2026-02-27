/*
 * test_mth_routines.c - Unit tests for MTH$ math RTL
 *
 * Tests mth$sin, mth$cos, mth$sqrt, mth$exp, mth$alog (ln),
 * mth$alog10, mth$abs, mth$power, mth$nint, mth$floor, mth$ceil,
 * mth$sind, mth$cosd, mth$atan2, mth$sincos, mth$min, mth$max.
 *
 * VMS calling convention: all args passed by pointer.
 */

#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "mth$routines.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

static int approx_eq(double a, double b, double eps)
{
    double diff = a - b;
    if (diff < 0) diff = -diff;
    return diff < eps;
}

static void check_d(double got, double expected, double eps, const char *name)
{
    if (approx_eq(got, expected, eps)) {
        printf("  OK: %s\n", name);
    } else {
        /* Use integer-scaled output since we can't rely on %f in all contexts */
        long g = (long)(got * 100000);
        long e = (long)(expected * 100000);
        printf("  FAIL: %s (got %ld/100000, expected %ld/100000)\n", name, g, e);
        failures++;
    }
}

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* Trigonometric functions                                             */
/* ------------------------------------------------------------------ */
static void test_trig(void)
{
    printf("Testing MTH$ trigonometric functions...\n");
    double x;

    x = 0.0;
    check_d(mth$sin(&x), 0.0,    1e-10, "sin(0) = 0");
    check_d(mth$cos(&x), 1.0,    1e-10, "cos(0) = 1");

    x = M_PI / 2.0;
    check_d(mth$sin(&x), 1.0,    1e-10, "sin(pi/2) = 1");
    check_d(mth$cos(&x), 0.0,    1e-10, "cos(pi/2) = 0");

    x = M_PI;
    check_d(mth$sin(&x), 0.0,    1e-10, "sin(pi) = 0");
    check_d(mth$cos(&x), -1.0,   1e-10, "cos(pi) = -1");

    x = M_PI / 4.0;
    double root2over2 = sqrt(2.0) / 2.0;
    check_d(mth$sin(&x), root2over2, 1e-10, "sin(pi/4) = sqrt(2)/2");
    check_d(mth$cos(&x), root2over2, 1e-10, "cos(pi/4) = sqrt(2)/2");
}

/* ------------------------------------------------------------------ */
/* Degree trigonometric functions                                      */
/* ------------------------------------------------------------------ */
static void test_trig_degrees(void)
{
    printf("Testing MTH$ degree trig functions...\n");
    double x;

    x = 0.0;
    check_d(mth$sind(&x), 0.0,   1e-10, "sind(0) = 0");
    check_d(mth$cosd(&x), 1.0,   1e-10, "cosd(0) = 1");

    x = 90.0;
    check_d(mth$sind(&x), 1.0,   1e-10, "sind(90) = 1");
    check_d(mth$cosd(&x), 0.0,   1e-10, "cosd(90) = 0");

    x = 45.0;
    double root2over2 = sqrt(2.0) / 2.0;
    check_d(mth$sind(&x), root2over2, 1e-10, "sind(45) = sqrt(2)/2");
}

/* ------------------------------------------------------------------ */
/* Square root                                                         */
/* ------------------------------------------------------------------ */
static void test_sqrt(void)
{
    printf("Testing mth$sqrt...\n");
    double x;

    x = 4.0;
    check_d(mth$sqrt(&x), 2.0,   1e-10, "sqrt(4) = 2");

    x = 2.0;
    check_d(mth$sqrt(&x), 1.41421356, 1e-7, "sqrt(2) = 1.41421...");

    x = 0.0;
    check_d(mth$sqrt(&x), 0.0,   1e-10, "sqrt(0) = 0");

    x = 1.0;
    check_d(mth$sqrt(&x), 1.0,   1e-10, "sqrt(1) = 1");
}

/* ------------------------------------------------------------------ */
/* Exponential                                                         */
/* ------------------------------------------------------------------ */
static void test_exp(void)
{
    printf("Testing mth$exp...\n");
    double x;

    x = 0.0;
    check_d(mth$exp(&x), 1.0,       1e-10, "exp(0) = 1");

    x = 1.0;
    check_d(mth$exp(&x), 2.71828182845, 1e-7, "exp(1) = e");

    x = -1.0;
    check_d(mth$exp(&x), 1.0 / 2.71828182845, 1e-7, "exp(-1) = 1/e");
}

/* ------------------------------------------------------------------ */
/* Logarithm                                                           */
/* ------------------------------------------------------------------ */
static void test_log(void)
{
    printf("Testing mth$alog (ln) and mth$alog10...\n");
    double x;

    x = 1.0;
    check_d(mth$alog(&x), 0.0,  1e-10, "ln(1) = 0");

    x = 2.71828182845;
    check_d(mth$alog(&x), 1.0,  1e-5, "ln(e) = 1");

    x = 100.0;
    check_d(mth$alog10(&x), 2.0, 1e-10, "log10(100) = 2");

    x = 1.0;
    check_d(mth$alog10(&x), 0.0, 1e-10, "log10(1) = 0");

    x = 10.0;
    check_d(mth$alog10(&x), 1.0, 1e-10, "log10(10) = 1");
}

/* ------------------------------------------------------------------ */
/* Absolute value                                                      */
/* ------------------------------------------------------------------ */
static void test_abs(void)
{
    printf("Testing mth$abs...\n");
    double x;

    x = -3.14;
    check_d(mth$abs(&x), 3.14,  1e-10, "abs(-3.14) = 3.14");

    x = 3.14;
    check_d(mth$abs(&x), 3.14,  1e-10, "abs(3.14) = 3.14");

    x = 0.0;
    check_d(mth$abs(&x), 0.0,   1e-10, "abs(0) = 0");
}

/* ------------------------------------------------------------------ */
/* Power functions                                                     */
/* ------------------------------------------------------------------ */
static void test_power(void)
{
    printf("Testing mth$power and mth$power_ji...\n");
    double base, exp;

    base = 2.0; exp = 10.0;
    check_d(mth$power(&base, &exp), 1024.0, 1e-6, "2^10 = 1024");

    base = 3.0; exp = 0.0;
    check_d(mth$power(&base, &exp), 1.0, 1e-10, "3^0 = 1");

    base = 10.0; exp = 3.0;
    check_d(mth$power(&base, &exp), 1000.0, 1e-6, "10^3 = 1000");

    /* Integer power */
    int32_t ibase = 2, iexp = 8;
    check(mth$power_ji(&ibase, &iexp) == 256, "power_ji 2^8 = 256");

    int32_t ibase2 = 5, iexp2 = 0;
    check(mth$power_ji(&ibase2, &iexp2) == 1, "power_ji 5^0 = 1");

    int32_t ibase3 = 3, iexp3 = 3;
    check(mth$power_ji(&ibase3, &iexp3) == 27, "power_ji 3^3 = 27");
}

/* ------------------------------------------------------------------ */
/* Rounding functions                                                  */
/* ------------------------------------------------------------------ */
static void test_rounding(void)
{
    printf("Testing mth$floor, mth$ceil, mth$nint...\n");
    double x;

    x = 3.7;
    check_d(mth$floor(&x), 3.0, 1e-10, "floor(3.7) = 3");

    x = -3.7;
    check_d(mth$floor(&x), -4.0, 1e-10, "floor(-3.7) = -4");

    x = 3.2;
    check_d(mth$ceil(&x), 4.0, 1e-10, "ceil(3.2) = 4");

    x = -3.2;
    check_d(mth$ceil(&x), -3.0, 1e-10, "ceil(-3.2) = -3");

    x = 3.5;
    check(mth$nint(&x) == 4, "nint(3.5) = 4");

    x = 2.4;
    check(mth$nint(&x) == 2, "nint(2.4) = 2");

    x = -1.5;
    /* round() rounds half away from zero: -1.5 → -2 */
    check(mth$nint(&x) == -2, "nint(-1.5) = -2");
}

/* ------------------------------------------------------------------ */
/* min / max                                                           */
/* ------------------------------------------------------------------ */
static void test_minmax(void)
{
    printf("Testing mth$min / mth$max...\n");
    double a, b;

    a = 3.0; b = 7.0;
    check_d(mth$min(&a, &b), 3.0, 1e-10, "min(3, 7) = 3");
    check_d(mth$max(&a, &b), 7.0, 1e-10, "max(3, 7) = 7");

    a = -5.0; b = -2.0;
    check_d(mth$min(&a, &b), -5.0, 1e-10, "min(-5, -2) = -5");
    check_d(mth$max(&a, &b), -2.0, 1e-10, "max(-5, -2) = -2");

    a = 0.0; b = 0.0;
    check_d(mth$min(&a, &b), 0.0, 1e-10, "min(0, 0) = 0");
    check_d(mth$max(&a, &b), 0.0, 1e-10, "max(0, 0) = 0");
}

/* ------------------------------------------------------------------ */
/* atan2                                                               */
/* ------------------------------------------------------------------ */
static void test_atan2(void)
{
    printf("Testing mth$atan2...\n");
    double y, x;

    y = 1.0; x = 1.0;
    check_d(mth$atan2(&y, &x), M_PI / 4.0, 1e-10, "atan2(1,1) = pi/4");

    y = 0.0; x = 1.0;
    check_d(mth$atan2(&y, &x), 0.0, 1e-10, "atan2(0,1) = 0");

    y = 1.0; x = 0.0;
    check_d(mth$atan2(&y, &x), M_PI / 2.0, 1e-10, "atan2(1,0) = pi/2");
}

/* ------------------------------------------------------------------ */
/* sincos                                                              */
/* ------------------------------------------------------------------ */
static void test_sincos(void)
{
    printf("Testing mth$sincos...\n");
    double x = M_PI / 3.0;  /* 60 degrees */
    double sin_val = 0.0, cos_val = 0.0;

    mth$sincos(&x, &sin_val, &cos_val);
    check_d(sin_val, sqrt(3.0) / 2.0, 1e-10, "sincos: sin(pi/3) = sqrt(3)/2");
    check_d(cos_val, 0.5,             1e-10, "sincos: cos(pi/3) = 0.5");
}

/* ------------------------------------------------------------------ */
/* Random number generator                                             */
/* ------------------------------------------------------------------ */
static void test_random(void)
{
    printf("Testing mth$random...\n");
    uint32_t seed = 12345;
    float r1 = mth$random(&seed);
    float r2 = mth$random(&seed);

    check(r1 >= 0.0f && r1 < 1.0f, "random: r1 in [0, 1)");
    check(r2 >= 0.0f && r2 < 1.0f, "random: r2 in [0, 1)");
    check(r1 != r2, "random: consecutive values differ");

    /* Same seed gives same sequence */
    uint32_t seed2 = 12345;
    float r3 = mth$random(&seed2);
    check(r1 == r3, "random: same seed reproduces same sequence");
}

int main(void)
{
    printf("=== test_mth_routines: MTH$ math RTL ===\n");

    test_trig();
    test_trig_degrees();
    test_sqrt();
    test_exp();
    test_log();
    test_abs();
    test_power();
    test_rounding();
    test_minmax();
    test_atan2();
    test_sincos();
    test_random();

    if (failures == 0)
        printf("All mth_routines tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
