/*
 * ots_routines.c - OTS$ (Object Time System) Routines
 *
 * Integer/string conversion utilities from the VMS OTS runtime.
 * These convert between binary integer values and their textual
 * representations stored in VMS descriptors.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <complex.h>
#include "ssdef.h"
#include "descrip.h"
/* NB: ots$routines.h is deliberately NOT included here — several of the
 * older ots$cvt_* signatures in this file predate and diverge from the
 * prototypes in that header (a pre-existing divergence).  The routines
 * added below (complex arithmetic, cnvout, cvt_t) are ABI-compatible with
 * their header prototypes; the corpus and the header are the callers. */

/*
 * ots$cvt_l_ti - Convert longword integer to decimal text in a descriptor.
 *
 * Formats the integer value as a right-justified decimal string in the
 * descriptor's buffer, padding with leading spaces.
 *
 * Parameters:
 *   value      - Pointer to integer value to convert
 *   dest       - Descriptor to receive the text
 *   min_digits - Minimum number of digits (ignored for now)
 *   size       - Field size (ignored for now)
 *   flags      - Conversion flags (ignored for now)
 */
uint32_t ots$cvt_l_ti(const int32_t *value,
                      struct dsc$descriptor_s *dest,
                      const int32_t *min_digits,
                      const int32_t *size,
                      const uint32_t *flags) {
    (void)min_digits; (void)size; (void)flags;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", *value);

    uint16_t copylen = (uint16_t)len;
    if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;

    /* Right-justify in field, pad with leading spaces */
    if (copylen < dest->dsc$w_length) {
        memset(dest->dsc$a_pointer, ' ', dest->dsc$w_length - copylen);
        memcpy(dest->dsc$a_pointer + dest->dsc$w_length - copylen,
               buf, copylen);
    } else {
        memcpy(dest->dsc$a_pointer, buf + (len - copylen), copylen);
    }

    return SS$_NORMAL;
}

/*
 * ots$cvt_ti_l - Convert decimal text in a descriptor to a longword integer.
 *
 * Parses the text in the descriptor as a decimal integer, skipping
 * leading whitespace.
 *
 * Parameters:
 *   src   - Descriptor containing the text to convert
 *   value - Receives the converted integer value
 *   flags - Conversion flags (ignored for now)
 */
uint32_t ots$cvt_ti_l(const struct dsc$descriptor_s *src,
                      int32_t *value,
                      const uint32_t *flags) {
    (void)flags;
    if (!src || !value || !src->dsc$a_pointer) return SS$_BADPARAM;

    char buf[64];
    dsc$strncpy(buf, src, sizeof(buf));

    /* Skip leading whitespace */
    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;

    char *endp;
    long result = strtol(p, &endp, 10);

    /* Check that at least one digit was converted */
    if (endp == p) return SS$_BADPARAM;

    *value = (int32_t)result;
    return SS$_NORMAL;
}

/*
 * ots$cvt_l_tz - Convert longword to hexadecimal text.
 */
uint32_t ots$cvt_l_tz(const int32_t *value,
                      struct dsc$descriptor_s *dest,
                      const int32_t *min_digits) {
    (void)min_digits;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%X", (uint32_t)*value);

    uint16_t copylen = (uint16_t)len;
    if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;

    /* Right-justify with leading spaces */
    if (copylen < dest->dsc$w_length) {
        memset(dest->dsc$a_pointer, ' ', dest->dsc$w_length - copylen);
        memcpy(dest->dsc$a_pointer + dest->dsc$w_length - copylen,
               buf, copylen);
    } else {
        memcpy(dest->dsc$a_pointer, buf, copylen);
    }

    return SS$_NORMAL;
}

/*
 * ots$cvt_l_to - Convert longword to octal text.
 */
uint32_t ots$cvt_l_to(const int32_t *value,
                      struct dsc$descriptor_s *dest,
                      const int32_t *min_digits) {
    (void)min_digits;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%o", (uint32_t)*value);

    uint16_t copylen = (uint16_t)len;
    if (copylen > dest->dsc$w_length) copylen = dest->dsc$w_length;

    /* Right-justify with leading spaces */
    if (copylen < dest->dsc$w_length) {
        memset(dest->dsc$a_pointer, ' ', dest->dsc$w_length - copylen);
        memcpy(dest->dsc$a_pointer + dest->dsc$w_length - copylen,
               buf, copylen);
    } else {
        memcpy(dest->dsc$a_pointer, buf, copylen);
    }

    return SS$_NORMAL;
}

/* ================================================================
 * Additional OTS$ conversion / move / power routines (vms-801 R2.2
 * batch 2).  Signatures match ots$routines.h.  Grounded in the public
 * OpenVMS RTL "OTS$ (Object Time System) Reference" descriptions of
 * the CVT_x_y, MOVE3/MOVE5 and POWxx families, and in the demonstrated
 * I/O of the Eight-Cubed corpus (which was run on real OpenVMS):
 * "11000000111001"(bin) == "30071"(oct) == "3039"(hex) == 12345.
 * ================================================================ */

/*
 * right_justify_field - place `src` (len chars) right-justified into a
 * fixed-length descriptor, space-padding on the left.  If `src` is
 * longer than the field it is left-truncated (least-significant digits
 * kept, matching the VMS "field too small" behaviour of these calls).
 */
static void right_justify_field(struct dsc$descriptor_s *dest,
                                const char *src, int len) {
    uint16_t field = dest->dsc$w_length;
    if (len >= field) {
        memcpy(dest->dsc$a_pointer, src + (len - field), field);
    } else {
        memset(dest->dsc$a_pointer, ' ', field - len);
        memcpy(dest->dsc$a_pointer + (field - len), src, len);
    }
}

/*
 * fmt_radix - format an unsigned value in the given radix (2/8/10/16)
 * into buf (which must be large enough), left-padding with '0' up to
 * min_digits.  Returns the number of characters written.
 */
static int fmt_radix(uint32_t value, unsigned radix, unsigned min_digits,
                     char *buf, int bufsz) {
    static const char digits[] = "0123456789ABCDEF";
    char tmp[40];
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value != 0 && n < (int)sizeof(tmp)) {
            tmp[n++] = digits[value % radix];
            value /= radix;
        }
    }
    while (n < (int)min_digits && n < (int)sizeof(tmp)) {
        tmp[n++] = '0';
    }
    /* tmp holds the digits least-significant first; reverse into buf */
    int out = 0;
    for (int i = n - 1; i >= 0 && out < bufsz; i--) {
        buf[out++] = tmp[i];
    }
    return out;
}

/*
 * ots$cvt_l_tu - Convert unsigned longword to unsigned decimal text.
 */
uint32_t ots$cvt_l_tu(const uint32_t *value,
                      struct dsc$descriptor_s *dest,
                      const uint32_t min_digits,
                      const uint32_t size) {
    (void)size;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;
    char buf[40];
    int len = fmt_radix(*value, 10, min_digits, buf, sizeof(buf));
    right_justify_field(dest, buf, len);
    return SS$_NORMAL;
}

/*
 * ots$cvt_l_tb - Convert unsigned longword to binary (base-2) text.
 */
uint32_t ots$cvt_l_tb(const uint32_t *value,
                      struct dsc$descriptor_s *dest,
                      const uint32_t min_digits,
                      const uint32_t size) {
    (void)size;
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;
    char buf[40];
    int len = fmt_radix(*value, 2, min_digits, buf, sizeof(buf));
    right_justify_field(dest, buf, len);
    return SS$_NORMAL;
}

/*
 * ots$cvt_l_tl - Convert signed longword to decimal text (signed).
 */
uint32_t ots$cvt_l_tl(const int32_t *value,
                      struct dsc$descriptor_s *dest) {
    if (!value || !dest || !dest->dsc$a_pointer) return SS$_BADPARAM;
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d", *value);
    right_justify_field(dest, buf, len);
    return SS$_NORMAL;
}

/*
 * cvt_text_to_long - shared text->longword parser for a fixed radix,
 * skipping leading blanks and an optional leading sign.
 */
static uint32_t cvt_text_to_long(const struct dsc$descriptor_s *src,
                                 uint32_t *dest, unsigned radix) {
    if (!src || !dest || !src->dsc$a_pointer) return SS$_BADPARAM;
    const char *p = src->dsc$a_pointer;
    int n = src->dsc$w_length;
    int i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    int neg = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) {
        neg = (p[i] == '-');
        i++;
    }
    if (i >= n) return SS$_BADPARAM;  /* no digits */
    uint32_t acc = 0;
    for (; i < n; i++) {
        char c = p[i];
        if (c == ' ' || c == '\t') break;   /* trailing blanks OK */
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else return SS$_BADPARAM;
        if (d >= radix) return SS$_BADPARAM;
        acc = acc * radix + d;
    }
    *dest = neg ? (uint32_t)(-(int32_t)acc) : acc;
    return SS$_NORMAL;
}

/*
 * ots$cvt_tu_l - Convert unsigned decimal text to longword.
 */
uint32_t ots$cvt_tu_l(const struct dsc$descriptor_s *src, uint32_t *dest,
                      const uint32_t size, const uint32_t flags) {
    (void)size; (void)flags;
    return cvt_text_to_long(src, dest, 10);
}

/*
 * ots$cvt_to_l - Convert octal text to longword.
 */
uint32_t ots$cvt_to_l(const struct dsc$descriptor_s *src, uint32_t *dest,
                      const uint32_t size, const uint32_t flags) {
    (void)size; (void)flags;
    return cvt_text_to_long(src, dest, 8);
}

/*
 * ots$cvt_tz_l - Convert hexadecimal text to longword.
 */
uint32_t ots$cvt_tz_l(const struct dsc$descriptor_s *src, uint32_t *dest,
                      const uint32_t size, const uint32_t flags) {
    (void)size; (void)flags;
    return cvt_text_to_long(src, dest, 16);
}

/*
 * ots$cvt_tb_l - Convert binary (base-2) text to longword.
 */
uint32_t ots$cvt_tb_l(const struct dsc$descriptor_s *src, uint32_t *dest,
                      const uint32_t size, const uint32_t flags) {
    (void)size; (void)flags;
    return cvt_text_to_long(src, dest, 2);
}

/*
 * ots$cvt_tl_l - Convert FORTRAN logical text to longword.
 *
 * Accepts .TRUE./.FALSE. (with or without dots), T/F, Y/N, 1/0
 * (case-insensitive), skipping leading blanks and a leading '.'.
 * A true value is returned with its low bit set (VMS FORTRAN logical
 * convention); a false value is 0.
 */
uint32_t ots$cvt_tl_l(const struct dsc$descriptor_s *src, uint32_t *dest,
                      const uint32_t size, const uint32_t flags) {
    (void)size; (void)flags;
    if (!src || !dest || !src->dsc$a_pointer) return SS$_BADPARAM;
    const char *p = src->dsc$a_pointer;
    int n = src->dsc$w_length;
    int i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '.')) i++;
    if (i >= n) return SS$_BADPARAM;
    char c = (char)toupper((unsigned char)p[i]);
    if (c == 'T' || c == 'Y' || c == '1') {
        *dest = 1;
        return SS$_NORMAL;
    }
    if (c == 'F' || c == 'N' || c == '0') {
        *dest = 0;
        return SS$_NORMAL;
    }
    return SS$_BADPARAM;
}

/*
 * ots$move3 - Move `count` bytes from src to dest (handles overlap).
 */
void ots$move3(const uint32_t count, const void *src, void *dest) {
    if (!src || !dest) return;
    memmove(dest, src, count);
}

/*
 * ots$move5 - Move `srclen` bytes from src to dest; if destlen exceeds
 * srclen, pad the remaining destination bytes with `fill`; if destlen
 * is smaller, copy only destlen bytes (truncate).
 */
void ots$move5(const uint32_t srclen, const void *src, const uint8_t fill,
               const uint32_t destlen, void *dest) {
    if (!dest) return;
    uint32_t copy = (srclen < destlen) ? srclen : destlen;
    if (src && copy) memmove(dest, src, copy);
    if (destlen > copy) {
        memset((char *)dest + copy, fill, destlen - copy);
    }
}

/*
 * ots$powjj - Raise a longword integer to a longword integer power.
 *
 * Uses exponentiation by squaring.  A negative exponent yields 0 for
 * |base| > 1 (integer result), 1 for base == 1, and -1/1 for base == -1
 * (matching the integer-truncation of x**(-n) for VMS J**J).
 */
int32_t ots$powjj(const int32_t base, const int32_t exponent) {
    if (exponent == 0) return 1;
    if (exponent < 0) {
        if (base == 1) return 1;
        if (base == -1) return (exponent & 1) ? -1 : 1;
        return 0;
    }
    int32_t result = 1;
    int32_t b = base;
    uint32_t e = (uint32_t)exponent;
    while (e) {
        if (e & 1u) result *= b;
        e >>= 1;
        if (e) b *= b;
    }
    return result;
}

/* ================================================================
 * Complex arithmetic routines (vms-801 R2.2 batch 4)
 *
 * OTS$xxxCx_R3 are the compiler-support routines for COMPLEX
 * arithmetic.  The "_r3" forms take the two operands' real and
 * imaginary parts as separate scalar arguments and return the
 * complex result in the floating-point return registers (double
 * complex).  The trailing type letter is the float format:
 *   g = VAX G_floating, t = IEEE T_floating (both 64-bit double).
 *
 * On this IEEE platform G_float and T_float are both computed with
 * native IEEE double precision, so the G and T entry points share
 * the same arithmetic (the distinction is the caller's declared
 * operand format, which is already native double here).
 *
 * Reference: OpenVMS RTL "OTS$ (Object Time System) Reference" -
 *            OTS$DIVC/OTS$MULC/OTS$POWCC/OTS$POWCJ descriptions.
 * ================================================================ */

/* (real1 + i*imag1) / (real2 + i*imag2) */
static double complex ots_divc_impl(double r1, double i1, double r2, double i2) {
    return (r1 + i1 * I) / (r2 + i2 * I);
}
/* (real1 + i*imag1) * (real2 + i*imag2) */
static double complex ots_mulc_impl(double r1, double i1, double r2, double i2) {
    return (r1 + i1 * I) * (r2 + i2 * I);
}
/* (base_real + i*base_imag) ** (exp_real + i*exp_imag) */
static double complex ots_powcc_impl(double br, double bi, double er, double ei) {
    return cpow(br + bi * I, er + ei * I);
}
/* (base_real + i*base_imag) ** j  (complex raised to an integer power) */
static double complex ots_powcj_impl(double br, double bi, int32_t j) {
    double complex base = br + bi * I;
    double complex result = 1.0 + 0.0 * I;
    uint32_t e = (j < 0) ? (uint32_t)(-(int64_t)j) : (uint32_t)j;
    double complex b = base;
    while (e) {
        if (e & 1u) result *= b;
        e >>= 1;
        if (e) b *= b;
    }
    if (j < 0)
        result = (1.0 + 0.0 * I) / result;
    return result;
}

double complex ots$divct_r3(double r1, double i1, double r2, double i2) {
    return ots_divc_impl(r1, i1, r2, i2);
}
double complex ots$divcg_r3(double r1, double i1, double r2, double i2) {
    return ots_divc_impl(r1, i1, r2, i2);
}
double complex ots$mulct_r3(double r1, double i1, double r2, double i2) {
    return ots_mulc_impl(r1, i1, r2, i2);
}
double complex ots$mulcg_r3(double r1, double i1, double r2, double i2) {
    return ots_mulc_impl(r1, i1, r2, i2);
}
double complex ots$powctct_r3(double br, double bi, double er, double ei) {
    return ots_powcc_impl(br, bi, er, ei);
}
double complex ots$powcgcg_r3(double br, double bi, double er, double ei) {
    return ots_powcc_impl(br, bi, er, ei);
}
double complex ots$powctj_r3(double br, double bi, int32_t j) {
    return ots_powcj_impl(br, bi, j);
}
double complex ots$powcgj_r3(double br, double bi, int32_t j) {
    return ots_powcj_impl(br, bi, j);
}

/* ================================================================
 * OTS$CNVOUT - Convert a floating value to text (vms-801 R2.2 batch 4)
 *
 * Produces the VMS OTS$CNVOUT normalized-scientific representation:
 *   [-]0.dddddddddE(+/-)dd
 * where there are `precision` significant digits after "0." and the
 * mantissa is normalized to the range [0.1, 1.0).  For example
 * value 2.71828182 with precision 9 yields "0.271828182E+01".
 *
 * The result is placed in the caller's fixed-length string descriptor
 * and dsc$w_length is set to the number of characters written; the
 * text is truncated to the descriptor's capacity if necessary.
 *
 * Reference: OpenVMS RTL "OTS$ (Object Time System) Reference" -
 *            OTS$CNVOUT.
 * ================================================================ */
static uint32_t ots_cnvout_impl(double value, struct dsc$descriptor_s *dest,
                                uint32_t precision) {
    if (!dest || !dest->dsc$a_pointer)
        return SS$_BADPARAM;
    if (precision < 1) precision = 1;
    if (precision > 34) precision = 34;   /* well beyond IEEE double range */

    char out[80];
    int neg = signbit(value) && !isnan(value);
    double v = fabs(value);
    size_t pos = 0;

    if (neg) out[pos++] = '-';

    if (v == 0.0 || isnan(v) || isinf(v)) {
        /* Zero (and non-finite) get a normalized zero mantissa / exp 0. */
        out[pos++] = '0';
        out[pos++] = '.';
        for (uint32_t i = 0; i < precision; i++) out[pos++] = '0';
        out[pos++] = 'E';
        out[pos++] = '+';
        out[pos++] = '0';
        out[pos++] = '0';
    } else {
        /* Render with one leading digit and precision-1 fraction digits,
         * then shift the decimal point left one place (d.fff -> 0.dfff)
         * which raises the printed exponent by one. */
        char sci[80];
        snprintf(sci, sizeof(sci), "%.*E", (int)(precision - 1), v);

        /* Collect the significant digits (skip the '.') up to 'E'. */
        char digits[64];
        size_t nd = 0;
        const char *p = sci;
        for (; *p && *p != 'E' && *p != 'e'; p++) {
            if (*p >= '0' && *p <= '9' && nd < sizeof(digits))
                digits[nd++] = *p;
        }
        int sci_exp = 0;
        if (*p == 'E' || *p == 'e')
            sci_exp = (int)strtol(p + 1, NULL, 10);
        int vms_exp = sci_exp + 1;   /* mantissa becomes 0.ddd... */

        out[pos++] = '0';
        out[pos++] = '.';
        for (size_t i = 0; i < nd; i++) out[pos++] = digits[i];

        out[pos++] = 'E';
        out[pos++] = (vms_exp < 0) ? '-' : '+';
        int ae = (vms_exp < 0) ? -vms_exp : vms_exp;
        char expbuf[8];
        int el = snprintf(expbuf, sizeof(expbuf), "%02d", ae);
        for (int i = 0; i < el; i++) out[pos++] = expbuf[i];
    }

    uint16_t cap = dest->dsc$w_length;
    uint16_t n = (pos <= cap) ? (uint16_t)pos : cap;
    memcpy(dest->dsc$a_pointer, out, n);
    dest->dsc$w_length = n;
    return SS$_NORMAL;
}

uint32_t ots$cnvout_t(const double *value, struct dsc$descriptor_s *dest,
                      const uint32_t precision) {
    if (!value) return SS$_BADPARAM;
    return ots_cnvout_impl(*value, dest, precision);
}
uint32_t ots$cnvout_g(const double *value, struct dsc$descriptor_s *dest,
                      const uint32_t precision) {
    if (!value) return SS$_BADPARAM;
    return ots_cnvout_impl(*value, dest, precision);
}
uint32_t ots$cnvout_d(const double *value, struct dsc$descriptor_s *dest,
                      const uint32_t precision) {
    if (!value) return SS$_BADPARAM;
    return ots_cnvout_impl(*value, dest, precision);
}
uint32_t ots$cnvout_f(const float *value, struct dsc$descriptor_s *dest,
                      const uint32_t precision) {
    if (!value) return SS$_BADPARAM;
    return ots_cnvout_impl((double)*value, dest, precision);
}

/* ================================================================
 * OTS$CVT_T_x - Convert text to a floating value (vms-801 R2.2 batch 4)
 *
 * Parses a numeric character string into a floating value.  VMS
 * accepts the usual "[-]ddd.ddd[E(+/-)dd]" form and also the form
 * where the exponent is introduced directly by its sign with no 'E'
 * (e.g. "0.12456789+3" == 0.12456789E+3 == 124.56789).
 *
 * Signature (per the OTS$ manual): the source descriptor, the output
 * address, then optional digits-in-fraction, scale-factor and flags
 * arguments.  Leading/trailing blanks are ignored.
 *
 * Reference: OpenVMS RTL "OTS$ (Object Time System) Reference" -
 *            OTS$CVT_T_x (text to F/D/G/H/S/T floating).
 * ================================================================ */
static uint32_t ots_cvt_t_impl(const struct dsc$descriptor_s *src, double *out,
                               int digits) {
    if (!src || !src->dsc$a_pointer || !out)
        return SS$_BADPARAM;

    /* Build a strtod-parseable copy: skip blanks, and insert 'E' before a
     * '+'/'-' that follows a digit or '.' and is not already after 'E'. */
    char buf[128];
    size_t bn = 0;
    int seen_digit = 0;
    int seen_point = 0;
    for (uint16_t i = 0; i < src->dsc$w_length && bn < sizeof(buf) - 2; i++) {
        char c = src->dsc$a_pointer[i];
        if (c == ' ' || c == '\t')
            continue;                        /* ignore embedded blanks */
        if ((c == '+' || c == '-') && seen_digit &&
            bn > 0 && buf[bn - 1] != 'E' && buf[bn - 1] != 'e') {
            buf[bn++] = 'E';                 /* implied exponent marker */
        }
        if (c >= '0' && c <= '9') seen_digit = 1;
        if (c == '.') seen_point = 1;
        buf[bn++] = c;
    }
    buf[bn] = '\0';
    if (bn == 0 || !seen_digit)
        return SS$_BADPARAM;

    char *end = NULL;
    errno = 0;
    double v = strtod(buf, &end);
    if (end == buf)
        return SS$_BADPARAM;

    /* If no explicit decimal point was present and a fractional-digit count
     * was supplied, apply the implied decimal point (VMS "digits" arg). */
    if (!seen_point && digits > 0)
        v /= pow(10.0, (double)digits);

    *out = v;
    return SS$_NORMAL;
}

uint32_t ots$cvt_t_t(const struct dsc$descriptor_s *src, double *dest, ...) {
    return ots_cvt_t_impl(src, dest, 0);
}
uint32_t ots$cvt_t_g(const struct dsc$descriptor_s *src, double *dest, ...) {
    return ots_cvt_t_impl(src, dest, 0);
}
uint32_t ots$cvt_t_d(const struct dsc$descriptor_s *src, double *dest, ...) {
    return ots_cvt_t_impl(src, dest, 0);
}
uint32_t ots$cvt_t_s(const struct dsc$descriptor_s *src, float *dest, ...) {
    double v = 0.0;
    uint32_t st = ots_cvt_t_impl(src, &v, 0);
    if (st == SS$_NORMAL && dest) *dest = (float)v;
    return st;
}
uint32_t ots$cvt_t_f(const struct dsc$descriptor_s *src, float *dest, ...) {
    double v = 0.0;
    uint32_t st = ots_cvt_t_impl(src, &v, 0);
    if (st == SS$_NORMAL && dest) *dest = (float)v;
    return st;
}
