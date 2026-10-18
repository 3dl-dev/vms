/*
 * test_syssvc_convert_dcl.c - DCL CONVERT does not claim a conversion it did
 * not perform (vms-558e), driven through the shipped DCL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. CONVERT accepted /FDL, printed "accepted but ignored",
 * then line-copied the file and printed "%CONVERT-S-CONVERTED, N records
 * converted" -- claiming a record-format conversion it never did. Now: a plain
 * CONVERT is a record-for-record copy and silent on success (as on VMS); /FDL,
 * which asks for a real conversion, is refused with %CONVERT-E-NOFDL and writes
 * nothing.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-convert-claims-converted.defect.
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
    printf("=== test_syssvc_convert_dcl (CONVERT claims no conversion it did not do) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_convert_dcl: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    char dir[] = "/tmp/ovmx_cvt_XXXXXX";
    if (!mkdtemp(dir)) { printf("  FAIL: mkdtemp\n"); return 1; }
    char inpath[128];
    snprintf(inpath, sizeof inpath, "%s/in.txt", dir);
    int fd = open(inpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const char body[] = "alpha\nbeta\ngamma\n";
    if (fd < 0 || write(fd, body, sizeof body - 1) != (ssize_t)(sizeof body - 1)) {
        printf("  FAIL: could not write the input file\n");
        return 1;
    }
    close(fd);

    static char out1[16384], out2[16384];
    char script[512];

    snprintf(script, sizeof script,
             "DEFINE CVTDIR \"%s\"\nSET DEFAULT CVTDIR:[000000]\nCONVERT in.txt out.txt\n", dir);
    CHECK(run_dcl(script, out1, sizeof out1) == 0, "DCL ran the plain CONVERT");
    printf("  ---- plain CONVERT ----\n%s  ---- end ----\n", out1);

    char outpath[128], got[256] = "";
    snprintf(outpath, sizeof outpath, "%s/out.txt", dir);
    fd = open(outpath, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, got, sizeof got - 1) : -1;
    if (fd >= 0) close(fd);
    if (n >= 0) got[n] = '\0';
    CHECK(n == (ssize_t)(sizeof body - 1) && strcmp(got, body) == 0,
          "plain CONVERT wrote a record-for-record copy of the input");
    CHECK(strstr(out1, "CONVERTED") == NULL,
          "plain CONVERT is silent on success: no invented \"CONVERTED\" claim");

    snprintf(script, sizeof script,
             "DEFINE CVTDIR \"%s\"\nSET DEFAULT CVTDIR:[000000]\nCONVERT/FDL=x.fdl in.txt fdl_out.txt\n", dir);
    CHECK(run_dcl(script, out2, sizeof out2) == 0, "DCL ran CONVERT/FDL");
    printf("  ---- CONVERT/FDL ----\n%s  ---- end ----\n", out2);
    char fdlpath[128];
    snprintf(fdlpath, sizeof fdlpath, "%s/fdl_out.txt", dir);
    /* negctl: dcl-convert-claims-converted */
    CHECK(strstr(out2, "CONVERT-E-NOFDL") != NULL,
          "CONVERT/FDL is refused (%CONVERT-E-NOFDL), not accepted-and-ignored");
    /* negctl: dcl-convert-claims-converted */
    CHECK(access(fdlpath, F_OK) != 0,
          "CONVERT/FDL wrote no output file (no unconverted copy passed off as converted)");

    unlink(inpath); unlink(outpath); unlink(fdlpath); rmdir(dir);
    printf("=== test_syssvc_convert_dcl: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
