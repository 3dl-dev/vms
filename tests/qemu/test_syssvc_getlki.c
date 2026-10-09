/*
 * test_syssvc_getlki.c - SYS$GETLKI/SYS$GETLKIW answer real lock state
 * (vms-b71)
 *
 * Before this suite there was no sys$getlki in src/libvms at all (the kernel
 * handler and the ioctl existed, OVMX-UNWIRED, since vms-a86). This program
 * calls the PUBLIC sys$enqw / sys$getlkiw / sys$deq entry points (starlet.h,
 * implemented in src/libvms/syssvc/sys_lock.c) -- the same functions any
 * real OVMX program (the corpus's sys_enqw.c) links against.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened it exits EXIT_SKIP (77), never a fake pass (see
 * test_syssvc_lock_status.c's identical bootstrap/skip shape).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "lckdef.h"
#include "lkidef.h"
#include "iledef.h"   /* ILE3 / ILE3_TERMINATOR */
#include "vms_kif.h"

struct lksb_caller {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];
};

#define EXIT_SKIP 77
#define REPORT_TIMEOUT_MS 5000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int bootstrap(const char *who)
{
    /* Opens /dev/vms ONLY to decide skip-vs-run (test_syssvc_lock_status.c's
     * bootstrap() shape); sys$enqw/sys$getlkiw reach kif_bind() on their own. */
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
        if (pr <= 0)
            return pr == 0 ? 0 : -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 1;
}

/* ================================================================
 * Scenario 1: a single EX holder -- every item this suite's getlki_impl
 * answers reads back the lock's REAL state: its own lock ID, its resource
 * name, its granted/requested mode, LKI$_STATE, and a GRANTCOUNT of 1 (the
 * one lock this process itself holds).
 * ================================================================ */
static void scenario_single_holder(void)
{
    printf("--- scenario 1: single EX holder, every LKI$_ item this suite answers ---\n");

    $DESCRIPTOR(resnam, "SYSSVC_GETLKI_SINGLE");
    struct lksb_caller lksb = {0};
    uint32_t est = sys$enqw(0, LCK$K_EXMODE, &lksb, 0, &resnam, 0, NULL, 0, NULL, 0, 0, 0);
    if (!(est & 1)) {
        printf("  FAIL: setup: sys$enqw EX failed (status %u)\n", est);
        fail++;
        return;
    }

    uint32_t lkid = 0, grantcnt = 0, parent = 0xDEADBEEF;
    uint8_t statef[3] = {0xFF, 0xFF, 0xFF};
    char rname[32] = {0};
    ILE3 itms[] = {
        { 4,  LKI$_LOCKID,     &lkid,     NULL },
        { sizeof(rname), LKI$_RESNAM, rname, NULL },
        { 4,  LKI$_GRANTCOUNT, &grantcnt, NULL },
        { 4,  LKI$_PARENT,     &parent,   NULL },
        { sizeof(statef), LKI$_STATE, statef, NULL },
        ILE3_TERMINATOR
    };

    /* sys$getlki (the async entry point; getlki_impl runs synchronously
     * already, so this and sys$getlkiw below answer identically) answers a
     * single-item request directly, proving BOTH entry points against the
     * public API, not just the wait form. */
    uint32_t solo_lkid = 0;
    ILE3 solo_itm[] = { { 4, LKI$_LOCKID, &solo_lkid, NULL }, ILE3_TERMINATOR };
    uint32_t solo_lkidarg = lksb.lksb$l_lkid;
    uint32_t sst = sys$getlki(0, &solo_lkidarg, solo_itm, NULL, 0, 0, 0);
    CHECK(sst & 1, "sys$getlki on the lock this process holds reports success");
    CHECK(solo_lkid == lksb.lksb$l_lkid, "sys$getlki's LKI$_LOCKID matches $ENQW's lock ID too");

    uint32_t lkidarg = lksb.lksb$l_lkid;
    uint32_t gst = sys$getlkiw(0, &lkidarg, itms, NULL, 0, 0, 0);

    CHECK(gst & 1, "sys$getlkiw on the lock this process holds reports success");
    CHECK(lkid == lksb.lksb$l_lkid, "LKI$_LOCKID reads back the same lock ID $ENQW returned");
    CHECK(strncmp(rname, "SYSSVC_GETLKI_SINGLE", 20) == 0,
          "LKI$_RESNAM reads back the resource name this process enqueued");
    /* negctl-knockon: getlki-grantcount-not-counted */
    CHECK(grantcnt == 1, "LKI$_GRANTCOUNT reports 1 (this process is the only holder)");
    CHECK(parent == 0, "LKI$_PARENT is 0 (this is a root lock, not a sublock)");
    CHECK(statef[LKI$B_STATE_GRMODE] == LCK$K_EXMODE,
          "LKI$_STATE's GRMODE byte reads back EX, the mode actually granted");
    CHECK(statef[LKI$B_STATE_RQMODE] == LCK$K_EXMODE,
          "LKI$_STATE's RQMODE byte equals GRMODE when not converting");
    CHECK(statef[LKI$B_STATE_QUEUE] == LKI$C_GRANTED,
          "LKI$_STATE's QUEUE byte reports LKI$C_GRANTED, not CONVERT");

    sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
}

/* ================================================================
 * Scenario 2: TWO compatible (CR) holders on one resource -- LKI$_GRANTCOUNT
 * must report the resource's REAL granted-queue length (2), not merely "this
 * process holds a lock" (which the single-holder scenario above cannot tell
 * apart from a hardcoded 1). This is the anchor for getlki-grantcount-not-
 * counted: the mutation drops the kernel's counting loop's increment, so
 * grant_count reads 0 regardless of how many locks are actually granted.
 * ================================================================ */
static void child_cr_holder(int ready_w, int go_r)
{
    if (bootstrap("getlki CR child") < 0)
        _exit(1);

    $DESCRIPTOR(resnam, "SYSSVC_GETLKI_SHARED");
    struct lksb_caller lksb = {0};
    uint32_t st = sys$enqw(0, LCK$K_CRMODE, &lksb, 0, &resnam, 0, NULL, 0, NULL, 0, 0, 0);
    if (!(st & 1)) {
        char r = 'x';
        write(ready_w, &r, 1);
        _exit(1);
    }

    char r = 'r';
    if (write(ready_w, &r, 1) != 1)
        _exit(1);

    char go = 0;
    if (read(go_r, &go, 1) != 1)
        _exit(1);

    uint32_t dst = sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
    _exit((dst & 1) ? 0 : 1);
}

static void scenario_two_holders(void)
{
    printf("--- scenario 2: LKI$_GRANTCOUNT with two compatible (CR) holders ---\n");

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
        child_cr_holder(ready_pipe[1], go_pipe[0]);
        _exit(1); /* unreachable */
    }
    close(ready_pipe[1]);
    close(go_pipe[0]);

    char r = 0;
    int rr = read_bounded(ready_pipe[0], &r, 1, REPORT_TIMEOUT_MS);
    if (rr != 1 || r != 'r') {
        printf("  FAIL: child did not reach ready (rr=%d r=%d)\n", rr, (int)r);
        fail++;
        close(ready_pipe[0]); close(go_pipe[1]);
        waitpid(child_pid, NULL, 0);
        return;
    }

    $DESCRIPTOR(resnam, "SYSSVC_GETLKI_SHARED");
    struct lksb_caller lksb = {0};
    uint32_t est = sys$enqw(0, LCK$K_CRMODE, &lksb, 0, &resnam, 0, NULL, 0, NULL, 0, 0, 0);
    CHECK(est & 1, "parent: second CR request on the shared resource is also granted");

    uint32_t grantcnt = 0;
    ILE3 itms[] = {
        { 4, LKI$_GRANTCOUNT, &grantcnt, NULL },
        ILE3_TERMINATOR
    };
    uint32_t lkidarg = lksb.lksb$l_lkid;
    uint32_t gst = sys$getlkiw(0, &lkidarg, itms, NULL, 0, 0, 0);
    CHECK(gst & 1, "parent: sys$getlkiw on the shared resource succeeds");
    /* negctl: getlki-grantcount-not-counted */
    CHECK(grantcnt == 2,
          "LKI$_GRANTCOUNT reports BOTH holders (2), the resource's real granted-queue length");

    char go = 'g';
    write(go_pipe[1], &go, 1);
    close(go_pipe[1]);

    int ws = 0;
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(child_pid, &ws, WNOHANG);
        if (w == child_pid || w < 0)
            break;
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 10);
    }
    CHECK(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "parent: CR child released clean");

    sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
    close(ready_pipe[0]);
}

/* ================================================================
 * Scenario 3: fail-honest paths -- an item code this executive cannot
 * answer from real lock state is SS$_BADPARAM (INV-6: no made-up answer for
 * an item the executive never tried to resolve), and an unknown lock ID is
 * SS$_IVLOCKID with no item written.
 * ================================================================ */
static void scenario_fail_honest(void)
{
    printf("--- scenario 3: undefined item code / unknown lock ID fail honestly ---\n");

    uint32_t dummy = 0;
    ILE3 bad_itms[] = {
        { 4, 0x7FFF /* no LKI$_ item this executive answers is this code */, &dummy, NULL },
        ILE3_TERMINATOR
    };
    $DESCRIPTOR(resnam, "SYSSVC_GETLKI_HONEST");
    struct lksb_caller lksb = {0};
    uint32_t est = sys$enqw(0, LCK$K_EXMODE, &lksb, 0, &resnam, 0, NULL, 0, NULL, 0, 0, 0);
    CHECK(est & 1, "setup: sys$enqw EX for the fail-honest scenario succeeds");

    uint32_t lkidarg = lksb.lksb$l_lkid;
    uint32_t bst = sys$getlkiw(0, &lkidarg, bad_itms, NULL, 0, 0, 0);
    CHECK(bst == SS$_BADPARAM,
          "an undefined LKI$_ item code reports SS$_BADPARAM, not a fabricated value");

    uint32_t grantcnt = 0xDEADBEEF;
    ILE3 itms[] = {
        { 4, LKI$_GRANTCOUNT, &grantcnt, NULL },
        ILE3_TERMINATOR
    };
    uint32_t badlkid = 0xDEADBEEF;
    uint32_t ist = sys$getlkiw(0, &badlkid, itms, NULL, 0, 0, 0);
    CHECK(ist == SS$_IVLOCKID, "an unknown lock ID reports SS$_IVLOCKID");
    CHECK(grantcnt == 0xDEADBEEF,
          "an SS$_IVLOCKID refusal writes no item (the caller's buffer is untouched)");

    sys$deq(lksb.lksb$l_lkid, NULL, 0, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_getlki (SYS$GETLKI/SYS$GETLKIW, vms-b71) ===\n");

    if (bootstrap("parent") < 0) {
        /* NO-FABRICATED-SUCCESS PROOF ONLY, the CI negative-control rig
         * shape (test_syssvc_lock_status.c's identical parent branch). */
        uint32_t dummy = 0, badlkid = 0xDEADBEEF;
        ILE3 itms[] = { { 4, LKI$_GRANTCOUNT, &dummy, NULL }, ILE3_TERMINATOR };
        uint32_t st = sys$getlkiw(0, &badlkid, itms, NULL, 0, 0, 0);
        printf("  INFO: sys$getlkiw with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$getlkiw does NOT report success when the executive was never reached");

        printf("=== test_syssvc_getlki: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    scenario_single_holder();
    scenario_two_holders();
    scenario_fail_honest();

    printf("=== test_syssvc_getlki: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
