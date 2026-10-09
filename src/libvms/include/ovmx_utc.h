/*
 * ovmx_utc.h -- UTC broken-down time to seconds since 1970, with no C RTL
 * dependency (rd vms-3b3f).
 *
 * timegm() is not a C RTL entry point on OpenVMS (it is absent from the DEC C
 * RTL name map, tools/cross-alpha-vms/musl-arch/decc-crtl-names.txt), so a
 * service built for the OpenVMS Alpha calling standard that called it linked
 * the reference as 0 and jumped there ($BINTIM on OVMX/Alpha). This is the
 * proleptic Gregorian day count, valid for any year; fields are normalised
 * like timegm's for month and (linearly) for day/hour/minute/second overflow.
 */
#ifndef OVMX_UTC_H
#define OVMX_UTC_H

#include <stdint.h>
#include <time.h>

static inline int64_t ovmx_days_from_civil(int64_t y, int64_t m /* 1..12 */, int64_t d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline time_t ovmx_timegm(const struct tm *tm)
{
    int64_t y = (int64_t)tm->tm_year + 1900;
    int64_t mon = tm->tm_mon;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        y -= 1;
    }
    int64_t days = ovmx_days_from_civil(y, mon + 1, 1) + (tm->tm_mday - 1);
    return (time_t)(days * 86400 + (int64_t)tm->tm_hour * 3600 +
                    (int64_t)tm->tm_min * 60 + tm->tm_sec);
}

#endif /* OVMX_UTC_H */
