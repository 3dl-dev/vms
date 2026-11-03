/*
 * test_syssvc_crtl_time.c - the C RTL's clock and time zone are the VMS
 * system's (vms-593).
 *
 * On OpenVMS a C program's time()/gettimeofday()/clock_gettime() report the
 * system time and localtime()/mktime()/strftime()/ctime() convert with the
 * SYS$TIMEZONE_DIFFERENTIAL / _NAME / _RULE logicals -- never a TZ variable or
 * a zoneinfo file. OVMX's C RTL gets both from src/vmsrms/crtl_time.c through
 * the musl time hooks (tools/cross-alpha-vms/musl-arch/src/time/alpha-dec-vms/,
 * the same overlays this rig's musl is built with, src/crtl-musl-x86_64).
 *
 * TZ is set to "XXX-7" throughout, a zone musl itself would honour (UTC+7);
 * every expected value below differs from it, so a C RTL still reading TZ
 * fails. The zone logicals are defined in LNM$PROCESS_TABLE and changed
 * between conversions: the next localtime() must follow each DEFINE.
 *
 * Registered only where the layer is linked (OVMX_CRTL_FD_STATIC; tests/qemu/
 * CMakeLists.txt). No /dev/vms: SKIP.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

#if defined(OVMX_CRTLFD_STATIC)
void ovmx_crtl_fd_install(void);

static const struct dsc$descriptor_s ptab =
    { 17, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE" };

static int deflog(const char *name, const char *val)
{
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr = (void *)val;
    return (sys$crelnm(NULL, &ptab, &nd, NULL, il) & 1) != 0;
}

static void dellog(const char *name)
{
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    (void)sys$dellnm(&ptab, &nd, NULL);
}

/* The UTC seconds $GETUTC reports now. */
static long long getutc_secs(void)
{
    unsigned char u[16];
    uint64_t t = 0;
    if (!(sys$getutc(u) & 1)) return -1;
    for (int i = 0; i < 8; i++) t |= (uint64_t)u[i] << (8 * i);
    return (long long)((t - 0x0135886AC7960000ULL - 0x007C95674BEB4000ULL) / 10000000ULL);
}

static int near(long long a, long long b) { return a - b <= 2 && b - a <= 2; }

/* localtime of t is UTC + off seconds, under zone name zn, with isdst. */
static int local_is(time_t t, long off, const char *zn, int isdst)
{
    struct tm lt, ut;
    time_t s = t + off;
    if (!localtime_r(&t, &lt) || !gmtime_r(&s, &ut)) return 0;
    if (lt.tm_gmtoff != off || lt.tm_isdst != isdst || strcmp(lt.tm_zone, zn) != 0) {
        printf("    localtime: %04d-%02d-%02d %02d:%02d gmtoff=%ld isdst=%d zone=%s\n",
               lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
               (long)lt.tm_gmtoff, lt.tm_isdst, lt.tm_zone);
        return 0;
    }
    return lt.tm_year == ut.tm_year && lt.tm_yday == ut.tm_yday &&
           lt.tm_hour == ut.tm_hour && lt.tm_min == ut.tm_min && lt.tm_sec == ut.tm_sec;
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_crtl_time: the C RTL's clock and zone are $GETUTC and SYS$TIMEZONE_* ===\n");
#if !defined(OVMX_CRTLFD_STATIC)
#error "test_syssvc_crtl_time is registered only where the C RTL layer is linked (OVMX_CRTL_FD_STATIC, tests/qemu/CMakeLists.txt)"
#else
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || vms_kif_open() < 0) {
        printf("=== test_syssvc_crtl_time: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    setenv("TZ", "XXX-7", 1);
    ovmx_crtl_fd_install();

    /* The clock: time(), gettimeofday(), clock_gettime() are $GETUTC's. */
    {
        struct timeval tv;
        struct timespec ts;
        long long g = getutc_secs();
        time_t t = time(NULL);
        check(g > 0 && near((long long)t, g), "time() is the UTC time $GETUTC reports");
        check(gettimeofday(&tv, NULL) == 0 && near((long long)tv.tv_sec, g),
              "gettimeofday() is the same clock");
        check(clock_gettime(CLOCK_REALTIME, &ts) == 0 && near((long long)ts.tv_sec, g),
              "clock_gettime(CLOCK_REALTIME) is the same clock");
    }

    time_t now = time(NULL);
    /* Fixed instants: 2026-07-01 12:00:00 and 2026-01-15 12:00:00 UTC. */
    const time_t jul = 1782907200, jan = 1768478400;
    struct tm lt;
    char buf[64];

    dellog("SYS$TIMEZONE_RULE");
    check(deflog("SYS$TIMEZONE_DIFFERENTIAL", "0") && deflog("SYS$TIMEZONE_NAME", "UTC"),
          "DEFINE SYS$TIMEZONE_DIFFERENTIAL 0, SYS$TIMEZONE_NAME UTC");
    check(local_is(now, 0, "UTC", 0), "TDF 0: localtime() is UTC, not the TZ variable's UTC+7");

    check(deflog("SYS$TIMEZONE_DIFFERENTIAL", "19800") && deflog("SYS$TIMEZONE_NAME", "IST"),
          "DEFINE SYS$TIMEZONE_DIFFERENTIAL 19800, SYS$TIMEZONE_NAME IST");
    /* negctl: crtl-time-falls-to-linux */
    check(local_is(now, 19800, "IST", 0), "TDF 19800: the next localtime() is UTC+05:30, zone IST");
    lt = *localtime(&jan);
    check(mktime(&lt) == jan, "TDF 19800: mktime() inverts localtime()");
    check(strftime(buf, sizeof buf, "%Y-%m-%d %H:%M %Z %z", &lt) > 0 &&
              strcmp(buf, "2026-01-15 17:30 IST +0530") == 0,
          "TDF 19800: strftime() gives 2026-01-15 17:30 IST +0530");
    check(strcmp(ctime(&jan), "Thu Jan 15 17:30:00 2026\n") == 0,
          "TDF 19800: ctime() gives Thu Jan 15 17:30:00 2026");

    check(deflog("SYS$TIMEZONE_DIFFERENTIAL", "-18000") && deflog("SYS$TIMEZONE_NAME", "EST"),
          "DEFINE SYS$TIMEZONE_DIFFERENTIAL -18000, SYS$TIMEZONE_NAME EST");
    check(local_is(jan, -18000, "EST", 0), "TDF -18000: localtime() is UTC-05:00, zone EST");

    /* The rule: used when it agrees with the TDF (here its daylight offset). */
    check(deflog("SYS$TIMEZONE_DIFFERENTIAL", "-14400") &&
              deflog("SYS$TIMEZONE_NAME", "EDT") &&
              deflog("SYS$TIMEZONE_RULE", "EST5EDT,M3.2.0,M11.1.0"),
          "DEFINE TDF -14400, NAME EDT, RULE EST5EDT,M3.2.0,M11.1.0");
    check(local_is(jul, -14400, "EDT", 1), "the rule: July is daylight time, UTC-04:00 EDT");
    check(local_is(jan, -18000, "EST", 0), "the rule: January is standard time, UTC-05:00 EST");

    /* A rule that disagrees with the TDF is stale: the TDF governs. */
    check(deflog("SYS$TIMEZONE_DIFFERENTIAL", "3600") && deflog("SYS$TIMEZONE_NAME", "CET"),
          "DEFINE TDF 3600, NAME CET (RULE still EST5EDT)");
    check(local_is(jul, 3600, "CET", 0), "a disagreeing rule is not used: UTC+01:00 CET");

    dellog("SYS$TIMEZONE_RULE");
    dellog("SYS$TIMEZONE_NAME");
    dellog("SYS$TIMEZONE_DIFFERENTIAL");
    printf("=== test_syssvc_crtl_time: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
#endif
}
