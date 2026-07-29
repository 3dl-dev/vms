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
 * BOOTSTRAP NOTE (documented, not a workaround): the kernel module
 * requires a process to VMS_IOCTL_REGISTER before any other ioctl
 * (src/kernel/vms_module.c: "All other ioctls require a registered
 * process"). src/libvms/syssvc/sys_lock.c's ensure_kif_open() only opens
 * /dev/vms lazily; nothing in production code calls vms_kif_register()
 * yet -- process registration at image-activation time is Phase 3 work
 * (vms-pt1, "executive owns a process table"), not built. vms_kif.h's own
 * doc comment says the same thing ("1. Call vms_kif_open() ... 2. Call
 * vms_kif_register() ... 3. Use vms_kif_* functions"). This test performs
 * that bootstrap step directly via the real vms_kif_open()/vms_kif_register()
 * entry points (the same ones test_kmod_*.c's raw-ioctl tests use) before
 * calling the public sys$ API -- it does not touch or bypass sys_lock.c's
 * do_enq/do_deq, which still make the exact ioctl calls every caller uses.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened, exits with EXIT_SKIP (77) and an honest message -- never a fake
 * pass (CLAUDE.md Rule 9 / vms-6b8 constraint: no silent fallback).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

/* NOTE: deliberately do NOT include lckdef.h here. It duplicates the
 * LCK$K_ and LCK$M_ constants already declared in starlet.h with DIFFERENT
 * (stale/drifted) numeric values for the LCK$M_ flag bits -- e.g. its
 * LCK$M_NOQUEUE value is 0x00000008 vs. starlet.h's oracle-pinned 0x0004 (see
 * tests/conformance/vms_programs/test_lock_constants.c, which checks
 * starlet.h's values only). #include-ing both in one translation unit
 * silently lets the later one win via macro redefinition, which would
 * make this test pass the WRONG bit pattern to sys$enq/sys$enqw and
 * corrupt the very assertions it exists to make. Flagged as a separate
 * finding (see vms-1d9 return data) rather than fixed here -- lckdef.h
 * has other in-tree includers outside this item's scope. */
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "lksdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

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
    uint32_t st = vms_kif_register((uint32_t)getpid(), 0xFFFFFFFFFFFFFFFFULL);
    if (!(st & 1)) {
        printf("  FAIL: %s: vms_kif_register status=%u\n", who, st);
        return -1;
    }
    return 0;
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
    struct lksb lksb_ex = {0};
    uint32_t st = sys$enq(0, LCK$K_EXMODE, &lksb_ex, LCK$M_NOQUEUE,
                           &resnam, 0, NULL, 0, NULL, 0, 0);
    CHECK(st == SS$_NOTQUEUED,
          "child: sys$enq EX+NOQUEUE denied while parent holds EX (public API)");

    /* --- CR+NOQUEUE must also be denied: EX is incompatible with CR --- */
    struct lksb lksb_cr = {0};
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

    /* --- Now that the parent released, the SAME resource must grant --- */
    struct lksb lksb_retry = {0};
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
         * NO-SILENT-FALLBACK PROOF (vms-1d9 round 3 -- adversarial review
         * of round 2 found this path proved nothing: it printed a bootstrap
         * message and exited SKIP without ever calling a public sys$ entry
         * point, so CLAUDE.md Rule 9 / vms-6b8's "fail honestly, never fake
         * success" constraint for sys_lock.c was satisfied by code reading,
         * not by a running assertion).
         *
         * bootstrap()'s own vms_kif_open() call already failed above, but
         * vms_kif_open() is idempotent on failure (vms_kif.c: the
         * thread-local fd stays -1 and every call retries the same open),
         * so calling the PUBLIC sys$enqw/sys$deq entry points here drives
         * the REAL production code path: sys_lock.c's do_enq()/sys$deq()
         * each call ensure_kif_open() themselves, and when THAT fails
         * (same failure, independently re-derived, not reused from above)
         * return SS$_NOSUCHDEV -- both as the function's return value AND
         * written into the caller's LKSB. Assert on that status, not on a
         * string this program authored.
         */
        $DESCRIPTOR(resnam_absent, "SYSSVC_LOCK_TEST_ABSENT");
        struct lksb lksb_absent = {0};
        uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb_absent, 0,
                                &resnam_absent, 0, NULL, 0, NULL, 0, 0);
        CHECK(st == SS$_NOSUCHDEV,
              "parent: sys$enqw returns SS$_NOSUCHDEV when /dev/vms is absent (public API, real status, not a fake success)");
        CHECK(lksb_absent.lksb$w_status == SS$_NOSUCHDEV,
              "parent: sys$enqw's own LKSB records SS$_NOSUCHDEV when /dev/vms is absent");

        uint32_t dst = sys$deq(0xDEADBEEF, NULL, 0, 0);
        CHECK(dst == SS$_NOSUCHDEV,
              "parent: sys$deq returns SS$_NOSUCHDEV when /dev/vms is absent (public API, real status, not a fake success)");

        printf("=== test_syssvc_lock: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised, but the no-silent-fallback path above WAS) ===\n",
               pass, fail);
        /* A failed no-silent-fallback assertion is a real regression, not
         * an honest skip -- report it as FAIL (exit 1), never masked as 77. */
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    $DESCRIPTOR(resnam, "SYSSVC_LOCK_TEST1");

    /* 1. Parent takes EX via the PUBLIC sys$enqw -- not a raw ioctl. */
    struct lksb lksb = {0};
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
    ssize_t n = read(c2p[0], &msg, sizeof(msg));
    CHECK(n == (ssize_t)sizeof(msg) && msg.stage == 1 && msg.fail == 0,
          "parent: child's NOQUEUE-denial checks reported via public API");

    /* Release the lock (public sys$deq), then let the child retry. */
    uint32_t dst = sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
    CHECK(dst & 1, "parent: sys$deq released EX lock (public API)");

    char go = 'g';
    if (write(p2c[1], &go, 1) != 1)
        fail++;

    /* --- Receive child's final report (post-release retry) --- */
    n = read(c2p[0], &msg, sizeof(msg));
    CHECK(n == (ssize_t)sizeof(msg) && msg.stage == 2 && msg.fail == 0,
          "parent: child's post-release retry succeeded via public API");

    int wstatus = 0;
    waitpid(child_pid, &wstatus, 0);

    close(c2p[0]);
    close(p2c[1]);

    printf("=== test_syssvc_lock: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
