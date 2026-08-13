/*
 * loginout_display.c - OpenVMS-faithful LOGINOUT session-information block.
 *
 * See loginout_display.h for the oracle citations and the no-facade rule.
 */

#include "loginout_display.h"

/* VMS three-letter month abbreviations (upper case), as they appear in the
 * OpenVMS standard absolute date-time format. */
static const char *const loginout_months[] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
};

/*
 * Format a time_t as the OpenVMS standard absolute date-time
 *   dd-MMM-YYYY hh:mm:ss.cc
 * space-padded day, in LOCAL time.
 *
 * The hundredths field (.cc) is always ".00": the last-login value OVMX
 * records has whole-second resolution (a Unix timestamp), so the sub-second
 * digits are genuinely unknown, not withheld. Printing ".00" reproduces the
 * VMS field WIDTH honestly; it does not claim a precision OVMX has.
 */
static void loginout_fmt_time(time_t t, char *buf, size_t n)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    snprintf(buf, n, "%2d-%s-%04d %02d:%02d:%02d.00",
             tmv.tm_mday, loginout_months[tmv.tm_mon], tmv.tm_year + 1900,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

void loginout_display_session_info(FILE *out,
                                   time_t last_interactive,
                                   time_t last_noninteractive,
                                   unsigned login_failures,
                                   int new_mail_count)
{
    char when[40];

    if (!out)
        return;

    /* Last successful interactive login -- omitted when there is none, as on
     * VMS (a brand-new account's first login shows this line for nobody). */
    if (last_interactive > 0) {
        loginout_fmt_time(last_interactive, when, sizeof(when));
        fprintf(out, "    Last interactive login on %s\n", when);
    }

    /* Last successful non-interactive (batch/network) login -- omitted when
     * absent, as on VMS. */
    if (last_noninteractive > 0) {
        loginout_fmt_time(last_noninteractive, when, sizeof(when));
        fprintf(out, "    Last non-interactive login on %s\n", when);
    }

    /* Bad-password attempts since the last success -- omitted at zero, as on
     * VMS (an interactive login with no failures shows nothing here). No
     * trailing period, per the Guide to System Security. */
    if (login_failures > 0) {
        fprintf(out, "    %u failure%s since last successful login\n",
                login_failures, login_failures == 1 ? "" : "s");
    }

    /* Unread mail -- omitted at zero. Blank line first, as MAIL's login
     * notification is set off from the login lines above it. */
    if (new_mail_count > 0) {
        fprintf(out, "\n    You have %d new mail message%s.\n",
                new_mail_count, new_mail_count == 1 ? "" : "s");
    }
}
