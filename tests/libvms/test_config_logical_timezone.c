/*
 * test_config_logical_timezone.c - date/time formatting reads
 * SYS$TIMEZONE_DIFFERENTIAL live (vms-f89, Engine B dogfood, parent vms-704)
 *
 * WHAT THIS GATES. On origin/main src/libvms/rtl/lib_datetime.c formatted every
 * time with localtime_r() directly -- the time zone was baked into the libc
 * call, so the VMS time differential factor (TDF) logical
 * SYS$TIMEZONE_DIFFERENTIAL had no effect on displayed time. This item makes
 * the date/time FORMATTERS (lib$date_time, lib$format_date_time) read that
 * logical at point of use, so a DEFINE changes displayed time WITHOUT a
 * rebuild -- the "config comes from logicals" dogfood requirement.
 *
 * FAILS-ON-FACADE. The central assertion is that formatting one fixed absolute
 * instant under two different SYS$TIMEZONE_DIFFERENTIAL values yields two
 * DIFFERENT wall-clock strings that differ by exactly the differential delta.
 * On origin/main lib$format_date_time ignores the logical entirely, so both
 * formats equal localtime(t) regardless and the strings are IDENTICAL -- the
 * "differ by +1h" and "not equal" assertions below reject exactly that. The
 * differential==0 case pins the UTC wall clock, so the +3600 case must read
 * one hour later; a build that hardcodes localtime cannot move it.
 *
 * Doc pin (VSI OpenVMS System Manager's Manual, Vol. 1, "Managing the System
 * Time"): SYS$TIMEZONE_DIFFERENTIAL holds the TDF, the number of seconds from
 * UTC. gmtime(t + TDF) is the local wall clock for that differential.
 *
 * This drives the libvms logical store directly (sys$crelnm/sys$trnlnm), which
 * is exactly the store lib_datetime's point-of-use read consults -- no
 * executive / no /dev/vms needed, so it runs honestly on a host ctest. The
 * companion DCL test (tests/dcl/test_config_logical_timezone.sh) proves the
 * seeded defaults and the DEFINE override through F$TRNLNM.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "libdef.h"
#include "lnmdef.h"

extern uint32_t lib$format_date_time(struct dsc$descriptor_s *out,
                                     const void *in_time, void *context,
                                     uint16_t *out_len, const uint32_t *flags);

/* VMS epoch offset from Unix epoch in 100ns ticks (matches lib_datetime.c). */
#define VMS_EPOCH_OFFSET 0x007C95674BEB4000ULL

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* Define SYS$TIMEZONE_DIFFERENTIAL in the process logical table (the same
 * store lib_datetime's sys$trnlnm reads). */
static void set_differential(const char *seconds)
{
    struct dsc$descriptor_s tab = {
        (uint16_t)strlen("LNM$PROCESS_TABLE"), DSC$K_DTYPE_T, DSC$K_CLASS_S,
        (char *)"LNM$PROCESS_TABLE"
    };
    struct dsc$descriptor_s nam = {
        (uint16_t)strlen("SYS$TIMEZONE_DIFFERENTIAL"), DSC$K_DTYPE_T,
        DSC$K_CLASS_S, (char *)"SYS$TIMEZONE_DIFFERENTIAL"
    };
    struct item_list_3 itm[2];
    memset(itm, 0, sizeof(itm));
    itm[0].buflen = (uint16_t)strlen(seconds);
    itm[0].item_code = LNM$_STRING;
    itm[0].bufaddr = (void *)seconds;

    uint32_t st = sys$crelnm(NULL, &tab, &nam, NULL, itm);
    if (st != SS$_NORMAL && st != SS$_SUPERSEDE)
        printf("  (warn: sys$crelnm returned %u)\n", st);
}

/* Format one fixed VMS absolute time into a caller buffer. */
static void format_at(uint64_t vmstime, char *out, size_t outsz)
{
    uint32_t f = LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS;
    char buf[64];
    struct dsc$descriptor_s d = { (uint16_t)(sizeof(buf) - 1), DSC$K_DTYPE_T,
                                  DSC$K_CLASS_S, buf };
    uint16_t len = 0;
    memset(buf, 0, sizeof(buf));
    uint32_t st = lib$format_date_time(&d, &vmstime, NULL, &len, &f);
    if (st != SS$_NORMAL) {
        snprintf(out, outsz, "<format-error>");
        return;
    }
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    buf[len] = '\0';
    /* trim trailing blanks the S-class formatter may pad with */
    while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
    strncpy(out, buf, outsz - 1);
    out[outsz - 1] = '\0';
}

int main(void)
{
    printf("test_config_logical_timezone: formatting reads "
           "SYS$TIMEZONE_DIFFERENTIAL live (vms-f89)\n");

    /* A fixed absolute instant: 15-JUN-2023 12:00:00 UTC. */
    struct tm at;
    memset(&at, 0, sizeof(at));
    at.tm_year = 2023 - 1900;
    at.tm_mon = 6 - 1;
    at.tm_mday = 15;
    at.tm_hour = 12;
    at.tm_min = 0;
    at.tm_sec = 0;
    time_t base = timegm(&at);
    uint64_t vmstime = (uint64_t)base * 10000000ULL + VMS_EPOCH_OFFSET;

    /* TDF = 0: the UTC wall clock. Must read exactly 12:00. */
    set_differential("0");
    char utc[64];
    format_at(vmstime, utc, sizeof(utc));
    printf("  differential=0    -> %s\n", utc);
    check(strstr(utc, "15-JUN-2023") != NULL,
          "differential=0 shows the UTC date 15-JUN-2023");
    check(strstr(utc, "12:00:00") != NULL,
          "differential=0 shows the UTC wall clock 12:00:00");

    /* TDF = +3600 (one hour east): must read 13:00, i.e. one hour later. */
    set_differential("3600");
    char plus1[64];
    format_at(vmstime, plus1, sizeof(plus1));
    printf("  differential=3600 -> %s\n", plus1);
    check(strstr(plus1, "13:00:00") != NULL,
          "differential=3600 shifts the displayed time to 13:00:00");

    /* THE TRIPWIRE: the two must differ. On origin/main both equal
     * localtime(t) and this is identical -- the facade cannot move the clock. */
    check(strcmp(utc, plus1) != 0,
          "redefining SYS$TIMEZONE_DIFFERENTIAL changes displayed time live");

    /* TDF = -18000 (US Eastern winter): 12:00 UTC -> 07:00. */
    set_differential("-18000");
    char minus5[64];
    format_at(vmstime, minus5, sizeof(minus5));
    printf("  differential=-18000 -> %s\n", minus5);
    check(strstr(minus5, "07:00:00") != NULL,
          "differential=-18000 shifts the displayed time to 07:00:00");

    printf("\n%s: %d failure(s)\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
