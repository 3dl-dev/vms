/*
 * test_syssvc_dcl_fmode.c - F$MODE and F$GETJPI(...,"MODE") report the executive's
 * classification of the process ($GETJPI JPI$_MODE), driven through DCL.EXE.
 *
 * DCL Dictionary: F$MODE returns the mode of the process -- INTERACTIVE, BATCH,
 * NETWORK or OTHER -- the JPI$_MODE item. OVMX used to answer from whether DCL's
 * own input was a terminal, so any DCL fed from a pipe called itself BATCH on a
 * system with no batch engine. Now:
 *   - F$MODE() equals F$GETJPI("","MODE"), and is not BATCH (no batch job runs);
 *   - F$GETJPI(<this suite's PID>,"MODE") is what this suite reads for itself with
 *     $GETJPI JPI$_MODE (another process's mode, from its row);
 *   - a DETACHED process's DCL reports OTHER.
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "starlet.h"
#include "prcdef.h"
#include "lnmdef.h"
#include "prvdef.h"
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

/* detached: 0 = a login subprocess, 1 = a detached process. */
static int run_dcl(int detached, const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) return -1;
    fflush(NULL);
    pid_t pid;
    if (detached) {
        struct vms_procinfo me;
        memset(&me, 0, sizeof me);
        (void)vms_kif_getjpi_self(&me);
        pid = ovmx_fork_detached_st(me.username, me.uic, VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX, NULL);
    } else {
        pid = ovmx_fork_login();
    }
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

/* The value after "<tag>=" up to the end of its line. */
static void field(const char *out, const char *tag, char *v, size_t vsz)
{
    v[0] = '\0';
    const char *p = strstr(out, tag);
    if (!p) return;
    p += strlen(tag);
    size_t n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n < vsz - 1) { v[n] = p[n]; n++; }
    v[n] = '\0';
}

int main(void)
{
    static char out[8192];
    char script[512], m[32], g[32], o[32];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_fmode (F$MODE / F$GETJPI MODE from JPI$_MODE) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_fmode: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    /* This suite's own mode, as $GETJPI reads it. */
    uint32_t mode = 99, self = 0;
    uint16_t rl = 0, rl2 = 0;
    struct item_list_3 il[3];
    memset(il, 0, sizeof il);
    il[0].buflen = 4; il[0].item_code = JPI$_MODE; il[0].bufaddr = &mode; il[0].retlen = &rl;
    il[1].buflen = 4; il[1].item_code = JPI$_PID;  il[1].bufaddr = &self; il[1].retlen = &rl2;
    CHECK(sys$getjpiw(0, NULL, NULL, il, NULL, NULL, 0) & 1, "$GETJPI JPI$_MODE / JPI$_PID of this suite");
    const char *mine = mode == JPI$K_INTERACTIVE ? "INTERACTIVE" : mode == JPI$K_BATCH ? "BATCH"
                     : mode == JPI$K_NETWORK ? "NETWORK" : "OTHER";

    snprintf(script, sizeof script,
             "WRITE SYS$OUTPUT \"M=\" + F$MODE()\n"
             "WRITE SYS$OUTPUT \"G=\" + F$GETJPI(\"\",\"MODE\")\n"
             "WRITE SYS$OUTPUT \"O=\" + F$GETJPI(\"%08X\",\"MODE\")\n", self);
    CHECK(run_dcl(0, script, out, sizeof out) == 0, "DCL (a login subprocess, input from a pipe) ran");
    printf("  ---- subprocess ----\n%s  ---- end ----\n", out);
    field(out, "M=", m, sizeof m);
    field(out, "G=", g, sizeof g);
    field(out, "O=", o, sizeof o);
    /* negctl: dcl-fmode-from-terminal */
    CHECK(m[0] && strcmp(m, g) == 0 && strcmp(m, "BATCH") != 0,
          "F$MODE() is F$GETJPI(\"\",\"MODE\"), and not BATCH (no batch job is running)");
    CHECK(strcmp(o, mine) == 0, "F$GETJPI(<another process>,\"MODE\") is that process's JPI$_MODE");

    CHECK(run_dcl(1, "WRITE SYS$OUTPUT \"M=\" + F$MODE()\n", out, sizeof out) == 0,
          "DCL (a detached process) ran");
    printf("  ---- detached ----\n%s  ---- end ----\n", out);
    field(out, "M=", m, sizeof m);
    /* negctl: dcl-fmode-from-terminal */
    CHECK(strcmp(m, "OTHER") == 0, "a detached process's F$MODE() is OTHER");

    printf("=== test_syssvc_dcl_fmode: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
