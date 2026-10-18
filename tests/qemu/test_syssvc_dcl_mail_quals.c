/*
 * test_syssvc_dcl_mail_quals.c - DCL MAIL validates its qualifiers and
 * forwards the one it honours to MAIL.EXE intact (vms-097), driven through
 * the shipped DCL.EXE and MAIL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. MAIL was accept-all, and cmd_mail forwarded each
 * qualifier as its bare NAME (no "/", no "=value"): MAIL/SUBJECT="hi" user
 * handed MAIL.EXE the word SUBJECT, which it took for the recipient, and the
 * subject was lost. Now MAIL declares /SUBJECT (the only qualifier MAIL.EXE
 * reads), forwards it as /SUBJECT=value, and any other qualifier is
 * %DCL-W-IVQUAL.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROLS: tests/qemu/defects.d/dcl-mail-quals-accept-all.defect
 * and dcl-mail-qual-bare-name.defect.
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
    printf("=== test_syssvc_dcl_mail_quals (MAIL qualifiers validated and forwarded) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_mail_quals: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    static char out[16384];
    CHECK(run_dcl("MAIL/ZQBOGUS NOSUCHUSR\nMAIL/SUBJECT=\"hi\" NOSUCHUSR\n", out, sizeof out) == 0,
          "DCL ran the MAIL qualifier probes");
    printf("  ---- MAIL probes ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-mail-quals-accept-all */
    CHECK(strstr(out, "%DCL-W-IVQUAL") != NULL && strstr(out, "\\ZQBOGUS\\") != NULL,
          "MAIL/ZQBOGUS is %DCL-W-IVQUAL naming the qualifier");
    /* negctl: dcl-mail-qual-bare-name */
    CHECK(strstr(out, "no such user NOSUCHUSR") != NULL,
          "MAIL/SUBJECT=.. reaches MAIL.EXE as a qualifier: the recipient is still NOSUCHUSR");
    CHECK(strstr(out, "no such user SUBJECT") == NULL,
          "the qualifier name is not mistaken for the recipient");

    printf("=== test_syssvc_dcl_mail_quals: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
