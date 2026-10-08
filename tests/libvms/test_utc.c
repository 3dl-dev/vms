/*
 * test_utc.c - $BINUTC/$NUMUTC/$ASCUTC/$TIMCON/$GETUTC against the values the real
 * services returned on the lab OpenVMS Alpha V8.4 node (MACRO-32 probes in
 * tools/lab-alpha/probes, SYS$TIMEZONE_DIFFERENTIAL = 0):
 *
 *   $BINUTC("29-FEB-2000 12:23:45.67") -> longwords 11081A60 01D3EEA3 FFFFFFFF 1000FFFF
 *   $NUMUTC of that                    -> 2000 2 29 12 23 45 67
 *   $ASCUTC cvtflg 0 / 1               -> "29-FEB-2000 12:23:45.67" / "12:23:45.67"
 *   $TIMCON(utc, 0)                    -> quadword 009E6638 49721A60
 *   $TIMCON(quad, 1)                   -> the same UTC structure
 *   $GETUTC                            -> abstime = $GETTIM + 0x0135886AC7960000,
 *                                         inaccuracy all ones, TDF word 0x1000
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ssdef.h"
#include "descrip.h"
#include "starlet.h"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static uint32_t lw(const uint8_t *u, int i) { return u[4*i] | (u[4*i+1] << 8) | (u[4*i+2] << 16) | ((uint32_t)u[4*i+3] << 24); }

int main(void)
{
    printf("=== test_utc ===\n");
    uint8_t utc[16], utc2[16];
    uint16_t tv[7];
    uint64_t q = 0;
    char buf[64];
    struct dsc$descriptor_s in = { 23, DSC$K_DTYPE_T, DSC$K_CLASS_S, "29-FEB-2000 12:23:45.67" };
    struct dsc$descriptor_s out = { sizeof buf, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf };
    uint16_t len = 0;

    CHECK($VMS_STATUS_SUCCESS(sys$binutc(&in, utc)), "$BINUTC converts the string");
    CHECK(lw(utc, 0) == 0x11081A60u && lw(utc, 1) == 0x01D3EEA3u &&
          lw(utc, 2) == 0xFFFFFFFFu && lw(utc, 3) == 0x1000FFFFu,
          "the structure is byte-for-byte what the Alpha lab returned");

    CHECK($VMS_STATUS_SUCCESS(sys$numutc(tv, utc)) &&
          tv[0] == 2000 && tv[1] == 2 && tv[2] == 29 && tv[3] == 12 && tv[4] == 23 &&
          tv[5] == 45 && tv[6] == 67, "$NUMUTC gives 2000 2 29 12 23 45 67");

    memset(buf, 0, sizeof buf);
    CHECK($VMS_STATUS_SUCCESS(sys$ascutc(&len, &out, utc, 0)) && len == 23 &&
          memcmp(buf, "29-FEB-2000 12:23:45.67", 23) == 0, "$ASCUTC cvtflg 0: date and time");
    memset(buf, 0, sizeof buf);
    CHECK($VMS_STATUS_SUCCESS(sys$ascutc(&len, &out, utc, 1)) && len == 11 &&
          memcmp(buf, "12:23:45.67", 11) == 0, "$ASCUTC cvtflg 1: the time only");

    CHECK($VMS_STATUS_SUCCESS(sys$timcon(&q, utc, 0)) && q == 0x009E663849721A60ull,
          "$TIMCON UTC -> quadword is 009E6638 49721A60");
    memset(utc2, 0, sizeof utc2);
    CHECK($VMS_STATUS_SUCCESS(sys$timcon(&q, utc2, 1)) && memcmp(utc, utc2, 16) == 0,
          "$TIMCON quadword -> UTC returns the same structure");

    uint64_t t0 = 0, t1 = 0;
    sys$gettim(&t0);
    CHECK($VMS_STATUS_SUCCESS(sys$getutc(utc)), "$GETUTC");
    sys$gettim(&t1);
    uint64_t a = ((uint64_t)lw(utc, 1) << 32) | lw(utc, 0);
    CHECK(a >= t0 + 0x0135886AC7960000ull && a <= t1 + 0x0135886AC7960000ull,
          "$GETUTC abstime is $GETTIM + the 1582->1858 offset");
    CHECK(lw(utc, 2) == 0xFFFFFFFFu && (lw(utc, 3) >> 16) == 0x1000 &&
          (lw(utc, 3) & 0xFFFF) == 0xFFFF, "inaccuracy unspecified, TDF word 0x1000 (version 1, TDF 0)");
    CHECK(sys$getutc(NULL) == SS$_BADPARAM, "a NULL structure is refused");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
