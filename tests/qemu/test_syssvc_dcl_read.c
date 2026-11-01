/*
 * test_syssvc_dcl_read.c - DCL READ validates its qualifiers and honours the
 * ones it accepts (vms-097), driven through the shipped DCL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. READ was an accept-all verb: any /QUALIFIER was
 * silently taken, so READ/DELETE (not implemented) or a typo did something
 * other than what was asked with no diagnostic, and /END_OF_FILE=label /
 * /ERROR=label -- which real VMS programs use -- were swallowed unhonoured.
 * Now an unimplemented or unknown qualifier is %DCL-W-IVQUAL, and the two
 * label qualifiers branch like GOTO.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROLS: tests/qemu/defects.d/dcl-read-quals-accept-all.defect and
 * dcl-read-labels-ignored.defect.
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
    printf("=== test_syssvc_dcl_read (READ qualifiers validated and honoured) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_read: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    char dir[] = "/tmp/ovmx_rd_XXXXXX";
    if (!mkdtemp(dir)) { printf("  FAIL: mkdtemp\n"); return 1; }
    char path[128];
    snprintf(path, sizeof path, "%s/F.TXT", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, "one\n", 4) != 4) { printf("  FAIL: write F.TXT\n"); return 1; }
    close(fd);
    snprintf(path, sizeof path, "%s/T.COM", dir);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const char com[] =
        "$ DEFINE SYS$INPUT \"F.TXT\"\n"
        "$ READ/END_OF_FILE=EOFL/ERROR=ERRL SYS$INPUT REC\n"
        "$ WRITE SYS$OUTPUT \"first=''REC'\"\n"
        "$ READ/END_OF_FILE=EOFL/ERROR=ERRL SYS$INPUT REC\n"
        "$ WRITE SYS$OUTPUT \"NOT-REACHED\"\n"
        "$ EXIT\n"
        "$ EOFL:\n"
        "$ WRITE SYS$OUTPUT \"at-eof\"\n"
        "$ EXIT\n"
        "$ ERRL:\n"
        "$ WRITE SYS$OUTPUT \"wrong-err\"\n";
    if (fd < 0 || write(fd, com, sizeof com - 1) != (ssize_t)(sizeof com - 1)) {
        printf("  FAIL: write T.COM\n"); return 1;
    }
    close(fd);

    static char out1[16384], out2[16384];
    char script[512];

    snprintf(script, sizeof script,
             "DEFINE RDDIR \"%s\"\nSET DEFAULT RDDIR:[000000]\n@T.COM\n", dir);
    CHECK(run_dcl(script, out1, sizeof out1) == 0, "DCL ran the READ label procedure");
    printf("  ---- label procedure ----\n%s  ---- end ----\n", out1);
    CHECK(strstr(out1, "first=one") != NULL, "READ/END_OF_FILE=.. /ERROR=.. still reads a record normally");
    /* negctl: dcl-read-labels-ignored */
    CHECK(strstr(out1, "at-eof") != NULL,
          "READ/END_OF_FILE=EOFL branched to EOFL at end of file");
    /* negctl: dcl-read-labels-ignored */
    CHECK(strstr(out1, "NOT-REACHED") == NULL,
          "control did not fall through past the READ that hit end of file");
    CHECK(strstr(out1, "wrong-err") == NULL, "end of file went to /END_OF_FILE, not /ERROR");

    snprintf(script, sizeof script,
             "DEFINE RDDIR \"%s\"\nSET DEFAULT RDDIR:[000000]\nREAD/BOGUS SYS$INPUT X\nREAD/DELETE SYS$INPUT X\n", dir);
    CHECK(run_dcl(script, out2, sizeof out2) == 0, "DCL ran READ with unknown qualifiers");
    printf("  ---- bogus qualifiers ----\n%s  ---- end ----\n", out2);
    /* negctl: dcl-read-quals-accept-all */
    CHECK(strstr(out2, "IVQUAL") != NULL && strstr(out2, "\\BOGUS\\") != NULL,
          "READ/BOGUS is rejected as %DCL-W-IVQUAL naming the qualifier");
    /* negctl: dcl-read-quals-accept-all */
    CHECK(strstr(out2, "\\DELETE\\") != NULL,
          "READ/DELETE (not implemented) is rejected as %DCL-W-IVQUAL, not silently accepted");

    snprintf(path, sizeof path, "%s/F.TXT", dir); unlink(path);
    snprintf(path, sizeof path, "%s/T.COM", dir); unlink(path);
    rmdir(dir);
    printf("=== test_syssvc_dcl_read: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
