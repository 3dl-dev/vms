/*
 * test_lib_rtl.c - Unit tests for lib$, mth$, and ots$ RTL functions
 *
 * Tests date/time routines, conversion routines, process/system info,
 * output, math power/rounding, and integer/string conversions.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "mth$routines.h"
#include "ots$routines.h"
#include "prcdef.h"
#include "rmsdef.h"

static int failures = 0;
static int total = 0;

static void check(int cond, const char *name)
{
    total++;
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* lib$date_time - current date/time string                           */
/* ------------------------------------------------------------------ */
static void test_lib_date_time(void)
{
    printf("Testing lib$date_time...\n");

    char buf[24];
    memset(buf, 0, sizeof(buf));
    struct dsc$descriptor_s desc;
    desc.dsc$w_length = 23;
    desc.dsc$b_dtype = DSC$K_DTYPE_T;
    desc.dsc$b_class = DSC$K_CLASS_S;
    desc.dsc$a_pointer = buf;

    uint32_t st = lib$date_time(&desc);
    check(st == SS$_NORMAL, "lib$date_time returns SS$_NORMAL");

    /* Format: "DD-MMM-YYYY HH:MM:SS.CC" — 23 chars */
    /* Check that the dash separators are in place */
    check(buf[2] == '-', "lib$date_time has '-' at position 2");
    check(buf[6] == '-', "lib$date_time has '-' at position 6");
    check(buf[11] == ' ', "lib$date_time has ' ' at position 11");
    check(buf[14] == ':', "lib$date_time has ':' at position 14");

    /* Verify the month is a valid 3-letter abbreviation */
    static const char *months[] = {
        "JAN","FEB","MAR","APR","MAY","JUN",
        "JUL","AUG","SEP","OCT","NOV","DEC"
    };
    char mon[4] = { buf[3], buf[4], buf[5], '\0' };
    int valid_month = 0;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, months[i]) == 0) { valid_month = 1; break; }
    }
    check(valid_month, "lib$date_time contains valid month abbreviation");

    /* Bad parameter check */
    check(lib$date_time(NULL) == SS$_BADPARAM,
          "lib$date_time(NULL) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$day - day number since VMS epoch                               */
/* ------------------------------------------------------------------ */
static void test_lib_day(void)
{
    printf("Testing lib$day...\n");

    int32_t day_num = 0;
    int32_t day_of_year = 0;

    uint32_t st = lib$day(&day_num, NULL, &day_of_year);
    check(st == SS$_NORMAL, "lib$day returns SS$_NORMAL");

    /* VMS epoch is Nov 17 1858. Current day number should be large positive. */
    check(day_num > 50000, "lib$day: day_number > 50000 (post year 1995)");

    /* Day of year should be 1..366 */
    check(day_of_year >= 1 && day_of_year <= 366,
          "lib$day: day_of_year in [1, 366]");

    /* Bad parameter check */
    check(lib$day(NULL, NULL, NULL) == SS$_BADPARAM,
          "lib$day(NULL) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$day_of_week                                                    */
/* ------------------------------------------------------------------ */
static void test_lib_day_of_week(void)
{
    printf("Testing lib$day_of_week...\n");

    int32_t dow = 0;
    uint32_t st = lib$day_of_week(NULL, &dow);
    check(st == SS$_NORMAL, "lib$day_of_week returns SS$_NORMAL");

    /* VMS: 1=Monday .. 7=Sunday */
    check(dow >= 1 && dow <= 7, "lib$day_of_week in [1, 7]");
}

/* ------------------------------------------------------------------ */
/* lib$cvt_dtb / lib$cvt_htb / lib$cvt_otb                           */
/* ------------------------------------------------------------------ */
static void test_lib_cvt(void)
{
    printf("Testing lib$cvt_dtb / lib$cvt_htb / lib$cvt_otb...\n");

    int32_t val = 0;

    /* Decimal: "123" -> 123 */
    uint32_t st = lib$cvt_dtb(3, "123", &val);
    check(st == SS$_NORMAL && val == 123,
          "lib$cvt_dtb('123') = 123");

    /* Decimal: "0042" -> 42 */
    st = lib$cvt_dtb(4, "0042", &val);
    check(st == SS$_NORMAL && val == 42,
          "lib$cvt_dtb('0042') = 42");

    /* Hex: "FF" -> 255 */
    st = lib$cvt_htb(2, "FF", &val);
    check(st == SS$_NORMAL && val == 255,
          "lib$cvt_htb('FF') = 255");

    /* Hex: "1a" -> 26 (lowercase) */
    st = lib$cvt_htb(2, "1a", &val);
    check(st == SS$_NORMAL && val == 26,
          "lib$cvt_htb('1a') = 26");

    /* Octal: "77" -> 63 */
    st = lib$cvt_otb(2, "77", &val);
    check(st == SS$_NORMAL && val == 63,
          "lib$cvt_otb('77') = 63");

    /* Bad input: non-digit */
    st = lib$cvt_dtb(2, "XY", &val);
    check(st == SS$_BADPARAM,
          "lib$cvt_dtb('XY') returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$put_output                                                     */
/* ------------------------------------------------------------------ */
static void test_lib_put_output(void)
{
    printf("Testing lib$put_output...\n");

    char msg[] = "test_lib_rtl: lib$put_output works";
    struct dsc$descriptor_s desc = dsc$init(msg);

    uint32_t st = lib$put_output(&desc);
    check(st == SS$_NORMAL, "lib$put_output returns SS$_NORMAL");

    /* NULL descriptor */
    check(lib$put_output(NULL) == SS$_BADPARAM,
          "lib$put_output(NULL) returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$getjpi - process information                                   */
/* ------------------------------------------------------------------ */
/*
 * MOVED, NOT DELETED (vms-8019). The lib$getjpi assertions that stood here
 * -- JPI$_PID, JPI$_USERNAME and JPI$_PRCNAM all returning SS$_NORMAL --
 * now live in tests/qemu/test_syssvc_procnam.c (block P8), verbatim and
 * with two assertions ADDED (that the pid returned really is the caller's,
 * and the cross-process cases this file could never reach at all).
 *
 * They could not stay. lib$getjpi calls sys$getjpiw -> sys$getjpi, which is
 * now a READER OF THE EXECUTIVE process table behind /dev/vms, as a VMS
 * system service must be (CLAUDE.md Rule 11). ctest runs on a host where
 * /dev/vms does not exist and never will -- the only OVMX runtime is the
 * kernel/QEMU path (Rule 9) -- so what this block actually asserted after
 * the conversion was that a VMS system service succeeds with NO EXECUTIVE
 * PRESENT. That is a state OpenVMS is never in and OVMX refuses to boot
 * into (vms-0ff), so the assertion could only be kept green by giving
 * $GETJPI a per-process fallback: precisely the facade this item exists to
 * delete, reintroduced to satisfy a test.
 *
 * Weakening it to a skip was also not open: under Rule 10 a permanently
 * skipped test is a failing test. Relocating it to the one harness where
 * the service can actually run is the only answer that keeps the coverage
 * real. Everything else in this file is executive-independent and stays.
 */

/* ------------------------------------------------------------------ */
/* lib$getsyi - system information                                    */
/* ------------------------------------------------------------------ */
static void test_lib_getsyi(void)
{
    printf("Testing lib$getsyi...\n");

    /* Get NODENAME (string item) */
    char nbuf[64];
    memset(nbuf, 0, sizeof(nbuf));
    struct dsc$descriptor_s ndesc;
    ndesc.dsc$w_length = sizeof(nbuf) - 1;
    ndesc.dsc$b_dtype = DSC$K_DTYPE_T;
    ndesc.dsc$b_class = DSC$K_CLASS_S;
    ndesc.dsc$a_pointer = nbuf;
    uint16_t nlen = 0;

    uint32_t item = SYI$_NODENAME;
    uint32_t st = lib$getsyi(&item, NULL, &ndesc, &nlen, NULL, NULL);
    check(st == SS$_NORMAL, "lib$getsyi(SYI$_NODENAME) returns SS$_NORMAL");
    check(nlen > 0, "lib$getsyi(SYI$_NODENAME) returns non-empty string");

    /* Get HW_NAME (string item) */
    char hwbuf[64];
    memset(hwbuf, 0, sizeof(hwbuf));
    struct dsc$descriptor_s hwdesc;
    hwdesc.dsc$w_length = sizeof(hwbuf) - 1;
    hwdesc.dsc$b_dtype = DSC$K_DTYPE_T;
    hwdesc.dsc$b_class = DSC$K_CLASS_S;
    hwdesc.dsc$a_pointer = hwbuf;
    uint16_t hwlen = 0;

    item = SYI$_HW_NAME;
    st = lib$getsyi(&item, NULL, &hwdesc, &hwlen, NULL, NULL);
    check(st == SS$_NORMAL, "lib$getsyi(SYI$_HW_NAME) returns SS$_NORMAL");
    check(hwlen > 0, "lib$getsyi(SYI$_HW_NAME) returns non-empty string");
}

/* ------------------------------------------------------------------ */
/* mth$power_ji - integer power                                       */
/* ------------------------------------------------------------------ */
static void test_mth_power_ji(void)
{
    printf("Testing mth$power_ji...\n");

    int32_t base = 2, exp = 10;
    check(mth$power_ji(&base, &exp) == 1024, "power_ji(2, 10) = 1024");

    base = 3; exp = 0;
    check(mth$power_ji(&base, &exp) == 1, "power_ji(3, 0) = 1");

    base = -2; exp = 3;
    check(mth$power_ji(&base, &exp) == -8, "power_ji(-2, 3) = -8");
}

/* ------------------------------------------------------------------ */
/* mth$sincos                                                         */
/* ------------------------------------------------------------------ */
static void test_mth_sincos(void)
{
    printf("Testing mth$sincos...\n");

    double x = 0.0;
    double s = -1.0, c = -1.0;
    mth$sincos(&x, &s, &c);
    check(fabs(s) < 1e-10, "sincos(0): sin = 0");
    check(fabs(c - 1.0) < 1e-10, "sincos(0): cos = 1");
}

/* ------------------------------------------------------------------ */
/* mth$nint - nearest integer                                         */
/* ------------------------------------------------------------------ */
static void test_mth_nint(void)
{
    printf("Testing mth$nint...\n");

    double x = 3.7;
    check(mth$nint(&x) == 4, "nint(3.7) = 4");

    x = 3.2;
    check(mth$nint(&x) == 3, "nint(3.2) = 3");

    x = -0.5;
    /* round() rounds half away from zero: -0.5 -> -1 */
    check(mth$nint(&x) == -1, "nint(-0.5) = -1");
}

/* ------------------------------------------------------------------ */
/* ots$cvt_l_ti - integer to decimal string                           */
/* ------------------------------------------------------------------ */
static void test_ots_cvt_l_ti(void)
{
    printf("Testing ots$cvt_l_ti...\n");

    char buf[16];
    memset(buf, 0, sizeof(buf));
    struct dsc$descriptor_s desc;
    desc.dsc$w_length = 10;
    desc.dsc$b_dtype = DSC$K_DTYPE_T;
    desc.dsc$b_class = DSC$K_CLASS_S;
    desc.dsc$a_pointer = buf;

    int32_t val = 42;
    uint32_t st = ots$cvt_l_ti(&val, &desc, NULL, NULL, NULL);
    check(st == SS$_NORMAL, "ots$cvt_l_ti(42) returns SS$_NORMAL");

    /* Result should be right-justified: "        42" */
    /* Find the "42" at the end */
    check(buf[8] == '4' && buf[9] == '2',
          "ots$cvt_l_ti(42) right-justified '42' at end");

    /* Negative value */
    memset(buf, 0, sizeof(buf));
    val = -7;
    st = ots$cvt_l_ti(&val, &desc, NULL, NULL, NULL);
    check(st == SS$_NORMAL, "ots$cvt_l_ti(-7) returns SS$_NORMAL");

    /* Should contain "-7" right-justified */
    check(buf[8] == '-' && buf[9] == '7',
          "ots$cvt_l_ti(-7) right-justified '-7'");
}

/* ------------------------------------------------------------------ */
/* ots$cvt_ti_l - decimal string to integer                           */
/* ------------------------------------------------------------------ */
static void test_ots_cvt_ti_l(void)
{
    printf("Testing ots$cvt_ti_l...\n");

    int32_t result = 0;
    struct dsc$descriptor_s src = dsc$init("  123");

    uint32_t uresult = 0;
    uint32_t st = ots$cvt_ti_l(&src, &uresult, 4, 0);
    result = (int32_t)uresult;
    check(st == SS$_NORMAL, "ots$cvt_ti_l('  123') returns SS$_NORMAL");
    check(result == 123, "ots$cvt_ti_l('  123') = 123");

    /* Negative */
    src = dsc$init("-456");
    st = ots$cvt_ti_l(&src, &uresult, 4, 0);
    result = (int32_t)uresult;
    check(st == SS$_NORMAL, "ots$cvt_ti_l('-456') returns SS$_NORMAL");
    check(result == -456, "ots$cvt_ti_l('-456') = -456");
}

/* ------------------------------------------------------------------ */
/* ots$cvt_l_tz - integer to hex string                               */
/* ------------------------------------------------------------------ */
static void test_ots_cvt_l_tz(void)
{
    printf("Testing ots$cvt_l_tz...\n");

    char buf[16];
    memset(buf, 0, sizeof(buf));
    struct dsc$descriptor_s desc;
    desc.dsc$w_length = 8;
    desc.dsc$b_dtype = DSC$K_DTYPE_T;
    desc.dsc$b_class = DSC$K_CLASS_S;
    desc.dsc$a_pointer = buf;

    int32_t val = 255;
    uint32_t st = ots$cvt_l_tz(&val, &desc, NULL, 4);
    check(st == SS$_NORMAL, "ots$cvt_l_tz(255) returns SS$_NORMAL");

    /* Should contain "FF" right-justified in 8-char field */
    check(buf[6] == 'F' && buf[7] == 'F',
          "ots$cvt_l_tz(255) = '      FF'");

    /* Zero */
    memset(buf, 0, sizeof(buf));
    val = 0;
    st = ots$cvt_l_tz(&val, &desc, NULL, 4);
    check(st == SS$_NORMAL, "ots$cvt_l_tz(0) returns SS$_NORMAL");
    check(buf[7] == '0', "ots$cvt_l_tz(0) contains '0'");
}

/* ------------------------------------------------------------------ */
/* ots$cvt_l_to - integer to octal string                             */
/* ------------------------------------------------------------------ */
static void test_ots_cvt_l_to(void)
{
    printf("Testing ots$cvt_l_to...\n");

    char buf[16];
    memset(buf, 0, sizeof(buf));
    struct dsc$descriptor_s desc;
    desc.dsc$w_length = 8;
    desc.dsc$b_dtype = DSC$K_DTYPE_T;
    desc.dsc$b_class = DSC$K_CLASS_S;
    desc.dsc$a_pointer = buf;

    int32_t val = 8;
    uint32_t st = ots$cvt_l_to(&val, &desc, NULL, 4);
    check(st == SS$_NORMAL, "ots$cvt_l_to(8) returns SS$_NORMAL");

    /* 8 decimal = 10 octal, right-justified in 8-char field */
    check(buf[6] == '1' && buf[7] == '0',
          "ots$cvt_l_to(8) = '      10'");

    /* 63 decimal = 77 octal */
    memset(buf, 0, sizeof(buf));
    val = 63;
    st = ots$cvt_l_to(&val, &desc, NULL, 4);
    check(st == SS$_NORMAL, "ots$cvt_l_to(63) returns SS$_NORMAL");
    check(buf[6] == '7' && buf[7] == '7',
          "ots$cvt_l_to(63) = '      77'");
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== test_lib_rtl: lib$/mth$/ots$ RTL functions ===\n");

    test_lib_date_time();
    test_lib_day();
    test_lib_day_of_week();
    test_lib_cvt();
    test_lib_put_output();
    /* test_lib_getjpi() moved to tests/qemu/test_syssvc_procnam.c -- see
     * the block comment where it used to be defined. */
    test_lib_getsyi();
    test_mth_power_ji();
    test_mth_sincos();
    test_mth_nint();
    test_ots_cvt_l_ti();
    test_ots_cvt_ti_l();
    test_ots_cvt_l_tz();
    test_ots_cvt_l_to();

    printf("\n%d/%d assertions passed.\n", total - failures, total);

    if (failures == 0)
        printf("All lib_rtl tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
