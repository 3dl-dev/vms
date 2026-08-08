/*
 * test_syssvc_identcont.c - image activation CONTINUES the activating
 * process's VMS identity (vms-4d7, "Option B"), proven against a real
 * /dev/vms by running the shipped AUTHORIZE.EXE.
 *
 * THE BUG THIS PROVES FIXED. On OpenVMS, RUN AUTHORIZE (like any image
 * activation) runs IN the current process: same PID, same UIC, same
 * privileges. OVMX fork()s + execve()s a fresh Linux process for every
 * image, and that child used to auto-register a NEW PCB and DERIVE its own
 * privilege mask from capable(CAP_SYS_ADMIN) -- which never includes
 * SYSPRV. So SYSTEM, whose SYSPRV was stamped onto its DCL by LOGINOUT,
 * lost it the instant DCL forked AUTHORIZE, and AUTHORIZE refused SYSTEM.
 *
 * The fix: DCL calls vms_kif_register_continue() in the forked child before
 * execve (src/vmsdcl/dcl_cmd_process.c). That issues VMS_IOCTL_REGISTER_
 * CONTINUE, and the executive (src/kernel/vms_module.c) shares the parent's
 * VMS PID, UIC, user name and privileges onto the child instead of deriving
 * a fresh mask. This suite drives that exact path: a helper process
 * continues its parent's identity WITHOUT calling vms_kif_setident() itself,
 * then execs the real AUTHORIZE.EXE.
 *
 *   A) A parent holding SYSPRV activates AUTHORIZE via register_continue.
 *      The image inherits SYSPRV it never asked for and is ADMITTED.
 *      This is what test_syssvc_authorize.c could NOT prove: that suite
 *      setident()s the child itself before exec, so it never exercised
 *      inheritance across the activation fork.
 *
 *   B) A parent that setident'd DOWN to an unprivileged identity (FIELD,
 *      [200,10], no SYSPRV) activates AUTHORIZE via register_continue. The
 *      image inherits the REDUCED identity and is REFUSED. Privileges do
 *      NOT reappear across the activation fork -- the security half.
 *
 * A implies the child gained a privilege only its parent held; B implies
 * continuing cannot manufacture one. Both must hold at once.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no executive (the CI
 * executive-absent negative-control rig, vms-0ff) it exits EXIT_SKIP (77),
 * never a fake pass: register_continue cannot share an identity that was
 * never established, so neither direction is provable.
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

/* SYSTEM: the identity LOGINOUT stamps on the interactive SYSTEM session. */
#define SYS_NAME   "SYSTEM"
#define SYS_UIC    ((1u << 16) | 4u)
#define SYS_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV | PRV$M_SETPRV)

/* A real, populated, DELIBERATELY non-SYSPRV identity to drop down to. */
#define FIELD_NAME   "FIELD"
#define FIELD_UIC    ((200u << 16) | 10u)
#define FIELD_PRIVS  (PRV$M_TMPMBX | PRV$M_NETMBX)

static char *const authorize_env[] = {
    (char *)"PATH=/bin",
    NULL
};

/*
 * activate_authorize - fork an IMAGE ACTIVATION of AUTHORIZE.
 *
 * The child does what DCL's dcl_activate_image does: it calls
 * vms_kif_register_continue() (NOT setident) so the executive shares the
 * CALLING process's identity onto it, then execs the real AUTHORIZE.EXE.
 * The identity being continued is whatever the CALLER of this function has
 * already established for itself.
 *
 * The child prints CONTINUE_STATUS and SELF_PRIVS before exec, so the
 * identity actually established is part of the captured evidence.
 */
static int activate_authorize(char *out, size_t outsz, int *exit_code)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    *exit_code = -1;
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    fflush(NULL);   /* vms-cdb: never splice an inherited stdio buffer into the child */

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

        /* THE PATH UNDER TEST: continue the parent's identity, no setident. */
        uint32_t st = vms_kif_register_continue();
        printf("CONTINUE_STATUS=%u\n", (unsigned)st);

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
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b */
    printf("=== test_syssvc_identcont (image activation continues identity, vms-4d7) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_identcont: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    static char outA[8192], outB[8192];
    int rcA = -1, rcB = -1;

    /* --- A: a SYSPRV parent's activated image is ADMITTED ------------- */
    /* THIS process becomes SYSTEM (SYSPRV), then activates AUTHORIZE. The
     * image never setident's -- it inherits SYSPRV purely by continuing
     * this process across the activation fork. */
    {
        uint32_t ist = vms_kif_setident(SYS_NAME, SYS_UIC, SYS_PRIVS);
        CHECK(ist == 1, "A: this process established the SYSTEM/SYSPRV identity to be continued");
    }

    if (activate_authorize(outA, sizeof(outA), &rcA) != 0) {
        CHECK(0, "A: could not activate AUTHORIZE");
    } else {
        dump("A: SYSPRV parent, image continues identity", outA);
        CHECK(strstr(outA, "CONTINUE_STATUS=1\n") != NULL,
              "A: the executive accepted the continue-identity registration");
        {
            char want[64];
            snprintf(want, sizeof(want), "SELF_PRIVS=%016llx\n",
                     (unsigned long long)(uint64_t)SYS_PRIVS);
            /* negctl: register-continue-identity-dropped */
            CHECK(strstr(outA, want) != NULL,
                  "A: the continued image holds the PARENT's SYSPRV mask -- a "
                  "readback, not a claim, and it never called setident");
        }
        /* negctl: register-continue-identity-dropped */
        CHECK(strstr(outA, "%UAF-I-AUTHVERSION") != NULL,
              "A: AUTHORIZE printed its banner -- the continued image was ADMITTED");
        /* negctl: register-continue-identity-dropped */
        CHECK(strstr(outA, "%UAF-E-NAOFIL") == NULL,
              "A: the continued image was never refused");
        /* negctl: register-continue-identity-dropped */
        CHECK(rcA == 0, "A: AUTHORIZE exited 0 for the admitted session");
    }

    /* --- B: a setident'd-DOWN parent's activated image is REFUSED ----- */
    /* A child process drops to FIELD (no SYSPRV) and THEN activates
     * AUTHORIZE. Privileges must not reappear across the activation fork.
     * Done in a subprocess so this main (SYSTEM) is not itself reduced. */
    {
        int relay[2];
        if (pipe(relay) < 0) {
            CHECK(0, "B: pipe()");
        } else {
            pid_t p = fork();
            if (p < 0) {
                CHECK(0, "B: fork()");
                close(relay[0]); close(relay[1]);
            } else if (p == 0) {
                /* The reduced-privilege "DCL": drop to FIELD, then activate. */
                close(relay[0]);
                uint32_t ist = vms_kif_setident(FIELD_NAME, FIELD_UIC, FIELD_PRIVS);
                if (ist != 1) {
                    dprintf(relay[1], "SETIDENT_DOWN_FAILED=%u\n", (unsigned)ist);
                    _exit(2);
                }
                char child_out[8192];
                int child_rc = -1;
                if (activate_authorize(child_out, sizeof(child_out), &child_rc) != 0) {
                    dprintf(relay[1], "ACTIVATE_FAILED\n");
                    _exit(3);
                }
                dprintf(relay[1], "RC=%d\n%s", child_rc, child_out);
                close(relay[1]);
                _exit(0);
            } else {
                close(relay[1]);
                size_t used = 0;
                for (;;) {
                    ssize_t n = read(relay[0], outB + used, sizeof(outB) - 1 - used);
                    if (n <= 0) break;
                    used += (size_t)n;
                    if (used >= sizeof(outB) - 1) break;
                }
                outB[used] = '\0';
                close(relay[0]);
                waitpid(p, NULL, 0);
                if (sscanf(outB, "RC=%d", &rcB) != 1) rcB = -1;
            }
        }

        dump("B: FIELD parent (no SYSPRV), image continues identity", outB);
        CHECK(strstr(outB, "CONTINUE_STATUS=1\n") != NULL,
              "B: the executive accepted the continue-identity registration");
        {
            char want[64];
            snprintf(want, sizeof(want), "SELF_PRIVS=%016llx\n",
                     (unsigned long long)(uint64_t)FIELD_PRIVS);
            CHECK(strstr(outB, want) != NULL,
                  "B: the continued image holds the PARENT's REDUCED mask, with "
                  "SYSPRV genuinely absent -- privileges did not reappear");
        }
        CHECK(strstr(outB, "%UAF-E-NAOFIL, unable to open system authorization file (SYSUAF.DAT)") != NULL &&
              strstr(outB, "-RMS-E-PRV, insufficient privilege or file protection violation") != NULL,
              "B: AUTHORIZE refused the continued unprivileged image (oracle-grounded message)");
        CHECK(strstr(outB, "%UAF-I-AUTHVERSION") == NULL,
              "B: the continued unprivileged image never saw the banner");
        CHECK(rcB == 1, "B: AUTHORIZE exited 1 for the refused session");
    }

    printf("=== test_syssvc_identcont: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
