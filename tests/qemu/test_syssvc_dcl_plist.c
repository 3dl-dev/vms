/*
 * test_syssvc_dcl_plist.c - DCL file verbs take a P1 list of any length, as
 * OpenVMS does (rd vms-ba2), against a real /dev/vms and the Files-11 ACP.
 *
 * Oracle: real OpenVMS VAX V7.3, tests/lab/captures/dcl-plist-20261102
 * (PREREGISTERED.md, CONSOLE.txt, RESULTS.md). The DCL parser keeps P1-P8,
 * and the elements of a comma list past P8 in an overflow (rd vms-465); a
 * file verb answered %DCL-W-MAXPARM for a list past P8, and acted on the
 * FIRST element only of any shorter list. Each probe is one DCL session:
 *   1. DELETE of a 12-element list removes all twelve; $STATUS success (#1);
 *   2. DELETE K.TXT;* L.TXT;* is %DCL-W-MAXPARM \L\ and deletes nothing (#2);
 *   3. TYPE A,B,C prints all three, B and C headed by their full name (#3);
 *   4. COPY A,B C2 makes ONE C2.TXT holding A then B (#4);
 *   5. DIRECTORY A,B,C: one header, "Total of 3 files." (#7);
 *   6. DELETE [.SUB]A.TXT;*,B.TXT;*: B is taken from [.SUB] (#10);
 *   7. COPY A B,C is %DCL-W-NOLIST (#15);
 *   8. PURGE A,B purges both (#8).
 * No /dev/vms -> SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "stsdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) return -1;
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    close(in_pipe[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}


#define D "VDA0:[OVMXDIR.PLIST]"

/* append to a fixed buffer; a fixture that would not fit is a test bug -- fail, never truncate */
#define APPENDF(buf, u, ...) do { \
        int n_ = snprintf((buf) + (u), sizeof (buf) - (u), __VA_ARGS__); \
        if (n_ < 0 || (size_t)n_ >= sizeof (buf) - (u)) { \
            printf("  FAIL: fixture script overflows %zu bytes\n", sizeof (buf)); \
            return 1; \
        } \
        (u) += (size_t)n_; \
    } while (0)

static char out[16384];

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_plist (file verbs take a P1 list of any length, rd vms-ba2) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_plist: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    (void)vms_kif_acp_mount("VDA0:");   /* already mounted by an earlier suite is fine */

    /* the fixture: [OVMXDIR.PLIST] A..L.TXT (one record each) and [.SUB]B.TXT */
    static char setup[4096];
    size_t u = 0;
    APPENDF(setup, u, "CREATE/DIRECTORY " D "\nCREATE/DIRECTORY VDA0:[OVMXDIR.PLIST.SUB]\nSET DEFAULT " D "\n");
    for (char c = 'A'; c <= 'L'; c++)
        APPENDF(setup, u, "OPEN/WRITE F %c.TXT\nWRITE F \"REC-%c\"\nCLOSE F\n", c, c);
    APPENDF(setup, u, "OPEN/WRITE F [.SUB]B.TXT\nWRITE F \"SUB-B\"\nCLOSE F\nDIRECTORY\n");
    CHECK(run_dcl(setup, out, sizeof out) == 0, "DCL built the fixture");
    CHECK(strstr(out, "L.TXT;1") != NULL, "A..L.TXT exist in " D);

    /* 2: two parameters to a one-parameter verb -- refused whole */
    run_dcl("SET DEFAULT " D "\nDELETE K.TXT;* L.TXT;*\nSHOW SYMBOL $STATUS\nDIRECTORY K.TXT,L.TXT\n",
            out, sizeof out);
    printf("  ---- MAXPARM ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "%DCL-W-MAXPARM, too many parameters - reenter command with fewer parameters") &&
          strstr(out, " \\L\\"), "DELETE k l is %DCL-W-MAXPARM \\L\\ (oracle #2)");
    CHECK(strstr(out, "%X00038098") != NULL, "... $STATUS %X00038098");
    CHECK(strstr(out, "K.TXT;1") && strstr(out, "L.TXT;1"), "... and deleted nothing");

    /* 3: TYPE a list */
    run_dcl("SET DEFAULT " D "\nTYPE A.TXT,B.TXT,C.TXT\n", out, sizeof out);
    printf("  ---- TYPE list ----\n%s  ---- end ----\n", out);
    {
        const char *a = strstr(out, "REC-A"), *hb = strstr(out, "PLIST]B.TXT;1"),
                   *b = strstr(out, "REC-B"), *hc = strstr(out, "PLIST]C.TXT;1"),
                   *c = strstr(out, "REC-C");
        CHECK(a && b && c && a < b && b < c, "TYPE A,B,C prints all three in order (oracle #3)");
        CHECK(hb && hc && a < hb && hb < b && b < hc && hc < c,
              "... B and C each headed by its full name, A not (oracle #3)");
    }

    /* 4: COPY a list to one file concatenates */
    run_dcl("SET DEFAULT " D "\nCOPY A.TXT,B.TXT C2.TXT\nTYPE C2.TXT\nDIRECTORY C2.TXT\n", out, sizeof out);
    printf("  ---- COPY concatenate ----\n%s  ---- end ----\n", out);
    {
        const char *a = strstr(out, "REC-A"), *b = strstr(out, "REC-B");
        CHECK(a && b && a < b, "COPY A,B C2: C2 holds A then B (oracle #4)");
        CHECK(strstr(out, "C2.TXT;1") && !strstr(out, "C2.TXT;2"), "... in ONE file, C2.TXT;1");
    }

    /* 5: DIRECTORY a list */
    run_dcl("SET DEFAULT " D "\nDIRECTORY A.TXT,B.TXT,C.TXT\n", out, sizeof out);
    printf("  ---- DIRECTORY list ----\n%s  ---- end ----\n", out);
    {
        const char *h = strstr(out, "Directory VDA0:[OVMXDIR.PLIST]");
        CHECK(h && !strstr(h + 1, "Directory "), "DIRECTORY A,B,C: one header (oracle #7)");
        CHECK(strstr(out, "A.TXT;1") && strstr(out, "B.TXT;1") && strstr(out, "C.TXT;1") &&
              strstr(out, "Total of 3 files."), "... three files, Total of 3 files.");
    }

    /* 8: PURGE a list (a second version of A and B first) */
    run_dcl("SET DEFAULT " D "\nCOPY A.TXT A.TXT\nCOPY B.TXT B.TXT\nPURGE A.TXT,B.TXT\n"
            "SHOW SYMBOL $STATUS\nDIRECTORY A.TXT,B.TXT\n", out, sizeof out);
    printf("  ---- PURGE list ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "A.TXT;2") && strstr(out, "B.TXT;2") &&
          !strstr(out, "A.TXT;1") && !strstr(out, "B.TXT;1"),
          "PURGE A,B keeps only the newest of each (oracle #8)");

    /* 6: sticky directory */
    run_dcl("SET DEFAULT " D "\nDELETE [.SUB]A.TXT;*,B.TXT;*\nDIRECTORY [.SUB]\nDIRECTORY B.TXT\n",
            out, sizeof out);
    printf("  ---- sticky ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "%DIRECT-W-NOFILES") != NULL,
          "DELETE [.SUB]A,B took B from [.SUB] (oracle #10)");
    CHECK(strstr(out, "Directory VDA0:[OVMXDIR.PLIST]") && strstr(out, "B.TXT;2"),
          "... and left [OVMXDIR.PLIST]B.TXT alone");

    /* 7: a list in P2 */
    run_dcl("SET DEFAULT " D "\nCOPY A.TXT B.TXT,C.TXT\nSHOW SYMBOL $STATUS\n", out, sizeof out);
    printf("  ---- NOLIST ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "%DCL-W-NOLIST, list of parameter values not allowed - check use of comma (,)") &&
          strstr(out, " \\B.TXT,C\\") && strstr(out, "%X000380C0"),
          "COPY a b,c is %DCL-W-NOLIST \\B.TXT,C\\, %X000380C0 (oracle #15)");

    /* 1: twelve elements -- past P8 -- all deleted (last: it empties the fixture) */
    run_dcl("SET DEFAULT " D "\n"
            "DELETE A.TXT;*,B.TXT;*,C.TXT;*,D.TXT;*,E.TXT;*,F.TXT;*,G.TXT;*,H.TXT;*,"
            "I.TXT;*,J.TXT;*,K.TXT;*,L.TXT;*\nSHOW SYMBOL $STATUS\nDELETE C2.TXT;*\nDIRECTORY\n",
            out, sizeof out);
    printf("  ---- 12-element DELETE ----\n%s  ---- end ----\n", out);
    CHECK(!strstr(out, "MAXPARM") && strstr(out, "$STATUS == \"%X00000001\""),
          "DELETE of a 12-element list succeeds, no MAXPARM (oracle #1)");
    CHECK(!strstr(out, ".TXT;1"), "... every one of the twelve is gone");

    run_dcl("SET DEFAULT VDA0:[OVMXDIR]\nSET FILE/PROTECTION=O:RWED [.PLIST]SUB.DIR;1\n"
            "DELETE [.PLIST]SUB.DIR;1\nSET FILE/PROTECTION=O:RWED PLIST.DIR;1\nDELETE PLIST.DIR;1\n",
            out, sizeof out);

    printf("=== test_syssvc_dcl_plist: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
