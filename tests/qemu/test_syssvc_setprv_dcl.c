/*
 * test_syssvc_setprv_dcl.c - DCL's "SET PROCESS/PRIVILEGES" routes the
 * privilege MUTATION through the EXECUTIVE, which OWNS and AUTHORIZES it
 * (vms-e5d7, the DCL-surface remainder of vms-pv1).
 *
 * WHY THIS SUITE EXISTS, AND WHAT IT GUARDS AGAINST.
 *
 * tests/qemu/test_syssvc_setprv.c already proves the SERVICE sys$setprv is
 * executive-owned (mediation / denial / cross-process). But before vms-e5d7,
 * the DCL command a user actually types -- SET PROCESS/PRIVILEGES=(...) --
 * never reached sys$setprv at all: src/vmsdcl/dcl_cmd_set.c gated the command
 * behind a userspace enforced_privs_held() pre-check and then printed
 * "%OVMX-I-NOSETPRV ... no privileges were changed" (the HIDE answer,
 * CLAUDE.md Rule 10). This suite is the A-writes / B-reads proof that the
 * command now MATCHES VMS end to end:
 *
 *   P1-P3 (AUTHORIZED, run as root -> executive derives SETPRV): a single
 *         DCL.EXE runs SET PROCESS/PRIVILEGES=ALL then SHOW
 *         PROCESS/PRIVILEGES. The command reports success (no error text),
 *         and SHOW -- which reads the executive fresh (dcl_cmd_show.c ->
 *         vms_kif_getjpi_self) -- reflects the enabled mask. The stale
 *         %OVMX-I-NOSETPRV facade must be GONE.
 *
 *   P4-P5 (NEGATION, still root): SET PROCESS/PRIVILEGES=(NOWORLD) in the
 *         same session drops WORLD while CMKRNL stays -- disabling is always
 *         allowed (docs/oracle/vax73-privileges.md §3) and the NO<priv> form
 *         is the VMS negation syntax.
 *
 *   P6-P8 (UNAUTHORIZED -- the security property): a DCL.EXE forked with
 *         DROPPED Linux credentials (so the executive derives no SETPRV for
 *         it, only the default privileges) runs SET PROCESS/PRIVILEGES=CMKRNL
 *         -- a privilege outside its authorized mask. The executive REFUSES
 *         to widen it and returns SS$_NOTALLPRIV, so DCL prints the
 *         oracle-pinned %SYSTEM-W-NOTALLPRIV message
 *         (docs/oracle/vax73-privileges.md §1), and its own SHOW
 *         PROCESS/PRIVILEGES shows CMKRNL NOT held. A process cannot award
 *         itself through DCL a privilege it is not entitled to.
 *
 * The refusal MESSAGE is grounded, not invented: its text/ident/severity/
 * facility are the F$MESSAGE round-trips pinned in
 * docs/oracle/vax73-privileges.md §1, and the STATUS the executive returns
 * for reaching outside the authorized mask is pinned in §3. See §8 for the
 * one residual (bare pass-through vs %SET- wrapper) flagged for sign-off.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no /dev/vms -- which
 * happens ONLY in the CI negative-control rig, never in the product
 * (vms-0ff: PID 1 refuses to boot without the executive) -- it runs the
 * device-absent no-fabricated-success checks and exits EXIT_SKIP (77).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/* Credentials for the unprivileged child. setuid() away from root is what
 * makes the executive derive only the default privileges (no SETPRV) for it.
 * Same mechanism (and same numbers) as tests/qemu/test_syssvc_setprv.c. */
#define C_GID   402
#define C_UID   1005

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do {                                 \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }  \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }  \
    } while (0)

/* Oracle-pinned message text (docs/oracle/vax73-privileges.md §1, F$MESSAGE
 * round-trips on VAX1, OpenVMS VAX V7.3). dcl_cmd_set.c emits these with the
 * SYSTEM facility and the pinned severity letter for the executive status. */
#define MSG_NOTALLPRIV "%SYSTEM-W-NOTALLPRIV, not all requested privileges authorized"

/* The stale HIDE facade this item removes -- must never appear again. */
#define MSG_STALE_HIDE "NOSETPRV"

/* The executive-UNREACHABLE report used only in device_absent_checks(). NOT
 * oracle-pinned and must not be: "the executive is not there" is a condition
 * OpenVMS never faces, so dcl_cmd_set.c reports it under the OVMX facility
 * rather than inventing a %SYSTEM- message (Rule 10). */
#define MSG_UNREACHABLE "%OVMX-F-SETPRVFAIL"

static int write_full(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

/*
 * run_dcl_creds - feed `script` to /bin/DCL.EXE and capture its output to
 * completion. If drop_creds is set, the child setgid/setuid away from root
 * BEFORE exec, so the DCL.EXE registers with the executive under the dropped
 * credentials and the executive derives its unprivileged mask.
 *
 * Returns 0 on success. Modeled on tests/qemu/test_syssvc_setname.c's
 * run_dcl(); credential drop mirrors test_syssvc_setprv.c's child.
 */
static int run_dcl_creds(const char *script, char *out, size_t outsz,
                         int drop_creds)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        if (drop_creds) {
            /* Drop group then user, BEFORE exec, so DCL.EXE's first
             * vms_kif_* call registers under the dropped credentials. */
            if (setgid(C_GID) != 0 || setuid(C_UID) != 0) {
                printf("SETUP_FAIL dropcreds\n");
                fflush(stdout);
                _exit(2);
            }
        }
        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        printf("SETUP_FAIL exec\n");
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    (void)write_full(in_pipe[1], script, strlen(script));
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

static int run_dcl(const char *script, char *out, size_t outsz)
{
    return run_dcl_creds(script, out, outsz, 0);
}

/* in_proc_privs - is `name` listed under SHOW PROCESS/PRIVILEGES's
 * "Process privileges:" block (the CURRENT, executive-held mask)? The block
 * prints one privilege per line as " %-20s %s" (dcl_cmd_show.c, oracle §4),
 * so the row begins "\n NAME " with the name left-padded. Searching only
 * AFTER the "Process privileges:" header excludes the "Authorized
 * privileges:" grid printed before it, so a privilege that is authorized but
 * not currently enabled does not false-match. */
static int in_proc_privs(const char *out, const char *name)
{
    const char *hdr = strstr(out, "Process privileges:");
    if (!hdr) return 0;
    char needle[48];
    snprintf(needle, sizeof(needle), "\n %s ", name);
    return strstr(hdr, needle) != NULL;
}

/* Any error/diagnostic prefix -- used to assert a successful command is
 * SILENT, the way VMS is (oracle §3). */
static int has_any_error(const char *out)
{
    return strstr(out, "%SYSTEM-") || strstr(out, "%CLI-") ||
           strstr(out, "%OVMX-")   || strstr(out, "%SET-");
}

/* ---------------------------------------------------------------------
 * Device-absent negative control: with no /dev/vms, SET PROCESS/PRIVILEGES
 * must NOT fabricate a success, and must NOT invent a %SYSTEM- message for a
 * condition VMS never faces -- it reports the OVMX-branded honest refusal.
 * --------------------------------------------------------------------- */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    static char out[65536];

    /* The service itself must not report success with no executive. */
    uint64_t mask = 0;
    uint32_t st = sys$setprv(1, &mask, 0, NULL);
    /* mask==0 is a no-op set; force a real bit so the executive path matters. */
    {
        uint64_t m = 1; /* CMKRNL */
        st = sys$setprv(1, &m, 0, NULL);
    }
    CHECK(!(st & 1),
          "sys$setprv does NOT report success when the executive was never reached");

    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  (no /bin/DCL.EXE -- the SET PROCESS/PRIVILEGES assertion cannot run here)\n");
    } else if (run_dcl("SET PROCESS/PRIVILEGES=CMKRNL\nLOGOUT\n",
                       out, sizeof(out)) == 0) {
        CHECK(strstr(out, "SETUP_FAIL") == NULL,
              "DCL.EXE ran for the device-absent SET PROCESS/PRIVILEGES check");
        CHECK(strstr(out, MSG_UNREACHABLE) != NULL,
              "SET PROCESS/PRIVILEGES reports the executive UNREACHABLE with no "
              "executive, rather than fabricating a success");
        CHECK(strstr(out, MSG_STALE_HIDE) == NULL,
              "the stale %OVMX-I-NOSETPRV HIDE facade is gone even on the "
              "device-absent path");
    }

    printf("=== test_syssvc_setprv_dcl: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice
     * into a forked child's output. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* A broken pipe (a DCL.EXE that died before the script is written) must
     * be a named failure, not a SIGPIPE death -- see test_syssvc_setname.c. */
    signal(SIGPIPE, SIG_IGN);

    static char out[65536];

    printf("=== test_syssvc_setprv_dcl: SET PROCESS/PRIVILEGES routes through the executive ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  FAIL: /bin/DCL.EXE is not staged in this image\n");
        printf("=== test_syssvc_setprv_dcl: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* -------------------------------------------------------------
     * P1-P3. AUTHORIZED: this test runs as root, so the executive derives
     * SETPRV for the DCL.EXE it forks. SET PROCESS/PRIVILEGES=ALL enables
     * the mask and SHOW PROCESS/PRIVILEGES -- reading the executive fresh --
     * reflects it. One DCL.EXE process, so the SET and the SHOW address the
     * SAME executive row.
     * ------------------------------------------------------------- */
    CHECK(run_dcl("SET PROCESS/PRIVILEGES=ALL\n"
                  "SHOW PROCESS/PRIVILEGES\n"
                  "LOGOUT\n", out, sizeof(out)) == 0,
          "an authorized DCL.EXE ran SET PROCESS/PRIVILEGES=ALL ; SHOW");

    /* The command that SUCCEEDS is silent, and the stale HIDE facade is gone.
     * Cut the capture at the SHOW header so has_any_error() inspects only the
     * SET command's output, not SHOW's privilege descriptions. */
    {
        const char *show = strstr(out, "Authorized privileges:");
        size_t setlen = show ? (size_t)(show - out) : strlen(out);
        char setout[4096];
        size_t n = setlen < sizeof(setout) - 1 ? setlen : sizeof(setout) - 1;
        memcpy(setout, out, n);
        setout[n] = '\0';
        CHECK(!has_any_error(setout),
              "SET PROCESS/PRIVILEGES=ALL for an authorized process is silent (success)");
        CHECK(strstr(out, MSG_STALE_HIDE) == NULL,
              "the stale %OVMX-I-NOSETPRV HIDE facade is gone -- the command now MATCHES");
    }

    CHECK(in_proc_privs(out, "CMKRNL"),
          "SHOW PROCESS/PRIVILEGES reflects CMKRNL enabled by SET ...=ALL (executive mask)");
    CHECK(in_proc_privs(out, "SETPRV"),
          "SHOW PROCESS/PRIVILEGES reflects SETPRV enabled by SET ...=ALL (executive mask)");
    CHECK(in_proc_privs(out, "WORLD"),
          "SHOW PROCESS/PRIVILEGES reflects WORLD enabled by SET ...=ALL (executive mask)");
    if (!in_proc_privs(out, "CMKRNL")) {
        printf("  (P1-P3 output follows)\n%s\n  (end)\n", out);
    }

    /* -------------------------------------------------------------
     * P4-P5. NEGATION: NO<priv> disables (always allowed, oracle §3). Enable
     * everything, then drop only WORLD, and confirm WORLD is gone while
     * CMKRNL remains -- in one session so both address the same row.
     * ------------------------------------------------------------- */
    CHECK(run_dcl("SET PROCESS/PRIVILEGES=ALL\n"
                  "SET PROCESS/PRIVILEGES=(NOWORLD)\n"
                  "SHOW PROCESS/PRIVILEGES\n"
                  "LOGOUT\n", out, sizeof(out)) == 0,
          "an authorized DCL.EXE ran the NO<priv> negation script");
    CHECK(!in_proc_privs(out, "WORLD"),
          "SET PROCESS/PRIVILEGES=(NOWORLD) disabled WORLD (executive mask)");
    CHECK(in_proc_privs(out, "CMKRNL"),
          "the NOWORLD disable left CMKRNL enabled -- only the named privilege dropped");

    /* -------------------------------------------------------------
     * P6-P8. UNAUTHORIZED -- the security property. A DCL.EXE forked with
     * DROPPED credentials holds no SETPRV and is not authorized for CMKRNL.
     * SET PROCESS/PRIVILEGES=CMKRNL must be REFUSED by the executive with
     * SS$_NOTALLPRIV, DCL must print the oracle-pinned %SYSTEM-W-NOTALLPRIV
     * message, and the process's own SHOW must show CMKRNL NOT held. A
     * per-process fake grant (the vms-b2e LARP) would have "succeeded" here.
     * ------------------------------------------------------------- */
    CHECK(run_dcl_creds("SET PROCESS/PRIVILEGES=CMKRNL\n"
                        "SHOW PROCESS/PRIVILEGES\n"
                        "LOGOUT\n", out, sizeof(out), 1) == 0,
          "an unauthorized (credential-dropped) DCL.EXE ran SET PROCESS/PRIVILEGES=CMKRNL");
    CHECK(strstr(out, "SETUP_FAIL") == NULL,
          "the credential-dropped DCL.EXE started (setgid/setuid + exec)");
    /* negctl: setprv-grants-unauthorized */
    CHECK(strstr(out, MSG_NOTALLPRIV) != NULL,
          "an unauthorized SET PROCESS/PRIVILEGES=CMKRNL prints the oracle-pinned "
          "%SYSTEM-W-NOTALLPRIV, not a fabricated success");
    /* negctl: setprv-grants-unauthorized */
    CHECK(!in_proc_privs(out, "CMKRNL"),
          "SHOW PROCESS/PRIVILEGES shows CMKRNL NOT held for the unauthorized "
          "process -- the executive refused to widen it");
    if (strstr(out, MSG_NOTALLPRIV) == NULL) {
        printf("  (P6-P8 output follows)\n%s\n  (end)\n", out);
    }

    printf("=== test_syssvc_setprv_dcl: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
