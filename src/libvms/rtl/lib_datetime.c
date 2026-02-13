/*
 * lib_datetime.c - Date/Time and Conversion RTL Routines
 *
 * Implements VMS date/time library routines and number conversion
 * functions. Also includes the FAO (Formatted ASCII Output) routine
 * which handles VMS-style format directives.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

/* VMS epoch offset from Unix epoch in 100ns ticks */
#define VMS_EPOCH_OFFSET 0x007C95674BEB4000ULL

/* Days from November 17 1858 to January 1 1970 */
#define VMS_DAYS_TO_UNIX_EPOCH 40587

/*
 * lib$date_time - Get current date/time as a formatted ASCII string.
 *
 * Returns the current local time in VMS format:
 * "DD-MMM-YYYY HH:MM:SS.CC"
 */
uint32_t lib$date_time(struct dsc$descriptor_s *date_time_str) {
    if (!date_time_str || !date_time_str->dsc$a_pointer) return SS$_BADPARAM;

    time_t now = time(NULL);
    struct tm tm_result;
    localtime_r(&now, &tm_result);

    static const char *months[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };

    char buf[24];
    snprintf(buf, sizeof(buf), "%2d-%s-%04d %02d:%02d:%02d.00",
             tm_result.tm_mday, months[tm_result.tm_mon],
             tm_result.tm_year + 1900,
             tm_result.tm_hour, tm_result.tm_min, tm_result.tm_sec);

    uint16_t len = (uint16_t)strlen(buf);
    if (len > date_time_str->dsc$w_length) len = date_time_str->dsc$w_length;
    memcpy(date_time_str->dsc$a_pointer, buf, len);

    return SS$_NORMAL;
}

/*
 * lib$day - Return the day number since the VMS base date.
 *
 * Computes the number of days from November 17, 1858 to the
 * specified time (or the current time if time_value is NULL).
 * Optionally also returns the day-of-year (1-366).
 */
uint32_t lib$day(int32_t *day_number, const void *time_value,
                 int32_t *day_of_year) {
    if (!day_number) return SS$_BADPARAM;

    time_t now;
    if (time_value) {
        /* Convert VMS time to Unix time */
        uint64_t vmstime = *(const uint64_t *)time_value;
        vmstime -= VMS_EPOCH_OFFSET;
        now = (time_t)(vmstime / 10000000ULL);
    } else {
        now = time(NULL);
    }

    struct tm tm_result;
    localtime_r(&now, &tm_result);

    /* Days since Unix epoch + offset from VMS epoch to Unix epoch */
    *day_number = (int32_t)(now / 86400) + VMS_DAYS_TO_UNIX_EPOCH;

    if (day_of_year) {
        *day_of_year = tm_result.tm_yday + 1;
    }

    return SS$_NORMAL;
}

/*
 * lib$day_of_week - Return the day of the week.
 *
 * VMS convention: 1=Monday, 2=Tuesday, ..., 7=Sunday.
 * (C convention: 0=Sunday, 1=Monday, ..., 6=Saturday.)
 */
uint32_t lib$day_of_week(const void *time_value, int32_t *day) {
    if (!day) return SS$_BADPARAM;

    time_t now;
    if (time_value) {
        uint64_t vmstime = *(const uint64_t *)time_value;
        vmstime -= VMS_EPOCH_OFFSET;
        now = (time_t)(vmstime / 10000000ULL);
    } else {
        now = time(NULL);
    }

    struct tm tm_result;
    localtime_r(&now, &tm_result);

    /* Convert C weekday (0=Sun) to VMS weekday (1=Mon, 7=Sun) */
    *day = (tm_result.tm_wday == 0) ? 7 : tm_result.tm_wday;

    return SS$_NORMAL;
}

/*
 * lib$cvt_dtb - Convert decimal text to binary integer.
 *
 * Parses ndigits characters of decimal text and converts to int32_t.
 */
uint32_t lib$cvt_dtb(int32_t ndigits, const char *text, int32_t *value) {
    if (!text || !value || ndigits <= 0) return SS$_BADPARAM;

    int32_t result = 0;
    for (int32_t i = 0; i < ndigits; i++) {
        if (!isdigit((unsigned char)text[i])) return SS$_BADPARAM;
        result = result * 10 + (text[i] - '0');
    }
    *value = result;

    return SS$_NORMAL;
}

/*
 * lib$cvt_htb - Convert hexadecimal text to binary integer.
 */
uint32_t lib$cvt_htb(int32_t ndigits, const char *text, int32_t *value) {
    if (!text || !value || ndigits <= 0) return SS$_BADPARAM;

    int32_t result = 0;
    for (int32_t i = 0; i < ndigits; i++) {
        char c = (char)toupper((unsigned char)text[i]);
        if (c >= '0' && c <= '9') result = result * 16 + (c - '0');
        else if (c >= 'A' && c <= 'F') result = result * 16 + (c - 'A' + 10);
        else return SS$_BADPARAM;
    }
    *value = result;

    return SS$_NORMAL;
}

/*
 * lib$cvt_otb - Convert octal text to binary integer.
 */
uint32_t lib$cvt_otb(int32_t ndigits, const char *text, int32_t *value) {
    if (!text || !value || ndigits <= 0) return SS$_BADPARAM;

    int32_t result = 0;
    for (int32_t i = 0; i < ndigits; i++) {
        if (text[i] < '0' || text[i] > '7') return SS$_BADPARAM;
        result = result * 8 + (text[i] - '0');
    }
    *value = result;

    return SS$_NORMAL;
}

/*
 * lib$sys_fao - Formatted ASCII Output.
 *
 * Processes VMS FAO format directives in the control string and
 * produces formatted output. Supported directives:
 *
 *   !AS  - ASCII string from descriptor argument
 *   !AD  - ASCII descriptor (same as !AS)
 *   !AC  - ASCII counted string (first byte = length)
 *   !SL  - Signed longword (decimal)
 *   !UL  - Unsigned longword (decimal)
 *   !ZL  - Zero-filled longword (8 digits)
 *   !XL  - Hexadecimal longword (8 hex digits)
 *   !OL  - Octal longword
 *   !/   - Newline
 *   !_   - Tab
 *   !!   - Literal exclamation mark
 */
uint32_t lib$sys_fao(const struct dsc$descriptor_s *ctrl_str,
                     uint16_t *outlen,
                     struct dsc$descriptor_s *out_str, ...) {
    if (!ctrl_str || !out_str) return SS$_BADPARAM;
    if (!ctrl_str->dsc$a_pointer || !out_str->dsc$a_pointer) return SS$_BADPARAM;

    va_list args;
    va_start(args, out_str);

    char ctrl[512];
    dsc$strncpy(ctrl, ctrl_str, sizeof(ctrl));

    char output[4096];
    char *op = output;
    char *oend = output + sizeof(output) - 1;
    const char *cp = ctrl;

    while (*cp && op < oend) {
        if (*cp == '!') {
            cp++;
            if (!*cp) break;

            char directive = (char)toupper((unsigned char)*cp);
            cp++;

            switch (directive) {
                case 'A': {
                    /* !AS = ASCII string descriptor, !AD = same, !AC = counted */
                    char sub = *cp ? (char)toupper((unsigned char)*cp) : 0;
                    if (sub == 'S' || sub == 'D') {
                        cp++;
                        struct dsc$descriptor_s *arg =
                            va_arg(args, struct dsc$descriptor_s *);
                        if (arg && arg->dsc$a_pointer) {
                            int len = arg->dsc$w_length;
                            if (op + len > oend) len = (int)(oend - op);
                            memcpy(op, arg->dsc$a_pointer, len);
                            op += len;
                        }
                    } else if (sub == 'C') {
                        cp++;
                        const char *counted = va_arg(args, const char *);
                        if (counted) {
                            int len = (unsigned char)counted[0];
                            if (op + len > oend) len = (int)(oend - op);
                            memcpy(op, counted + 1, len);
                            op += len;
                        }
                    } else {
                        /* Treat bare !A as !AS */
                        struct dsc$descriptor_s *arg =
                            va_arg(args, struct dsc$descriptor_s *);
                        if (arg && arg->dsc$a_pointer) {
                            int len = arg->dsc$w_length;
                            if (op + len > oend) len = (int)(oend - op);
                            memcpy(op, arg->dsc$a_pointer, len);
                            op += len;
                        }
                    }
                    break;
                }

                case 'U': {
                    /* !UL = unsigned longword (decimal) */
                    if (*cp && (toupper((unsigned char)*cp) == 'L')) cp++;
                    uint32_t val = va_arg(args, uint32_t);
                    int n = snprintf(op, (size_t)(oend - op), "%u", val);
                    if (n > 0) op += n;
                    break;
                }

                case 'S': {
                    /* !SL = signed longword (decimal) */
                    if (*cp && (toupper((unsigned char)*cp) == 'L')) cp++;
                    int32_t val = va_arg(args, int32_t);
                    int n = snprintf(op, (size_t)(oend - op), "%d", val);
                    if (n > 0) op += n;
                    break;
                }

                case 'X': {
                    /* !XL = hexadecimal longword */
                    if (*cp && (toupper((unsigned char)*cp) == 'L')) cp++;
                    uint32_t val = va_arg(args, uint32_t);
                    int n = snprintf(op, (size_t)(oend - op), "%08X", val);
                    if (n > 0) op += n;
                    break;
                }

                case 'Z': {
                    /* !ZL = zero-filled longword */
                    if (*cp && (toupper((unsigned char)*cp) == 'L')) cp++;
                    uint32_t val = va_arg(args, uint32_t);
                    int n = snprintf(op, (size_t)(oend - op), "%08u", val);
                    if (n > 0) op += n;
                    break;
                }

                case 'O': {
                    /* !OL = octal longword */
                    if (*cp && (toupper((unsigned char)*cp) == 'L')) cp++;
                    uint32_t val = va_arg(args, uint32_t);
                    int n = snprintf(op, (size_t)(oend - op), "%o", val);
                    if (n > 0) op += n;
                    break;
                }

                case '/':
                    /* !/ = newline */
                    *op++ = '\n';
                    break;

                case '_':
                    /* !_ = tab */
                    *op++ = '\t';
                    break;

                case '!':
                    /* !! = literal exclamation mark */
                    *op++ = '!';
                    break;

                default:
                    /* Unknown directive - output as-is */
                    *op++ = '!';
                    if (op < oend) *op++ = directive;
                    break;
            }
        } else {
            *op++ = *cp++;
        }
    }

    va_end(args);

    uint16_t len = (uint16_t)(op - output);
    if (len > out_str->dsc$w_length) len = out_str->dsc$w_length;
    memcpy(out_str->dsc$a_pointer, output, len);
    if (outlen) *outlen = len;

    return SS$_NORMAL;
}

/*
 * lib$cvt_from_internal_time - Convert VMS internal time to various formats.
 * Stub implementation.
 */
uint32_t lib$cvt_from_internal_time(const uint32_t *operation,
                                     uint32_t *result, const void *time_val) {
    (void)operation; (void)result; (void)time_val;
    return SS$_NORMAL;
}
