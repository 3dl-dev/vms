/*
 * mth_routines.c - MTH$ Math Routines
 *
 * VMS math library wrappers. These provide VMS calling convention
 * (pass by reference) around standard C math.h functions.
 * On VMS, math routines receive pointers to their arguments
 * rather than values, following the standard VMS convention.
 */

#include <math.h>
#include <stdint.h>
#include "mth$routines.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Trigonometric functions */
double mth$sin(const double *x)  { return sin(*x); }
double mth$cos(const double *x)  { return cos(*x); }
double mth$tan(const double *x)  { return tan(*x); }

/* Inverse trigonometric functions */
double mth$asin(const double *x) { return asin(*x); }
double mth$acos(const double *x) { return acos(*x); }
double mth$atan(const double *x) { return atan(*x); }
double mth$atan2(const double *y, const double *x) { return atan2(*y, *x); }

/* Power and exponential functions */
double mth$sqrt(const double *x) { return sqrt(*x); }
double mth$exp(const double *x)  { return exp(*x); }

/* Logarithmic functions */
double mth$alog(const double *x)   { return log(*x); }   /* Natural log */
double mth$alog10(const double *x) { return log10(*x); } /* Log base 10 */

/* Absolute value */
double mth$abs(const double *x) { return fabs(*x); }

/* Hyperbolic functions */
double mth$sinh(const double *x) { return sinh(*x); }
double mth$cosh(const double *x) { return cosh(*x); }
double mth$tanh(const double *x) { return tanh(*x); }

/* Log base 2 */
double mth$alog2(const double *x) { return log2(*x); }

/* Power functions */
double mth$power(const double *base, const double *exp) { return pow(*base, *exp); }

int32_t mth$power_ji(const int32_t *base, const int32_t *exp) {
    int32_t result = 1;
    int32_t b = *base;
    int32_t e = *exp;

    if (e < 0) return 0;  /* Integer division by power */

    while (e > 0) {
        if (e & 1) result *= b;
        b *= b;
        e >>= 1;
    }
    return result;
}

/* Degree trigonometric functions */
double mth$sind(const double *x) { return sin(*x * M_PI / 180.0); }
double mth$cosd(const double *x) { return cos(*x * M_PI / 180.0); }
double mth$tand(const double *x) { return tan(*x * M_PI / 180.0); }

/* Simultaneous sin/cos */
void mth$sincos(const double *x, double *sin_val, double *cos_val) {
    *sin_val = sin(*x);
    *cos_val = cos(*x);
}

/* Miscellaneous functions */
float mth$absf(const float *x) { return fabsf(*x); }

double mth$sign(const double *x, const double *y) {
    return (*y >= 0.0) ? fabs(*x) : -fabs(*x);
}

int32_t mth$nint(const double *x) {
    return (int32_t)round(*x);
}

double mth$floor(const double *x) { return floor(*x); }
double mth$ceil(const double *x) { return ceil(*x); }
double mth$mod(const double *x, const double *y) { return fmod(*x, *y); }
double mth$max(const double *x, const double *y) { return (*x > *y) ? *x : *y; }
double mth$min(const double *x, const double *y) { return (*x < *y) ? *x : *y; }

/* Random number generation */
float mth$random(uint32_t *seed) {
    /* Multiplicative congruential generator */
    *seed = (uint32_t)((*seed * 69069UL + 1) & 0xFFFFFFFFUL);
    return (float)(*seed) / 4294967296.0f;
}

/* Single-precision variants */
float mth$sinf(const float *x)  { return sinf(*x); }
float mth$cosf(const float *x)  { return cosf(*x); }
float mth$tanf(const float *x)  { return tanf(*x); }
float mth$asinf(const float *x) { return asinf(*x); }
float mth$acosf(const float *x) { return acosf(*x); }
float mth$atanf(const float *x) { return atanf(*x); }
float mth$atan2f(const float *y, const float *x) { return atan2f(*y, *x); }
float mth$expf(const float *x)  { return expf(*x); }
float mth$alogf(const float *x) { return logf(*x); }
float mth$alog10f(const float *x) { return log10f(*x); }
float mth$sqrtf(const float *x) { return sqrtf(*x); }
