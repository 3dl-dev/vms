/*
 * boot_time.h - the boot-time date check and the operator's date entry
 * (rd vms-b06).
 *
 * A VAX time-of-year clock (the TODR) counts hundredths of a second since the
 * start of the year. It holds no year. VMS keeps the year with the system
 * image and, when the time it can derive is not valid, it does not guess: it
 * asks the operator before it configures devices. The real OpenVMS VAX V7.3
 * boot (docs/oracle/installation-media-vax73-alpha84.md §1) prints
 *
 *     PLEASE ENTER DATE AND TIME
 *      (DD-MMM-YYYY  HH:MM)  07-AUG-2026 15:00
 *
 * and OpenVMS Alpha V8.4 prints the same question on one line in mixed case.
 *
 * On the NetBSD substrate, the kernel combines the TODR with the year of the
 * root filesystem's last write, and reports whether that produced a usable
 * time (kern.ovmx_toy_valid, tools/cross-vax/netbsd-ovmx-todr.patch). PID 1
 * asks the operator whenever it is not, which is what VMS does. Before this
 * item the kernel fell back to the filesystem time (or its built-in
 * 1-JAN-2010 12:00) and OVMX booted in 2010 without a word.
 *
 * Header-only and pure (no I/O) so the host unit test
 * (tests/ovmx_init/test_boot_time.c) compiles exactly what PID 1 compiles.
 */
#ifndef OVMX_BOOT_TIME_H
#define OVMX_BOOT_TIME_H

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* The question, as the oracle prints it on each architecture. */
#if defined(__vax__)
#define OVMX_BOOT_DATE_PROMPT "PLEASE ENTER DATE AND TIME\n (DD-MMM-YYYY  HH:MM)  "
#else
#define OVMX_BOOT_DATE_PROMPT "Please enter date and time (DD-MMM-YYYY  HH:MM)  "
#endif

/* Days from 1-JAN-1970 to the given proleptic Gregorian date. */
static inline int64_t ovmx_boot_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static inline int ovmx_boot_days_in_month(int y, int m)
{
    static const int dim[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return dim[m - 1] + (m == 2 && leap);
}

/* Read 1..maxd decimal digits; returns the count read, 0 if none. */
static inline int ovmx_boot_digits(const char **pp, int maxd, int *val)
{
    const char *p = *pp;
    int n = 0, v = 0;
    while (n < maxd && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
        n++;
    }
    if (n) {
        *pp = p;
        *val = v;
    }
    return n;
}

/*
 * ovmx_boot_parse_datetime - parse the operator's answer to the date prompt.
 *
 * Accepts the absolute VMS form the prompt names, DD-MMM-YYYY HH:MM, with
 * optional :SS and .CC, any case, any run of blanks between date and time,
 * and surrounding blanks. Every field is range-checked (31-FEB is refused).
 * The entered wall-clock time is the system time: the substrate keeps the
 * clock in the zone the operator gives it, as VMS does.
 *
 * Returns 1 and stores the time in *out on success, 0 on anything else.
 */
static inline int ovmx_boot_parse_datetime(const char *s, struct timespec *out)
{
    static const char *const mon[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const char *p = s;
    int d, m = 0, y, hh, mm, ss = 0, cc = 0;

    while (*p == ' ' || *p == '\t') p++;
    if (!ovmx_boot_digits(&p, 2, &d) || *p++ != '-')
        return 0;
    for (int i = 0; i < 12; i++) {
        if (toupper((unsigned char)p[0]) == mon[i][0] &&
            toupper((unsigned char)p[1]) == mon[i][1] &&
            toupper((unsigned char)p[2]) == mon[i][2]) {
            m = i + 1;
            break;
        }
    }
    if (!m)
        return 0;
    p += 3;
    if (*p++ != '-' || ovmx_boot_digits(&p, 4, &y) != 4)
        return 0;
    if (*p != ' ' && *p != '\t')
        return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!ovmx_boot_digits(&p, 2, &hh) || *p++ != ':' ||
        ovmx_boot_digits(&p, 2, &mm) != 2)
        return 0;
    if (*p == ':') {
        p++;
        if (ovmx_boot_digits(&p, 2, &ss) != 2)
            return 0;
        if (*p == '.') {
            p++;
            if (ovmx_boot_digits(&p, 2, &cc) != 2)
                return 0;
        }
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p)
        return 0;
    if (y < 1970 || d < 1 || d > ovmx_boot_days_in_month(y, m) ||
        hh > 23 || mm > 59 || ss > 59)
        return 0;

    out->tv_sec = (time_t)(ovmx_boot_days_from_civil(y, m, d) * 86400 +
                           hh * 3600 + mm * 60 + ss);
    out->tv_nsec = (long)cc * 10000000L;
    return 1;
}

/*
 * ovmx_boot_time_needs_operator - should PID 1 ask for the date?
 *
 *   substrate_valid  1: the substrate derived the time and vouches for it;
 *                    0: it derived none it trusts (TOY stopped or unset, or
 *                       no year to put it in);
 *                   -1: the substrate does not say (Linux, whose RTC carries
 *                       the year; a NetBSD kernel without the TOY patch).
 *
 * Asks only on 0. A clock the substrate reads without complaint is the
 * machine's clock even when it is wrong (qemu-system-alpha's clipper RTC reads
 * 2006), and VMS does not question a valid TOY either.
 */
static inline int ovmx_boot_time_needs_operator(int substrate_valid)
{
    return substrate_valid == 0;
}

#endif /* OVMX_BOOT_TIME_H */
