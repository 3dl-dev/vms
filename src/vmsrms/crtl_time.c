/*
 * crtl_time.c - the C RTL's clock and time zone are the VMS system's (vms-593).
 *
 * On OpenVMS a C program's time(), gettimeofday() and clock_gettime() report
 * the system time the executive keeps, and localtime()/mktime()/strftime()/
 * ctime() convert with the system's time-zone description, the logical names
 *
 *   SYS$TIMEZONE_DIFFERENTIAL  the time differential factor: seconds EAST of
 *                              UTC, the current local offset (daylight time
 *                              included when it is in effect)
 *   SYS$TIMEZONE_NAME          the local zone's abbreviation
 *   SYS$TIMEZONE_RULE          the standard/daylight changeover rule, a POSIX
 *                              zone string ("EST5EDT,M3.2.0,M11.1.0")
 *
 * (VSI OpenVMS System Manager's Manual, Vol. 1, "Managing the System Time").
 * A program never reads the substrate kernel's clock or a TZ variable or a
 * zoneinfo file. OVMX's DECC$SHR is musl; musl's time family funnels through
 * two hooks this file installs (tools/cross-alpha-vms/musl-arch/src/time/
 * alpha-dec-vms/{clock_gettime,__tz}.c):
 *
 *   __ovmx_clock_hook  the realtime clock: SYS$GETUTC's UTC time (the same
 *                      clock SYS$GETTIM reports: the executive's system time, rd vms-642)
 *   __ovmx_tz_hook     the zone: the three logicals above, through SYS$TRNLNM
 *                      on LNM$FILE_DEV, as SYS$GETUTC/$TIMCON read the TDF
 *
 * THE DIFFERENTIAL GOVERNS. The TDF is the one number every OVMX time service
 * converts with ($GETUTC/$TIMCON/$ASCUTC, lib_datetime.c), so the C RTL uses it
 * too: local time is UTC + TDF. SYS$TIMEZONE_RULE adds the daylight changeover
 * (and so the right offset for OTHER dates) only when it agrees with the TDF --
 * its standard or its daylight offset IS the TDF; a rule that disagrees is
 * stale against the TDF and is not used. With no TDF logical the rule alone
 * describes the zone, and with neither the zone is UTC.
 *
 * decc$main installs the hooks for a DECC$SHR image (crtl_rms_fd.c's
 * ovmx_crtl_fd_install); a static OVMX image's start-up calls the same.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* A class-S string descriptor and an item_list_3 entry, laid out as
 * src/libvms/include/descrip.h and lnmdef.h declare them. */
struct ct_dsc { unsigned short len; unsigned char dtype, cls; const char *ptr; };
struct ct_ile3 { uint16_t buflen, code; void *buf; uint16_t *retlen; };

extern unsigned int sys$getutc(void *utcadr);
extern unsigned int sys$trnlnm(const unsigned int *attr, const struct ct_dsc *tabnam,
                               const struct ct_dsc *lognam, const unsigned char *acmode,
                               const struct ct_ile3 *itmlst);

extern int (*__ovmx_clock_hook)(clockid_t, struct timespec *);
extern size_t (*__ovmx_tz_hook)(char *, size_t);

/* $UTCDEF's UTC$Q_ABSTIME counts 100 ns from 15-OCT-1582; the UNIX epoch is
 * 100840 + 40587 days later (1582 -> 1858 -> 1970). */
#define CT_UTC_TO_UNIX_100NS (0x0135886AC7960000ULL + 0x007C95674BEB4000ULL)

static int ct_clock(clockid_t clk, struct timespec *ts)
{
    unsigned char u[16];
    uint64_t t = 0;
    (void)clk;
    if (!(sys$getutc(u) & 1u))
        return -EIO;
    for (int i = 0; i < 8; i++)
        t |= (uint64_t)u[i] << (8 * i);
    if (t < CT_UTC_TO_UNIX_100NS)
        return -EOVERFLOW;
    t -= CT_UTC_TO_UNIX_100NS;
    ts->tv_sec = (time_t)(t / 10000000ULL);
    ts->tv_nsec = (long)((t % 10000000ULL) * 100u);
    return 0;
}

/* One logical through LNM$FILE_DEV; 0 when it is not defined. */
static size_t ct_logical(const char *name, char *val, size_t cap)
{
    static const struct ct_dsc tab = { 12, 14 /* DSC$K_DTYPE_T */, 1 /* DSC$K_CLASS_S */, "LNM$FILE_DEV" };
    struct ct_dsc nam = { (unsigned short)strlen(name), 14, 1, name };
    uint16_t len = 0;
    struct ct_ile3 il[2];
    il[0].buflen = (uint16_t)(cap - 1); il[0].code = 2 /* LNM$_STRING */;
    il[0].buf = val; il[0].retlen = &len;
    il[1].buflen = 0; il[1].code = 0; il[1].buf = NULL; il[1].retlen = NULL;
    if (!(sys$trnlnm(NULL, &tab, &nam, NULL, il) & 1u) || len == 0 || len >= cap)
        return 0;
    val[len] = 0;
    return len;
}

/* The POSIX zone grammar's name and offset (offset = seconds WEST of UTC). */
static int ct_name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        while (*s && *s != '>') s++;
        if (*s != '>') return 0;
        *p = s + 1;
        return 1;
    }
    while ((unsigned)((*s | 32) - 'a') < 26u) s++;
    if (s - *p < 3) return 0;
    *p = s;
    return 1;
}

static int ct_off(const char **p, long *off)
{
    const char *s = *p;
    int neg = 0;
    long v = 0, part;
    if (*s == '+' || *s == '-') neg = (*s++ == '-');
    if ((unsigned)(*s - '0') > 9u) return 0;
    for (int f = 0; f < 3; f++) {
        if ((unsigned)(*s - '0') > 9u) return 0;
        for (part = 0; (unsigned)(*s - '0') <= 9u; s++)
            part = part * 10 + (*s - '0');
        v += part * (f == 0 ? 3600 : f == 1 ? 60 : 1);
        if (*s != ':') break;
        s++;
    }
    *off = neg ? -v : v;
    *p = s;
    return 1;
}

/* Does the rule's standard or daylight offset equal the TDF (seconds east)? */
static int ct_rule_agrees(const char *rule, long tdf)
{
    const char *p = rule;
    long std, dst;
    if (!ct_name(&p) || !ct_off(&p, &std)) return 0;
    if (-std == tdf) return 1;
    if (!*p || *p == ',') return 0;              /* no daylight time */
    if (!ct_name(&p)) return 0;
    dst = std - 3600;                            /* POSIX default: one hour ahead */
    if (*p && *p != ',' && !ct_off(&p, &dst)) return 0;
    return -dst == tdf;
}

static int ct_digits(const char *s)
{
    if (*s == '+' || *s == '-') s++;
    if (!*s) return 0;
    for (; *s; s++)
        if ((unsigned)(*s - '0') > 9u) return 0;
    return 1;
}

static size_t ct_zone(char *buf, size_t cap)
{
    char tdfs[32], name[32], rule[96], *o = buf;
    size_t ntdf = ct_logical("SYS$TIMEZONE_DIFFERENTIAL", tdfs, sizeof tdfs);
    size_t nrule = ct_logical("SYS$TIMEZONE_RULE", rule, sizeof rule);
    long tdf = 0, w;

    if (ntdf && !ct_digits(tdfs))
        ntdf = 0;                                /* not a differential */
    if (ntdf) {
        for (const char *s = tdfs + (*tdfs == '+' || *tdfs == '-'); *s; s++)
            tdf = tdf * 10 + (*s - '0');
        if (*tdfs == '-') tdf = -tdf;
        if (tdf < -86400 || tdf > 86400) ntdf = 0;
    }
    if (nrule && (!ntdf || ct_rule_agrees(rule, tdf))) {
        if (nrule >= cap) return 0;
        memcpy(buf, rule, nrule + 1);
        return nrule;
    }
    if (!ntdf)
        return 0;                                /* no VMS zone: UTC */

    /* The fixed differential: "<NAME>[+-]hh:mm:ss" (POSIX: offset WEST). */
    size_t nn = ct_logical("SYS$TIMEZONE_NAME", name, sizeof name), k = 0;
    if (cap < 32) return 0;
    *o++ = '<';
    for (size_t i = 0; i < nn && k < 6; i++) {
        char c = name[i];
        if ((unsigned)(((unsigned char)c | 32) - 'a') < 26u || (unsigned)(c - '0') <= 9u || c == '+' || c == '-')
            *o++ = c, k++;
    }
    if (k < 3) {                                 /* no usable name: "+hhmm", as zoneinfo does */
        long a = tdf < 0 ? -tdf : tdf;
        o = buf + 1;
        *o++ = tdf < 0 ? '-' : '+';
        *o++ = (char)('0' + a / 36000); *o++ = (char)('0' + a / 3600 % 10);
        *o++ = (char)('0' + a / 600 % 6); *o++ = (char)('0' + a / 60 % 10);
    }
    *o++ = '>';
    w = -tdf;
    *o++ = w < 0 ? '-' : '+';
    if (w < 0) w = -w;
    *o++ = (char)('0' + w / 36000); *o++ = (char)('0' + w / 3600 % 10); *o++ = ':';
    *o++ = (char)('0' + w / 600 % 6); *o++ = (char)('0' + w / 60 % 10); *o++ = ':';
    *o++ = (char)('0' + w % 60 / 10); *o++ = (char)('0' + w % 10);
    *o = 0;
    return (size_t)(o - buf);
}

/* Turn the VMS clock and zone on for this image. */
void ovmx_crtl_time_install(void)
{
    __ovmx_clock_hook = ct_clock;
    __ovmx_tz_hook = ct_zone;
}
