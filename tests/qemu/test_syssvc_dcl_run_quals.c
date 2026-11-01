/*
 * test_syssvc_dcl_run_quals.c - DCL refuses an unknown or ambiguous RUN
 * qualifier before RUN is entered (vms-097), driven through the shipped
 * DCL.EXE in the guest.
 *
 * ORACLE (VAX V7.3, captures/run-qualifier-abbrev-vax1-2026-07-31.txt):
 * RUN/<no such qualifier> is %DCL-W-IVQUAL and RUN/P=4 is %DCL-W-ABKEYW; a
 * resolved abbreviation (RUN/DETACH) is not refused. OVMX validated no RUN
 * qualifier, so a typo ran the image with the instruction silently dropped.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-run-quals-unvalidated.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
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

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_run_quals (RUN refuses unknown / ambiguous qualifiers) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_run_quals: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    static char out[16384];
    CHECK(run_dcl("RUN/ZQBOGUS NOSUCH_IMAGE\nRUN/P=4 NOSUCH_IMAGE\nRUN/DETACH NOSUCH_IMAGE\n", out, sizeof out) == 0,
          "DCL ran the RUN qualifier probes");
    printf("  ---- RUN probes ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-run-quals-unvalidated */
    CHECK(strstr(out, "%DCL-W-IVQUAL") != NULL && strstr(out, "\\ZQBOGUS\\") != NULL,
          "RUN/ZQBOGUS is %DCL-W-IVQUAL naming the qualifier (oracle)");
    /* negctl: dcl-run-quals-unvalidated */
    CHECK(strstr(out, "%DCL-W-ABKEYW") != NULL,
          "RUN/P=4 is %DCL-W-ABKEYW, an ambiguous abbreviation (oracle)");
    CHECK(strstr(out, "\\DETACH\\") == NULL,
          "RUN/DETACH resolves to /DETACHED and is NOT refused as an unknown qualifier");

    /* The condition the ambiguous abbreviation leaves in $STATUS is the real
     * CLI$_ABKEYW, %X00038010 (OpenVMS Alpha V8.4 $GETMSG: "%CLI-W-ABKEYW,
     * ambiguous qualifier or keyword - supply more characters",
     * docs/oracle/messages/alpha84-msgcat2.txt) -- not an invented DCL code. */
    CHECK(run_dcl("RUN/P=4 NOSUCH_IMAGE\nSHOW SYMBOL $STATUS\n", out, sizeof out) == 0,
          "DCL ran the ABKEYW $STATUS probe");
    printf("  ---- ABKEYW $STATUS ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "%X00038010") != NULL,
          "RUN/P=4 leaves $STATUS = %X00038010 (CLI$_ABKEYW)");

    printf("=== test_syssvc_dcl_run_quals: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
