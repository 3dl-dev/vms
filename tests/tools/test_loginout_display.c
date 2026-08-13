/*
 * test_loginout_display.c - LOGINOUT session-sequence fidelity (vms-417).
 *
 * Two kinds of assertion:
 *
 *  (1) BEHAVIOURAL, on loginout_display_session_info(): the authentic lines
 *      appear in the right order and format, plural/singular is correct, and
 *      -- the no-facade heart of this -- every line is OMITTED when its value
 *      is absent (a first login with no failures and no mail prints NOTHING,
 *      never a "no previous login recorded" sentence VMS never emits).
 *
 *  (2) SOURCE GUARDS, on tools/vms_login.c and src/vmsdcl/dcl_main.c: the two
 *      invented strings this item deleted stay deleted, the dead divergent
 *      dcl_main.c emitter stays gone, and no hardcoded "Welcome to OpenVMS"
 *      identity claim creeps back onto the login path (INV-0 -- identity is
 *      SYS$WELCOME/ovmx_banner_welcome's job).
 *
 * Oracle for the expected strings/format: see loginout_display.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "loginout_display.h"
#include "test_paths.h"   /* generated: VMS_LOGIN_SRC, VMS_DCL_MAIN_SRC */

static int failures = 0;

#define CHECK(cond, msg) do {                                            \
        if (!(cond)) { printf("FAIL: %s\n", (msg)); failures++; }        \
        else         { printf("ok:   %s\n", (msg)); }                    \
    } while (0)

/* Capture loginout_display_session_info() output into 'buf'. */
static void capture(char *buf, size_t bufsz,
                    time_t li, time_t ln, unsigned fails, int mail)
{
    FILE *fp = tmpfile();
    if (!fp) { perror("tmpfile"); exit(2); }
    loginout_display_session_info(fp, li, ln, fails, mail);
    long n = ftell(fp);
    if (n < 0) n = 0;
    if ((size_t)n >= bufsz) n = (long)bufsz - 1;
    rewind(fp);
    size_t got = fread(buf, 1, (size_t)n, fp);
    buf[got] = '\0';
    fclose(fp);
}

/* Read an entire file into a malloc'd NUL-terminated buffer, or NULL. */
static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    rewind(fp);
    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(fp); return NULL; }
    size_t got = fread(b, 1, (size_t)n, fp);
    b[got] = '\0';
    fclose(fp);
    return b;
}

int main(void)
{
    char buf[2048];

    /* Deterministic timestamps: format in UTC so the assertions are stable
     * regardless of the host time zone. */
    setenv("TZ", "UTC", 1);
    tzset();

    /* A known instant: 2026-08-13 14:05:09 UTC. */
    struct tm t = {0};
    t.tm_year = 2026 - 1900; t.tm_mon = 7; t.tm_mday = 13;
    t.tm_hour = 14; t.tm_min = 5; t.tm_sec = 9;
    time_t when = timegm(&t);
    const char *WHEN = "13-AUG-2026 14:05:09.00";

    /* ---- Scenario A: first login, no failures, no mail -> EMPTY ---- */
    capture(buf, sizeof(buf), 0, 0, 0, 0);
    CHECK(buf[0] == '\0',
          "first login (no data) prints nothing -- no invented sentence");

    /* ---- Scenario B: interactive login only ---- */
    capture(buf, sizeof(buf), when, 0, 0, 0);
    {
        char want[128];
        snprintf(want, sizeof(want), "    Last interactive login on %s\n", WHEN);
        CHECK(strcmp(buf, want) == 0,
              "interactive-only: exact line, 4-space indent, dd-MMM-YYYY hh:mm:ss.cc");
        CHECK(strstr(buf, "non-interactive") == NULL,
              "interactive-only: no non-interactive line");
        CHECK(strstr(buf, "failure") == NULL,
              "interactive-only: no failures line");
        CHECK(strstr(buf, "mail") == NULL,
              "interactive-only: no mail line");
    }

    /* ---- Scenario C: all four lines, plural forms, correct order ---- */
    capture(buf, sizeof(buf), when, when, 2, 3);
    {
        char *i  = strstr(buf, "    Last interactive login on ");
        char *ni = strstr(buf, "    Last non-interactive login on ");
        char *f  = strstr(buf, "    2 failures since last successful login\n");
        char *m  = strstr(buf, "    You have 3 new mail messages.\n");
        CHECK(i && ni && f && m, "all four lines present");
        CHECK(i && ni && i < ni, "interactive precedes non-interactive");
        CHECK(ni && f && ni < f, "non-interactive precedes failures");
        CHECK(f && m && f < m,   "failures precedes mail");
        CHECK(strstr(buf, "successful login.") == NULL,
              "failures line has NO trailing period");
    }

    /* ---- Scenario D: singular forms (1 failure, 1 mail) ---- */
    capture(buf, sizeof(buf), 0, 0, 1, 1);
    CHECK(strstr(buf, "    1 failure since last successful login\n") != NULL,
          "one failure uses singular 'failure'");
    CHECK(strstr(buf, "failures") == NULL,
          "one failure does not say 'failures'");
    CHECK(strstr(buf, "    You have 1 new mail message.\n") != NULL,
          "one message uses singular 'message.'");

    /* ---- Scenario E: non-interactive present, interactive absent ---- */
    capture(buf, sizeof(buf), 0, when, 0, 0);
    {
        char want[128];
        snprintf(want, sizeof(want),
                 "    Last non-interactive login on %s\n", WHEN);
        CHECK(strcmp(buf, want) == 0,
              "non-interactive-only: exact line, interactive omitted");
    }

    /* ---- Source guard: invented strings stay deleted ---- */
    {
        char *login = slurp(VMS_LOGIN_SRC);
        CHECK(login != NULL, "can read tools/vms_login.c");
        if (login) {
            CHECK(strstr(login, "No previous interactive login recorded") == NULL,
                  "vms_login.c: invented 'No previous ... login recorded' is gone");
            CHECK(strstr(login, "Maximum login attempts exceeded") == NULL,
                  "vms_login.c: invented 'Maximum login attempts exceeded' is gone");
            CHECK(strstr(login, "Welcome to OpenVMS") == NULL,
                  "vms_login.c: no hardcoded 'Welcome to OpenVMS' identity (INV-0)");
            free(login);
        }
    }

    {
        char *dcl = slurp(VMS_DCL_MAIN_SRC);
        CHECK(dcl != NULL, "can read src/vmsdcl/dcl_main.c");
        if (dcl) {
            /* No definition and no call -- both would contain the token
             * followed by '('. (Prose mentions of the name are fine.) */
            CHECK(strstr(dcl, "display_banner(") == NULL,
                  "dcl_main.c: dead divergent display_banner emitter is gone");
            free(dcl);
        }
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
