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
#include "libdef.h"
#include "gen64def.h"

/* Forward declaration for sys$faol from sys_fao.c */
extern uint32_t sys$faol(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    const uint64_t *prmlst);

/* Forward declaration for sys$asctim from sys_time.c */
extern uint32_t sys$asctim(
    uint16_t *timlen,
    struct dsc$descriptor_s *timbuf,
    const uint64_t *timadr,
    uint32_t cvtflg);

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
 * lib$sys_fao - Formatted ASCII output (wrapper)
 *
 * Thin wrapper around sys$fao for LIB$ compatibility.
 * See sys$fao in sys_fao.c for full implementation.
 */
/* Forward declaration for directive counter in sys_fao.c */
extern int sys$fao_count_args(const char *ctrl, uint16_t len);

uint32_t lib$sys_fao(const struct dsc$descriptor_s *ctrl_str,
                     uint16_t *outlen,
                     struct dsc$descriptor_s *out_str, ...) {
    if (!ctrl_str || !ctrl_str->dsc$a_pointer) return SS$_BADPARAM;

    /* Count actual directive arguments needed by the control string */
    int needed = sys$fao_count_args(ctrl_str->dsc$a_pointer, ctrl_str->dsc$w_length);
    if (needed > 256) needed = 256;

    /* Build argument array from varargs — only read what's needed */
    uint64_t arglist[256];
    va_list args;
    va_start(args, out_str);
    for (int i = 0; i < needed; i++) {
        arglist[i] = va_arg(args, uint64_t);
    }
    va_end(args);

    /* Call sys$faol with the argument array */
    return sys$faol(ctrl_str, outlen, out_str, arglist);
}

/*
 * lib$sys_faol - Formatted ASCII output with argument list (wrapper)
 *
 * Thin wrapper around sys$faol for LIB$ compatibility.
 * See sys$faol in sys_fao.c for full implementation.
 */
uint32_t lib$sys_faol(const struct dsc$descriptor_s *ctrl_str,
                      uint16_t *outlen,
                      struct dsc$descriptor_s *out_str,
                      const uint32_t *prmlst) {
    if (!ctrl_str || !ctrl_str->dsc$a_pointer) return SS$_BADPARAM;

    /* sys$faol expects a uint64_t array. Build one from the uint32_t parameter
     * list to avoid strict-aliasing violations from type-punning. */
    int needed = sys$fao_count_args(ctrl_str->dsc$a_pointer, ctrl_str->dsc$w_length);
    if (needed > 256) needed = 256;

    uint64_t args[256];
    if (prmlst) {
        for (int i = 0; i < needed; i++) {
            args[i] = (uint64_t)prmlst[i];
        }
    }

    return sys$faol(ctrl_str, outlen, out_str, prmlst ? args : NULL);
}

/*
 * VMS internal time representation used by the routines below:
 *   - An ABSOLUTE time is a POSITIVE 64-bit count of 100ns intervals
 *     since 17-Nov-1858.
 *   - A DELTA time is a NEGATIVE 64-bit value whose magnitude is the
 *     interval in 100ns intervals.  (This matches the convention
 *     sys$setimr already uses: negative == delta, and sys$bintim now
 *     produces a negative quadword for a delta-format string.)
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$ADD_TIMES,
 * LIB$SUB_TIMES; OpenVMS Programming Concepts (system time format).
 */

/*
 * lib$add_times - Add two quadword times.
 *
 * Documented combinations (at least one operand must be a delta time):
 *   absolute + delta -> absolute
 *   delta    + delta -> delta
 * Adding two absolute times is invalid.
 */
uint32_t lib$add_times(const void *time1, const void *time2, void *result) {
    if (!time1 || !time2 || !result) return SS$_BADPARAM;

    int64_t a = (int64_t)*(const uint64_t *)time1;
    int64_t b = (int64_t)*(const uint64_t *)time2;
    int a_delta = (a < 0), b_delta = (b < 0);

    int64_t r;
    if (!a_delta && !b_delta) {
        return LIB$_INVARG;             /* absolute + absolute is invalid */
    } else if (a_delta && b_delta) {
        r = a + b;                      /* delta + delta -> (longer) delta */
    } else {
        int64_t abs_t = a_delta ? b : a;   /* the absolute operand */
        int64_t delt  = a_delta ? a : b;   /* the delta operand (negative) */
        r = abs_t - delt;               /* absolute + |delta| -> later abs */
    }

    *(uint64_t *)result = (uint64_t)r;
    return SS$_NORMAL;
}

/*
 * lib$sub_times - Subtract time2 from time1.
 *
 * Documented combinations:
 *   absolute - absolute -> delta
 *   absolute - delta    -> absolute
 *   delta    - delta    -> delta
 * Returns LIB$_NEGTIM if the result would be a negative absolute time
 * (i.e. time1 < time2 for the absolute-minus-absolute case).
 */
uint32_t lib$sub_times(const void *time1, const void *time2, void *result) {
    if (!time1 || !time2 || !result) return SS$_BADPARAM;

    int64_t a = (int64_t)*(const uint64_t *)time1;
    int64_t b = (int64_t)*(const uint64_t *)time2;
    int a_delta = (a < 0), b_delta = (b < 0);

    int64_t r;
    if (!a_delta && !b_delta) {
        /* absolute - absolute -> delta (stored negative) */
        int64_t diff = a - b;
        if (diff < 0) return LIB$_NEGTIM;
        r = -diff;
    } else if (!a_delta && b_delta) {
        /* absolute - delta -> earlier absolute: abs - |delta| = a + b */
        r = a + b;
    } else if (a_delta && b_delta) {
        /* delta - delta -> delta */
        r = a - b;
    } else {
        /* delta - absolute is invalid */
        return LIB$_INVARG;
    }

    *(uint64_t *)result = (uint64_t)r;
    return SS$_NORMAL;
}

/*
 * lib$cvt_vectim - Convert a 7-word numeric time vector to VMS binary time.
 *
 * Input vector (7 words):
 *   [0] year (e.g. 2003)   [1] month (1-12)  [2] day (1-31)
 *   [3] hour (0-23)        [4] minute (0-59) [5] second (0-59)
 *   [6] hundredths (0-99)
 *
 * Produces an absolute VMS quadword time.
 */
uint32_t lib$cvt_vectim(const uint16_t timvec[7], void *resultant_time) {
    if (!timvec || !resultant_time) return SS$_BADPARAM;

    int year = timvec[0], month = timvec[1], day = timvec[2];
    int hour = timvec[3], min = timvec[4], sec = timvec[5], hun = timvec[6];

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59 || hun > 99) {
        return LIB$_INVARG;
    }

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = year - 1900;
    tm_val.tm_mon  = month - 1;
    tm_val.tm_mday = day;
    tm_val.tm_hour = hour;
    tm_val.tm_min  = min;
    tm_val.tm_sec  = sec;

    time_t t = timegm(&tm_val);
    if (t == (time_t)-1) return LIB$_INVARG;

    uint64_t vmstime = (uint64_t)t * 10000000ULL
                     + (uint64_t)hun * 100000ULL
                     + VMS_EPOCH_OFFSET;
    *(uint64_t *)resultant_time = vmstime;

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

/*
 * lib$sys_asctim - Convert binary time to ASCII string (RTL entry point).
 *
 * See the doc comment in lib$routines.h: a thin wrapper around
 * sys$asctim (starlet.h/sys_time.c), declared with a GENERIC_64* time
 * argument to match corpus call sites (tests/corpus/tier1-examples/
 * lib_sys_asctim.c and friends) that pass "&binary_time" where
 * binary_time is GENERIC_64. GENERIC_64's gen64$q_quadword member is
 * the same uint64_t quadword sys$asctim's timadr already expects, so
 * this just forwards the address through.
 */
uint32_t lib$sys_asctim(uint16_t *timlen, struct dsc$descriptor_s *timbuf,
                        const struct _generic_64 *timadr, uint32_t cvtflg) {
    const uint64_t *raw_timadr = timadr ? &timadr->gen64$q_quadword : NULL;

    return sys$asctim(timlen, timbuf, raw_timadr, cvtflg);
}
