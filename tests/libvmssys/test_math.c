/*
 * test_math.c - Test math library
 */

#include "vmssys.h"

static int failures = 0;

static int approx_eq(double a, double b, double eps)
{
    double diff = a - b;
    if (diff < 0) diff = -diff;
    return diff < eps;
}

static void check_approx(double got, double expected, double eps, const char *name)
{
    if (approx_eq(got, expected, eps)) {
        vms_printf("  OK: %s\n", name);
    } else {
        /* Print without %f (we only have %d); use integer representation */
        long got_int = (long)(got * 10000);
        long exp_int = (long)(expected * 10000);
        vms_printf("  FAIL: %s (got %ld/10000, expected %ld/10000)\n",
                   name, got_int, exp_int);
        failures++;
    }
}

/* Boolean predicate check (for inf/NaN sentinels, which we can't print with the
 * integer-only vms_printf). */
static void check_true(int cond, const char *name)
{
    if (cond) {
        vms_printf("  OK: %s\n", name);
    } else {
        vms_printf("  FAIL: %s\n", name);
        failures++;
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    vms_printf("=== libvmssys math test ===\n");

    check_approx(vms_fabs(-3.14), 3.14, 0.001, "fabs");
    check_approx(vms_floor(3.7), 3.0, 0.001, "floor");
    check_approx(vms_floor(-3.7), -4.0, 0.001, "floor negative");
    check_approx(vms_ceil(3.2), 4.0, 0.001, "ceil");
    check_approx(vms_ceil(-3.2), -3.0, 0.001, "ceil negative");
    check_approx(vms_sqrt(4.0), 2.0, 0.0001, "sqrt(4)");
    check_approx(vms_sqrt(2.0), 1.41421356, 0.0001, "sqrt(2)");
    check_approx(vms_sin(0.0), 0.0, 0.0001, "sin(0)");
    check_approx(vms_cos(0.0), 1.0, 0.0001, "cos(0)");
    check_approx(vms_exp(0.0), 1.0, 0.0001, "exp(0)");
    check_approx(vms_exp(1.0), 2.71828, 0.001, "exp(1)");
    check_approx(vms_log(1.0), 0.0, 0.0001, "log(1)");
    check_approx(vms_pow(2.0, 10.0), 1024.0, 0.001, "pow(2,10)");
    check_approx(vms_pow(3.0, 0.0), 1.0, 0.001, "pow(x,0)");
    check_approx(vms_log10(100.0), 2.0, 0.001, "log10(100)");
    check_approx(vms_fmod(7.0, 3.0), 1.0, 0.001, "fmod(7,3)");
    check_approx(vms_atan2(1.0, 1.0), 0.7854, 0.001, "atan2(1,1)");

    /* ---------------------------------------------------------------
     * Float-format refactor coverage (rd vms-30a, item 5.3). These
     * exercise the exp()/log() reconstruction and the inf/NaN sentinel
     * helpers that were split into an IEEE bit path and a VAX arithmetic
     * path.  On this (IEEE) host the bit path runs; the assertions pin the
     * behavior so the split did not regress it.  The VAX arithmetic path is
     * compile-proven by the netbsd-vax cross gate and runtime-validated on
     * SIMH in P4.
     * --------------------------------------------------------------- */

    /* exp/log reconstruction over a wider range than exp(1) alone. */
    check_approx(vms_exp(5.0),  148.4131591, 0.01,  "exp(5)");
    check_approx(vms_exp(-3.0), 0.049787068, 0.001, "exp(-3)");
    check_approx(vms_log(2.718281828), 1.0, 0.0001, "log(e)");
    check_approx(vms_log(1000.0), 6.907755279, 0.001, "log(1000)");
    check_approx(vms_pow(2.0, 0.5), 1.414213562, 0.0001, "pow(2,0.5)");
    /* exp(log(x)) round-trip stresses both reconstruction paths together. */
    check_approx(vms_exp(vms_log(42.0)), 42.0, 0.01, "exp(log(42)) round-trip");

    /* Sentinel helpers: +inf from overflow, -inf from log(0), NaN from log(<0).
     * Detected via magnitude / self-inequality, not printed (no %f). */
    check_true(vms_exp(1000.0) > 1e300,  "exp(1000) -> +inf sentinel");
    check_true(vms_log(0.0)    < -1e300, "log(0) -> -inf sentinel");
    { double nanv = vms_log(-1.0); check_true(nanv != nanv, "log(-1) -> NaN sentinel"); }
    { double nanv = vms_asin(2.0); check_true(nanv != nanv, "asin(2) -> NaN sentinel"); }

    if (failures == 0)
        vms_printf("All math tests passed.\n");
    else
        vms_printf("Some math tests FAILED (%d).\n", failures);

    return failures;
}
