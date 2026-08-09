/*
 * test_syssvc_setprv.c - $SETPRV routes the PRIVILEGE MUTATION through the
 * EXECUTIVE, which OWNS and AUTHORIZES it (vms-pv1).
 *
 * ============================================================
 * THE PROPERTY, AND WHY A PER-PROCESS FAKE CANNOT PASS IT.
 *
 * Before this item, sys$setprv (src/libvms/syssvc/sys_misc.c) wrote
 * pcb->cur_privs / pcb->perm_privs in process-local memory and returned
 * SS$_NORMAL for ANYTHING asked -- a process awarded itself any privilege and
 * the "grant" bound nothing but its own later in-process checks. That is the
 * vms-b2e privilege LARP. sys$setprv now routes to vms_kif_setprv
 * (VMS_IOCTL_SETPRV -> vms_ioctl_setprv, kernel/vms_access.c): the executive
 * authorizes the request against this process's AUTHORIZED (permanent) mask and
 * owns the result.
 *
 *   (A) MEDIATION. A process that DOES hold SETPRV disables and re-enables a
 *       privilege through the PUBLIC sys$setprv; the executive -- queried
 *       independently through vms_kif_chkpriv -- reflects each mutation. The
 *       verification never reads pcb->cur_privs, so a local write could not
 *       satisfy it.
 *
 *   (B) DENIAL (the security property). A forked child drops its Linux
 *       credentials, so the executive derives NO SETPRV for it (only the
 *       default TMPMBX/NETMBX). It then tries to $SETPRV itself CMKRNL -- a
 *       privilege outside its authorized mask. The executive REFUSES to widen
 *       it (SS$_NOTALLPRIV, the authorized subset -- here empty -- is applied
 *       and nothing more) and chkpriv still reports CMKRNL NOT held. A process
 *       cannot grant itself a privilege it is not entitled to.
 *
 *   (C) CROSS-PROCESS. At the same instant the child is refused CMKRNL, the
 *       privileged parent still holds it. The privilege state is the
 *       executive's, keyed per process -- not per-process fiction that reports
 *       whatever each process asserts about itself. A self-asserted
 *       pcb->cur_privs facade, sharing nothing, could not produce that split
 *       from the same source.
 * ============================================================
 *
 * Do not weaken it, and do not "fix" a future failure by asserting whatever
 * OVMX happens to do -- fix the product.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"        /* pulls in vms_ioctl.h: VMS_PRV_M_* */

#define EXIT_SKIP 77

/* Credentials for the unprivileged child. setuid() away from root is what
 * makes capable(CAP_SYS_ADMIN) genuinely false in the executive, so it derives
 * only the default privileges (TMPMBX|NETMBX) and no SETPRV. Same mechanism as
 * test_kmod_access.c / test_syssvc_ast_secmode.c. */
#define C_GID   402
#define C_UID   1005

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What the unprivileged child reports back to the parent. */
struct child_report {
    uint32_t setprv_status;    /* status of $SETPRV(enable CMKRNL) */
    uint32_t chkpriv_after;    /* executive's CMKRNL verdict afterwards */
};

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
    uint32_t st;
    uint64_t mask, prev;

    /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into
     * a forked child's output. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_syssvc_setprv ($SETPRV mutation is executive-owned) ===\n");

    if (!executive_present()) {
        /*
         * CI negative control (a rig booted without insmod'ing vms.ko). The
         * no-fabricated-success property still holds and is still asserted: a
         * public sys$ entry point must never report success for an executive
         * mutation it could not have performed.
         */
        printf("  FAIL: cannot open /dev/vms\n");
        mask = VMS_PRV_M_CMKRNL;
        st = sys$setprv(1, &mask, 0, NULL);
        printf("  INFO: sys$setprv with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "sys$setprv does NOT report success when the executive was never reached");
        printf("=== test_syssvc_setprv: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ==================================================================
     * PART A -- MEDIATION. This process runs as root, so the executive
     * DERIVES the enforced privileges for it, SETPRV among them. Holding
     * SETPRV is the authority to enable a privilege beyond the authorized
     * mask, so its own disable/enable of CMKRNL must round-trip through the
     * executive -- observed through vms_kif_chkpriv, never pcb->cur_privs.
     * ================================================================== */
    st = vms_kif_chkpriv(VMS_PRV_M_SETPRV);
    CHECK(st & 1,
          "the executive reports SETPRV held for the privileged (root) process");

    /* Disable CMKRNL through the PUBLIC sys$setprv. Disabling is always
     * allowed; the executive must then report it cleared. */
    mask = VMS_PRV_M_CMKRNL;
    prev = 0;
    st = sys$setprv(0 /* disable */, &mask, 0, &prev);
    printf("  INFO: sys$setprv(disable CMKRNL) returned status %u\n", st);
    CHECK(st & 1, "sys$setprv(disable CMKRNL) succeeded");
    CHECK(!(vms_kif_chkpriv(VMS_PRV_M_CMKRNL) & 1),
          "the executive reports CMKRNL cleared after sys$setprv(disable)");

    /* Re-enable CMKRNL. We hold SETPRV, so the executive grants it and reports
     * it held again -- proof the mutation reached the executive, not a local
     * word the query never reads. */
    mask = VMS_PRV_M_CMKRNL;
    st = sys$setprv(1 /* enable */, &mask, 0, &prev);
    printf("  INFO: sys$setprv(enable CMKRNL) returned status %u\n", st);
    CHECK(st & 1, "sys$setprv(enable CMKRNL) succeeded (holding SETPRV)");
    CHECK(vms_kif_chkpriv(VMS_PRV_M_CMKRNL) & 1,
          "the executive reports CMKRNL restored after sys$setprv(enable) -- mediated, not a local write");

    /* ==================================================================
     * PART B -- DENIAL, and PART C -- CROSS-PROCESS. Fork a child that drops
     * root: the executive derives no SETPRV for it, so it cannot $SETPRV
     * itself a privilege outside its authorized mask. The parent keeps CMKRNL
     * the whole time -- the two processes' privilege state is the executive's,
     * kept apart.
     * ================================================================== */
    int pfd[2];
    if (pipe(pfd) != 0) {
        printf("  FAIL: pipe() failed\n");
        fail++;
    } else {
        pid_t pid = fork();
        if (pid < 0) {
            printf("  FAIL: fork() failed\n");
            fail++;
        } else if (pid == 0) {
            /* CHILD */
            struct child_report rep;
            uint64_t m = VMS_PRV_M_CMKRNL, pv = 0;

            close(pfd[0]);

            /* Drop root BEFORE any vms_kif_* call, so registration (kif_bind)
             * runs under the dropped credentials and the executive derives the
             * unprivileged mask for this process. */
            if (setgid(C_GID) != 0 || setuid(C_UID) != 0)
                _exit(2);

            /* Try to award itself CMKRNL through the PUBLIC sys$setprv. It holds
             * no SETPRV, and CMKRNL is outside its authorized mask. */
            rep.setprv_status = sys$setprv(1 /* enable */, &m, 0, &pv);
            rep.chkpriv_after = vms_kif_chkpriv(VMS_PRV_M_CMKRNL);

            (void)write(pfd[1], &rep, sizeof(rep));
            close(pfd[1]);
            _exit(0);
        } else {
            /* PARENT */
            struct child_report rep;
            ssize_t r;
            int status = 0;

            close(pfd[1]);
            r = read(pfd[0], &rep, sizeof(rep));
            close(pfd[0]);
            waitpid(pid, &status, 0);

            CHECK(r == (ssize_t)sizeof(rep),
                  "the child reported its executive verdicts back");
            printf("  INFO: child sys$setprv(enable CMKRNL) status=%u chkpriv_after=%u\n",
                   rep.setprv_status, rep.chkpriv_after);

            /* negctl: setprv-grants-unauthorized */
            CHECK(rep.setprv_status == SS$_NOTALLPRIV,
                  "a forked child without SETPRV is REFUSED CMKRNL it is not authorized for (SS$_NOTALLPRIV)");
            /* negctl: setprv-grants-unauthorized */
            CHECK(!(rep.chkpriv_after & 1),
                  "the executive still reports CMKRNL NOT held for the unauthorized child");

            /* PART C: at the same instant, the privileged parent still holds
             * CMKRNL. The privilege state is the executive's, keyed per
             * process -- the child's refusal and the parent's grant coexist. */
            CHECK(vms_kif_chkpriv(VMS_PRV_M_CMKRNL) & 1,
                  "the parent still holds CMKRNL the child was refused (privilege state is executive-keyed per process)");
        }
    }

    printf("=== test_syssvc_setprv: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
