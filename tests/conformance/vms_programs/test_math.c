/*
 * test_math.c - VMS Math Library Conformance Test
 *
 * Tests mth$ mathematical functions:
 * - mth$sqrt, mth$sin, mth$cos, mth$exp, mth$alog
 * - mth$sinh, mth$cosh, mth$tanh
 * - Verify results within tolerance
 *
 * Note: VMS mth$ functions take pointers to arguments, not values.
 */

#include <stdio.h>
#include <math.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <mth$routines.h>

#define TOLERANCE 1e-10

/* Helper to check if two doubles are approximately equal */
static int approx_equal(double a, double b, double tol) {
    double diff = a - b;
    if (diff < 0) diff = -diff;
    return diff < tol;
}

int main(void) {
    int failures = 0;
    double result;

    printf("VMS Math Library Test\n");
    printf("=====================\n");

    /* Test 1: mth$sqrt */
    double val1 = 16.0;
    result = mth$sqrt(&val1);
    if (approx_equal(result, 4.0, TOLERANCE)) {
        printf("PASS: mth$sqrt(16.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$sqrt(16.0) expected 4.0, got %.10f\n", result);
        failures++;
    }

    /* Test 2: mth$sin */
    double val2 = 0.0;
    result = mth$sin(&val2);
    if (approx_equal(result, 0.0, TOLERANCE)) {
        printf("PASS: mth$sin(0.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$sin(0.0) expected 0.0, got %.10f\n", result);
        failures++;
    }

    /* Test 3: mth$sin(pi/2) should be 1.0 */
    double val3 = M_PI / 2.0;
    result = mth$sin(&val3);
    if (approx_equal(result, 1.0, TOLERANCE)) {
        printf("PASS: mth$sin(pi/2) = %.10f\n", result);
    } else {
        printf("FAIL: mth$sin(pi/2) expected 1.0, got %.10f\n", result);
        failures++;
    }

    /* Test 4: mth$cos */
    double val4 = 0.0;
    result = mth$cos(&val4);
    if (approx_equal(result, 1.0, TOLERANCE)) {
        printf("PASS: mth$cos(0.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$cos(0.0) expected 1.0, got %.10f\n", result);
        failures++;
    }

    /* Test 5: mth$cos(pi) should be -1.0 */
    double val5 = M_PI;
    result = mth$cos(&val5);
    if (approx_equal(result, -1.0, TOLERANCE)) {
        printf("PASS: mth$cos(pi) = %.10f\n", result);
    } else {
        printf("FAIL: mth$cos(pi) expected -1.0, got %.10f\n", result);
        failures++;
    }

    /* Test 6: mth$exp */
    double val6 = 0.0;
    result = mth$exp(&val6);
    if (approx_equal(result, 1.0, TOLERANCE)) {
        printf("PASS: mth$exp(0.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$exp(0.0) expected 1.0, got %.10f\n", result);
        failures++;
    }

    /* Test 7: mth$exp(1.0) should be e */
    double val7 = 1.0;
    result = mth$exp(&val7);
    if (approx_equal(result, M_E, TOLERANCE)) {
        printf("PASS: mth$exp(1.0) = %.10f (e)\n", result);
    } else {
        printf("FAIL: mth$exp(1.0) expected %.10f, got %.10f\n", M_E, result);
        failures++;
    }

    /* Test 8: mth$alog (natural log) */
    double val8 = M_E;
    result = mth$alog(&val8);
    if (approx_equal(result, 1.0, TOLERANCE)) {
        printf("PASS: mth$alog(e) = %.10f\n", result);
    } else {
        printf("FAIL: mth$alog(e) expected 1.0, got %.10f\n", result);
        failures++;
    }

    /* Test 9: mth$alog(1.0) should be 0.0 */
    double val9 = 1.0;
    result = mth$alog(&val9);
    if (approx_equal(result, 0.0, TOLERANCE)) {
        printf("PASS: mth$alog(1.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$alog(1.0) expected 0.0, got %.10f\n", result);
        failures++;
    }

    /* Test 10: mth$sinh */
    double val10 = 0.0;
    result = mth$sinh(&val10);
    if (approx_equal(result, 0.0, TOLERANCE)) {
        printf("PASS: mth$sinh(0.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$sinh(0.0) expected 0.0, got %.10f\n", result);
        failures++;
    }

    /* Test 11: mth$cosh */
    double val11 = 0.0;
    result = mth$cosh(&val11);
    if (approx_equal(result, 1.0, TOLERANCE)) {
        printf("PASS: mth$cosh(0.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$cosh(0.0) expected 1.0, got %.10f\n", result);
        failures++;
    }

    /* Test 12: mth$tanh */
    double val12 = 0.0;
    result = mth$tanh(&val12);
    if (approx_equal(result, 0.0, TOLERANCE)) {
        printf("PASS: mth$tanh(0.0) = %.10f\n", result);
    } else {
        printf("FAIL: mth$tanh(0.0) expected 0.0, got %.10f\n", result);
        failures++;
    }

    /* Test 13: mth$tanh(large value) should approach 1.0 */
    double val13 = 10.0;
    result = mth$tanh(&val13);
    if (approx_equal(result, 1.0, 1e-6)) {
        printf("PASS: mth$tanh(10.0) = %.10f (approaches 1.0)\n", result);
    } else {
        printf("FAIL: mth$tanh(10.0) expected ~1.0, got %.10f\n", result);
        failures++;
    }

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All math library tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
