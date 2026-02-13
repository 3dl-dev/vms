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

    if (failures == 0)
        vms_printf("All math tests passed.\n");
    else
        vms_printf("Some math tests FAILED (%d).\n", failures);

    return failures;
}
