/*
 * mth_routines.c - MTH$ Math Routines
 *
 * VMS math library wrappers. These provide VMS calling convention
 * (pass by reference) around standard C math.h functions.
 * On VMS, math routines receive pointers to their arguments
 * rather than values, following the standard VMS convention.
 */

#include <math.h>
#include "mth$routines.h"

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

/* Single-precision variants */
float mth$sinf(const float *x)  { return sinf(*x); }
float mth$cosf(const float *x)  { return cosf(*x); }
float mth$sqrtf(const float *x) { return sqrtf(*x); }
