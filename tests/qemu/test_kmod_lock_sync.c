/*
 * test_kmod_lock_sync.c - Sync ($ENQW) blocking + completion-AST tests for the
 * kernel lock manager (src/kernel/vms_lock.c), exercised via the raw /dev/vms
 * ioctl layer (the same layer sys$enqw/$enq/$deq delegate to through the
 * vms_kif_* wrappers).
 *
 * Covers the completion path added in vms-ab6 -- the kernel now BLOCKS in
 * kernel for a synchronous request (LCK_M_SYNC) and delivers a completion AST
 * on grant for an asynchronous request -- replacing the old userspace GETLKI
 * poll loop. Three scenarios, each with a real fork() so two distinct
 * registered vms_proc structures contend (a single process cannot: an
 * incompatible request on a resource it already holds is a self-deadlock):
 *
 *   1. Sync block + grant-on-release:
 *        child does a sync ENQ (LCK_M_SYNC) EX behind the parent's EX; the
 *        ioctl must NOT return until the parent DEQs, and then returns with
 *        the lock granted at EX (GETLKI granted_mode == EX). If in-kernel
 *        blocking were absent the ioctl would return immediately with the
 *        request queued at NL.
 *
 *   2. Post-queue deadlock detection:
 *        child holds Y and blocks (sync) requesting X (held by parent);
 *        the parent then requests Y, which CLOSES the cycle. The parent's
 *        sync request must be rejected with SS$_DEADLOCK -- a deadlock
 *        involving a request that was already queued/blocked in-kernel.
 *        The parent then releases X so the child's blocked request drains.
 *
 *   3. Completion AST on grant + DELIVERAST:
 *        child does an async ENQ (no LCK_M_SYNC) CR with a completion-AST
 *        sentinel, queued behind the parent's EX. When the parent DEQs, the
 *        kernel grants the child's CR and queues a completion AST; the child
 *        retrieves it via DELIVERAST (astadr/astprm == sentinels).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"

#define SS_NORMAL       1
#define SS_DEADLOCK     100

#define AST_ASTADR      0xCAFEF00DULL
#define AST_ASTPRM      0x12345678ULL

/* Give the blocking child time to reach its in-kernel wait before the parent
 * takes the action that grants/deadlocks it. Generous for a loaded CI VM. */
#define SETTLE_US       300000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Child -> parent result report. */
struct child_msg {
    uint32_t pass;
    uint32_t fail;
};

static int open_and_register(void)
{
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return -1;

    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    /* NO init_privs (vms-2b8): VMS_IOCTL_REGISTER no longer carries a
     * privilege mask. It used to, and this line asked for all 64 bits --
     * a process declaring its own privileges, which is the honor system
     * that item removed. The executive now DERIVES the mask from the
     * task's real credentials, and nothing below needs a privilege. */
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    if (reg.status != SS_NORMAL) {
        close(fd);
        return -1;
    }
    return fd;
}

static void report(int wfd, int p, int f)
{
    struct child_msg m = { .pass = (uint32_t)p, .fail = (uint32_t)f };
    if (write(wfd, &m, sizeof(m)) != (ssize_t)sizeof(m))
        _exit(2);
}

/* ================================================================
 * Scenario 1: sync block, granted when the conflicting lock is DEQ'd
 * ================================================================ */
static void child_sync_block(int c2p_w)
{
    int fd = open_and_register();
    if (fd < 0) _exit(1);

    /* Signal "ready", then issue the blocking sync ENQ. */
    char r = 'R';
    if (write(c2p_w, &r, 1) != 1) _exit(2);

    struct vms_enq_args enq = {0};
    enq.lkmode = LCK_K_EXMODE;
    enq.flags = LCK_M_SYNC;
    strncpy(enq.resnam, "SYNCRES1", sizeof(enq.resnam));
    /* Blocks in-kernel until the parent releases its EX. */
    ioctl(fd, VMS_IOCTL_ENQ, &enq);
    CHECK(enq.status == SS_NORMAL, "child: sync ENQ EX returned granted");

    /* Prove the request returned only after actually being granted at EX,
     * not immediately-queued at NL. */
    struct vms_getlki_args lki = {0};
    lki.lkid = enq.lkid;
    ioctl(fd, VMS_IOCTL_GETLKI, &lki);
    CHECK(lki.status == SS_NORMAL && lki.granted_mode == LCK_K_EXMODE,
          "child: sync ENQ blocked until granted at EX (GETLKI == EX)");

    struct vms_deq_args deq = {0};
    deq.lkid = enq.lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &deq);

    report(c2p_w, pass, fail);
    close(fd);
    _exit(fail > 0 ? 1 : 0);
}

static void run_sync_block(int pfd, int *tot_pass, int *tot_fail)
{
    printf("--- scenario 1: sync block + grant on release ---\n");

    struct vms_enq_args pex = {0};
    pex.lkmode = LCK_K_EXMODE;
    strncpy(pex.resnam, "SYNCRES1", sizeof(pex.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &pex);
    CHECK(pex.status == SS_NORMAL && pex.lkid != 0, "parent: EX granted on SYNCRES1");

    int c2p[2];
    if (pipe(c2p) < 0) { fail++; return; }

    pid_t pid = fork();
    if (pid < 0) { fail++; return; }
    if (pid == 0) {
        close(c2p[0]);
        child_sync_block(c2p[1]);
    }
    close(c2p[1]);

    /* Wait for child ready, let it reach the in-kernel wait, then release. */
    char r = 0;
    if (read(c2p[0], &r, 1) != 1) fail++;
    usleep(SETTLE_US);

    struct vms_deq_args pdeq = {0};
    pdeq.lkid = pex.lkid;
    ioctl(pfd, VMS_IOCTL_DEQ, &pdeq);
    CHECK(pdeq.status == SS_NORMAL, "parent: released EX (should grant child)");

    struct child_msg m = {0};
    if (read(c2p[0], &m, sizeof(m)) == (ssize_t)sizeof(m)) {
        *tot_pass += (int)m.pass;
        *tot_fail += (int)m.fail;
    } else {
        fail++;
    }

    int ws = 0;
    waitpid(pid, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: child (sync block) exited clean");
    close(c2p[0]);
}

/* ================================================================
 * Scenario 2: deadlock detected for an already-queued (blocked) request
 * ================================================================ */
static void child_deadlock(int c2p_w)
{
    int fd = open_and_register();
    if (fd < 0) _exit(1);

    /* Hold Y. */
    struct vms_enq_args ey = {0};
    ey.lkmode = LCK_K_EXMODE;
    strncpy(ey.resnam, "DLRES_Y", sizeof(ey.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &ey);
    CHECK(ey.status == SS_NORMAL, "child: EX granted on DLRES_Y");

    /* Tell parent we hold Y, then block requesting X (parent holds X). */
    char a = 'A';
    if (write(c2p_w, &a, 1) != 1) _exit(2);

    struct vms_enq_args ex = {0};
    ex.lkmode = LCK_K_EXMODE;
    ex.flags = LCK_M_SYNC;
    strncpy(ex.resnam, "DLRES_X", sizeof(ex.resnam));
    /* Blocks until the parent releases X (parent's own request on Y is the
     * one rejected for deadlock; ours drains once X is free). */
    ioctl(fd, VMS_IOCTL_ENQ, &ex);
    CHECK(ex.status == SS_NORMAL, "child: blocked EX on DLRES_X granted after parent releases X");

    struct vms_deq_args dx = {0};
    dx.lkid = ex.lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &dx);
    struct vms_deq_args dy = {0};
    dy.lkid = ey.lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &dy);

    report(c2p_w, pass, fail);
    close(fd);
    _exit(fail > 0 ? 1 : 0);
}

static void run_deadlock(int pfd, int *tot_pass, int *tot_fail)
{
    printf("--- scenario 2: post-queue deadlock detection ---\n");

    struct vms_enq_args px = {0};
    px.lkmode = LCK_K_EXMODE;
    strncpy(px.resnam, "DLRES_X", sizeof(px.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &px);
    CHECK(px.status == SS_NORMAL && px.lkid != 0, "parent: EX granted on DLRES_X");

    int c2p[2];
    if (pipe(c2p) < 0) { fail++; return; }

    pid_t pid = fork();
    if (pid < 0) { fail++; return; }
    if (pid == 0) {
        close(c2p[0]);
        child_deadlock(c2p[1]);
    }
    close(c2p[1]);

    /* Wait until the child holds Y and has entered its blocking request on X. */
    char a = 0;
    if (read(c2p[0], &a, 1) != 1) fail++;
    usleep(SETTLE_US);

    /* Parent requests Y (held by child) -> closes the cycle
     * (parent waits on child's Y; child waits on parent's X). This request
     * must be rejected with SS$_DEADLOCK. */
    struct vms_enq_args py = {0};
    py.lkmode = LCK_K_EXMODE;
    py.flags = LCK_M_SYNC;
    strncpy(py.resnam, "DLRES_Y", sizeof(py.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &py);
    CHECK(py.status == SS_DEADLOCK,
          "parent: sync ENQ closing the cycle rejected SS$_DEADLOCK");

    /* Break the standoff: release X so the child's blocked request drains. */
    struct vms_deq_args dpx = {0};
    dpx.lkid = px.lkid;
    ioctl(pfd, VMS_IOCTL_DEQ, &dpx);

    struct child_msg m = {0};
    if (read(c2p[0], &m, sizeof(m)) == (ssize_t)sizeof(m)) {
        *tot_pass += (int)m.pass;
        *tot_fail += (int)m.fail;
    } else {
        fail++;
    }

    int ws = 0;
    waitpid(pid, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: child (deadlock) exited clean");
    close(c2p[0]);
}

/* ================================================================
 * Scenario 3: completion AST delivered on grant, retrieved via DELIVERAST
 * ================================================================ */
static void child_completion_ast(int c2p_w, int p2c_r)
{
    int fd = open_and_register();
    if (fd < 0) _exit(1);

    /* Async ENQ (no LCK_M_SYNC) CR with a completion AST, queued behind the
     * parent's EX. Returns immediately with the request queued. */
    struct vms_enq_args enq = {0};
    enq.lkmode = LCK_K_CRMODE;
    enq.astadr = AST_ASTADR;
    enq.astprm = AST_ASTPRM;
    strncpy(enq.resnam, "ASTRES", sizeof(enq.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq);
    CHECK(enq.status == SS_NORMAL && enq.lkid != 0, "child: async CR queued behind parent EX");

    /* Tell parent it's queued; wait for parent to release its EX. */
    char q = 'Q';
    if (write(c2p_w, &q, 1) != 1) _exit(2);
    char go = 0;
    if (read(p2c_r, &go, 1) != 1) _exit(2);

    /* Now granted. Confirm the grant, then drain the completion AST. */
    struct vms_getlki_args lki = {0};
    lki.lkid = enq.lkid;
    ioctl(fd, VMS_IOCTL_GETLKI, &lki);
    CHECK(lki.status == SS_NORMAL && lki.granted_mode == LCK_K_CRMODE,
          "child: async request granted at CR after parent release");

    struct vms_ast_args ast = {0};
    int ar = ioctl(fd, VMS_IOCTL_DELIVERAST, &ast);
    CHECK(ar == 0 && ast.status == SS_NORMAL &&
          ast.astadr == AST_ASTADR && ast.astprm == AST_ASTPRM,
          "child: DELIVERAST returned completion AST (astadr/astprm sentinels)");

    struct vms_deq_args deq = {0};
    deq.lkid = enq.lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &deq);

    report(c2p_w, pass, fail);
    close(fd);
    _exit(fail > 0 ? 1 : 0);
}

static void run_completion_ast(int pfd, int *tot_pass, int *tot_fail)
{
    printf("--- scenario 3: completion AST on grant + DELIVERAST ---\n");

    struct vms_enq_args pex = {0};
    pex.lkmode = LCK_K_EXMODE;
    strncpy(pex.resnam, "ASTRES", sizeof(pex.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &pex);
    CHECK(pex.status == SS_NORMAL && pex.lkid != 0, "parent: EX granted on ASTRES");

    int c2p[2], p2c[2];
    if (pipe(c2p) < 0 || pipe(p2c) < 0) { fail++; return; }

    pid_t pid = fork();
    if (pid < 0) { fail++; return; }
    if (pid == 0) {
        close(c2p[0]);
        close(p2c[1]);
        child_completion_ast(c2p[1], p2c[0]);
    }
    close(c2p[1]);
    close(p2c[0]);

    /* Wait for the child's request to be queued, then release EX (grants it
     * and queues the child's completion AST). */
    char q = 0;
    if (read(c2p[0], &q, 1) != 1) fail++;

    struct vms_deq_args pdeq = {0};
    pdeq.lkid = pex.lkid;
    ioctl(pfd, VMS_IOCTL_DEQ, &pdeq);
    CHECK(pdeq.status == SS_NORMAL, "parent: released EX on ASTRES (grants child + queues AST)");

    char go = 'g';
    if (write(p2c[1], &go, 1) != 1) fail++;

    struct child_msg m = {0};
    if (read(c2p[0], &m, sizeof(m)) == (ssize_t)sizeof(m)) {
        *tot_pass += (int)m.pass;
        *tot_fail += (int)m.fail;
    } else {
        fail++;
    }

    int ws = 0;
    waitpid(pid, &ws, 0);
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: child (completion AST) exited clean");
    close(c2p[0]);
    close(p2c[1]);
}

int main(void)
{
    printf("=== test_kmod_lock_sync ===\n");

    int pfd = open_and_register();
    CHECK(pfd >= 0, "parent: open + register /dev/vms");
    if (pfd < 0) {
        printf("=== test_kmod_lock_sync: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    int tot_pass = 0, tot_fail = 0;
    run_sync_block(pfd, &tot_pass, &tot_fail);
    run_deadlock(pfd, &tot_pass, &tot_fail);
    run_completion_ast(pfd, &tot_pass, &tot_fail);

    close(pfd);

    tot_pass += pass;
    tot_fail += fail;
    printf("=== test_kmod_lock_sync: %d passed, %d failed ===\n", tot_pass, tot_fail);
    return tot_fail > 0 ? 1 : 0;
}
