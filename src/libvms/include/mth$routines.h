/*
 * MTH$ROUTINES.H - VMS Mathematics (MTH$) Routine Prototypes
 *
 * OpenVMX compatibility layer - Declares the MTH$ run-time library
 * routines for mathematical operations.  In VMS, the MTH$ routines
 * provide high-quality mathematical functions with well-defined
 * error handling through the VMS condition mechanism.
 *
 * VMS calling convention: Arguments are passed BY REFERENCE (pointer
 * to the value), not by value.  This is critical -- passing a double
 * instead of a pointer-to-double will crash.  Return values are
 * returned by value.
 *
 * Naming convention:
 *   mth$xxx   - double precision (F_floating or D_floating on VAX,
 *               T_floating on Alpha/IA-64)
 *   mth$xxxf  - single precision (F_floating on VAX, S_floating on Alpha)
 *   mth$xxxg  - G_floating (VAX double extended)
 *
 * Reference: OpenVMS RTL Mathematics (MTH$) Manual
 */

#ifndef __MTH_ROUTINES_H
#define __MTH_ROUTINES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Trigonometric Functions - Double Precision
 * ================================================================ */

/**
 * mth$sin - Sine (double precision)
 *
 * @param x  Pointer to angle in radians
 * @return   sin(*x)
 */
double mth$sin(const double *x);

/**
 * mth$cos - Cosine (double precision)
 *
 * @param x  Pointer to angle in radians
 * @return   cos(*x)
 */
double mth$cos(const double *x);

/**
 * mth$tan - Tangent (double precision)
 *
 * @param x  Pointer to angle in radians
 * @return   tan(*x)
 *
 * Signals MTH$_SIGLOSMAT if argument is too large for meaningful result.
 */
double mth$tan(const double *x);

/**
 * mth$sincos - Simultaneous sine and cosine (double precision)
 *
 * @param x         Pointer to angle in radians
 * @param sin_val   Pointer to receive sin(*x)
 * @param cos_val   Pointer to receive cos(*x)
 *
 * More efficient than calling mth$sin and mth$cos separately.
 */
void mth$sincos(const double *x, double *sin_val, double *cos_val);

/* ================================================================
 * Inverse Trigonometric Functions - Double Precision
 * ================================================================ */

/**
 * mth$asin - Arc sine (double precision)
 *
 * @param x  Pointer to value (must be in [-1, 1])
 * @return   asin(*x) in radians [-pi/2, pi/2]
 *
 * Signals MTH$_INVARGMAT if |*x| > 1.
 */
double mth$asin(const double *x);

/**
 * mth$acos - Arc cosine (double precision)
 *
 * @param x  Pointer to value (must be in [-1, 1])
 * @return   acos(*x) in radians [0, pi]
 *
 * Signals MTH$_INVARGMAT if |*x| > 1.
 */
double mth$acos(const double *x);

/**
 * mth$atan - Arc tangent (double precision)
 *
 * @param x  Pointer to value
 * @return   atan(*x) in radians (-pi/2, pi/2)
 */
double mth$atan(const double *x);

/**
 * mth$atan2 - Arc tangent of y/x (double precision)
 *
 * @param y  Pointer to y value
 * @param x  Pointer to x value
 * @return   atan2(*y, *x) in radians (-pi, pi]
 *
 * Returns the angle whose tangent is *y / *x, using the signs
 * of both arguments to determine the quadrant.
 * Signals MTH$_INVARGMAT if both *x and *y are zero.
 */
double mth$atan2(const double *y, const double *x);

/* ================================================================
 * Exponential and Logarithmic Functions - Double Precision
 * ================================================================ */

/**
 * mth$exp - Exponential (double precision)
 *
 * @param x  Pointer to exponent value
 * @return   e^(*x)
 *
 * Signals MTH$_FLOOVEMAT on overflow.
 */
double mth$exp(const double *x);

/**
 * mth$alog - Natural logarithm (double precision)
 *
 * @param x  Pointer to value (must be > 0)
 * @return   ln(*x)
 *
 * Note: Named "alog" following Fortran convention (not "log"
 * to avoid conflict with C library).
 * Signals MTH$_INVARGMAT if *x <= 0.
 */
double mth$alog(const double *x);

/**
 * mth$alog10 - Common (base 10) logarithm (double precision)
 *
 * @param x  Pointer to value (must be > 0)
 * @return   log10(*x)
 *
 * Signals MTH$_INVARGMAT if *x <= 0.
 */
double mth$alog10(const double *x);

/**
 * mth$alog2 - Base 2 logarithm (double precision)
 *
 * @param x  Pointer to value (must be > 0)
 * @return   log2(*x)
 */
double mth$alog2(const double *x);

/* ================================================================
 * Power and Root Functions - Double Precision
 * ================================================================ */

/**
 * mth$sqrt - Square root (double precision)
 *
 * @param x  Pointer to value (must be >= 0)
 * @return   sqrt(*x)
 *
 * Signals MTH$_INVARGMAT if *x < 0.
 */
double mth$sqrt(const double *x);

/**
 * mth$power - Raise to power (double precision)
 *
 * @param base  Pointer to base value
 * @param exp   Pointer to exponent value
 * @return      (*base) ^ (*exp)
 */
double mth$power(const double *base, const double *exp);

/**
 * mth$power_ji - Raise integer to integer power
 *
 * @param base  Pointer to integer base
 * @param exp   Pointer to integer exponent
 * @return      (*base) ^ (*exp)
 */
int32_t mth$power_ji(const int32_t *base, const int32_t *exp);

/* ================================================================
 * Hyperbolic Functions - Double Precision
 * ================================================================ */

/**
 * mth$sinh - Hyperbolic sine (double precision)
 * @param x  Pointer to value
 * @return   sinh(*x)
 */
double mth$sinh(const double *x);

/**
 * mth$cosh - Hyperbolic cosine (double precision)
 * @param x  Pointer to value
 * @return   cosh(*x)
 */
double mth$cosh(const double *x);

/**
 * mth$tanh - Hyperbolic tangent (double precision)
 * @param x  Pointer to value
 * @return   tanh(*x)
 */
double mth$tanh(const double *x);

/* ================================================================
 * Trigonometric Functions - Single Precision (float)
 * ================================================================ */

/**
 * mth$sinf - Sine (single precision)
 * @param x  Pointer to angle in radians
 * @return   sin(*x)
 */
float mth$sinf(const float *x);

/**
 * mth$cosf - Cosine (single precision)
 * @param x  Pointer to angle in radians
 * @return   cos(*x)
 */
float mth$cosf(const float *x);

/**
 * mth$tanf - Tangent (single precision)
 * @param x  Pointer to angle in radians
 * @return   tan(*x)
 */
float mth$tanf(const float *x);

/* ================================================================
 * Inverse Trigonometric Functions - Single Precision (float)
 * ================================================================ */

/**
 * mth$asinf - Arc sine (single precision)
 * @param x  Pointer to value [-1, 1]
 * @return   asin(*x) in radians
 */
float mth$asinf(const float *x);

/**
 * mth$acosf - Arc cosine (single precision)
 * @param x  Pointer to value [-1, 1]
 * @return   acos(*x) in radians
 */
float mth$acosf(const float *x);

/**
 * mth$atanf - Arc tangent (single precision)
 * @param x  Pointer to value
 * @return   atan(*x) in radians
 */
float mth$atanf(const float *x);

/**
 * mth$atan2f - Arc tangent of y/x (single precision)
 * @param y  Pointer to y value
 * @param x  Pointer to x value
 * @return   atan2(*y, *x) in radians
 */
float mth$atan2f(const float *y, const float *x);

/* ================================================================
 * Exponential and Logarithmic Functions - Single Precision (float)
 * ================================================================ */

/**
 * mth$expf - Exponential (single precision)
 * @param x  Pointer to exponent
 * @return   e^(*x)
 */
float mth$expf(const float *x);

/**
 * mth$alogf - Natural logarithm (single precision)
 * @param x  Pointer to value (> 0)
 * @return   ln(*x)
 */
float mth$alogf(const float *x);

/**
 * mth$alog10f - Common logarithm (single precision)
 * @param x  Pointer to value (> 0)
 * @return   log10(*x)
 */
float mth$alog10f(const float *x);

/**
 * mth$sqrtf - Square root (single precision)
 * @param x  Pointer to value (>= 0)
 * @return   sqrt(*x)
 */
float mth$sqrtf(const float *x);

/* ================================================================
 * Miscellaneous Mathematical Functions
 * ================================================================ */

/**
 * mth$abs - Absolute value (double precision)
 * @param x  Pointer to value
 * @return   |*x|
 */
double mth$abs(const double *x);

/**
 * mth$absf - Absolute value (single precision)
 * @param x  Pointer to value
 * @return   |*x|
 */
float mth$absf(const float *x);

/**
 * mth$sign - Transfer of sign (double precision)
 * @param x  Pointer to magnitude value
 * @param y  Pointer to sign value
 * @return   |*x| with sign of *y
 */
double mth$sign(const double *x, const double *y);

/**
 * mth$nint - Nearest integer (double precision)
 * @param x  Pointer to value
 * @return   Nearest integer to *x (as int32_t)
 */
int32_t mth$nint(const double *x);

/**
 * mth$floor - Floor function (double precision)
 * @param x  Pointer to value
 * @return   Greatest integer <= *x
 */
double mth$floor(const double *x);

/**
 * mth$ceil - Ceiling function (double precision)
 * @param x  Pointer to value
 * @return   Least integer >= *x
 */
double mth$ceil(const double *x);

/**
 * mth$mod - Modulus (double precision)
 * @param x  Pointer to dividend
 * @param y  Pointer to divisor
 * @return   Remainder of *x / *y
 */
double mth$mod(const double *x, const double *y);

/**
 * mth$max - Maximum of two values (double precision)
 * @param x  Pointer to first value
 * @param y  Pointer to second value
 * @return   max(*x, *y)
 */
double mth$max(const double *x, const double *y);

/**
 * mth$min - Minimum of two values (double precision)
 * @param x  Pointer to first value
 * @param y  Pointer to second value
 * @return   min(*x, *y)
 */
double mth$min(const double *x, const double *y);

/* ================================================================
 * Random Number Generation
 * ================================================================ */

/**
 * mth$random - Generate uniform random number
 *
 * @param seed  Pointer to seed value (modified on return)
 * @return      Random float in [0.0, 1.0)
 *
 * Uses a multiplicative congruential algorithm.  The seed is
 * updated in place for sequential calls.
 */
float mth$random(uint32_t *seed);

/* ================================================================
 * Degree/Radian Conversion
 * ================================================================ */

/**
 * mth$sind - Sine of angle in degrees (double precision)
 * @param x  Pointer to angle in degrees
 * @return   sin(*x degrees)
 */
double mth$sind(const double *x);

/**
 * mth$cosd - Cosine of angle in degrees (double precision)
 * @param x  Pointer to angle in degrees
 * @return   cos(*x degrees)
 */
double mth$cosd(const double *x);

/**
 * mth$tand - Tangent of angle in degrees (double precision)
 * @param x  Pointer to angle in degrees
 * @return   tan(*x degrees)
 */
double mth$tand(const double *x);

/* ================================================================
 * MTH$ condition value definitions
 * ================================================================ */

#define MTH$_NORMAL      0x00901001  /* Normal completion */
#define MTH$_FLOOVEMAT   0x00901024  /* Floating overflow in math library */
#define MTH$_FLOUNDMAT   0x0090102C  /* Floating underflow in math library */
#define MTH$_INVARGMAT   0x00901034  /* Invalid argument to math library */
#define MTH$_SIGLOSMAT   0x0090103C  /* Significance loss in math library */
#define MTH$_LOGZERNEG   0x00901044  /* Log of zero or negative number */
#define MTH$_SQUROONEG   0x0090104C  /* Square root of negative number */
#define MTH$_WRONUMARG   0x00901054  /* Wrong number of arguments */
#define MTH$_UNDEXP      0x0090105C  /* Undefined exponentiation */

#ifdef __cplusplus
}
#endif

#endif /* __MTH_ROUTINES_H */
