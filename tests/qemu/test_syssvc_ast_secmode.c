/*
 * test_syssvc_ast_secmode.c - AST delivery respects the caller's ACCESS MODE,
 * and the executive keys ASTs and privileges PER PROCESS (vms-as1).
 *
 * ============================================================
 * WRITTEN AS THE PROOF FOR TWO PROPERTIES test_syssvc_ast.c does NOT cover:
 *
 *   (A) SECURITY / CROSS-MODE. A user-mode sys$setast(1) must NOT deliver
 *       (and thus EXECUTE) an AST queued for a MORE privileged access mode.
 *       Before this fix, src/kernel/vms_ast.c's vms_ioctl_deliverast scanned
 *       from PSL_C_KERNEL unconditionally, and src/kernel/vms_module.c
 *       registers every mode's queue enabled=1, so a user-mode $SETAST drained
 *       the KERNEL/EXEC/SUPER queues and ran those routines -- an access-mode
 *       ESCALATION. deliver is now bounded below by the caller's current mode.
 *
 *   (B) CROSS-PROCESS. The AST queue and the privilege mask both live in the
 *       EXECUTIVE, keyed per process. A forked child that drops its Linux
 *       credentials does NOT inherit the parent's executive CMKRNL (the
 *       executive derives privilege from real credentials, not from anything
 *       the process asserts), and a child cannot drain an AST the parent
 *       queued into the parent's PCB. Both are mediated by the executive and
 *       correct across the process boundary -- not per-process fiction.
 * ============================================================
 *
 * WHY THIS CAN TELL THE EXECUTIVE FROM A PER-PROCESS FAKE.
 *
 * (A) is a CROSS-MODE proof: the same process declares an AST for KERNEL mode
 * (holding CMKRNL) and one for USER mode, then, at USER mode, enables delivery
 * through the PUBLIC sys$setast. The executive hands back ONLY the user-mode
 * AST; the kernel-mode routine does not run. It is then shown NOT LOST: raising
 * to kernel mode and draining delivers it. A deliver path that ignored the
 * caller's mode (the defect) runs the kernel routine at the user-mode drain.
 *
 * (B) is a genuine CROSS-PROCESS discriminator for PRIVILEGE: the parent is
 * root, so the executive derives CMKRNL for it (capable(CAP_SYS_ADMIN)); the
 * child setuid()s away from root, so the executive derives none, and chkpriv
 * REFUSES it. A privilege word a process could assert for itself -- the
 * pre-executive sys_misc.c pcb->cur_privs facade -- could not produce that
 * split from the same source. For the AST QUEUE, (B) proves per-process
 * residency (the child cannot see the parent's queued AST); the discriminator
 * that rules out a per-process userspace fake for ASTs specifically is the
 * cross-LAYER one in test_syssvc_ast.c (public writer, raw kernel reader),
 * which this suite does not restate.
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
#include "vms_kif.h"        /* pulls in vms_ioctl.h: PSL_C_*, VMS_PRV_M_* */

#define EXIT_SKIP 77

/* Credentials for the unprivileged child. setuid() away from root is what
 * makes capable(CAP_SYS_ADMIN) genuinely false in the executive, exactly as
 * test_kmod_access.c does. */
#define C_GID   401
#define C_UID   1004

/* AST parameter magics -- distinct so a confused astprm is visible. */
#define MAGIC_K  0x0CAFu   /* the KERNEL-mode AST */
#define MAGIC_U  0x0BEEu   /* the USER-mode AST */

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Delivered-AST recorders. volatile: written from the routines the executive
 * hands back, read by the assertions. */
static volatile uint32_t g_kfired = 0;
static volatile uint32_t g_ufired = 0;

static void kernel_ast(uint32_t prm) { (void)prm; g_kfired++; }
static void user_ast(uint32_t prm)   { (void)prm; g_ufired++; }

/* What the unprivileged child reports back to the parent. */
struct child_report {
    uint32_t chkpriv_cmkrnl;      /* executive's CMKRNL verdict for the child */
    int      deliver_rc;          /* rc of the child draining ITS own queue */
    uint32_t dclast_super_status; /* status of the child's sys$dclast at SUPER (vms-95a) */
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
    uint8_t  mode = 0xFF;

    printf("=== test_syssvc_ast_secmode (AST delivery respects access mode) ===\n");

    if (!executive_present()) {
        /*
         * CI negative control (a rig booted without insmod'ing vms.ko). The
         * no-fabricated-success property still holds and is still asserted:
         * a public sys$ entry point must never report success for an
         * executive operation it could not have performed.
         */
        printf("  FAIL: cannot open /dev/vms\n");
        st = sys$dclast(user_ast, MAGIC_U, PSL_C_USER);
        printf("  INFO: sys$dclast with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "sys$dclast does NOT report success when the executive was never reached");
        printf("=== test_syssvc_ast_secmode: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ==================================================================
     * PART A -- ACCESS-MODE SCOPE OF DELIVERY (the security property).
     *
     * The process starts at USER mode (vms_module.c registers current_mode =
     * PSL_C_USER). It runs as root, so the executive derives CMKRNL for it --
     * the privilege needed to DECLARE an AST for a more privileged mode.
     * ================================================================== */
    st = vms_kif_chkpriv(VMS_PRV_M_CMKRNL);
    CHECK(st & 1,
          "the executive reports CMKRNL held for the privileged (root) process");

    st = vms_kif_getmode(&mode, NULL, NULL);
    CHECK((st & 1) && mode == PSL_C_USER,
          "the executive reports this process starts at USER access mode");

    g_kfired = 0;
    g_ufired = 0;

    /* Declare one AST for KERNEL mode (allowed: we hold CMKRNL) and one for
     * USER mode. Neither runs yet -- $DCLAST only queues. */
    st = sys$dclast(kernel_ast, MAGIC_K, PSL_C_KERNEL);
    printf("  INFO: sys$dclast(kernel_ast, KERNEL) returned status %u\n", st);
    CHECK(st & 1, "sys$dclast queued a KERNEL-mode AST (holding CMKRNL)");

    st = sys$dclast(user_ast, MAGIC_U, PSL_C_USER);
    printf("  INFO: sys$dclast(user_ast, USER) returned status %u\n", st);
    CHECK(st & 1, "sys$dclast queued a USER-mode AST");

    /* Still at USER mode: enable delivery for THIS mode and drain. The
     * executive must hand back ONLY the user-mode AST. */
    st = sys$setast(1);
    printf("  INFO: sys$setast(1) at USER mode: g_ufired=%u g_kfired=%u\n",
           g_ufired, g_kfired);

    CHECK(g_ufired == 1,
          "the USER-mode AST was delivered by the user-mode sys$setast(1)");
    /* negctl: ast-deliver-crossmode-escalation */
    CHECK(g_kfired == 0,
          "a user-mode $SETAST(1) did NOT execute the KERNEL-mode AST");

    /* The kernel-mode AST is NOT lost: raise to kernel mode (we hold CMKRNL)
     * and drain. Now it is delivered. */
    st = vms_kif_setmode(PSL_C_KERNEL);
    CHECK(st & 1, "raised to KERNEL access mode (holding CMKRNL)");

    mode = 0xFF;
    st = vms_kif_getmode(&mode, NULL, NULL);
    CHECK((st & 1) && mode == PSL_C_KERNEL,
          "the executive reports this process is now at KERNEL access mode");

    st = sys$setast(1);
    printf("  INFO: sys$setast(1) at KERNEL mode: g_kfired=%u\n", g_kfired);
    CHECK(g_kfired == 1,
          "the KERNEL-mode AST survived the user-mode drain and delivers at kernel mode");

    /* ==================================================================
     * PART B -- CROSS-PROCESS. Privilege and the AST queue are the
     * executive's, keyed per process.
     *
     * Return to USER mode, queue a USER AST WITHOUT delivering it, then fork.
     * The child drops its credentials: the executive must refuse it CMKRNL
     * and must not let it drain the parent's AST. The parent then delivers
     * its own AST -- proof the queue was there, in the parent's PCB, all along.
     * ================================================================== */
    st = vms_kif_setmode(PSL_C_USER);
    CHECK(st & 1, "returned to USER access mode (a drop is always allowed)");

    g_ufired = 0;
    st = sys$dclast(user_ast, MAGIC_U, PSL_C_USER);
    CHECK(st & 1, "parent queued a USER-mode AST for the cross-process check");

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
            uint64_t a = 0, p = 0;
            uint8_t  am = 0xFF;

            close(pfd[0]);

            /* Drop root so the executive derives no privilege for this task.
             * Done BEFORE any vms_kif_* call, so registration (kif_bind) runs
             * under the dropped credentials. */
            if (setgid(C_GID) != 0 || setuid(C_UID) != 0)
                _exit(2);

            rep.chkpriv_cmkrnl = vms_kif_chkpriv(VMS_PRV_M_CMKRNL);

            /* SECURITY (vms-95a): this child is at USER mode and, having
             * dropped root, holds NO CMEXEC/CMKRNL. Declaring an AST at the
             * more-privileged SUPER mode must be REFUSED by the executive. The
             * $DCLAST access-mode gate checked KERNEL and EXEC but not SUPER,
             * so before the fix this returned SS$_NORMAL and queued a SUPER AST
             * an untrusted USER-mode image could plant for DCL to run. */
            rep.dclast_super_status = sys$dclast(user_ast, MAGIC_U, PSL_C_SUPER);

            /* The child enables delivery for its own USER mode and drains.
             * The parent's AST is in the PARENT's executive PCB -- the child
             * must get nothing. */
            (void)vms_kif_setast(1);
            rep.deliver_rc = vms_kif_deliverast(&a, &p, &am);

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
            CHECK(!(rep.chkpriv_cmkrnl & 1),
                  "a forked+dropped child is REFUSED CMKRNL by the executive "
                  "(privilege is executive-mediated per process, not self-asserted)");
            CHECK(rep.deliver_rc != 0,
                  "the parent's queued AST is invisible to the child "
                  "(the executive keys the AST queue per process)");

            /* negctl: dclast-super-mode-escalation */
            CHECK(!(rep.dclast_super_status & 1),
                  "a USER-mode child without CMEXEC/CMKRNL is REFUSED sys$dclast at SUPER mode");

            /* The parent still holds its own AST: deliver it now. */
            (void)sys$setast(1);
            CHECK(g_ufired == 1,
                  "the parent's own sys$setast(1) delivers the AST the child could not see");
        }
    }

    printf("=== test_syssvc_ast_secmode: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
