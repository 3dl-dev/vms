/*
 * float.h - Alpha long double characteristics.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * GCC's alpha target uses LONG_DOUBLE_TYPE_SIZE = 64, i.e. `long double` is the
 * same IEEE binary64 (T_floating) as `double`. `double` being IEEE binary64 was
 * established by the tprintf work. The build recipe asserts
 * __SIZEOF_LONG_DOUBLE__ == 8 against the real cross compiler; if a future
 * toolchain configures 128-bit X_floating long double, swap this file for the
 * IEEE-quad variant (LDBL_MANT_DIG 113) and update the assertion.
 */
#define FLT_EVAL_METHOD 0

#define LDBL_TRUE_MIN 4.94065645841246544177e-324L
#define LDBL_MIN 2.22507385850720138309e-308L
#define LDBL_MAX 1.79769313486231570815e+308L
#define LDBL_EPSILON 2.22044604925031308085e-16L

#define LDBL_MANT_DIG 53
#define LDBL_MIN_EXP (-1021)
#define LDBL_MAX_EXP 1024

#define LDBL_DIG 15
#define LDBL_MIN_10_EXP (-307)
#define LDBL_MAX_10_EXP 308

#define DECIMAL_DIG 17
