/*
 * test_syssvc_authorize.c - AUTHORIZE's SYSPRV check has a POSITIVE control,
 * not only a negative one (vms-4c2, the deferred half of vms-b2e / PR #76).
 *
 * WHAT PR #76 ALREADY PROVED, on a host build with no /dev/vms: getenv("USER")
 * no longer decides who may manage SYSUAF. USER=baron / USER=SYSTEM /
 * USER=OPERATOR were ALL refused with %UAF-F-NOAUTH, where USER=SYSTEM used
 * to open a full SYSUAF-management session.
 *
 * WHAT IT DID NOT PROVE. The dev host has no /dev/vms, so every one of those
 * three refusals is equally explained by "the executive read failed" as by
 * "the SYSPRV bit was clear" -- tools/vms_authorize.c's check_privilege()
 * calls vms_kif_getjpi_self() and returns 0 (refuse) whenever that call does
 * not come back with status bit 0 set, which is exactly what happens with no
 * executive present. A check_privilege() that returned 0 UNCONDITIONALLY
 * would pass every test that ran on that host, and AUTHORIZE would be
 * bricked on the real runtime with nothing red.
 *
 * So this suite runs against a REAL, insmod'd /dev/vms and proves both
 * directions the same way tests/qemu/test_syssvc_ident.c proves identity:
 * a helper process stamps an AUTHENTICATED identity onto itself through the
 * executive (vms_kif_setident, which requires SETPRV to grant -- this suite
 * runs as the root-capable test harness process, which vms_proc_register()
 * gives a fully authorized mask), reads its own row back to confirm the
 * mask the executive is actually holding, and only THEN execs the real
 * shipped AUTHORIZE.EXE and captures what it printed.
 *
 *   A holds PRV$M_SYSPRV and must be ADMITTED (the banner prints).
 *   B does not hold PRV$M_SYSPRV and must be REFUSED (%UAF-F-NOAUTH).
 *
 * A and B are two different processes (two separate fork/exec pairs), so
 * neither run can be explained by state the other left behind.
 *
 * A check_privilege() that always returned 1 would pass A but fail B's
 * refusal check; one that always returned 0 would pass B but fail A's
 * admission check (and every prior host-build assertion would have looked
 * identical). Both directions have to hold at once for this suite to be
 * green, which is the property PR #76 left unproven.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. In the CI negative-control
 * rig (deliberately booted with no executive -- vms-0ff: PID 1 refuses to
 * boot without one, so this is a test RIG state, never a product state) it
 * exits EXIT_SKIP (77), never a fake pass: with no executive, AUTHORIZE
 * refuses EVERY identity, so the positive half (A) cannot be proven and
 * an unconditional-refuse implementation would look identical to a correct
 * one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

#define AUTHORIZE_PATH "/bin/AUTHORIZE.EXE"

/* Identity A: holds SYSPRV -- must be ADMITTED to manage SYSUAF. */
#define A_NAME   "SECURITY"
#define A_GRP    20u
#define A_MEM    5u
#define A_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV)

/* Identity B: a real, populated privilege mask that deliberately never sets
 * SYSPRV -- must be REFUSED. OPER and WORLD are included specifically so a
 * check_privilege() that tested "the mask is non-empty" or "the mask holds
 * some elevated bit" rather than the SYSPRV bit itself would fail B here. */
#define B_NAME   "FIELDREP"
#define B_GRP    21u
#define B_MEM    6u
#define B_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_OPER | PRV$M_WORLD)

static char *const authorize_env[] = {
    (char *)"PATH=/bin",
    NULL
};

/*
 * run_authorize - stamp an identity, exec the real AUTHORIZE, capture its
 * output.
 *
 * The child prints SETIDENT_STATUS and SELF_PRIVS on the captured stream
 * BEFORE exec'ing, so the identity this run actually established is part
 * of the evidence rather than something this file asserts from outside.
 * "EXIT\n" is fed on stdin so an admitted session exits cleanly instead of
 * hanging on the UAF> prompt; a refused session never reads it.
 */
static int run_authorize(const char *username, uint32_t uic, uint64_t privs,
                          char *out, size_t outsz, int *exit_code)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    *exit_code = -1;
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    /*
     * Flush every stdio buffer -- including this process's own (parent)
     * stdout -- before forking. Without this, the child inherits a copy
     * of the PARENT's unflushed stdout buffer; the child's own
     * fflush(stdout) after dup2'ing the capture pipe then writes that
     * inherited parent data into the child's pipe, splicing a prior run's
     * transcript into this run's capture. Latent when stdout is
     * line-buffered (console); live the moment stdout is fully buffered
     * (redirected to a file) -- see vms-cdb.
     */
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        struct vms_procinfo self;

        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        uint32_t st = vms_kif_setident(username, uic, privs);
        printf("SETIDENT_STATUS=%u\n", (unsigned)st);

        memset(&self, 0, sizeof(self));
        if (vms_kif_getjpi_self(&self) & 1)
            printf("SELF_PRIVS=%016llx\n",
                   (unsigned long long)self.cur_privs);
        fflush(stdout);

        execle(AUTHORIZE_PATH, "AUTHORIZE.EXE", (char *)NULL, authorize_env);
        printf("EXEC_FAILED=%d\n", errno);
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    {
        const char *script = "EXIT\n";
        ssize_t w = write(in_pipe[1], script, strlen(script));
        (void)w;
    }
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

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(st))
        *exit_code = WEXITSTATUS(st);
    return 0;
}

static void dump(const char *label, const char *text)
{
    printf("  ---- %s ----\n%s  ---- end %s ----\n", label, text, label);
}

/*
 * Probe only -- vms_kif_open()/close() decide skip-vs-run and nothing else.
 * Every assertion below runs a real subprocess through vms_kif_setident and
 * the shipped AUTHORIZE.EXE.
 */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: keep stdout line-buffered even when init.sh redirects it to a file, so an unflushed fork() cannot splice output */
    printf("=== test_syssvc_authorize (AUTHORIZE's SYSPRV check, both directions, vms-4c2) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI negative-control rig, not the product\n");
        printf("=== test_syssvc_authorize: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    static char outA[8192], outB[8192];
    int rcA = -1, rcB = -1;

    /* --- A: SYSPRV holder must be ADMITTED --------------------------- */
    if (run_authorize(A_NAME, (A_GRP << 16) | A_MEM, A_PRIVS,
                       outA, sizeof(outA), &rcA) != 0) {
        CHECK(0, "A: could not run the SYSPRV-holder subprocess");
    } else {
        dump("A: SYSPRV holder", outA);
        /* negctl-knockon: bind-client-no-register */
        CHECK(strstr(outA, "SETIDENT_STATUS=1\n") != NULL,
              "A: the executive accepted the SYSPRV identity");
        {
            char want[64];
            snprintf(want, sizeof(want), "SELF_PRIVS=%016llx\n",
                     (unsigned long long)(uint64_t)A_PRIVS);
            /* negctl-knockon: bind-client-no-register */
            CHECK(strstr(outA, want) != NULL,
                  "A: the executive's own row holds exactly the SYSPRV mask "
                  "this run stamped -- not a claim, a readback");
        }
        /* negctl-knockon: bind-client-no-register */
        CHECK(strstr(outA, "%UAF-I-AUTHVERSION") != NULL,
              "A: AUTHORIZE printed its version banner -- the SYSPRV holder "
              "was ADMITTED");
        /* negctl-knockon: bind-client-no-register */
        CHECK(strstr(outA, "%UAF-F-NOAUTH") == NULL,
              "A: the SYSPRV holder was never refused");
        /* negctl-knockon: bind-client-no-register */
        CHECK(rcA == 0,
              "A: AUTHORIZE exited 0 for the admitted session");
    }

    /* --- B: non-SYSPRV process must be REFUSED ------------------------ */
    if (run_authorize(B_NAME, (B_GRP << 16) | B_MEM, B_PRIVS,
                       outB, sizeof(outB), &rcB) != 0) {
        CHECK(0, "B: could not run the non-SYSPRV subprocess");
    } else {
        dump("B: non-SYSPRV (OPER|WORLD, no SYSPRV)", outB);
        /* negctl-knockon: bind-client-no-register */
        CHECK(strstr(outB, "SETIDENT_STATUS=1\n") != NULL,
              "B: the executive accepted the non-SYSPRV identity");
        {
            char want[64];
            snprintf(want, sizeof(want), "SELF_PRIVS=%016llx\n",
                     (unsigned long long)(uint64_t)B_PRIVS);
            /* negctl-knockon: bind-client-no-register */
            CHECK(strstr(outB, want) != NULL,
                  "B: the executive's own row holds exactly the OPER|WORLD "
                  "mask this run stamped, with SYSPRV genuinely absent");
        }
        CHECK(strstr(outB, "%UAF-F-NOAUTH, insufficient privilege to manage SYSUAF") != NULL,
              "B: AUTHORIZE refused the non-SYSPRV process with %UAF-F-NOAUTH");
        CHECK(strstr(outB, "%UAF-I-AUTHVERSION") == NULL,
              "B: the non-SYSPRV process never saw the version banner -- it "
              "was refused before SYSUAF was ever touched");
        CHECK(rcB == 1,
              "B: AUTHORIZE exited 1 for the refused session");
    }

    printf("=== test_syssvc_authorize: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
