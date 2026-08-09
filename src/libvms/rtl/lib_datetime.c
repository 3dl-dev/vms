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
    /*
     * A delta time is a negative quadword (the OVMX negative-is-delta
     * convention). sys$asctim only formats absolute times, so format the
     * delta here as "dddd hh:mm:ss.cc" — the VMS delta-time text form.
     * This is what makes LIB$STAT_TIMER's elapsed-time quadword print.
     */
    if (timbuf && timbuf->dsc$a_pointer && timadr &&
        (int64_t)timadr->gen64$q_quadword < 0) {
        uint64_t ticks = (uint64_t)(-(int64_t)timadr->gen64$q_quadword);
        uint64_t hun = ticks / 100000ULL;                 /* hundredths */
        unsigned cc = (unsigned)(hun % 100); hun /= 100;
        unsigned ss = (unsigned)(hun % 60);  hun /= 60;
        unsigned mm = (unsigned)(hun % 60);  hun /= 60;
        unsigned hh = (unsigned)(hun % 24);  hun /= 24;
        unsigned dd = (unsigned)hun;

        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%u %02u:%02u:%02u.%02u",
                           dd, hh, mm, ss, cc);
        uint16_t copylen = (uint16_t)len;
        if (copylen > timbuf->dsc$w_length) copylen = timbuf->dsc$w_length;
        memcpy(timbuf->dsc$a_pointer, buf, copylen);
        if (timlen) *timlen = copylen;
        return SS$_NORMAL;
    }

    const uint64_t *raw_timadr = timadr ? &timadr->gen64$q_quadword : NULL;

    return sys$asctim(timlen, timbuf, raw_timadr, cvtflg);
}

/* ================================================================
 * Locale-independent date/time formatting (LIB$DT facility)
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$CONVERT_DATE_STRING,
 * LIB$GET_MAXIMUM_DATE_LENGTH, LIB$FORMAT_DATE_TIME, LIB$GET_DATE_FORMAT.
 * ================================================================ */

/* Forward decls for the logical-name translation used by get_date_format. */
struct item_list_3;
extern uint32_t sys$trnlnm(const uint32_t *attr,
                           const struct dsc$descriptor_s *tabnam,
                           const struct dsc$descriptor_s *lognam,
                           const uint8_t *acmode,
                           const struct item_list_3 *itmlst);

/* Item-list entry (matches lnmdef.h struct item_list_3). */
struct dt_item_list_3 {
    uint16_t  buflen;
    uint16_t  item_code;
    void     *bufaddr;
    uint16_t *retlen;
};
#define DT_LNM_STRING 2   /* LNM$_STRING */

static const char *const dt_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/*
 * lib$convert_date_string - Convert an absolute date/time string (or one
 * of the relative keywords TODAY / TOMORROW / YESTERDAY / NOW) to a VMS
 * absolute binary quadword time.
 *
 * The optional defaulted-date, flags, defaulted-fields and context
 * arguments are accepted for source compatibility.
 */
uint32_t lib$convert_date_string(const struct dsc$descriptor_s *input,
                                 uint64_t *out_time, ...) {
    if (!input || !input->dsc$a_pointer || !out_time)
        return LIB$_INVARG;

    /* Uppercase-copy and trim the input into a local buffer. */
    char str[128];
    uint16_t n = input->dsc$w_length;
    if (n > sizeof(str) - 1) n = sizeof(str) - 1;
    uint16_t j = 0;
    for (uint16_t i = 0; i < n; i++) {
        char c = input->dsc$a_pointer[i];
        str[j++] = (char)toupper((unsigned char)c);
    }
    str[j] = '\0';
    /* Trim leading/trailing spaces. */
    char *s = str;
    while (*s == ' ' || *s == '\t') s++;
    size_t sl = strlen(s);
    while (sl > 0 && (s[sl - 1] == ' ' || s[sl - 1] == '\t')) s[--sl] = '\0';

    time_t base = time(NULL);
    struct tm tmv;
    localtime_r(&base, &tmv);

    if (strcmp(s, "NOW") == 0) {
        uint64_t vt = (uint64_t)base * 10000000ULL + VMS_EPOCH_OFFSET;
        *out_time = vt;
        return SS$_NORMAL;
    }

    int rel_days = 0;
    int relative = 1;
    if (strcmp(s, "TODAY") == 0)          rel_days = 0;
    else if (strcmp(s, "TOMORROW") == 0)  rel_days = 1;
    else if (strcmp(s, "YESTERDAY") == 0) rel_days = -1;
    else relative = 0;

    if (relative) {
        /* Midnight of the current day, shifted by rel_days. */
        struct tm midnight = tmv;
        midnight.tm_hour = 0;
        midnight.tm_min = 0;
        midnight.tm_sec = 0;
        midnight.tm_mday += rel_days;
        midnight.tm_isdst = -1;
        time_t t = mktime(&midnight);
        if (t == (time_t)-1) return LIB$_INVARG;
        *out_time = (uint64_t)t * 10000000ULL + VMS_EPOCH_OFFSET;
        return SS$_NORMAL;
    }

    /* Absolute form: "dd-MMM-yyyy[ hh:mm:ss[.cc]]". */
    int day = 0, year = 0, hour = 0, minute = 0, sec = 0, hun = 0;
    char mon[4] = {0};
    int matched = sscanf(s, "%d-%3[A-Z]-%d %d:%d:%d.%d",
                         &day, mon, &year, &hour, &minute, &sec, &hun);
    if (matched < 3)
        return LIB$_INVARG;

    int month = -1;
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, dt_months[i], 3) == 0) { month = i; break; }
    }
    if (month < 0)
        return LIB$_INVARG;

    struct tm at;
    memset(&at, 0, sizeof(at));
    at.tm_year = year - 1900;
    at.tm_mon  = month;
    at.tm_mday = day;
    at.tm_hour = hour;
    at.tm_min  = minute;
    at.tm_sec  = sec;
    time_t t = timegm(&at);
    if (t == (time_t)-1) return LIB$_INVARG;

    *out_time = (uint64_t)t * 10000000ULL
              + (uint64_t)hun * 100000ULL
              + VMS_EPOCH_OFFSET;
    return SS$_NORMAL;
}

/*
 * lib$get_maximum_date_length - Return the maximum length, in bytes, of a
 * formatted output date/time string for the requested fields.
 *
 * The OVMX standard output format is "dd-MMM-yyyy hh:mm:ss.cc"; we report
 * a length that safely bounds any date+time field combination.
 */
uint32_t lib$get_maximum_date_length(int32_t *length, void *context,
                                     const uint32_t *flags) {
    (void)context;
    if (!length)
        return LIB$_INVARG;

    uint32_t f = flags ? *flags : (LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS);
    int len = 0;
    if (f & LIB$M_DATE_FIELDS) len += 11;   /* dd-MMM-yyyy */
    if ((f & LIB$M_DATE_FIELDS) && (f & LIB$M_TIME_FIELDS)) len += 1; /* space */
    if (f & LIB$M_TIME_FIELDS) len += 11;   /* hh:mm:ss.cc */
    if (len == 0) len = 23;                 /* both, by default */

    *length = len;
    return SS$_NORMAL;
}

/*
 * lib$format_date_time - Format an input (or the current) binary time as a
 * date/time string, honoring the date/time field flags.
 */
uint32_t lib$format_date_time(struct dsc$descriptor_s *out,
                              const void *in_time, void *context,
                              uint16_t *out_len, const uint32_t *flags) {
    (void)context;
    if (!out || !out->dsc$a_pointer)
        return LIB$_INVARG;

    time_t t;
    long hundredths = 0;
    if (in_time) {
        uint64_t vt = *(const uint64_t *)in_time;
        if (vt < VMS_EPOCH_OFFSET) return LIB$_INVARG;
        vt -= VMS_EPOCH_OFFSET;
        t = (time_t)(vt / 10000000ULL);
        hundredths = (long)((vt % 10000000ULL) / 100000ULL);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        t = ts.tv_sec;
        hundredths = ts.tv_nsec / 10000000L;
    }

    struct tm tmv;
    localtime_r(&t, &tmv);

    uint32_t f = flags ? *flags : (LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS);
    if ((f & (LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS)) == 0)
        f = LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS;

    char buf[64];
    int len = 0;
    if (f & LIB$M_DATE_FIELDS) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                        "%02d-%s-%04d",
                        tmv.tm_mday, dt_months[tmv.tm_mon],
                        tmv.tm_year + 1900);
    }
    if ((f & LIB$M_DATE_FIELDS) && (f & LIB$M_TIME_FIELDS)) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, " ");
    }
    if (f & LIB$M_TIME_FIELDS) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len,
                        "%02d:%02d:%02d.%02ld",
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, hundredths);
    }

    uint16_t copylen = (uint16_t)len;
    if (copylen > out->dsc$w_length) copylen = out->dsc$w_length;
    memcpy(out->dsc$a_pointer, buf, copylen);
    if (out->dsc$b_class == DSC$K_CLASS_S && copylen < out->dsc$w_length) {
        memset(out->dsc$a_pointer + copylen, ' ',
               out->dsc$w_length - copylen);
    }
    if (out_len) *out_len = copylen;

    return SS$_NORMAL;
}

/*
 * lib$get_date_format - Return the current output date format string.
 *
 * The format is taken from the logical name LIB$DT_FORMAT. When that
 * logical is not defined (as is the case in the default OVMX environment)
 * the routine returns LIB$_DEFFORUSE — "default format used" — exactly as
 * documented, leaving the caller's descriptor empty.
 */
uint32_t lib$get_date_format(struct dsc$descriptor_s *out,
                             void *context) {
    (void)context;
    if (!out)
        return LIB$_INVARG;

    char value[256];
    uint16_t retlen = 0;
    struct dt_item_list_3 itm[2];
    memset(itm, 0, sizeof(itm));
    itm[0].buflen = sizeof(value);
    itm[0].item_code = DT_LNM_STRING;
    itm[0].bufaddr = value;
    itm[0].retlen = &retlen;

    struct dsc$descriptor_s lognam = {
        13, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LIB$DT_FORMAT"
    };

    uint32_t st = sys$trnlnm(NULL, NULL, &lognam, NULL,
                             (const struct item_list_3 *)itm);
    if (st != SS$_NORMAL || retlen == 0) {
        /* Logical undefined — the default format is used. */
        return LIB$_DEFFORUSE;
    }

    /* Return the translated format string into the caller's descriptor. */
    (void)lib$scopy_r_dx(&retlen, value, out);
    return SS$_NORMAL;
}
