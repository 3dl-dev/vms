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

/* posix_openpt()/grantpt()/unlockpt()/ptsname() for the terminal-read tests. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "loginout_display.h"
#include "login_input.h"  /* the prompt reader + type-ahead drain (vms-3e9) */
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

/* Capture loginout_display_system_identification() output into 'buf'. */
static void capture_ident(char *buf, size_t bufsz, const char *product,
                          const char *arch, const char *version,
                          const char *badge)
{
    FILE *fp = tmpfile();
    if (!fp) { perror("tmpfile"); exit(2); }
    loginout_display_system_identification(fp, product, arch, version, badge);
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

/* ---- a REAL terminal for the login-read tests (rd vms-29e) ----------- */
/*
 * Opens a pseudo-terminal pair. *slave is the terminal LOGINOUT reads and
 * writes (default termios: ICANON, ECHO, ICRNL, ONLCR -- a console), *master is
 * the "operator's keyboard and screen". Returns 0 on success.
 */
static int open_terminal(int *master, int *slave)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    if (grantpt(m) != 0 || unlockpt(m) != 0) { close(m); return -1; }
    const char *name = ptsname(m);
    int s = name ? open(name, O_RDWR | O_NOCTTY) : -1;
    if (s < 0) { close(m); return -1; }
    *master = m;
    *slave = s;
    return 0;
}

/* Everything the terminal has shown the operator so far (bounded wait). */
static size_t screen(int master, char *buf, size_t bufsz)
{
    size_t len = 0;
    for (;;) {
        struct pollfd p = { master, POLLIN, 0 };
        if (poll(&p, 1, 300) <= 0 || !(p.revents & POLLIN)) break;
        ssize_t n = read(master, buf + len, bufsz - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        if (len >= bufsz - 1) break;
    }
    buf[len] = '\0';
    return len;
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

    /* ================================================================
     * (a) THE PRE-Username: SYSTEM-IDENTIFICATION LINE (vms-3e9).
     * ================================================================ */

    /* ---- Shape: oracle order/indent, every value the caller supplied ---- */
    capture_ident(buf, sizeof(buf), "OpenVMX", "VAX", "V0.0-test",
                  "OpenVMS-compatible");
    CHECK(strcmp(buf,
                 "\n Welcome to the OpenVMX VAX Operating System, "
                 "Version V0.0-test (OpenVMS-compatible)\n\n") == 0,
          "identification: blank line, ONE leading space, product/arch/version/badge, blank line");

    /*
     * ---- THE ANTI-HOLLOWING GUARD, and it is load-bearing ----
     *
     * ~30 runtime gates use the substring 'Welcome to <product>' as their
     * PROOF THAT A LOGIN SUCCEEDED (it is the SYS$WELCOME default, printed
     * only after the password is accepted). This line is printed BEFORE the
     * Username: prompt, so if it ever matched that substring every one of
     * those gates would pass without a login ever happening. Reword this line
     * and this assertion reds -- which is the point: the collision must be
     * caught here, not discovered as a silently-hollow battery.
     */
    CHECK(strstr(buf, "Welcome to OpenVMX") == NULL,
          "identification: NOT matched by the SYS$WELCOME login-success token 'Welcome to <product>'");
    CHECK(strstr(buf, "Welcome to the OpenVMX") != NULL,
          "identification: does say 'Welcome to the <product>' (the deliberate one-word difference)");

    /* ---- INV-0: no bare 'OpenVMS <version>' identity claim ---- */
    CHECK(strstr(buf, "Welcome to OpenVMS") == NULL,
          "identification: never claims to BE OpenVMS (INV-0 trademark ceiling)");

    /* ---- Omission, never invention (INV-6) ---- */
    capture_ident(buf, sizeof(buf), "OpenVMX", NULL, "V0.0-test", NULL);
    CHECK(strcmp(buf,
                 "\n Welcome to the OpenVMX Operating System, "
                 "Version V0.0-test\n\n") == 0,
          "identification: unknown arch and absent badge are OMITTED, not guessed");

    capture_ident(buf, sizeof(buf), "OpenVMX", "VAX", NULL, "OpenVMS-compatible");
    CHECK(buf[0] == '\0',
          "identification: no version -> NOTHING printed (a half-known identity is not completed)");

    capture_ident(buf, sizeof(buf), "", "VAX", "V0.0-test", "OpenVMS-compatible");
    CHECK(buf[0] == '\0',
          "identification: no product -> NOTHING printed");

    /* ================================================================
     * (c) THE LGI-STYLE IDLE DEADLINE ON THE LOGIN PROMPTS (vms-3e9).
     *
     * Exercised against a REAL descriptor (a pipe), not a stub: the
     * poll()/read() deadline is the mechanism LOGINOUT runs, so the same
     * code path decides these results as decides a console login.
     * ================================================================ */
    {
        int pfd[2];
        char line[16];

        /* --- a complete line arrives -> LOGIN_READ_OK, trailing CR eaten --- */
        CHECK(pipe(pfd) == 0, "idle deadline: test pipe created");
        CHECK(write(pfd[1], "SYSTEM\r\n", 8) == 8, "idle deadline: line written");
        CHECK(login_read_line_timed(pfd[0], line, sizeof(line), 0, pfd[1], 5)
                  == LOGIN_READ_OK,
              "prompt read: a complete line reads OK");
        CHECK(strcmp(line, "SYSTEM") == 0,
              "prompt read: the line is delivered without CR/LF");
        close(pfd[0]); close(pfd[1]);

        /* --- NOTHING arrives -> the deadline expires (this is the feature) --- */
        CHECK(pipe(pfd) == 0, "idle deadline: second test pipe created");
        {
            long long t0 = login_now_ms();
            int rc = login_read_line_timed(pfd[0], line, sizeof(line), 0,
                                           pfd[1], 1);
            long long elapsed = login_now_ms() - t0;
            CHECK(rc == LOGIN_READ_TIMEOUT,
                  "prompt read: an idle prompt times out (the LGI_RETRY_TMO behaviour, rd vms-29e)");
            /* NEGATIVE CONTROL: it must actually have WAITED. A stub that
             * returned TIMEOUT immediately would pass the line above and be
             * useless as a login deadline. */
            CHECK(elapsed >= 900,
                  "NEGCTL: the timeout really waited the full window (>=900ms for a 1s deadline)");
            CHECK(line[0] == '\0',
                  "prompt read: a timed-out prompt yields no partial response");
        }
        close(pfd[0]); close(pfd[1]);

        /* --- a HALF-TYPED response still times out (the deadline covers the
         *     whole response, not just the first keystroke) --- */
        CHECK(pipe(pfd) == 0, "idle deadline: third test pipe created");
        CHECK(write(pfd[1], "SYST", 4) == 4, "idle deadline: partial line written");
        CHECK(login_read_line_timed(pfd[0], line, sizeof(line), 0, pfd[1], 1)
                  == LOGIN_READ_TIMEOUT,
              "prompt read: a half-typed response times out too (partial input is not a response)");
        close(pfd[0]); close(pfd[1]);

        /* --- EOF (the connection closed with nothing typed) --- */
        CHECK(pipe(pfd) == 0, "idle deadline: fourth test pipe created");
        close(pfd[1]);
        CHECK(login_read_line_timed(pfd[0], line, sizeof(line), 0, pfd[0], 5)
                  == LOGIN_READ_EOF,
              "prompt read: EOF is reported as EOF, not as a timeout");
        close(pfd[0]);

        /* --- an over-long line is truncated AND fully consumed, so its tail
         *     cannot be read back as the next prompt's response --- */
        CHECK(pipe(pfd) == 0, "idle deadline: fifth test pipe created");
        CHECK(write(pfd[1], "ABCDEFGHIJKLMNOPQRSTUVWXYZ\nNEXT\n", 32) == 32,
              "idle deadline: over-long line written");
        CHECK(login_read_line_timed(pfd[0], line, sizeof(line), 0, pfd[1], 5)
                  == LOGIN_READ_OK,
              "prompt read: an over-long response still reads OK");
        CHECK(strcmp(line, "ABCDEFGHIJKLMNO") == 0,
              "prompt read: over-long response truncated to the buffer");
        CHECK(login_read_line_timed(pfd[0], line, sizeof(line), 0, pfd[1], 5)
                  == LOGIN_READ_OK && strcmp(line, "NEXT") == 0,
              "prompt read: the NEXT line is the next response (no tail leaks into it)");
        close(pfd[0]); close(pfd[1]);
    }


    /* ================================================================
     * (d) THE LOGIN READ ON A REAL TERMINAL: WHAT THE OPERATOR SEES
     *     (rd vms-29e).
     *
     * Oracle: a real OpenVMS VAX V7.3 console, isolated vaxlab replica,
     * 2026-09-25 (tests/lab/captures/vms-29e-login-read-oracle-20260925/):
     *
     *   "Username: \r\nError reading command input\r\nTimeout period expired\r\n"
     *   "Password: \r\nError reading command input\r\nTimeout period expired\r\n"
     *
     * after ~20 s -- LGI_RETRY_TMO, not LGI_PWD_TMO: moving RETRY_TMO to 60
     * moved both reads to ~60 s, moving PWD_TMO to 60 moved neither. OVMX timed
     * out after 30 s and printed NOTHING, so an idle-timed-out console looked
     * like a live "Username:" -- the whole of the vms-29e symptom.
     *
     * Run on a pseudo-terminal, not a pipe: ECHO, ONLCR and the canonical
     * line buffer are the terminal driver's, and they decide these bytes.
     * ================================================================ */
    {
        static const char want[] =
            "\r\nError reading command input\r\nTimeout period expired\r\n";
        char line[16];
        char scr[512];
        int m, t;

        CHECK(LOGIN_INPUT_TIMEOUT_SEC == 20,
              "login read deadline is 20 s -- the LGI_RETRY_TMO default the V7.3 oracle applies to both login reads");

        /* --- Username: left idle --- */
        if (open_terminal(&m, &t) == 0) {
            CHECK(isatty(t), "NEGCTL: the login-read tests really run on a terminal");
            int rc = login_read_line_timed(t, line, sizeof(line), 0, t, 1);
            size_t n = screen(m, scr, sizeof(scr));
            CHECK(rc == LOGIN_READ_TIMEOUT, "terminal: an idle Username: read times out");
            CHECK(n == sizeof(want) - 1 && memcmp(scr, want, n) == 0,
                  "terminal: an expired Username: read shows EXACTLY the VMS report (prompt line ended, "
                  "'Error reading command input', 'Timeout period expired') -- not silence");
            if (!(n == sizeof(want) - 1 && memcmp(scr, want, n) == 0))
                printf("      screen was: [%s]\n", scr);
            close(m); close(t);
        } else {
            CHECK(0, "terminal: could not open a pseudo-terminal");
        }

        /* --- Username: half typed, then left --- */
        if (open_terminal(&m, &t) == 0) {
            CHECK(write(m, "SYS", 3) == 3, "terminal: operator types SYS and stops");
            int rc = login_read_line_timed(t, line, sizeof(line), 0, t, 1);
            size_t n = screen(m, scr, sizeof(scr));
            CHECK(rc == LOGIN_READ_TIMEOUT && line[0] == '\0',
                  "terminal: a half-typed username times out and yields nothing");
            CHECK(n == 3 + sizeof(want) - 1 && memcmp(scr, "SYS", 3) == 0 &&
                  memcmp(scr + 3, want, sizeof(want) - 1) == 0,
                  "terminal: the oracle's 'Username: SYS' + report shape -- the typed echo, then the report on the next line");
            /* The half-typed line must not survive into the next session's read. */
            CHECK(write(m, "TEM\r", 4) == 4, "terminal: operator then finishes the line");
            rc = login_read_line_timed(t, line, sizeof(line), 0, t, 1);
            CHECK(rc == LOGIN_READ_OK && strcmp(line, "TEM") == 0,
                  "terminal: the timed-out partial 'SYS' was discarded -- it does not prefix the next read");
            close(m); close(t);
        } else {
            CHECK(0, "terminal: could not open a pseudo-terminal");
        }

        /* --- Password: (echo off) left idle: the prompt line ends ONCE --- */
        if (open_terminal(&m, &t) == 0) {
            int rc = login_read_line_timed(t, line, sizeof(line), 1, t, 1);
            size_t n = screen(m, scr, sizeof(scr));
            CHECK(rc == LOGIN_READ_TIMEOUT, "terminal: an idle Password: read times out");
            CHECK(n == sizeof(want) - 1 && memcmp(scr, want, n) == 0,
                  "terminal: an expired Password: read shows EXACTLY the VMS report (one line end, not two)");
            if (!(n == sizeof(want) - 1 && memcmp(scr, want, n) == 0))
                printf("      screen was: [%s]\n", scr);
            close(m); close(t);
        } else {
            CHECK(0, "terminal: could not open a pseudo-terminal");
        }

        /* --- the vms-29e PREMISE, measured: output written to the console by
         *     someone else while the operator is typing (the executive's
         *     %DLM/%CNXMAN lines, a REPLY, OPCOM) does NOT end the read. The
         *     V7.3 oracle carried on the same way: "Username: SYS", a REPLY and
         *     an OPCOM message, then "TEM" completed SYSTEM. --- */
        if (open_terminal(&m, &t) == 0) {
            static const char opline[] =
                "\n[  340.849897] %DLM, refusing a lock message from a system that has not proved it runs this implementation\n";
            int other = open(ptsname(m), O_WRONLY | O_NOCTTY);
            CHECK(write(m, "SYS", 3) == 3, "terminal: operator types SYS");
            (void)screen(m, scr, sizeof(scr));
            CHECK(other >= 0 && write(other, opline, sizeof(opline) - 1) ==
                                    (ssize_t)(sizeof(opline) - 1),
                  "terminal: an operator line is written to the console mid-read");
            CHECK(write(m, "TEM\r", 4) == 4, "terminal: operator finishes with TEM<CR>");
            int rc = login_read_line_timed(t, line, sizeof(line), 0, t, 2);
            CHECK(rc == LOGIN_READ_OK && strcmp(line, "SYSTEM") == 0,
                  "terminal: the read survives console output and returns the whole username SYSTEM");
            if (other >= 0) close(other);
            close(m); close(t);
        } else {
            CHECK(0, "terminal: could not open a pseudo-terminal");
        }
    }

    /* ================================================================
     * (b) THE TYPE-AHEAD DRAIN IS SUBSTRATE-INDEPENDENT (vms-3e9).
     *
     * On the VAX rail the console is not a tty, so tcflush() discarded
     * nothing and the RETURNs struck during the boot came back as a burst of
     * empty usernames. The drain must work on a NON-TTY descriptor -- which
     * is exactly what this pipe is.
     * ================================================================ */
    {
        int pfd[2];
        char line[16];

        CHECK(pipe(pfd) == 0, "type-ahead drain: test pipe created");
        CHECK(!isatty(pfd[0]),
              "NEGCTL: the drain is being tested on a NON-tty descriptor (where tcflush does nothing)");
        CHECK(write(pfd[1], "\n\n\n\n\n", 5) == 5,
              "type-ahead drain: five queued RETURNs written");
        login_drain_typeahead(pfd[0], 64 * 1024);
        CHECK(write(pfd[1], "SYSTEM\n", 7) == 7,
              "type-ahead drain: the operator then types a username");
        CHECK(login_read_line_timed(pfd[0], line, sizeof(line), 0, pfd[1], 5)
                  == LOGIN_READ_OK && strcmp(line, "SYSTEM") == 0,
              "type-ahead drain: the queued RETURNs are gone -- the first read is the typed username, not an empty one");
        close(pfd[0]); close(pfd[1]);
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

            /* vms-3e9 (a): the identification line is emitted, and it is
             * emitted BEFORE the Username: prompt, not after it. */
            {
                const char *ident = strstr(login,
                        "loginout_display_system_identification(stdout");
                const char *prompt = strstr(login, "printf(\"Username: \")");
                CHECK(ident != NULL,
                      "vms_login.c: emits the pre-Username system-identification line (vms-3e9 a)");
                CHECK(prompt != NULL,
                      "vms_login.c: still prints the Username: prompt");
                CHECK(ident && prompt && ident < prompt,
                      "vms_login.c: the identification line precedes the Username: prompt");
            }

            /* vms-3e9 (b): the console wake is gated on the EXECUTIVE's
             * terminal binding, not on a bare isatty() the VAX rail answers
             * FALSE. The predicate keeps isatty as a fallback INSIDE itself,
             * so what must not come back is the bare `if (isatty(...))' wake. */
            CHECK(strstr(login, "loginout_at_operator_terminal()") != NULL,
                  "vms_login.c: the OPA0: wake asks the executive whether this session is on a terminal (vms-3e9 b)");
            CHECK(strstr(login, "if (isatty(STDIN_FILENO)) {") == NULL,
                  "vms_login.c: the bare isatty()-gated wake (x86_64-true, VAX-false) is gone");
            CHECK(strstr(login, "login_drain_typeahead(STDIN_FILENO") != NULL,
                  "vms_login.c: type-ahead is drained substrate-independently, not by tcflush alone");

            /* vms-3e9 (c): both prompts carry the idle deadline. */
            CHECK(strstr(login, "LOGIN_INPUT_TIMEOUT_SEC") != NULL,
                  "vms_login.c: the login prompts carry the LGI-style idle deadline (vms-3e9 c)");
            CHECK(strstr(login, "fgets(username") == NULL,
                  "vms_login.c: the deadline-less fgets() username read is gone");

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
