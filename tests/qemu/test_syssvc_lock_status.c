/*
 * test_syssvc_lock_status.c - the lock manager yields VMS condition values
 * (vms-2e5)
 *
 * src/libvms/syssvc/sys_lock.c's kstat_to_ss() is the SINGLE POINT where a
 * raw kernel lock-manager status crosses into the public ssdef.h SS$_xxx
 * contract. Before this suite, nothing at any layer asserted the specific
 * VMS status a caller receives for SS$_DEADLOCK, SS$_IVLOCKID or
 * SS$_CVTUNGRANT -- proven by mutation: flipping `case 100: return
 * SS$_DEADLOCK;` to `return SS$_NOTQUEUED;`, rebuilding, and rebooting QEMU
 * produced "FINAL RESULTS: 23 suites passed, 0 suites failed", rc=0. A
 * caller told "not queued" when the real condition was DEADLOCK retries
 * forever instead of breaking the cycle -- this is the reported-but-wrong
 * shape Rule 10 forbids, at the one function whose entire job is being
 * right about status values.
 *
 * WHY A SEPARATE SUITE FROM test_syssvc_lock.c: that file's process
 * topology is a single resource with one holder and one contender (the
 * NOQUEUE-denial / cross-process-release scenario). SS$_DEADLOCK needs TWO
 * resources and a genuine wait-for cycle; SS$_CVTUNGRANT needs a lock
 * caught mid-queue when a second CONVERT lands on it. Folding those into
 * the existing file would make one mutation's blast radius impossible to
 * read cleanly against the other suite's assertions; a dedicated suite
 * keeps facility_defects.sh's per-defect suites_red attribution exact (see
 * that manifest's WHY THIS FILE EXISTS header on the minimality rule).
 *
 * SS$_SUBLOCKS and SS$_VALNOTVALID are DELIBERATELY NOT COVERED HERE.
 * Exhaustive grep of every `args.status = ` assignment in
 * src/kernel/vms_lock.c (the entire kernel lock manager) finds SS__BADPARAM,
 * SS__INSFMEM, SS__NORMAL, SS__NOTQUEUED, SS__DEADLOCK, SS__IVLOCKID and
 * SS__CANCELGRANT -- never SS__SUBLOCKS or SS__VALNOTVALID.
 * vms_ioctl_getlki even carries `args.parent_id = 0; -- TODO: parent lock
 * support`: the kernel implements no lock hierarchy and no value-block
 * validity tracking, so nothing can ever emit those two condition values at
 * all. They are correct-if-ever-reached
 * but currently unreachable dead code, not a fidelity gap this suite can
 * provoke by exercising real behaviour -- inventing a kernel path that
 * emits them just to make an assertion pass would be the illegal third
 * answer Rule 10 names (a handler for a condition nothing in OVMX yet
 * faces). Tracked for follow-up, not silently dropped.
 *
 * This program calls the PUBLIC sys$enq / sys$enqw / sys$deq entry points
 * (starlet.h, implemented in src/libvms/syssvc/sys_lock.c) -- the same
 * functions any real OVMX program links against.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened -- the CI negative-control rig only, never the product (vms-0ff:
 * PID 1 refuses to boot without the executive) -- it exercises the
 * no-fabricated-success checks in main() and exits EXIT_SKIP (77), never a
 * fake pass. No VMS-defined status exists for "the executive was never
 * reached" (see test_syssvc_lock.c's parent branch for the same reasoning),
 * so that branch asserts only the odd/even success bit, never a specific
 * mapped constant.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

/* See test_syssvc_lock.c's file header for why lckdef.h is deliberately not
 * included here (LCK$M_NOQUEUE disagrees with starlet.h's value; the
 * divergence is vms-5bd and not this file's call). starlet.h alone is used
 * because it is what sys_lock.c itself compiles against. */
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

/* Caller-allocated Lock Status Block -- see test_syssvc_lock.c's file header
 * for why there is no shared public lksdef.h (Rule 8: no VMS-published byte
 * layout to pin one against). Field order mirrors sys_lock.c's private
 * struct, which is what the implementation under test writes through this
 * pointer. */
struct lksb_caller {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];
};

#define EXIT_SKIP 77

/* Bound on any pipe read that could otherwise hang the whole QEMU boot --
 * see test_syssvc_lock.c's read_bounded comment for why this bound must
 * live in the reader, not an alarm in the blocked side (a process blocked
 * in-kernel on a sync $ENQW cannot be woken by a userspace signal). */
#define REPORT_TIMEOUT_MS 20000

/* How long the parent waits, after the child reports it has entered its
 * blocking wait, before issuing the closing request -- gives the child time
 * to actually reach the in-kernel wait via the ioctl, not just return from
 * write(). Mirrors tests/qemu/test_kmod_lock_sync.c's SETTLE_US. */
#define SETTLE_US 300000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct child_msg {
    uint32_t pass;
    uint32_t fail;
};

static int bootstrap(const char *who)
{
    /* Opens /dev/vms ONLY to decide skip-vs-run, the same shape
     * test_syssvc_showdev.c uses (see that suite and facility_defects.sh's
     * bind-client-no-register blind_why). It deliberately does NOT also
     * call vms_kif_register() here: this program drives sys$enq/enqw/deq,
     * the PUBLIC API, and those wrappers reach kif_bind()
     * (src/libvmssys/vms_kif.c) on their own, the same auto-bind path any
     * real OVMX image goes through. A suite that hand-registers before
     * ever calling the public API cannot see the bind-client-no-register
     * defect (kif_bind() dropping its own vms_kif_register() call): the
     * explicit call would still bind the process, and kif_bind()'s
     * pid-match check would then skip re-registering, staying green under
     * that mutation. Not calling it here is what makes this suite bind
     * exactly the way a product image binds, and therefore able to go red
     * for that defect instead of silently joining vms-f27's blind set. */
    if (vms_kif_open() < 0) {
        printf("  FAIL: %s: cannot open /dev/vms\n", who);
        return -1;
    }
    return 0;
}

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

/* ================================================================
 * Scenario 1: SS$_IVLOCKID -- sys$deq on a lock ID that does not exist,
 * with a REAL executive present (distinct from the absent-executive branch
 * in main(), which reaches the same call but for a different reason and
 * asserts no specific value).
 * ================================================================ */
static void scenario_ivlockid(void)
{
    printf("--- scenario 1: SS$_IVLOCKID (sys$deq, unknown lock ID, real executive) ---\n");

    uint32_t st = sys$deq(0xDEADBEEF, NULL, 0, 0);
    printf("  INFO: sys$deq(0xDEADBEEF) returned status %u\n", st);
    /* negctl: kstat-ivlockid-mismapped */
    /* negctl-knockon: bind-client-no-register */
    CHECK(st == SS$_IVLOCKID,
          "sys$deq on an unknown lock ID reports SS$_IVLOCKID (public API, real executive)");
}

/* ================================================================
 * Scenario 2: SS$_CVTUNGRANT -- a CONVERT that lands on a lock still
 * queued (waiting) from an earlier request. src/kernel/vms_lock.c's
 * vms_ioctl_convert checks `lock->waiting` before anything else and
 * returns SS__CANCELGRANT immediately if true.
 *
 * Child holds EX on the resource. Parent's own sys$enq (async, no
 * LCK$M_NOQUEUE) for the same resource is incompatible with the child's
 * EX, so it QUEUES -- the kernel still assigns and returns a real lock ID
 * for the queued request (src/kernel/vms_lock.c ~line 764). The parent
 * then immediately issues a second sys$enq with LCK$M_CONVERT on that same
 * lock ID: it is still `waiting` (the child has not released), so the
 * kernel rejects with SS__CANCELGRANT -> SS$_CVTUNGRANT. Neither of the
 * parent's calls blocks in-kernel, so no timeout handling is needed here.
 * ================================================================ */
static void child_cvtungrant_holder(int ready_w, int go_r)
{
    if (bootstrap("cvtungrant child") < 0)
        _exit(1);

    $DESCRIPTOR(resnam, "SYSSVC_STATUS_CVT");
    struct lksb_caller lksb = {0};
    uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb, 0, &resnam, 0, NULL, 0, NULL, 0, 0);
    if (!(st & 1) || lksb.lksb$l_lkid == 0)
        _exit(1);

    char r = 'r';
    if (write(ready_w, &r, 1) != 1)
        _exit(1);

    char go = 0;
    if (read(go_r, &go, 1) != 1)
        _exit(1);

    uint32_t dst = sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
    _exit((dst & 1) ? 0 : 1);
}

static void scenario_cvtungrant(void)
{
    printf("--- scenario 2: SS$_CVTUNGRANT (convert lands on an already-queued lock) ---\n");

    int ready_pipe[2], go_pipe[2];
    if (pipe(ready_pipe) < 0 || pipe(go_pipe) < 0) {
        printf("  FAIL: pipe() setup\n");
        fail++;
        return;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        return;
    }
    if (child_pid == 0) {
        close(ready_pipe[0]);
        close(go_pipe[1]);
        child_cvtungrant_holder(ready_pipe[1], go_pipe[0]);
        _exit(1); /* unreachable */
    }
    close(ready_pipe[1]);
    close(go_pipe[0]);

    char r = 0;
    int rr = read_bounded(ready_pipe[0], &r, 1, REPORT_TIMEOUT_MS);
    /* negctl-knockon: bind-client-no-register */
    CHECK(rr == 1, "parent: child took EX before the CVTUNGRANT probe (setup, not the property under test)");

    $DESCRIPTOR(resnam, "SYSSVC_STATUS_CVT");
    struct lksb_caller lksb_q = {0};
    uint32_t st = sys$enq(0, LCK$K_CRMODE, &lksb_q, 0, &resnam, 0, NULL, 0, NULL, 0, 0);
    /* negctl-knockon: bind-client-no-register */
    CHECK((st & 1) && lksb_q.lksb$l_lkid != 0,
          "parent: sys$enq CR queues behind the child's EX and still returns a real lock ID (public API)");

    if ((st & 1) && lksb_q.lksb$l_lkid != 0) {
        struct lksb_caller lksb_cvt = {0};
        lksb_cvt.lksb$l_lkid = lksb_q.lksb$l_lkid;
        uint32_t cst = sys$enq(0, LCK$K_PRMODE, &lksb_cvt, LCK$M_CONVERT,
                                &resnam, 0, NULL, 0, NULL, 0, 0);
        printf("  INFO: sys$enq(CONVERT) on the still-queued lock returned status %u\n", cst);
        /* negctl: kstat-cvtungrant-mismapped */
        /* negctl-knockon: lock-compat-cr-ex */
        CHECK(cst == SS$_CVTUNGRANT,
              "sys$enq(LCK$M_CONVERT) on a lock still queued (waiting) reports SS$_CVTUNGRANT (public API)");

        /* Clean up the queued lock regardless of the convert's outcome --
         * sys$deq handles a still-waiting lock (src/kernel/vms_lock.c's
         * vms_ioctl_deq removes from res_waiting, not res_granted). */
        uint32_t dst = sys$deq(lksb_q.lksb$l_lkid, NULL, 0, 0);
        CHECK(dst & 1, "parent: dequeued its still-queued CR lock");
    }

    char go = 'g';
    if (write(go_pipe[1], &go, 1) != 1)
        fail++;

    int ws = 0;
    for (int i = 0; i < 50; i++) {
        pid_t w = waitpid(child_pid, &ws, WNOHANG);
        if (w == child_pid || w < 0)
            break;
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 100);
    }
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: child (CVTUNGRANT holder) exited clean");

    close(ready_pipe[0]);
    close(go_pipe[1]);
}

/* ================================================================
 * Scenario 3: SS$_DEADLOCK -- a genuine cross-process wait-for cycle,
 * mirrored from tests/qemu/test_kmod_lock_sync.c's scenario 2 (kernel
 * ioctl layer) through the PUBLIC sys$enqw entry point instead. Parent
 * holds X, child holds Y; child then blocks (sync) requesting X; parent
 * then requests Y, closing the cycle. Per that suite's already-measured
 * behaviour, the PARENT's closing request is the one the kernel rejects.
 * ================================================================ */
static void child_deadlock(int c2p_w)
{
    /* This process is a fork() taken AFTER scenario_ivlockid() and
     * scenario_cvtungrant() have already run in the parent and incremented
     * the file-scope `pass`/`fail` globals there. fork() copies the
     * parent's address space, so without this reset the child would inherit
     * whatever fail count an EARLIER, UNRELATED scenario left behind and
     * report it in its own child_msg -- making "parent: child's report ...
     * has no failures" go red for a reason that has nothing to do with this
     * scenario. MEASURED, not theoretical: this is exactly what happened
     * under the kstat-ivlockid-mismapped and kstat-cvtungrant-mismapped
     * negative controls before this reset was added -- scenario 1's (or
     * 2's) intentional, correctly-attributed failure leaked into scenario
     * 3's child via the inherited global, and the equality check in
     * tests/qemu/run_facility_negctl.sh caught the false attribution. */
    pass = 0;
    fail = 0;

    if (bootstrap("deadlock child") < 0)
        _exit(1);

    $DESCRIPTOR(resy, "SYSSVC_STATUS_DLY");
    struct lksb_caller lksb_y = {0};
    uint32_t sty = sys$enqw(0, LCK$K_EXMODE, &lksb_y, 0, &resy, 0, NULL, 0, NULL, 0, 0);
    CHECK(sty & 1, "child: EX granted on SYSSVC_STATUS_DLY (public API)");

    char a = 'A';
    if (write(c2p_w, &a, 1) != 1)
        _exit(2);

    /* Blocks in-kernel until the parent releases X (parent's own request on
     * Y is the one rejected for deadlock; this one drains once X is free). */
    $DESCRIPTOR(resx, "SYSSVC_STATUS_DLX");
    struct lksb_caller lksb_x = {0};
    uint32_t stx = sys$enqw(0, LCK$K_EXMODE, &lksb_x, 0, &resx, 0, NULL, 0, NULL, 0, 0);
    CHECK(stx & 1, "child: blocked EX on SYSSVC_STATUS_DLX granted after parent releases X (public API)");

    if (stx & 1)
        sys$deq(lksb_x.lksb$l_lkid, NULL, 0, 0);
    if (sty & 1)
        sys$deq(lksb_y.lksb$l_lkid, NULL, 0, 0);

    struct child_msg m = { (uint32_t)pass, (uint32_t)fail };
    if (write(c2p_w, &m, sizeof(m)) != (ssize_t)sizeof(m))
        fail++;

    _exit(fail > 0 ? 1 : 0);
}

static void scenario_deadlock(void)
{
    printf("--- scenario 3: SS$_DEADLOCK (cross-process wait-for cycle, public API) ---\n");

    $DESCRIPTOR(resx, "SYSSVC_STATUS_DLX");
    struct lksb_caller lksb_x = {0};
    uint32_t stx = sys$enqw(0, LCK$K_EXMODE, &lksb_x, 0, &resx, 0, NULL, 0, NULL, 0, 0);
    CHECK((stx & 1) && lksb_x.lksb$l_lkid != 0,
          "parent: EX granted on SYSSVC_STATUS_DLX (public API)");

    int c2p[2];
    if (pipe(c2p) < 0) {
        printf("  FAIL: pipe() setup\n");
        fail++;
        return;
    }
    pid_t child_pid = fork();
    if (child_pid < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        return;
    }
    if (child_pid == 0) {
        close(c2p[0]);
        child_deadlock(c2p[1]);
        _exit(1); /* unreachable */
    }
    close(c2p[1]);

    char a = 0;
    if (read_bounded(c2p[0], &a, 1, REPORT_TIMEOUT_MS) != 1)
        fail++;
    usleep(SETTLE_US);

    /* Parent requests Y (held by child) -> closes the cycle (parent waits
     * on child's Y; child waits on parent's X). Rejected with SS$_DEADLOCK. */
    $DESCRIPTOR(resy, "SYSSVC_STATUS_DLY");
    struct lksb_caller lksb_y = {0};
    uint32_t sty = sys$enqw(0, LCK$K_EXMODE, &lksb_y, 0, &resy, 0, NULL, 0, NULL, 0, 0);
    printf("  INFO: parent's closing sys$enqw returned status %u\n", sty);
    /* negctl: kstat-deadlock-mismapped */
    CHECK(sty == SS$_DEADLOCK,
          "parent: sync sys$enqw closing the cycle rejected SS$_DEADLOCK (public API)");

    /* Break the standoff: release X so the child's blocked request drains. */
    uint32_t dst = sys$deq(lksb_x.lksb$l_lkid, NULL, 0, 0);
    CHECK(dst & 1, "parent: released X (should unblock the child)");

    struct child_msg m = {0};
    int r = read_bounded(c2p[0], &m, sizeof(m), REPORT_TIMEOUT_MS);
    if (r == 0)
        printf("  (child's post-release report never arrived within %d ms: X did not really release in the executive)\n",
               REPORT_TIMEOUT_MS);
    CHECK(r == 1 && m.fail == 0, "parent: child's report after the deadlock/release sequence has no failures");

    int ws = 0;
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(child_pid, &ws, WNOHANG);
        if (w == child_pid || w < 0)
            break;
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 100);
    }
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: child (deadlock) exited clean");

    close(c2p[0]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: keep stdout line-buffered even when init.sh redirects it to a file, so an unflushed fork() cannot splice output */
    printf("=== test_syssvc_lock_status (executive-yielded VMS statuses, vms-2e5/vms-82a) ===\n");

    if (bootstrap("parent") < 0) {
        /*
         * NO-FABRICATED-SUCCESS PROOF ONLY -- reached solely inside the CI
         * negative-control rig (deliberately booted without insmod'ing
         * vms.ko; vms-0ff ruled OVMX has no product "executive absent"
         * state at all). No VMS status is pinned here: see
         * test_syssvc_lock.c's parent branch for the identical reasoning.
         */
        uint32_t st = sys$deq(0xDEADBEEF, NULL, 0, 0);
        printf("  INFO: sys$deq with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$deq does NOT report success when the executive was never reached (public API, real returned status)");

        printf("=== test_syssvc_lock_status: %d passed, %d failed (SKIPPED: no /dev/vms -- status-mapping scenarios not exercised, but the no-fabricated-success check above WAS) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    scenario_ivlockid();
    scenario_cvtungrant();
    scenario_deadlock();

    printf("=== test_syssvc_lock_status: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
