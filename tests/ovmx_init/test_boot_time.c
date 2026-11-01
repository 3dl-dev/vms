/*
 * test_boot_time.c - the boot date check and the operator's date entry
 * (rd vms-b06), against the REAL src/ovmx_init/boot_time.h PID 1 compiles.
 *
 * The defect: OVMX/VAX booted at 1-JAN-2010 without a word because the
 * NetBSD/vax kernel found no year for the TOY clock and fell back to a
 * built-in date. The fix has the kernel say so (kern.ovmx_toy_valid) and PID 1
 * ask, as VMS does. This pins the PID 1 half:
 *
 *   1. the boot asks exactly when the substrate says the time is not valid,
 *      and not when it vouches for it or does not say;
 *   2. the VMS date form the prompt names parses to the right instant,
 *      including leap days, seconds and hundredths, any case;
 *   3. malformed or out-of-range answers are refused (so the prompt repeats);
 *   4. the prompt text is the oracle's.
 */
#include "boot_time.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, msg) do { \
    if (cond) printf("PASS: %s\n", msg); \
    else { printf("FAIL: %s\n", msg); fails++; } \
} while (0)

static int parses_to(const char *s, long long sec, long nsec)
{
    struct timespec ts = {0, 0};
    return ovmx_boot_parse_datetime(s, &ts) && (long long)ts.tv_sec == sec &&
           ts.tv_nsec == nsec;
}

static int refused(const char *s)
{
    struct timespec ts = {0, 0};
    return !ovmx_boot_parse_datetime(s, &ts);
}

int main(void)
{
    /* 1. when to ask */
    CHECK(ovmx_boot_time_needs_operator(0), "asks when the substrate says the time is not valid");
    CHECK(!ovmx_boot_time_needs_operator(1), "does not ask when the substrate vouches for the time");
    CHECK(!ovmx_boot_time_needs_operator(-1), "does not ask when the substrate does not say");

    /* 2. good answers. 21-OCT-2026 15:00 UTC = 1792594800. */
    CHECK(parses_to("21-OCT-2026 15:00", 1792594800LL, 0), "21-OCT-2026 15:00");
    CHECK(parses_to("21-oct-2026 15:00\n", 1792594800LL, 0), "lower case + trailing newline");
    CHECK(parses_to("  21-Oct-2026   15:00  \r\n", 1792594800LL, 0), "blanks around and between");
    CHECK(parses_to("21-OCT-2026 15:00:07.25", 1792594807LL, 250000000L), "seconds and hundredths");
    CHECK(parses_to("7-AUG-2026 15:00", 1786114800LL, 0), "one-digit day (oracle answer 07-AUG-2026 15:00)");
    CHECK(parses_to("07-AUG-2026 15:00", 1786114800LL, 0), "two-digit day");
    CHECK(parses_to("29-FEB-2028 00:00", 1835395200LL, 0), "leap day 2028");
    CHECK(parses_to("1-JAN-1970 00:00", 0LL, 0), "epoch");
    CHECK(parses_to("31-DEC-2026 23:59:59", 1798761599LL, 0), "last second of 2026");

    /* 3. refused answers */
    CHECK(refused(""), "empty");
    CHECK(refused("\n"), "bare RETURN");
    CHECK(refused("21-OCT-2026"), "date without time");
    CHECK(refused("21-XYZ-2026 15:00"), "unknown month");
    CHECK(refused("29-FEB-2026 12:00"), "29-FEB in a common year");
    CHECK(refused("31-APR-2026 12:00"), "31-APR");
    CHECK(refused("0-OCT-2026 12:00"), "day 0");
    CHECK(refused("21-OCT-26 15:00"), "two-digit year");
    CHECK(refused("21-OCT-2026 24:00"), "hour 24");
    CHECK(refused("21-OCT-2026 15:60"), "minute 60");
    CHECK(refused("21-OCT-2026 15:00 junk"), "trailing junk");
    CHECK(refused("21-OCT-2026 15:0"), "one-digit minute");
    CHECK(refused("21-OCT-1969 15:00"), "before 1970");

    /* 4. the oracle's question (docs/oracle/installation-media-vax73-alpha84.md) */
#if defined(__vax__)
    CHECK(strcmp(OVMX_BOOT_DATE_PROMPT,
                 "PLEASE ENTER DATE AND TIME\n (DD-MMM-YYYY  HH:MM)  ") == 0,
          "VAX prompt is the V7.3 oracle's");
#else
    CHECK(strcmp(OVMX_BOOT_DATE_PROMPT,
                 "Please enter date and time (DD-MMM-YYYY  HH:MM)  ") == 0,
          "non-VAX prompt is the Alpha V8.4 oracle's");
#endif

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
