/*
 * test_syssvc_lock.c - Lock manager via the PUBLIC sys$ API (vms-1d9)
 *
 * Every other program in tests/qemu/ (test_kmod_*.c) drives /dev/vms with
 * raw ioctl(2) calls against src/kernel/vms_ioctl.h structures. That proves
 * the KERNEL lock manager works, but says nothing about whether the
 * userspace system-service layer (src/libvms/syssvc/sys_lock.c: sys$enq,
 * sys$enqw, sys$deq) still calls through to it -- an adversary reverting
 * sys_lock.c to a stub that always reports success would leave every
 * test_kmod_* test untouched and green (see vms-1d9's cold-start context:
 * the SAME defect class was already proven once, one layer down, in
 * src/libvms/syssvc/sys_event.c).
 *
 * This program calls the PUBLIC sys$enqw / sys$enq / sys$deq entry points
 * (declared in starlet.h, implemented in src/libvms/syssvc/sys_lock.c) --
 * the same functions any real OVMX program links against -- and proves,
 * across a real fork()'d second Linux process, that:
 *
 *   1. sys$enqw grants an EX lock and returns a real kernel lock ID
 *      (POSITIVE: the call actually reaches the kernel lock manager).
 *   2. A second process's sys$enq for the SAME resource, EX or CR, with
 *      LCK$M_NOQUEUE, is denied (SS$_NOTQUEUED) while the first process
 *      still holds EX (NEGATIVE, paired with #1: a stub that always
 *      returns SS$_NORMAL would pass #1 but WRONGLY grant this too --
 *      this is the exact shape of the sys_event.c regression this item
     exists to catch, one facility over).
 *   3. After the first process calls sys$deq to release, the SECOND
 *      process's sys$enqw for the same resource now succeeds (POSITIVE:
 *      the release is visible cross-process through the public API, not
 *      just within one process's private state).
 *
 * BOOTSTRAP NOTE, REMEASURED (vms-47b round 5): vms-9fc has landed.
 * sys_lock.c no longer has a bind_to_executive() -- it was deleted, not
 * fixed in place -- and $ENQ/$DEQ now call vms_kif_enq()/vms_kif_deq(),
 * which bind through kif_bind() (src/libvmssys/vms_kif.c) on their own
 * first call, the same way every other vms_kif_* entry point does. A
 * normally activated image reaching sys$enqw would register itself that
 * way; it does not need the manual step below any more.
 *
 * This test still calls vms_kif_open()/vms_kif_register() explicitly,
 * before touching sys$enqw, exactly as facility_defects.sh's
 * bind-client-no-register control documents it (see that manifest entry's
 * blind_why): that is what makes this suite BLIND to the kif_bind-removed
 * defect the control restores -- if kif_bind() stopped calling
 * vms_kif_register(), this program's own explicit call would still
 * register it, and the suite would stay green. It does not touch or
 * bypass sys_lock.c's do_enq/sys$deq, which still make the exact ioctl
 * calls every caller uses.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened -- which happens ONLY in the CI negative-control rig, never in
 * the product (vms-0ff: PID 1 refuses to boot without the executive) --
 * it exercises the no-fabricated-success checks in main() and exits with
 * EXIT_SKIP (77), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

/* NOTE: deliberately do NOT include lckdef.h here. It duplicates the
 * LCK$K_ and LCK$M_ constants already declared in starlet.h with DIFFERENT
 * numeric values for the LCK$M_ flag bits -- lckdef.h:44 has LCK$M_NOQUEUE
 * 0x00000008, starlet.h:846 has 0x0004, and eight more disagree. Which
 * header is right is NOT settled here and is not this file's call: the
 * divergence is tracked as vms-5bd and needs the oracle plus operator
 * sign-off. What matters for this test is that #include-ing both in one
 * translation unit silently lets the later one win via macro redefinition,
 * which would pass a bit pattern to sys$enq/sys$enqw that neither header
 * intended and corrupt the very assertions this file exists to make.
 * starlet.h alone is used because it is what src/libvms/syssvc/sys_lock.c
 * itself compiles against -- caller and implementation therefore agree,
 * whatever the eventual oracle ruling turns out to be. */
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

/*
 * Lock Status Block. Declared BY THE CALLER, deliberately -- there is no
 * OVMX public lksdef.h and this test does not create one. On OpenVMS the
 * LKSB is caller-allocated storage (traditionally two longwords plus an
 * optional 16-byte value block); SYS$LIBRARY:STARLET.MLB on the VAX 7.3
 * oracle carries NO $LKSB macro at all (%LIBRAR-W-NOMTCHFOU), so there is
 * no VMS-published byte layout to pin a shared header against. Per Rule 8
 * an OVMX-invented representation must be labelled as such and not
 * presented as VMS-authentic; promoting sys_lock.c's private struct to a
 * public header is a separate change needing operator sign-off, so it is
 * NOT done here. The field order below mirrors
 * src/libvms/syssvc/sys_lock.c's private struct because that is what the
 * implementation under test writes through this pointer.
 */
struct lksb_caller {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];
};

#define EXIT_SKIP 77

/*
 * How long the parent waits for each of the child's two reports before
 * declaring the child wedged. Must stay well under run_tests.sh's 120s QEMU
 * TIMEOUT so the harness still reaches its own FINAL RESULTS accounting and
 * CI can name WHICH suite broke rather than reporting an unattributable
 * timeout. Both reports arrive in milliseconds on a healthy run.
 */
#define CHILD_REPORT_TIMEOUT_MS 20000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Message passed from child to parent over a pipe. */
struct child_msg {
    uint32_t stage;   /* 1 = NOQUEUE-denial report, 2 = final report */
    uint32_t pass;
    uint32_t fail;
};

/*
 * bootstrap - open /dev/vms and register this (Linux) process with the
 * kernel executive. See the file-header BOOTSTRAP NOTE. Returns 0 on
 * success, -1 on failure (caller decides skip vs. hard fail).
 */
static int bootstrap(const char *who)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: %s: cannot open /dev/vms\n", who);
        return -1;
    }
    /* NO privilege argument (vms-2b8): VMS_IOCTL_REGISTER no longer carries
     * one. This call used to ask for all 64 bits, which is a process naming
     * its own privileges. The executive DERIVES the mask from the task's
     * real credentials now; $ENQ/$DEQ need no privilege either way. */
    uint32_t st = vms_kif_register(NULL);
    if (!(st & 1)) {
        printf("  FAIL: %s: vms_kif_register status=%u\n", who, st);
        return -1;
    }
    return 0;
}

/*
 * read_bounded - read exactly `len` bytes from `fd`, giving up after
 * `timeout_ms`. Returns 1 on success, 0 on timeout, -1 on EOF/error.
 *
 * WHY THE BOUND LIVES IN THE PARENT, NOT THE CHILD (do not "simplify" this
 * back into an alarm in the child -- it was tried and it provably cannot
 * work). The child's post-release sys$enqw blocks IN THE KERNEL: the lock
 * manager's sync waiter (src/kernel/vms_lock.c, enq_wait_sync) loops on
 * wait_event_interruptible_timeout and deliberately re-arms on a signal
 * wake, and the ioctl never returns to user mode, so no userspace signal
 * is ever delivered -- a SIGALRM in the child is swallowed. Verified by
 * running it: with sys$deq stubbed to report success without reaching the
 * executive, a child-side alarm(20) never fired and the whole VM sat until
 * run_tests.sh's 120s QEMU timeout killed it. Every later suite then never
 * ran, and CI saw a timeout indistinguishable from a boot panic instead of
 * a named assertion failure. The parent is NOT blocked in the kernel, so
 * it is the only process that can bound this.
 *
 * This is a FAILURE bound, not sleep-based pacing: the happy path still
 * synchronises on the child's real output, and the timeout only converts
 * "hang until QEMU dies" into a named FAIL line.
 */
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
 * Child process: attempts to acquire the resource the parent holds.
 * ================================================================ */
static int run_child(int c2p_write, int p2c_read)
{
    if (bootstrap("child") < 0) {
        struct child_msg msg = {1, 0, 1};
        if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg)) { /* best effort */ }
        return 1;
    }

    $DESCRIPTOR(resnam, "SYSSVC_LOCK_TEST1");

    /* --- EX+NOQUEUE must be denied: parent holds EX --- */
    struct lksb_caller lksb_ex = {0};
    uint32_t st = sys$enq(0, LCK$K_EXMODE, &lksb_ex, LCK$M_NOQUEUE,
                           &resnam, 0, NULL, 0, NULL, 0, 0);
    CHECK(st == SS$_NOTQUEUED,
          "child: sys$enq EX+NOQUEUE denied while parent holds EX (public API)");

    /* --- CR+NOQUEUE must also be denied: EX is incompatible with CR --- */
    struct lksb_caller lksb_cr = {0};
    st = sys$enq(0, LCK$K_CRMODE, &lksb_cr, LCK$M_NOQUEUE,
                 &resnam, 0, NULL, 0, NULL, 0, 0);
    CHECK(st == SS$_NOTQUEUED,
          "child: sys$enq CR+NOQUEUE denied while parent holds EX (public API)");

    struct child_msg msg1 = {1, (uint32_t)pass, (uint32_t)fail};
    if (write(c2p_write, &msg1, sizeof(msg1)) != (ssize_t)sizeof(msg1))
        fail++;

    /* Wait for the parent to sys$deq before retrying. */
    char go;
    if (read(p2c_read, &go, 1) != 1)
        fail++;

    /*
     * The sys$enqw below is a genuinely BLOCKING call -- $ENQW asks the
     * kernel lock manager to sleep in-kernel until the request is granted.
     * If the parent's sys$deq did not really release the lock, this child
     * never returns and cannot be woken from userspace (see read_bounded's
     * comment). The PARENT bounds this; nothing here can.
     */

    /* --- Now that the parent released, the SAME resource must grant --- */
    struct lksb_caller lksb_retry = {0};
    st = sys$enqw(0, LCK$K_EXMODE, &lksb_retry, 0,
                  &resnam, 0, NULL, 0, NULL, 0, 0);
    CHECK((st & 1) && lksb_retry.lksb$l_lkid != 0,
          "child: sys$enqw EX granted after parent's sys$deq (cross-process release, public API)");

    if (st & 1) {
        uint32_t dst = sys$deq(lksb_retry.lksb$l_lkid, NULL, 0, 0);
        CHECK(dst & 1, "child: sys$deq released its own EX lock");
    }

    struct child_msg msg2 = {2, (uint32_t)pass, (uint32_t)fail};
    if (write(c2p_write, &msg2, sizeof(msg2)) != (ssize_t)sizeof(msg2))
        fail++;

    return fail > 0 ? 1 : 0;
}

/* ================================================================
 * Parent process: drives the test, prints combined results.
 * ================================================================ */
int main(void)
{
    printf("=== test_syssvc_lock (public sys$enq/sys$enqw/sys$deq API) ===\n");

    if (bootstrap("parent") < 0) {
        /*
         * NO-FABRICATED-SUCCESS PROOF, executed against the running
         * artifact rather than asserted by code reading.
         *
         * This branch is reached ONLY inside the CI negative control, a rig
         * deliberately booted without insmod'ing vms.ko. It is NOT a product
         * state and there is no product code path being validated here:
         * vms-0ff ruled that OVMX has no "executive absent" mode at all
         * (PID 1 refuses to boot without /dev/vms and pins it open for the
         * life of the system), and the per-call SS$_NOSUCHDEV returns
         * sys_lock.c used to make for this case were DELETED as a fiction.
         *
         * So this asserts a PROPERTY, not a VMS behaviour, and deliberately
         * pins NO status value: a public sys$ entry point must never report
         * SUCCESS when it did not reach the executive. That is the one thing
         * that stays true regardless of what an unreachable executive would
         * "mean", and it is the epic's signature defect -- a per-process
         * fake that returns SS$_NORMAL and a fabricated lock ID looks
         * identical to a working lock manager from inside one process.
         * Asserting a specific constant here would freeze the superseded
         * contract into a gate; asserting the odd/even success bit does not.
         *
         * bootstrap()'s vms_kif_open() already failed above, but it is
         * idempotent on failure (vms_kif.c keeps the thread-local fd at -1
         * and retries), so these calls drive the REAL production path in
         * sys_lock.c -- do_enq()/sys$deq() reaching an ioctl on a closed
         * descriptor -- and report whatever that produces.
         */
        $DESCRIPTOR(resnam_absent, "SYSSVC_LOCK_TEST_ABSENT");
        struct lksb_caller lksb_absent = {0};
        uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb_absent, 0,
                                &resnam_absent, 0, NULL, 0, NULL, 0, 0);
        printf("  INFO: sys$enqw with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$enqw does NOT report success when the executive was never reached (public API, real returned status)");
        CHECK(!(lksb_absent.lksb$w_status & 1),
              "parent: sys$enqw's own LKSB does NOT record a success status when the executive was never reached");
        CHECK(lksb_absent.lksb$l_lkid == 0,
              "parent: sys$enqw fabricates no lock ID when the executive was never reached");

        uint32_t dst = sys$deq(0xDEADBEEF, NULL, 0, 0);
        printf("  INFO: sys$deq with no executive returned status %u\n", dst);
        CHECK(!(dst & 1),
              "parent: sys$deq does NOT report success when the executive was never reached (public API, real returned status)");

        printf("=== test_syssvc_lock: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised, but the no-fabricated-success checks above WERE) ===\n",
               pass, fail);
        /* A failed no-fabricated-success assertion is a real regression, not
         * an honest skip -- report it as FAIL (exit 1), never masked as 77. */
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    $DESCRIPTOR(resnam, "SYSSVC_LOCK_TEST1");

    /* 1. Parent takes EX via the PUBLIC sys$enqw -- not a raw ioctl. */
    struct lksb_caller lksb = {0};
    uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb, 0,
                            &resnam, 0, NULL, 0, NULL, 0, 0);
    CHECK((st & 1) && lksb.lksb$l_lkid != 0,
          "parent: sys$enqw EX granted, real lock ID returned (public API)");

    int c2p[2], p2c[2];
    if (pipe(c2p) < 0 || pipe(p2c) < 0) {
        printf("  FAIL: pipe() setup\n");
        return 1;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        printf("  FAIL: fork()\n");
        return 1;
    }

    if (child_pid == 0) {
        close(c2p[0]);
        close(p2c[1]);
        int rc = run_child(c2p[1], p2c[0]);
        close(c2p[1]);
        close(p2c[0]);
        _exit(rc);
    }

    close(c2p[1]);
    close(p2c[0]);

    /* --- Receive child's NOQUEUE-denial report --- */
    struct child_msg msg = {0};
    int r = read_bounded(c2p[0], &msg, sizeof(msg), CHILD_REPORT_TIMEOUT_MS);
    if (r == 0)
        printf("  (child produced no NOQUEUE-denial report within %d ms)\n",
               CHILD_REPORT_TIMEOUT_MS);
    CHECK(r == 1 && msg.stage == 1 && msg.fail == 0,
          "parent: child's NOQUEUE-denial checks reported via public API");

    /* Release the lock (public sys$deq), then let the child retry. */
    uint32_t dst = sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
    CHECK(dst & 1, "parent: sys$deq released EX lock (public API)");

    char go = 'g';
    if (write(p2c[1], &go, 1) != 1)
        fail++;

    /* --- Receive child's final report (post-release retry) --- */
    r = read_bounded(c2p[0], &msg, sizeof(msg), CHILD_REPORT_TIMEOUT_MS);
    if (r == 0)
        printf("  (child's post-release sys$enqw never returned within %d ms:"
               " the resource is still held, so the parent's sys$deq reported"
               " success without releasing it in the executive)\n",
               CHILD_REPORT_TIMEOUT_MS);
    CHECK(r == 1 && msg.stage == 2 && msg.fail == 0,
          "parent: child's post-release retry succeeded via public API");

    /*
     * Reap only if the child can be reaped. A child wedged in the kernel's
     * sync lock wait is not merely slow, it is unkillable from userspace
     * (enq_wait_sync re-arms on every signal wake), so a blocking waitpid
     * here would hang the SUITE for the same reason the read above would
     * have. Poll briefly, then give up and report -- the child is reaped by
     * the VM's reboot at the end of the run either way, and this suite has
     * already recorded its verdict.
     */
    int wstatus = 0;
    for (int i = 0; i < 20; i++) {
        pid_t w = waitpid(child_pid, &wstatus, WNOHANG);
        if (w == child_pid || w < 0)
            break;
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 100);
    }

    close(c2p[0]);
    close(p2c[1]);

    printf("=== test_syssvc_lock: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
