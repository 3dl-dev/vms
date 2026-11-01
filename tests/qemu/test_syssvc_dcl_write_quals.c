/*
 * test_syssvc_dcl_write_quals.c - DCL WRITE validates its qualifiers and
 * honours the ones it accepts (vms-097), driven through the shipped DCL.EXE
 * in the guest.
 *
 * THE BUG THIS GUARDS. WRITE was accept-all and did not even parse its
 * qualifiers: WRITE/ZQBOGUS chan text ran with the qualifier folded into the
 * argument list, and WRITE/SYMBOL SYS$OUTPUT X printed the channel name
 * instead of X's value. Now an unknown or unimplemented qualifier (/UPDATE)
 * is %DCL-W-IVQUAL, /SYMBOL writes the named symbol's value (an undefined
 * symbol is %DCL-W-UNDSYM), and /ERROR=label branches like GOTO on failure.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROLS: tests/qemu/defects.d/dcl-write-quals-accept-all.defect
 * and dcl-write-symbol-ignored.defect.
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
    printf("=== test_syssvc_dcl_write_quals (WRITE qualifiers validated and honoured) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_write_quals: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    char dir[] = "/tmp/ovmx_wr_XXXXXX";
    if (!mkdtemp(dir)) { printf("  FAIL: mkdtemp\n"); return 1; }
    char path[128];
    snprintf(path, sizeof path, "%s/T.COM", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const char com[] =
        "$ WRITE/ERROR=ERRL NOCHAN \"x\"\n"
        "$ WRITE SYS$OUTPUT \"NOT-REACHED\"\n"
        "$ EXIT\n"
        "$ ERRL:\n"
        "$ WRITE SYS$OUTPUT \"at-err\"\n";
    if (fd < 0 || write(fd, com, sizeof com - 1) != (ssize_t)(sizeof com - 1)) {
        printf("  FAIL: write T.COM\n"); return 1;
    }
    close(fd);

    static char out1[16384], out2[16384];
    char script[512];

    CHECK(run_dcl("SYM = \"long-record-value\"\nWRITE/SYMBOL SYS$OUTPUT SYM\nWRITE/SYMBOL SYS$OUTPUT NOSUCHSYM\n"
                  "WRITE/ZQBOGUS SYS$OUTPUT \"hi\"\nWRITE/UPDATE SYS$OUTPUT \"hi\"\n", out1, sizeof out1) == 0,
          "DCL ran the WRITE qualifier probes");
    printf("  ---- WRITE probes ----\n%s  ---- end ----\n", out1);
    /* negctl: dcl-write-symbol-ignored */
    CHECK(strstr(out1, "long-record-value") != NULL,
          "WRITE/SYMBOL writes the named symbol's value");
    CHECK(strstr(out1, "%DCL-W-UNDSYM") != NULL && strstr(out1, "\\NOSUCHSYM\\") != NULL,
          "WRITE/SYMBOL of an undefined symbol is %DCL-W-UNDSYM");
    /* ...and leaves the real CLI$_UNDSYM in $STATUS, %X00038140 (OpenVMS Alpha
     * V8.4 $GETMSG, docs/oracle/messages/alpha84-msgcat2.txt). */
    CHECK(run_dcl("WRITE/SYMBOL SYS$OUTPUT NOSUCHSYM\nSHOW SYMBOL $STATUS\n", out2, sizeof out2) == 0,
          "DCL ran the UNDSYM $STATUS probe");
    CHECK(strstr(out2, "%X00038140") != NULL,
          "WRITE/SYMBOL of an undefined symbol leaves $STATUS = %X00038140 (CLI$_UNDSYM)");
    /* negctl: dcl-write-quals-accept-all */
    CHECK(strstr(out1, "\\ZQBOGUS\\") != NULL,
          "WRITE/ZQBOGUS is %DCL-W-IVQUAL naming the qualifier");
    /* negctl: dcl-write-quals-accept-all */
    CHECK(strstr(out1, "\\UPDATE\\") != NULL,
          "WRITE/UPDATE (not implemented) is %DCL-W-IVQUAL, not silently accepted");

    snprintf(script, sizeof script,
             "DEFINE WRDIR \"%s\"\nSET DEFAULT WRDIR:[000000]\n@T.COM\n", dir);
    CHECK(run_dcl(script, out2, sizeof out2) == 0, "DCL ran the WRITE /ERROR procedure");
    printf("  ---- /ERROR procedure ----\n%s  ---- end ----\n", out2);
    /* negctl: dcl-write-error-ignored */
    CHECK(strstr(out2, "at-err") != NULL, "WRITE/ERROR=ERRL branched to ERRL when the write failed");
    CHECK(strstr(out2, "NOT-REACHED") == NULL, "control did not fall through past the failed WRITE");

    snprintf(path, sizeof path, "%s/T.COM", dir); unlink(path);
    rmdir(dir);
    printf("=== test_syssvc_dcl_write_quals: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
