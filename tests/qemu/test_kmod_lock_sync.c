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
 *
 *   4. Value block delivered to a waiter that BLOCKED and was later granted
 *      (vms-413):
 *        try_grant_waiters() (src/kernel/vms_lock.c:412) is the SOLE site
 *        that copies res->valblk into a waiter's own lock->valblk at grant
 *        time; every OTHER valblk assertion in this tree (test_kmod_lock.c's
 *        "value block preserved across DEQ/ENQ") re-acquires UNCONTENDED,
 *        which takes the immediate-grant branch in vms_ioctl_enq
 *        (vms_lock.c:664-680) and never reaches try_grant_waiters() at all.
 *        This scenario is the one place in the tree that forces the
 *        BLOCKED-then-granted path: the parent takes EX with a distinctive
 *        16-byte value block (VALBLK_SEED) -- published to res->valblk at
 *        vms_lock.c:677, since the parent's own ENQ is itself uncontended.
 *        The child then does an ASYNC ENQ (no LCK_M_SYNC) for EX with a
 *        DIFFERENT 16-byte value block (VALBLK_CHILD_SENTINEL) of its own;
 *        since the parent holds EX this is incompatible and the child's
 *        ioctl call queues the request and returns -- GUARANTEED already on
 *        res->waiting by the time the ioctl returns (vms_ioctl_enq takes
 *        res->lock, decides queue-or-grant, and only then unlocks and
 *        returns; there is no async completion step it could return before).
 *        That guarantee is what makes the parent -> child 'Q' handshake
 *        below race-free without a sleep: the child cannot send 'Q' until
 *        its own request is already enqueued, and the parent cannot DEQ
 *        until it has read 'Q'.
 *
 *        The parent's subsequent DEQ (vms_ioctl_deq, vms_lock.c:782) runs
 *        try_grant_waiters() synchronously, in-kernel, before the DEQ ioctl
 *        returns -- so by the time the parent signals 'go', the child's lock
 *        has already been granted (or not, under the mutation) and a single
 *        GETLKI is enough; no poll loop is needed either. The child asserts
 *        its own lock's valblk, read back via GETLKI (vms_lock.c:982, which
 *        unconditionally echoes lock->valblk and is a different site from
 *        the one under test), now equals the PARENT's VALBLK_SEED -- not its
 *        own VALBLK_CHILD_SENTINEL, which is what it would still read if
 *        vms_lock.c:425-426 stopped running.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

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
     * not immediately-queued at NL.
     *
     * CONVERTED (vms-290): was raw ioctl(fd, VMS_IOCTL_GETLKI, &lki). Now
     * exercises vms_kif_getlki() (src/libvmssys/vms_kif.c), which had zero
     * callers anywhere in the checkout before this. */
    uint32_t granted_mode = 0;
    uint32_t getlki_st = vms_kif_getlki(enq.lkid, &granted_mode, NULL, NULL, NULL);
    CHECK(getlki_st == SS_NORMAL && granted_mode == LCK_K_EXMODE,
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

    /*
     * Now granted. Confirm the grant, then drain the completion AST.
     *
     * CONVERTED (vms-290): both calls below used to be raw ioctls
     * (VMS_IOCTL_GETLKI, VMS_IOCTL_DELIVERAST) against `fd`. They now go
     * through vms_kif_getlki()/vms_kif_deliverast() (src/libvmssys/
     * vms_kif.c), which had zero callers anywhere in the checkout before
     * this. The DELIVERAST assertion drops the explicit `status ==
     * SS_NORMAL` check: the kernel only ever writes SS__NORMAL on the
     * success path that returns 0 (src/kernel/vms_ast.c
     * vms_ioctl_deliverast(), `args.status = SS__NORMAL;` immediately
     * before the 0 return) -- so `ar == 0` already IS "status ==
     * SS_NORMAL"; vms_kif_deliverast()'s int return is that same fact, and
     * it has no separate status out-parameter to check.
     *
     * CORRECTED (vms-290 round 3, false emphatic): a prior version of this
     * comment claimed "the only other path returns -EAGAIN with args
     * untouched". Read against src/kernel/vms_ioctl_deliverast() that is
     * false -- there are TWO non-success returns, not one: -EAGAIN when no
     * AST is pending for any enabled mode, and -EFAULT if copy_to_user()
     * fails on the caller's buffer (reachable with a bad pointer, and
     * test_kmod_ast.c's deliberately-raw NULL-arg DELIVERAST call, kept
     * unconverted for exactly this reason, exercises it). Neither matters
     * to `ar == 0` above: vms_kif_deliverast() only writes its out-params
     * when the ioctl call returns >= 0 (src/libvmssys/vms_kif.c), so both
     * non-success kernel returns are indistinguishable at this call site --
     * this scenario's buffer is always the wrapper's own valid on-stack
     * struct, so -EFAULT is not reachable HERE, but the general claim
     * about the kernel function was still wrong and is removed.
     */
    uint32_t granted_mode = 0;
    uint32_t getlki_st = vms_kif_getlki(enq.lkid, &granted_mode, NULL, NULL, NULL);
    CHECK(getlki_st == SS_NORMAL && granted_mode == LCK_K_CRMODE,
          "child: async request granted at CR after parent release");

    uint64_t d_astadr = 0, d_astprm = 0;
    uint8_t d_acmode = 0;
    int ar = vms_kif_deliverast(&d_astadr, &d_astprm, &d_acmode);
    CHECK(ar == 0 && d_astadr == AST_ASTADR && d_astprm == AST_ASTPRM,
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

/* ================================================================
 * Scenario 4: value block delivered to a waiter that BLOCKED and was
 * later granted (vms-413). See the file header for the full design.
 * ================================================================ */

/* Two distinct, non-trivial 16-byte patterns (LCK_VALBLK_SIZE == 16) --
 * neither all-zero nor all-same-byte, so a match can only come from an
 * actual copy of one into the other, never from an uninitialized or
 * memset(0) buffer coincidentally "matching". */
static const uint8_t VALBLK_SEED[16]            = "VMS413-VALBLOCKA"; /* parent's, published at ENQ-grant */
static const uint8_t VALBLK_CHILD_SENTINEL[16]  = "CHILDOWNVALBLK00"; /* child's own, must NOT survive grant */

static void child_valblk_grant(int c2p_w, int p2c_r)
{
    /* SNAPSHOT, not a fresh counter: `pass`/`fail` are ONE static pair
     * shared by the whole process, inherited by every fork() -- including
     * whatever scenarios 1-3 already accumulated in the PARENT before this
     * child was forked. MEASURED (vms-413, isolated repro against
     * lock-compat-cr-ex): under that mutation scenario 3's PARENT-side
     * "parent: child (completion AST) exited clean" check fails and
     * increments the PARENT's `fail` to 1 -- and this child then inherits
     * fail==1 at fork(), through zero failures of its OWN (every CHECK()
     * below prints PASS), and `_exit(fail > 0 ? 1 : 0)` would still exit 1,
     * making THIS child look like it failed for a defect that belongs to a
     * different scenario entirely. Comparing against the count AT ENTRY,
     * not the raw total, is what makes this child's own exit code report
     * only its own scenario -- exactly the property "parent: child (valblk
     * grant) exited clean" exists to check. This is a pre-existing latent
     * risk in scenarios 1-3's identical `_exit(fail > 0 ? 1 : 0)` pattern
     * too (nothing before scenario 3 has ever failed under this manifest,
     * so it was never observed) -- out of scope here; not touched. */
    int fail_at_entry = fail;

    int fd = open_and_register();
    if (fd < 0) _exit(1);

    /* Async ENQ (no LCK_M_SYNC) EX + VALBLK, queued behind the parent's EX.
     * Returns immediately with the request already on res->waiting -- see
     * the file header for why that return is a synchronous guarantee, not a
     * race. The value block sent here is the child's OWN sentinel, distinct
     * from the parent's VALBLK_SEED, so a later readback that still shows
     * this sentinel proves delivery did NOT happen. */
    struct vms_enq_args enq = {0};
    enq.lkmode = LCK_K_EXMODE;
    enq.flags = LCK_M_VALBLK;
    memcpy(enq.valblk, VALBLK_CHILD_SENTINEL, sizeof(VALBLK_CHILD_SENTINEL));
    strncpy(enq.resnam, "VALBLKRES", sizeof(enq.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq);
    CHECK(enq.status == SS_NORMAL && enq.lkid != 0,
          "child: async EX+VALBLK queued behind parent's EX");

    /* Tell parent it's queued; wait for parent to release its EX. */
    char q = 'Q';
    if (write(c2p_w, &q, 1) != 1) _exit(2);
    char go = 0;
    if (read(p2c_r, &go, 1) != 1) _exit(2);

    /* Now granted (the parent's DEQ already ran try_grant_waiters()
     * in-kernel before it wrote 'go' -- see the file header). Confirm the
     * grant, then read back this lock's OWN value block via GETLKI
     * (vms_lock.c:982, an unconditional read of lock->valblk -- a
     * different site from vms_lock.c:425-426, the copy under test). */
    uint32_t granted_mode = 0;
    char resnam_out[32] = {0};
    uint8_t valblk_out[16] = {0};
    uint32_t getlki_st = vms_kif_getlki(enq.lkid, &granted_mode, NULL,
                                         resnam_out, valblk_out);
    CHECK(getlki_st == SS_NORMAL && granted_mode == LCK_K_EXMODE,
          "child: async EX request granted after parent released VALBLKRES");
    CHECK(memcmp(valblk_out, VALBLK_SEED, sizeof(VALBLK_SEED)) == 0,
          "child: GETLKI value block equals the PARENT's, not this lock's own pre-grant value (blocked-then-granted valblk delivery)");

    struct vms_deq_args deq = {0};
    deq.lkid = enq.lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &deq);
    CHECK(deq.status == SS_NORMAL, "child: released its own granted EX on VALBLKRES");

    report(c2p_w, pass, fail);
    close(fd);
    _exit((fail - fail_at_entry) > 0 ? 1 : 0);
}

static void run_valblk_grant(int pfd, int *tot_pass, int *tot_fail)
{
    printf("--- scenario 4: value block delivered to a blocked-then-granted waiter (vms-413) ---\n");

    /* Parent takes EX with a distinctive value block. Uncontended (fresh
     * resource), so this is the IMMEDIATE-grant branch (vms_lock.c:664-680)
     * -- NOT the code under test -- and publishes VALBLK_SEED into
     * res->valblk at vms_lock.c:677. */
    struct vms_enq_args pex = {0};
    pex.lkmode = LCK_K_EXMODE;
    pex.flags = LCK_M_VALBLK;
    memcpy(pex.valblk, VALBLK_SEED, sizeof(VALBLK_SEED));
    strncpy(pex.resnam, "VALBLKRES", sizeof(pex.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &pex);
    CHECK(pex.status == SS_NORMAL && pex.lkid != 0,
          "parent: EX+VALBLK granted immediately on fresh VALBLKRES (publishes VALBLK_SEED)");

    int c2p[2], p2c[2];
    if (pipe(c2p) < 0 || pipe(p2c) < 0) { fail++; return; }

    pid_t pid = fork();
    if (pid < 0) { fail++; return; }
    if (pid == 0) {
        close(c2p[0]);
        close(p2c[1]);
        child_valblk_grant(c2p[1], p2c[0]);
    }
    close(c2p[1]);
    close(p2c[0]);

    /* Wait for the child's request to be queued (deterministic: see file
     * header), then release EX -- this runs try_grant_waiters() in-kernel
     * before the DEQ ioctl returns. */
    char q = 0;
    if (read(c2p[0], &q, 1) != 1) fail++;

    struct vms_deq_args pdeq = {0};
    pdeq.lkid = pex.lkid;
    ioctl(pfd, VMS_IOCTL_DEQ, &pdeq);
    CHECK(pdeq.status == SS_NORMAL,
          "parent: released EX on VALBLKRES (grants child's queued request)");

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
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: child (valblk grant) exited clean");
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
    run_valblk_grant(pfd, &tot_pass, &tot_fail);

    close(pfd);

    tot_pass += pass;
    tot_fail += fail;
    printf("=== test_kmod_lock_sync: %d passed, %d failed ===\n", tot_pass, tot_fail);
    return tot_fail > 0 ? 1 : 0;
}
