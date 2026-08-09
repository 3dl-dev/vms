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
#include "ssdef.h"
#include "descrip.h"

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
