/*
 * test_syssvc_ef_mproc.c - common event flag clusters through the PUBLIC
 * sys$ API, A-writes / B-reads (vms-f1f).
 *
 * ============================================================
 * WRITTEN RED (vms-f1f). MADE GREEN BY FIXING THE PRODUCT (vms-2a8).
 * NOT ONE ASSERTION WAS CHANGED TO GET THERE.
 * ============================================================
 *
 * It was born as the reproduction of a defect and is now the regression
 * guard for the fix. Do not delete it, do not weaken it, and do not "fix" a
 * future failure of it by asserting the behaviour OVMX happens to have.
 * What it reported when it was written was true:
 *
 *   src/libvms/syssvc/sys_event.c implemented the ENTIRE event flag facility
 *   in per-process memory (struct vms_pcb's ef_clusters[]) and NEVER called
 *   the executive. Every vms_kif_* event-flag entry point --
 *   vms_kif_setef, vms_kif_clref, vms_kif_readef, vms_kif_waitfr,
 *   vms_kif_wflor, vms_kif_wfland, vms_kif_ascefc, vms_kif_dacefc -- had
 *   ZERO callers product-wide, exactly like vms_kif_register() before
 *   vms-9fc. src/kernel/vms_eflag.c implemented common clusters properly, on
 *   a module-global list (vms_common_ef_list), and nothing in the product
 *   reached it.
 *
 *   sys$ascefc was worse than unwired: it was `return SS$_NORMAL;` with a
 *   TODO and no side effect at all. A process asked to join a named common
 *   cluster, was told it succeeded, and shared nothing. Under CLAUDE.md
 *   Rule 10 that is the illegal third answer -- a plausible-looking handler
 *   for a condition VMS never faces -- and under Rule 11 it is a facade: a
 *   system facility living in per-process memory that reports success while
 *   sharing nothing.
 *
 * vms-2a8 rewrote sys_event.c as a pure translation layer over the
 * executive. The measurement that closed it, same boot, same harness:
 *
 *   before  test_kmod_eflag_mproc  13 passed,  0 failed   (executive: fine)
 *           test_syssvc_ef_mproc    2 passed, 11 failed   (public API: facade)
 *   after   test_kmod_eflag_mproc  13 passed,  0 failed
 *           test_syssvc_ef_mproc   ALL passed
 *
 * WHY THE PROPERTY BELOW IS VMS BEHAVIOUR AND NOT AN OVMX INVENTION
 *
 * Public OpenVMS documentation (System Services Reference, $ASCEFC/$SETEF;
 * Programming Concepts, event flags) states that flags 0-63 are local to the
 * process while flags 64-127 belong to COMMON event flag clusters that
 * processes associate with BY NAME precisely in order to share them. Sharing
 * IS the facility. A $ASCEFC that shares nothing has not implemented it.
 *
 * STILL NO STATUS CONSTANT IS ASSERTED BY VALUE, and that is deliberate
 * even though the constants are now oracle-pinned (vms-68c,
 * docs/oracle/vax73-event-flags.md). Assertions here use the VMS odd/even
 * success convention only, so this suite measures the SHARING property and
 * cannot go red for a status-numbering reason that belongs to another item.
 * The values themselves are pinned and tested where they are defined.
 *
 * The mirror-image suite tests/qemu/test_kmod_eflag_mproc.c asserts the SAME
 * property one layer down, through raw ioctls. Keeping both is what located
 * the defect exactly -- the executive shared correctly and the public system
 * service never asked it to -- and it is what will locate the next one.
 *
 * SYNCHRONISATION: pipes only, no sleeps; the parent bounds each wait so a
 * failure is a named FAIL line rather than a harness-wide QEMU timeout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
#define PEER_TIMEOUT_MS 20000

#define COMMON_BASE     64
#define CLUSTER_EFN_A   70
#define CLUSTER_EFN_B   75
#define LOCAL_EFN       5
#define LOCAL_EFN_CHILD 9

/* Common cluster 3 (flags 96-127), used only by the vms-2a8 lifetime block
 * at the end of main() so it cannot collide with the cluster-2 flags the
 * A-writes/B-reads measurement above uses. PERM_EFN is deliberately NOT the
 * base number 96 -- see the oracle note at that block. */
#define COMMON_BASE_3   96
#define PERM_EFN        100

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct child_msg {
    uint32_t pass;
    uint32_t fail;
};

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;

    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0)
            return 0;
        if (pr < 0)
            return -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 1;
}

static int send_token(int fd, char tok)
{
    return write(fd, &tok, 1) == 1 ? 0 : -1;
}

/*
 * The executive bootstrap the lock suite performs (see
 * tests/qemu/test_syssvc_lock.c's BOOTSTRAP NOTE) is NOT performed here, and
 * that is deliberate: since vms-9fc, src/libvmssys/vms_kif.c's kif_bind()
 * completes open->register itself before every ioctl, so a public sys$ entry
 * point that reaches the executive needs no help from its caller. Calling
 * vms_kif_register() by hand here would be the exact blindness vms-f27
 * records in four existing suites -- it would mask an unbound client. The
 * only /dev/vms call this file makes is the open below, and it makes it to
 * decide skip-vs-run, nothing else.
 */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

static int run_child(int c2p_write, int p2c_read)
{
    struct child_msg msg;
    char tok;
    uint32_t state;
    uint32_t st;
    $DESCRIPTOR(clusnam, "OVMX$F1F_SVC");

    /* Baseline + negative control, paired (see the parent's copy for why).
     * A DIFFERENT local flag from the parent's, so it cannot disturb the
     * local-is-not-shared discriminator checked at the end. */
    uint32_t st_local = sys$setef(LOCAL_EFN_CHILD);
    printf("  INFO: child: sys$setef(%d) [LOCAL cluster 0] returned status %u\n",
           LOCAL_EFN_CHILD, st_local);
    CHECK(st_local & 1,
          "child: sys$setef on a LOCAL flag succeeds (baseline: the event flag facility is operative in this process at all)");

    st = sys$setef(CLUSTER_EFN_A);
    CHECK((st_local & 1) && !(st & 1),
          "child: sys$setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')");

    st = sys$ascefc(COMMON_BASE, &clusnam, 0, 0);
    printf("  INFO: child: sys$ascefc returned status %u\n", st);
    CHECK(st & 1, "child: sys$ascefc joined the named common cluster");

    if (send_token(c2p_write, 'A') < 0)
        fail++;

    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'B') {
        printf("  FAIL: child: never saw the parent's post-$SETEF token\n");
        fail++;
    } else {
        state = 0;
        st = sys$readef(CLUSTER_EFN_A, &state);
        printf("  INFO: child: sys$readef(%d) status=%u cluster-state=0x%08x (expect bit %d set)\n",
               CLUSTER_EFN_A, st, state, CLUSTER_EFN_A - COMMON_BASE);
        CHECK((st & 1) && (state & (1u << (CLUSTER_EFN_A - COMMON_BASE))),
              "child: a common flag SET BY THE PARENT via sys$setef is visible here (A writes, B reads, public API)");
    }

    st = sys$setef(CLUSTER_EFN_B);
    CHECK(st & 1, "child: sys$setef on the associated common cluster reported success");
    if (send_token(c2p_write, 'C') < 0)
        fail++;

    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'D') {
        printf("  FAIL: child: never saw the parent's post-$CLREF token\n");
        fail++;
    } else {
        state = 0;
        st = sys$readef(CLUSTER_EFN_A, &state);
        CHECK((st & 1) && !(state & (1u << (CLUSTER_EFN_A - COMMON_BASE))),
              "child: a common flag CLEARED BY THE PARENT via sys$clref reads clear here (A clears, B reads, public API)");

        /* Discriminator: local clusters are per-process on VMS too. */
        state = 0;
        st = sys$readef(LOCAL_EFN, &state);
        CHECK((st & 1) && !(state & (1u << LOCAL_EFN)),
              "child: a LOCAL flag set by the parent is NOT visible here (local clusters stay per-process)");
    }

    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return fail > 0 ? 1 : 0;
}

int main(void)
{
    int p2c[2], c2p[2];
    char tok;
    uint32_t st;
    $DESCRIPTOR(clusnam, "OVMX$F1F_SVC");

    printf("=== test_syssvc_ef_mproc (common event flags via the PUBLIC sys$ API) ===\n");

    if (!executive_present()) {
        /*
         * Reached ONLY in the CI negative control (a rig booted without
         * insmod'ing vms.ko); vms-0ff removed OVMX's executive-absent state.
         * Even here the no-fabricated-success property still holds and is
         * still asserted: a public sys$ entry point must never report
         * success for a shared-state operation it could not have performed.
         * sys$ascefc fails this even WITH an executive -- which is the point
         * of the suite -- so it is asserted in both branches.
         */
        printf("  FAIL: parent: cannot open /dev/vms\n");
        st = sys$ascefc(COMMON_BASE, &clusnam, 0, 0);
        printf("  INFO: sys$ascefc with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$ascefc does NOT report success when the executive was never reached");
        printf("=== test_syssvc_ef_mproc: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        printf("  FAIL: pipe() failed\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork() failed\n");
        return 1;
    }
    if (pid == 0) {
        close(p2c[1]);
        close(c2p[0]);
        _exit(run_child(c2p[1], p2c[0]));
    }

    close(p2c[0]);
    close(c2p[1]);

    /*
     * BASELINE FIRST, THEN THE NEGATIVE CONTROL, AND THE TWO ARE ASSERTED
     * TOGETHER. Round 1 of this file asserted only "setef on an
     * unassociated common flag is refused", and it PASSED -- for the wrong
     * reason. src/libvms/syssvc/sys_event.c reads vms_pcb_get() and returns
     * SS$_ILLEFC when there is no per-process PCB, which is the state of any
     * ordinary image, so EVERY sys$setef fails and a control asserting only
     * "this one fails" is satisfied by a facility that does nothing at all.
     * That is the exact shape this dispatch keeps catching: an assertion
     * satisfiable by something other than the behaviour under test. The
     * control below therefore requires the LOCAL flag to succeed AND the
     * unassociated common flag to fail, in the same expression.
     *
     * The local flag set here is also the discriminator the child re-reads
     * at the end: local clusters are per-process ON VMS TOO, so it must NOT
     * be visible over there.
     */
    uint32_t st_local = sys$setef(LOCAL_EFN);
    printf("  INFO: sys$setef(%d) [LOCAL cluster 0] returned status %u\n",
           LOCAL_EFN, st_local);
    CHECK(st_local & 1,
          "parent: sys$setef on a LOCAL flag succeeds (baseline: the event flag facility is operative in this process at all)");

    st = sys$setef(CLUSTER_EFN_A);
    printf("  INFO: sys$setef(%d) [COMMON, not yet associated] returned status %u\n",
           CLUSTER_EFN_A, st);
    CHECK((st_local & 1) && !(st & 1),
          "parent: sys$setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')");

    st = sys$ascefc(COMMON_BASE, &clusnam, 0, 0);
    printf("  INFO: sys$ascefc(%d, \"OVMX$F1F_SVC\") returned status %u\n",
           COMMON_BASE, st);
    CHECK(st & 1, "parent: sys$ascefc created/joined the named common cluster");

    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'A') {
        printf("  FAIL: parent: child never reported that it associated\n");
        fail++;
    }

    st = sys$setef(CLUSTER_EFN_A);
    printf("  INFO: sys$setef(%d) [COMMON, after $ASCEFC] returned status %u\n",
           CLUSTER_EFN_A, st);
    CHECK(st & 1, "parent: sys$setef on the associated common cluster reported success");
    if (send_token(p2c[1], 'B') < 0)
        fail++;

    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'C') {
        printf("  FAIL: parent: child never reported its own sys$setef\n");
        fail++;
    } else {
        uint32_t state = 0;
        st = sys$readef(CLUSTER_EFN_B, &state);
        CHECK((st & 1) && (state & (1u << (CLUSTER_EFN_B - COMMON_BASE))),
              "parent: a common flag SET BY THE CHILD via sys$setef is visible here (B writes, A reads, public API)");
    }

    st = sys$clref(CLUSTER_EFN_A);
    printf("  INFO: sys$clref(%d) [COMMON, after $ASCEFC] returned status %u\n",
           CLUSTER_EFN_A, st);
    CHECK(st & 1, "parent: sys$clref on the associated common cluster reported success");
    /* The LOCAL flag the child must NOT see was set at the top of main(),
     * as the baseline half of the negative control. */
    if (send_token(p2c[1], 'D') < 0)
        fail++;

    struct child_msg msg = {0, 0};
    int r = read_bounded(c2p[0], &msg, sizeof(msg), PEER_TIMEOUT_MS);
    if (r != 1) {
        printf("  FAIL: parent: no final report from the child (r=%d)\n", r);
        fail++;
    } else {
        pass += (int)msg.pass;
        fail += (int)msg.fail;
    }

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    /* =====================================================================
     * ADDED BY vms-2a8. Everything above is unchanged from the round that
     * wrote this suite red; nothing above was relaxed to make it pass.
     *
     * These cover the three things the original suite could not: the
     * lifetime half of the facility ($DACEFC / $DLCEFC), and the two
     * oracle-pinned rules the executive was getting wrong underneath them.
     * They run last, after the child is reaped, so they cannot perturb the
     * A-writes/B-reads measurement above.
     * ===================================================================== */

    /* $DACEFC ON A NON-BASE FLAG NUMBER. ORACLE-PINNED (HELP
     * SYSTEM_SERVICES $ASCEFC Arguments, docs/oracle/vax73-event-flags.md):
     * "To associate with common event flag cluster 2, specify any flag
     * number in the cluster (64 to 95)". CLUSTER_EFN_A is 70, not 64 --
     * before vms-2a8 the executive accepted ONLY 64 and 96 here and
     * answered SS$_ILLEFC for every other legal number. */
    st = sys$dacefc(CLUSTER_EFN_A);
    printf("  INFO: sys$dacefc(%d) [non-base flag number] returned status %u\n",
           CLUSTER_EFN_A, st);
    CHECK(st & 1,
          "parent: sys$dacefc identifies the cluster from ANY flag number in it, not only the base");

    /* And the disassociation was real, not reported: an unassociated common
     * flag is refused again, exactly as it was before the first $ASCEFC. */
    st = sys$setef(CLUSTER_EFN_A);
    printf("  INFO: sys$setef(%d) after $DACEFC returned status %u\n",
           CLUSTER_EFN_A, st);
    CHECK(!(st & 1),
          "parent: after sys$dacefc the common flag is refused again (the disassociation took effect, it was not merely reported)");

    /* ---- $DLCEFC, and the permanence it is defined against ----
     *
     * ORACLE-PINNED (HELP SYSTEM_SERVICES $DLCEFC): "Marks a permanent
     * common event flag cluster for deletion." So the property has two
     * halves and each is asserted separately:
     *   (a) a PERMANENT cluster outlives its last association -- without
     *       that, $DLCEFC would have nothing to delete and a stub could
     *       pass;
     *   (b) after $DLCEFC the cluster really is gone, observed as a
     *       re-association seeing FRESH (zero) flags rather than the ones
     *       set before.
     * sys$dlcefc used to be `return SS$_NORMAL;` -- it passed (a) and (b)
     * would have caught it. */
    {
        $DESCRIPTOR(permnam, "OVMX$2A8_PERM");
        $DESCRIPTOR(nonam,   "OVMX$2A8_NOSUCH");
        uint32_t state = 0;

        st = sys$ascefc(PERM_EFN, &permnam, 0, 1 /* perm */);
        printf("  INFO: sys$ascefc(%d, \"OVMX$2A8_PERM\", perm=1) returned status %u\n",
               PERM_EFN, st);
        CHECK(st & 1, "parent: sys$ascefc created a PERMANENT common cluster");

        st = sys$setef(PERM_EFN);
        CHECK(st & 1, "parent: sys$setef on the permanent cluster reported success");

        st = sys$dacefc(PERM_EFN);
        CHECK(st & 1, "parent: sys$dacefc released the last association to the permanent cluster");

        st = sys$ascefc(PERM_EFN, &permnam, 0, 0);
        CHECK(st & 1, "parent: sys$ascefc re-joined the permanent cluster by name");
        state = 0;
        st = sys$readef(PERM_EFN, &state);
        printf("  INFO: after re-join, sys$readef(%d) status=%u state=0x%08x\n",
               PERM_EFN, st, state);
        CHECK((st & 1) && (state & (1u << (PERM_EFN - COMMON_BASE_3))),
              "parent: a PERMANENT cluster survived losing its last association (its flags are still set)");

        st = sys$dlcefc(&permnam);
        printf("  INFO: sys$dlcefc(\"OVMX$2A8_PERM\") returned status %u\n", st);
        CHECK(st & 1, "parent: sys$dlcefc accepted the permanent cluster by name");

        st = sys$dacefc(PERM_EFN);
        CHECK(st & 1, "parent: sys$dacefc released the marked cluster");

        st = sys$ascefc(PERM_EFN, &permnam, 0, 0);
        CHECK(st & 1, "parent: sys$ascefc after the deletion created a cluster of that name again");
        state = 0;
        st = sys$readef(PERM_EFN, &state);
        printf("  INFO: after $DLCEFC + re-create, sys$readef(%d) status=%u state=0x%08x (expect 0)\n",
               PERM_EFN, st, state);
        CHECK((st & 1) && !(state & (1u << (PERM_EFN - COMMON_BASE_3))),
              "parent: sys$dlcefc really deleted the cluster (the re-created one is FRESH, not the old flags)");

        /* Negative control for $DLCEFC: it must not report success for a
         * cluster that does not exist. A `return SS$_NORMAL;` stub fails
         * exactly here and nowhere else. */
        st = sys$dlcefc(&nonam);
        printf("  INFO: sys$dlcefc on a nonexistent name returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$dlcefc does NOT report success for a cluster name the executive does not have");

        (void)sys$dacefc(PERM_EFN);
    }

    printf("=== test_syssvc_ef_mproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
