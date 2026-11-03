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
 *   - a DETACHED process's DCL reports OTHER;
 *   - a network object's server process -- $CREPRC with PRC$M_NETWRK, as NETACP
 *     creates FAL -- is NETWORK: JPI$_MODE and JPI$_JOBTYPE both JPI$K_NETWORK
 *     (DCL Dictionary F$MODE; JPIDEF $EQU JPI$K_NETWORK 1), and its DCL's
 *     F$MODE() says so.
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
#include "descrip.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

/* detached: 0 = a login subprocess, 1 = a detached process, 2 = a detached
 * NETWORK process (VMS_CREPRC_PCB_F_NETWORK, what $CREPRC PRC$M_NETWRK asks).
 * DCL's output comes back from its SYS$OUTPUT mailbox (rd vms-2f39). */
static int run_dcl(int detached, const char *script, char *out, size_t outsz)
{
    int in_pipe[2];
    struct ovmx_dcl_sysout so;
    out[0] = '\0';
    if (!(ovmx_dcl_sysout_open(&so) & 1)) return -1;
    if (pipe(in_pipe) < 0) { ovmx_dcl_sysout_collect(&so, NULL, 0); return -1; }
    fflush(NULL);
    pid_t pid;
    if (detached) {
        struct vms_procinfo me;
        memset(&me, 0, sizeof me);
        (void)vms_kif_getjpi_self(&me);
        pid = ovmx_fork_detached_flags_st(me.username, me.uic, VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX,
                                          detached == 2 ? VMS_CREPRC_PCB_F_NETWORK : 0u, NULL);
    } else {
        pid = ovmx_fork_login();
    }
    if (pid < 0) { ovmx_dcl_sysout_collect(&so, NULL, 0); return -1; }
    if (pid == 0) {
        ovmx_dcl_sysout_child(&so);
        dup2(in_pipe[0], STDIN_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    close(in_pipe[1]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    ovmx_dcl_sysout_collect(&so, out, outsz);
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

/* $CREPRC a hibernating detached DCL.EXE with `stsflg` and read its JPI$_MODE
 * and JPI$_JOBTYPE from its row by PID, then delete it. 0 if it ran. */
static int creprc_mode(uint32_t stsflg, uint32_t *mode, uint32_t *jobtype)
{
    static const char img[] = DCL_PATH;
    struct dsc$descriptor_s imgd;
    uint32_t pid = 0, st;
    uint16_t r1 = 0, r2 = 0;
    struct item_list_3 il[3];

    imgd.dsc$w_length = (uint16_t)strlen(img);
    imgd.dsc$b_dtype = DSC$K_DTYPE_T;
    imgd.dsc$b_class = DSC$K_CLASS_S;
    imgd.dsc$a_pointer = (char *)img;
    *mode = *jobtype = 99;
    st = sys$creprc(&pid, &imgd, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0,
                    stsflg | PRC$M_DETACH | PRC$M_HIBER);
    if (!(st & 1) || pid == 0)
        return -1;
    memset(il, 0, sizeof il);
    il[0].buflen = 4; il[0].item_code = JPI$_MODE;    il[0].bufaddr = mode;    il[0].retlen = &r1;
    il[1].buflen = 4; il[1].item_code = JPI$_JOBTYPE; il[1].bufaddr = jobtype; il[1].retlen = &r2;
    st = sys$getjpiw(0, &pid, NULL, il, NULL, NULL, 0);
    (void)sys$delprc(&pid, NULL);
    return (st & 1) ? 0 : -1;
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

    /* A network object's server process: NETACP's $CREPRC passes PRC$M_NETWRK. */
    uint32_t nm = 99, nj = 99;
    CHECK(creprc_mode(PRC$M_NETWRK, &nm, &nj) == 0,
          "$CREPRC PRC$M_NETWRK creates the process and $GETJPI reads it by PID");
    /* negctl: creprc-netwrk-not-classified */
    CHECK(nm == JPI$K_NETWORK, "a PRC$M_NETWRK process's JPI$_MODE is NETWORK");
    CHECK(nj == JPI$K_NETWORK, "a PRC$M_NETWRK process's JPI$_JOBTYPE is NETWORK");
    CHECK(creprc_mode(0, &nm, &nj) == 0 && nm == JPI$K_OTHER,
          "the same $CREPRC without PRC$M_NETWRK is OTHER (NETWORK is the flag's, not detachment's)");

    CHECK(run_dcl(2, "WRITE SYS$OUTPUT \"M=\" + F$MODE()\n", out, sizeof out) == 0,
          "DCL (a network process) ran");
    printf("  ---- network ----\n%s  ---- end ----\n", out);
    field(out, "M=", m, sizeof m);
    CHECK(strcmp(m, "NETWORK") == 0, "a network process's F$MODE() is NETWORK");

    printf("=== test_syssvc_dcl_fmode: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
