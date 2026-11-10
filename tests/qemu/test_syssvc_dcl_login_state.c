/*
 * test_syssvc_dcl_login_state.c - DCL learns that it is a login, its CAPTIVE
 * flag and its login command file from the EXECUTIVE, never from argv
 * (rd vms-838b), against a real /dev/vms.
 *
 * On VMS, LOGINOUT builds the process and DCL reads its login data from process
 * state. Here LOGINOUT stores it with VMS_IOCTL_LGI (vms_kif_lgi SET) under the
 * SETIDENT gate, and DCL.EXE -- started with NO arguments -- reads it back. Each
 * case runs the shipped DCL.EXE in a LOGIN process the suite makes
 * (ovmx_fork_login); the child plays LOGINOUT's part, stores the login data,
 * and execs DCL. The login command file L.COM writes LGI-RAN; the commands fed
 * on SYS$INPUT write REPL-REACHED.
 *
 *   1. The executive: a login process may store login data and reads it back
 *      byte-exact; a process that is NOT logging in is refused SS$_NOPRIV (an
 *      image cannot rewrite its own CAPTIVE flag); an unknown flag bit is
 *      SS$_BADPARAM; a process nobody logged in reads flags 0.
 *   2. LOGIN, not captive: DCL runs the LGICMD file, then reads commands.
 *   3. LOGIN + UAI$M_CAPTIVE: DCL runs the LGICMD file and logs out -- the
 *      commands after it are never read (no "$" prompt).
 *   4. No login data: a plain non-login DCL -- the LGICMD file is not run.
 *   5. The retired flags on argv (--login --captive --lgicmd <file>) and no
 *      login data: DCL ignores them -- it is a plain DCL, there is no argv
 *      fallback.
 * No /dev/vms -> SKIP (77).
 * NEGATIVE CONTROLS: tests/qemu/defects.d/lgi-set-outside-login.defect,
 * dcl-captive-from-lgi-ignored.defect.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "starlet.h"
#include "descrip.h"
#include "rms/rms.h"
#include "lnmdef.h"
#include "uaidef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"
#define LGI_LOGICAL "LGI838B"
#define LGI_SPEC LGI_LOGICAL ":[OVMXDIR]OVMX838B_L.COM"
/* @ reaches a procedure only through RMS (vms-20c94), so L.COM lives on the
 * mounted ODS-2 volume, written with $CREATE/$PUT, never a host /tmp file. */
#define LGI_UNIT "VDA0:"
#define LGI_DIR  LGI_UNIT "[OVMXDIR]"
#define LGI_FILE LGI_DIR "OVMX838B_L.COM"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

/* How the child starts DCL. */
enum lgi_case { C_NONE, C_LOGIN, C_CAPTIVE, C_ARGV };

/*
 * Run DCL.EXE in a fresh login process. The child stores the case's login data
 * (as LOGINOUT does), then execs DCL. Returns DCL's output in out.
 */
static int run_dcl(enum lgi_case c, const char *script, char *out, size_t outsz)
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
        if (c == C_LOGIN || c == C_CAPTIVE) {
            uint32_t fl = VMS_LGI_F_LOGIN;
            uint32_t uaf = (c == C_CAPTIVE) ? UAI$M_CAPTIVE : 0;
            char spec[] = LGI_SPEC;
            uint32_t st = vms_kif_lgi(VMS_LGI_OP_SET, &fl, &uaf, spec, 0);
            if (!(st & 1)) {
                printf("LGI-SET-REFUSED %u\n", (unsigned)st);
                fflush(stdout);
                _exit(2);
            }
        }
        if (c == C_ARGV)
            execle(DCL_PATH, "DCL.EXE", "--login", "--captive", "--lgicmd",
                   LGI_SPEC, (char *)NULL, dcl_env);
        else
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

/* The executive half, in a child of the given kind; exit code = failures. */
static int child_exec_checks(int login)
{
    int bad = 0;
    uint32_t fl = 0, uaf = 0;
    char cmd[VMS_LGICMD_SIZE];

    if (!login) {
        /* A process nobody logged in: flags 0, and it may not store any. */
        cmd[0] = 'x'; cmd[1] = '\0';
        uint32_t st = vms_kif_lgi(VMS_LGI_OP_GET, &fl, &uaf, cmd, sizeof cmd);
        if (!(st & 1) || fl != 0 || uaf != 0 || cmd[0] != '\0') {
            printf("  INFO: non-login GET st=%u flags=%u uaf=%u cmd=\"%s\"\n",
                   (unsigned)st, (unsigned)fl, (unsigned)uaf, cmd);
            bad |= 1;
        }
        fl = VMS_LGI_F_LOGIN; uaf = 0;
        char spec[] = LGI_SPEC;
        st = vms_kif_lgi(VMS_LGI_OP_SET, &fl, &uaf, spec, 0);
        if (st != SS$_NOPRIV) {
            printf("  INFO: non-login SET st=%u (want SS$_NOPRIV)\n", (unsigned)st);
            bad |= 2;
        }
        return bad;
    }
    uint32_t badfl = 0x2, z = 0;
    char spec[] = LGI_SPEC;
    uint32_t st = vms_kif_lgi(VMS_LGI_OP_SET, &badfl, &z, spec, 0);
    if (st != SS$_BADPARAM) {
        printf("  INFO: SET unknown flag st=%u (want SS$_BADPARAM)\n", (unsigned)st);
        bad |= 4;
    }
    fl = VMS_LGI_F_LOGIN; uaf = UAI$M_CAPTIVE | UAI$M_DISCTLY;
    st = vms_kif_lgi(VMS_LGI_OP_SET, &fl, &uaf, spec, 0);
    if (!(st & 1)) {
        printf("  INFO: login SET st=%u\n", (unsigned)st);
        bad |= 8;
    }
    fl = 0; uaf = 0; cmd[0] = '\0';
    st = vms_kif_lgi(VMS_LGI_OP_GET, &fl, &uaf, cmd, sizeof cmd);
    if (!(st & 1) || fl != VMS_LGI_F_LOGIN ||
        uaf != (UAI$M_CAPTIVE | UAI$M_DISCTLY) || strcmp(cmd, LGI_SPEC) != 0) {
        printf("  INFO: login GET st=%u flags=%u uaf=%#x cmd=\"%s\"\n",
               (unsigned)st, (unsigned)fl, (unsigned)uaf, cmd);
        bad |= 16;
    }
    return bad;
}

static int run_child_checks(int login)
{
    fflush(NULL);
    pid_t pid = login ? ovmx_fork_login() : ovmx_fork_subprocess();
    if (pid < 0) return -1;
    if (pid == 0)
        _exit(child_exec_checks(login));
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

/* A procedure as variable-length carriage-return records, one $PUT a line. */
static int write_com_rms(const char *spec, const char *text)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_fac = FAB$M_PUT;
    if (!(sys$create(&fab, 0, 0) & 1)) return -1;
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    if (!(sys$connect(&rab, 0, 0) & 1)) { sys$close(&fab, 0, 0); return -1; }
    uint32_t st = RMS$_NORMAL;
    while (*text && (st & 1)) {
        const char *nl = strchr(text, '\n');
        size_t n = nl ? (size_t)(nl - text) : strlen(text);
        rab.rab$l_rbf = (char *)text;
        rab.rab$w_rsz = (uint16_t)n;
        st = sys$put(&rab, 0, 0);
        text += n + (nl ? 1 : 0);
    }
    uint32_t cst = sys$close(&fab, 0, 0);
    return ((st & 1) && (cst & 1)) ? 0 : -1;
}

static void erase_rms(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

static uint32_t define_job_logical(const char *name, const char *val)
{
    static const char tab[] = "LNM$JOB";
    struct dsc$descriptor_s td = { sizeof tab - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)tab };
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    struct item_list_3 il[2];
    memset(il, 0, sizeof il);
    il[0].buflen = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr = (void *)val;
    return sys$crelnm(NULL, &td, &nd, NULL, il);
}

static void delete_job_logical(const char *name)
{
    static const char tab[] = "LNM$JOB";
    struct dsc$descriptor_s td = { sizeof tab - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)tab };
    struct dsc$descriptor_s nd = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    (void)sys$dellnm(&td, &nd, NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_login_state (DCL login data from the executive, not argv) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_login_state: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    /* 1. The executive half. */
    int r = run_child_checks(0);
    /* negctl: lgi-set-outside-login */
    CHECK(r == 0, "a process nobody logged in reads no login data and may not store any (SS$_NOPRIV)");
    r = run_child_checks(1);
    CHECK(r == 0, "a login process stores login data (bad flag SS$_BADPARAM) and reads it back byte-exact");

    /* The login command file, reached through a JOB logical every DCL below
     * (a subprocess of this suite's job) translates. */
    if (!(vms_kif_acp_mount(LGI_UNIT) & 1)) {
        printf("  FAIL: mount " LGI_UNIT "\n"); return 1;
    }
    if (write_com_rms(LGI_FILE, "$ WRITE SYS$OUTPUT \"LGI-RAN\"\n") != 0) {
        printf("  FAIL: write " LGI_FILE " through RMS\n"); return 1;
    }
    uint32_t lst = define_job_logical(LGI_LOGICAL, LGI_UNIT);
    CHECK((lst & 1) || lst == SS$_SUPERSEDE, "LNM$JOB " LGI_LOGICAL " names the login command file's volume (precondition)");

    static const char script[] = "WRITE SYS$OUTPUT \"REPL-REACHED\"\n";
    static char out[16384];

    /* 2. LOGIN, not captive. */
    CHECK(run_dcl(C_LOGIN, script, out, sizeof out) == 0, "DCL ran (login)");
    printf("  ---- login ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "LGI-SET-REFUSED") == NULL,
          "login: the executive took the login data");
    CHECK(strstr(out, "LGI-RAN") != NULL,
          "login: DCL ran the login command file the executive holds (LGICMD)");
    CHECK(strstr(out, "REPL-REACHED") != NULL,
          "login: a non-captive account reaches DCL command level after it");

    /* 3. LOGIN + CAPTIVE. */
    CHECK(run_dcl(C_CAPTIVE, script, out, sizeof out) == 0, "DCL ran (captive)");
    printf("  ---- captive ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "LGI-RAN") != NULL,
          "captive: DCL ran the login command file");
    /* negctl: dcl-captive-from-lgi-ignored */
    CHECK(strstr(out, "REPL-REACHED") == NULL,
          "captive: DCL logged out after it -- no command was read at DCL level");

    /* 4. No login data: a plain DCL. */
    CHECK(run_dcl(C_NONE, script, out, sizeof out) == 0, "DCL ran (no login data)");
    printf("  ---- plain ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "LGI-RAN") == NULL,
          "plain: a DCL nobody logged in runs no login command file");
    CHECK(strstr(out, "REPL-REACHED") != NULL, "plain: DCL reads commands");

    /* 5. The retired argv flags, no login data: no argv fallback. */
    CHECK(run_dcl(C_ARGV, script, out, sizeof out) == 0, "DCL ran (argv flags, no login data)");
    printf("  ---- argv ----\n%s  ---- end ----\n", out);
    CHECK(strstr(out, "LGI-RAN") == NULL,
          "argv: --login/--lgicmd on the command line do not make a login (no argv fallback)");
    CHECK(strstr(out, "REPL-REACHED") != NULL,
          "argv: --captive on the command line does not confine DCL");

    delete_job_logical(LGI_LOGICAL);
    erase_rms(LGI_FILE);
    printf("=== test_syssvc_dcl_login_state: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
