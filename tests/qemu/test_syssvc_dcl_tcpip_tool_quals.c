/*
 * test_syssvc_dcl_tcpip_tool_quals.c - DCL TELNET and FTP refuse an
 * unrecognized qualifier before the client is entered (vms-097), driven
 * through the shipped DCL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. TELNET and FTP were accept-all verbs (quals == NULL):
 * TELNET/BOGUS host connected with the qualifier silently dropped, and
 * FTP/BOGUS carried on as if it had been honoured. Now each declares exactly
 * the qualifiers its handler reads (TELNET none; FTP /PORT /USER /PASSWORD
 * /GET /PUT /REMOTE /OUTPUT) and anything else is %DCL-W-IVQUAL.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-tcpip-tool-quals-accept-all.defect.
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
    printf("=== test_syssvc_dcl_tcpip_tool_quals (TELNET/FTP refuse unknown qualifiers) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_tcpip_tool_quals: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    static char out[16384];
    CHECK(run_dcl("TELNET/ZQBOGUS 127.0.0.1\nFTP/ZQBOGUS 127.0.0.1\nFTP/PORT=21 127.0.0.1\n", out, sizeof out) == 0,
          "DCL ran the TELNET/FTP qualifier probes");
    printf("  ---- probes ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-tcpip-tool-quals-accept-all */
    CHECK(strstr(out, "%DCL-W-IVQUAL") != NULL && strstr(out, "\\ZQBOGUS\\") != NULL,
          "TELNET/ZQBOGUS and FTP/ZQBOGUS are %DCL-W-IVQUAL naming the qualifier");
    /* negctl: dcl-tcpip-tool-quals-accept-all */
    CHECK(strstr(out, "%TELNET-I-TRYING") == NULL,
          "TELNET/ZQBOGUS never reaches the client (no connection attempt)");
    CHECK(strstr(out, "%FTP-E-NOOP") != NULL,
          "FTP/PORT=21 is a declared qualifier: it reaches the handler (not IVQUAL)");

    printf("=== test_syssvc_dcl_tcpip_tool_quals: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
