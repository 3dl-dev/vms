/*
 * test_kmod_eflag_mproc.c - A-writes / B-reads for COMMON event flag
 * clusters, against a real /dev/vms (vms-f1f).
 *
 * WHY THIS SUITE EXISTS
 *
 * CLAUDE.md Rule 11: a VMS system facility is executive-resident shared
 * state, and the decisive check is A-writes/B-reads -- perform the operation
 * in process A and observe it from process B. A per-process fake passes
 * every single-process test perfectly, which is exactly how the logical name
 * tables and the process table went unnoticed.
 *
 * Before this file, event flags had NO multi-process coverage at any layer.
 * tests/qemu/test_kmod_eflag.c drives setef/clref/readef entirely inside one
 * process and never calls ASCEFC at all, so every one of its assertions is
 * satisfied by a purely per-process implementation of event flags. The lock
 * manager has test_kmod_lock_mproc.c; event flags had nothing equivalent.
 *
 * WHAT IS ASSERTED, AND WHY IT IS VMS BEHAVIOUR AND NOT AN OVMX INVENTION
 *
 * Public OpenVMS documentation (System Services Reference, $ASCEFC/$SETEF,
 * and the Programming Concepts manual's event flag chapter) states the
 * split this file pins:
 *
 *   - flags 0-31 and 32-63 are LOCAL to the process;
 *   - flags 64-95 and 96-127 belong to COMMON event flag clusters, which
 *     processes associate with BY NAME via $ASCEFC in order to share them;
 *   - $SETEF on a common flag the process has not associated with fails
 *     (SS$_UNASEFC, "unassociated event flag cluster").
 *
 * NO STATUS CONSTANT IS PINNED HERE. The success/failure assertions test the
 * VMS odd/even status convention only. The reason is deliberate: the tree
 * carries TWO different values for the very statuses this facility returns
 * -- src/kernel/vms_internal.h has SS__WASCLR 5, src/libvms/include/ssdef.h
 * has SS$_WASCLR 1 -- and neither is oracle-pinned. Which one is right needs
 * the reference lab and operator sign-off (project rule: never self-certify
 * a constant), so this file asserts the SHARING PROPERTY, which is
 * documented and is not in dispute, rather than freezing an unverified
 * number into a gate.
 *
 * SYNCHRONISATION: pipes only, no sleeps. Every step waits on a byte the
 * other process wrote after completing the step being observed. The only
 * timeout is a FAILURE bound in the parent (see read_bounded), which turns
 * "hang until run_tests.sh's 120s QEMU timeout kills the whole harness and
 * CI cannot name the suite" into a named FAIL line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <stdint.h>
#include "vms_ioctl.h"

#define EXIT_SKIP 77

/* Bound on each wait for the peer's next handshake byte. Well under
 * run_tests.sh's 120s QEMU TIMEOUT so this suite fails by name rather than
 * killing the harness. Every step completes in milliseconds when healthy. */
#define PEER_TIMEOUT_MS 20000

/* Cluster 2 (flags 64-95). CLUSTER_EFN_A is bit 6, CLUSTER_EFN_B is bit 11. */
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

/* Open /dev/vms and register with the executive. Returns fd, or -1. */
static int bootstrap(const char *who)
{
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: %s: cannot open /dev/vms\n", who);
        return -1;
    }
    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    if (!(reg.status & 1)) {
        printf("  FAIL: %s: VMS_IOCTL_REGISTER status=%u\n", who, reg.status);
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t do_setef(int fd, uint32_t efn)
{
    struct vms_ef_args ef = {0};
    ef.efn = efn;
    ioctl(fd, VMS_IOCTL_SETEF, &ef);
    return ef.status;
}

static uint32_t do_clref(int fd, uint32_t efn)
{
    struct vms_ef_args ef = {0};
    ef.efn = efn;
    ioctl(fd, VMS_IOCTL_CLREF, &ef);
    return ef.status;
}

static uint32_t do_readef(int fd, uint32_t efn, uint32_t *state)
{
    struct vms_ef_read_args rd = {0};
    rd.efn = efn;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    if (state)
        *state = rd.state;
    return rd.status;
}

static uint32_t do_ascefc(int fd, uint32_t efn, const char *name)
{
    struct vms_ef_common_args ac = {0};
    ac.efn = efn;
    strncpy(ac.name, name, sizeof(ac.name) - 1);
    ac.prot = 0;
    ac.perm = 0;
    ioctl(fd, VMS_IOCTL_ASCEFC, &ac);
    return ac.status;
}

/* ================================================================
 * Child: process B. Observes what process A wrote.
 * ================================================================ */
static int run_child(int c2p_write, int p2c_read)
{
    struct child_msg msg;
    char tok;
    uint32_t state;
    uint32_t st;

    int fd = bootstrap("child");
    if (fd < 0) {
        msg.pass = 0;
        msg.fail = 1;
        if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg)) { /* best effort */ }
        return 1;
    }

    /*
     * NEGATIVE CONTROL, taken BEFORE associating, paired with a baseline in
     * the same expression (see the parent's copy for why the pairing is
     * load-bearing). A DIFFERENT local flag from the parent's is used, so
     * this baseline cannot disturb the local-is-not-shared discriminator
     * checked at the end.
     */
    uint32_t st_local = do_setef(fd, LOCAL_EFN_CHILD);
    CHECK(st_local & 1,
          "child: setef on a LOCAL flag succeeds (baseline: the facility is operative in this process)");

    st = do_setef(fd, CLUSTER_EFN_A);
    CHECK((st_local & 1) && !(st & 1),
          "child: setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')");

    /*
     * Wait for A to have created the cluster first. Without this, A and B's
     * own ascefc calls below race to be the FIRST caller (which CREATES the
     * cluster, vms_ioctl_ascefc's "Create new cluster" branch) versus the
     * SECOND (which FINDS it and re-associates, the "Found it -- associate"
     * branch) -- unordered, so which process lands on which branch is
     * scheduler-dependent. That made eflag-ascefc-reassoc-status-wrong
     * genuinely nondeterministic (vms-2b2/vms-400): the assertion that goes
     * red depends on who wins the race, not on the defect. This handshake
     * decides the winner: A always creates, B always re-associates.
     */
    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'P') {
        printf("  FAIL: child: never saw the parent's cluster-created token\n");
        fail++;
    }

    st = do_ascefc(fd, COMMON_BASE, "OVMX$F1F_EFC");
    /* negctl: eflag-ascefc-reassoc-status-wrong */
    CHECK(st & 1, "child: ascefc joined the named common cluster");

    /* Tell A we are associated. */
    if (send_token(c2p_write, 'A') < 0)
        fail++;

    /* Wait for A to have set CLUSTER_EFN_A. */
    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'B') {
        printf("  FAIL: child: never saw the parent's post-setef token\n");
        fail++;
    } else {
        state = 0;
        st = do_readef(fd, CLUSTER_EFN_A, &state);
        CHECK((st & 1) && (state & (1u << (CLUSTER_EFN_A - COMMON_BASE))),
              "child: a common flag SET BY THE PARENT is visible here (A writes, B reads)");
    }

    /* Write in the other direction, then hand back to A. */
    st = do_setef(fd, CLUSTER_EFN_B);
    CHECK(st & 1, "child: setef on the associated common cluster reported success");
    if (send_token(c2p_write, 'C') < 0)
        fail++;

    /* Wait for A to have cleared CLUSTER_EFN_A and set its LOCAL flag. */
    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'D') {
        printf("  FAIL: child: never saw the parent's post-clref token\n");
        fail++;
    } else {
        state = 0;
        st = do_readef(fd, CLUSTER_EFN_A, &state);
        /* negctl: eflag-clref-noop */
        CHECK((st & 1) && !(state & (1u << (CLUSTER_EFN_A - COMMON_BASE))),
              "child: a common flag CLEARED BY THE PARENT reads clear here (A clears, B reads)");

        /*
         * The discriminator. Local clusters are per-process ON VMS TOO, so
         * this must NOT be shared. If it is, the "sharing" observed above is
         * not the common-cluster mechanism at all -- it would mean the two
         * processes are looking at one blob of state for everything, which
         * would make every positive assertion above meaningless.
         */
        state = 0;
        st = do_readef(fd, LOCAL_EFN, &state);
        CHECK((st & 1) && !(state & (1u << LOCAL_EFN)),
              "child: a LOCAL flag set by the parent is NOT visible here (local clusters stay per-process)");
    }

    close(fd);

    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return fail > 0 ? 1 : 0;
}

/* ================================================================
 * Parent: process A.
 * ================================================================ */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    int p2c[2], c2p[2];
    char tok;
    uint32_t st;

    printf("=== test_kmod_eflag_mproc (common event flag clusters, A writes / B reads) ===\n");

    int fd = bootstrap("parent");
    if (fd < 0) {
        /*
         * Reached ONLY in the CI negative control, a rig deliberately booted
         * without insmod'ing vms.ko. Not a product state: vms-0ff ruled that
         * OVMX has no executive-absent mode (PID 1 refuses to boot without
         * /dev/vms). Nothing is asserted here beyond the honest skip -- this
         * suite drives raw ioctls, so with no descriptor there is no product
         * code path left to make a claim about.
         */
        printf("=== test_kmod_eflag_mproc: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
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
        close(fd);
        close(p2c[1]);
        close(c2p[0]);
        _exit(run_child(c2p[1], p2c[0]));
    }

    close(p2c[0]);
    close(c2p[1]);

    /*
     * BASELINE + NEGATIVE CONTROL, asserted together. "setef on an
     * unassociated common flag is refused" is on its own satisfiable by an
     * implementation in which EVERY setef fails -- which is not a
     * hypothetical: the public sys$ layer one file over
     * (tests/qemu/test_syssvc_ef_mproc.c) does exactly that, because
     * src/libvms/syssvc/sys_event.c returns SS$_ILLEFC when the process has
     * no PCB. Requiring the LOCAL flag to succeed in the same expression is
     * what makes the control name the property it claims.
     *
     * The local flag is also the discriminator the child re-reads at the
     * end: local clusters are per-process on VMS, so it must NOT be visible
     * from the child.
     */
    uint32_t st_local = do_setef(fd, LOCAL_EFN);
    CHECK(st_local & 1,
          "parent: setef on a LOCAL flag succeeds (baseline: the facility is operative in this process)");

    st = do_setef(fd, CLUSTER_EFN_A);
    CHECK((st_local & 1) && !(st & 1),
          "parent: setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')");

    st = do_ascefc(fd, COMMON_BASE, "OVMX$F1F_EFC");
    CHECK(st & 1, "parent: ascefc created/joined the named common cluster");

    /* Tell B the cluster now exists, so B's own ascefc (below, via the
     * matching wait on its side) deterministically re-associates rather
     * than racing A to create it -- see B's matching comment. */
    if (send_token(p2c[1], 'P') < 0)
        fail++;

    /* Wait for B to have associated, so the write below cannot be observed
     * by accident through some pre-association path. */
    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'A') {
        printf("  FAIL: parent: child never reported that it associated\n");
        fail++;
    }

    st = do_setef(fd, CLUSTER_EFN_A);
    CHECK(st & 1, "parent: setef on the associated common cluster reported success");
    if (send_token(p2c[1], 'B') < 0)
        fail++;

    /* Wait for B's write in the other direction. */
    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'C') {
        printf("  FAIL: parent: child never reported its own setef\n");
        fail++;
    } else {
        uint32_t state = 0;
        st = do_readef(fd, CLUSTER_EFN_B, &state);
        CHECK((st & 1) && (state & (1u << (CLUSTER_EFN_B - COMMON_BASE))),
              "parent: a common flag SET BY THE CHILD is visible here (B writes, A reads)");
    }

    /* Clear in A, and set a LOCAL flag that must stay invisible to B. */
    st = do_clref(fd, CLUSTER_EFN_A);
    CHECK(st & 1, "parent: clref on the associated common cluster reported success");
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
    close(fd);

    printf("=== test_kmod_eflag_mproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
