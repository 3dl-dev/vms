/*
 * test_syssvc_ef_mproc.c - common event flag clusters through the PUBLIC
 * sys$ API, A-writes / B-reads (vms-f1f).
 *
 * ============================================================
 * THIS SUITE IS EXPECTED TO FAIL ON THE TREE THAT INTRODUCED IT.
 * ============================================================
 *
 * It is the reproduction of a defect, not a regression guard for working
 * code. Do not delete it, do not weaken it, and do not "fix" it by asserting
 * the behaviour OVMX currently has. What it reports is true:
 *
 *   src/libvms/syssvc/sys_event.c implements the ENTIRE event flag facility
 *   in per-process memory (struct vms_pcb's ef_clusters[]) and NEVER calls
 *   the executive. Every vms_kif_* event-flag entry point --
 *   vms_kif_setef, vms_kif_clref, vms_kif_readef, vms_kif_waitfr,
 *   vms_kif_wflor, vms_kif_wfland, vms_kif_ascefc, vms_kif_dacefc -- has
 *   ZERO callers product-wide, exactly like vms_kif_register() before
 *   vms-9fc. src/kernel/vms_eflag.c implements common clusters properly, on
 *   a module-global list (vms_common_ef_list), and nothing in the product
 *   reaches it.
 *
 *   sys$ascefc is worse than unwired: it is `return SS$_NORMAL;` with a TODO
 *   and no side effect at all. A process asks to join a named common
 *   cluster, is told it succeeded, and shares nothing. Under CLAUDE.md
 *   Rule 10 that is the illegal third answer -- a plausible-looking handler
 *   for a condition VMS never faces -- and under Rule 11 it is a facade: a
 *   system facility living in per-process memory that reports success while
 *   sharing nothing.
 *
 * WHY THE PROPERTY BELOW IS VMS BEHAVIOUR AND NOT AN OVMX INVENTION
 *
 * Public OpenVMS documentation (System Services Reference, $ASCEFC/$SETEF;
 * Programming Concepts, event flags) states that flags 0-63 are local to the
 * process while flags 64-127 belong to COMMON event flag clusters that
 * processes associate with BY NAME precisely in order to share them. Sharing
 * IS the facility. A $ASCEFC that shares nothing has not implemented it.
 *
 * NO STATUS CONSTANT IS PINNED. The tree carries two different values for
 * this facility's own statuses (src/kernel/vms_internal.h SS__WASCLR 5 vs
 * src/libvms/include/ssdef.h SS$_WASCLR 1) and neither is oracle-pinned, so
 * assertions here use the VMS odd/even success convention only. Pinning an
 * unverified number would be self-certifying it.
 *
 * The mirror-image suite tests/qemu/test_kmod_eflag_mproc.c asserts the SAME
 * property one layer down, through raw ioctls, and PASSES -- so the two
 * together locate the defect exactly: the executive shares correctly, and
 * the public system service never asks it to.
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

    printf("=== test_syssvc_ef_mproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
