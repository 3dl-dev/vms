/*
 * test_syssvc_qio_terminal.c - $ASSIGN / $QIO to a terminal, via the PUBLIC
 * sys$ API, against a real /dev/vms (vms-1c57).
 *
 * WHAT THIS PROVES, AND WHY test_kmod_devtab.c AND test_syssvc_showdev.c
 * ARE NOT ENOUGH. vms-d0b proved the executive's device table is real and
 * that SHOW DEVICE / SHOW TERMINAL read it -- but nothing that reached
 * either of those proofs was the path a VMS PROGRAM actually uses to talk
 * to a terminal. Before this item, src/libvms/syssvc/sys_assign.c resolved
 * "TT:" straight to open("/dev/tty", ...) and src/libvms/syssvc/sys_qio.c
 * did raw read()/write() on that fd -- a channel obtained through $ASSIGN
 * was NOT the channel the executive issued, so nothing the device table
 * recorded could constrain or inform real terminal I/O. Two models of the
 * same device (CLAUDE.md Rule 11).
 *
 * THE DESIGN PRINCIPLE, restated because it is the whole point: the channel
 * is the identity. $ASSIGN now calls vms_kif_assign() (VMS_IOCTL_ASSIGN,
 * src/kernel/vms_devtab.c) to obtain a channel FROM the executive before
 * opening anything locally, and $QIO reconfirms that channel with the
 * executive (VMS_IOCTL_GETDVI by channel) before doing real I/O on it, on
 * every call. This program does NOT make $QIO write the device table as a
 * side effect -- that would make the table's CONTENTS agree with reality
 * while leaving the I/O PATH free to diverge from it again the moment
 * nobody was checking, which is the exact facade shape one layer up from
 * the one this item exists to kill.
 *
 * THE DECISIVE TEST IS A-WRITES / B-READS (Rule 11), not "the local call
 * returned SS$_NORMAL": a per-process fake $ASSIGN can return SS$_NORMAL
 * and a plausible channel number all day. So:
 *
 *   1. Process A (this program) calls the PUBLIC sys$assign("TT:") --
 *      not vms_kif_assign() directly -- and gets a channel.
 *   2. A FRESH, unrelated process (a forked child that touched none of A's
 *      state) reads the executive's OPA0: row directly and must see the
 *      reference A's $ASSIGN added -- A writes, B reads.
 *   3. A does REAL I/O through the PUBLIC sys$qiow on that channel: a
 *      WRITEVBLK to the console, exercised end to end.
 *   4. A calls the PUBLIC sys$dassgn, and a SECOND fresh child must see
 *      the executive's OPA0: row back at its baseline reference count --
 *      the release reached the executive too, not just local bookkeeping.
 *   5. A further sys$qio on the now-deassigned local channel number is
 *      refused (SS$_IVCHAN): the channel cannot go on moving bytes on the
 *      strength of a stale local slot once the executive no longer holds it.
 *
 * Steps 2 and 4 both use vms_kif_getdvi_devnam() directly (not another
 * sys$assign) so the read side does not depend on anything this item
 * changed -- it is the same read SHOW DEVICE already uses (vms-fb9).
 *
 * REFERENCE COUNT DELTAS, NOT ABSOLUTE VALUES: this suite runs inside the
 * same QEMU boot as every other tests/qemu/test_{kmod,syssvc}_*.c program,
 * several of which also touch OPA0: (test_kmod_devtab, test_syssvc_showdev).
 * Asserting refcnt == 0 at any point would be a claim about run ORDER, not
 * about this item; every assertion here compares against a freshly-read
 * baseline instead.
 *
 * NEGATIVE CONTROL RIG (NEGATIVE_CONTROL=1 in tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko): not a product state (vms-0ff -- PID 1 refuses
 * to boot without the executive), so the cross-process scenario above is
 * not exercised. What IS asserted there is the property that must survive
 * regardless: sys$assign("TT:") must not report success, and must not hand
 * back a channel, when it never reached the executive -- there is no
 * per-process fallback identity for a terminal (Rule 11). Exits EXIT_SKIP
 * (77) unless that property itself is violated, in which case it is a
 * real regression and exits 1, exactly as the other test_syssvc_* suites do.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms for the full scenario.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>
#include <errno.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "iodef.h"
#include "iosbdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* Same bound as the other cross-process test_syssvc_* suites: generous for
 * a healthy run (these are ioctl round trips, not blocking kernel waits),
 * short enough that a genuinely wedged child still lets run_tests.sh's
 * overall QEMU timeout name a suite instead of eating the whole run. */
#define CHILD_REPORT_TIMEOUT_MS 20000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Message a child reports back over a pipe: the executive's OPA0: row as
 * that child, and only that child, observed it. */
struct child_msg {
    uint32_t status;     /* vms_kif_getdvi_devnam()'s returned VMS status */
    uint32_t refcnt;
    uint32_t owner_pid;
};

static int bootstrap(const char *who)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: %s: cannot open /dev/vms\n", who);
        return -1;
    }
    uint32_t st = vms_kif_register(NULL);
    if (!(st & 1)) {
        printf("  FAIL: %s: vms_kif_register status=%u\n", who, st);
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

/*
 * run_reader_child - a FRESH process (this program's own fork, but one that
 * has touched none of the parent's channel state) reads the executive's
 * OPA0: row directly and reports it back. Exits after one report; the
 * caller forks a new one for each read it needs, so nothing here carries
 * state between the "before assign" / "after assign" / "after dassgn" reads
 * -- each is read by a process that did not exist for the others.
 */
static int run_reader_child(int c2p_write)
{
    struct child_msg msg = {0, 0, 0};

    if (bootstrap("reader child") < 0) {
        msg.status = 0;
        if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg)) { /* best effort */ }
        return 1;
    }

    struct vms_devinfo info;
    msg.status = vms_kif_getdvi_devnam("OPA0:", &info);
    if (msg.status & 1) {
        msg.refcnt = info.refcnt;
        msg.owner_pid = info.owner_pid;
    }

    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return (msg.status & 1) ? 0 : 1;
}

/* fork() a reader child, collect its report, reap it. Returns 1 on a
 * usable report (out filled), 0 on any failure (out left as reported by
 * the child if any bytes arrived, zeroed otherwise). */
static int read_via_fresh_child(struct child_msg *out)
{
    int p[2];
    memset(out, 0, sizeof(*out));
    if (pipe(p) < 0) {
        printf("  FAIL: pipe() setup for reader child\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("  FAIL: fork() for reader child\n");
        close(p[0]); close(p[1]);
        return 0;
    }
    if (pid == 0) {
        close(p[0]);
        int rc = run_reader_child(p[1]);
        close(p[1]);
        _exit(rc);
    }
    close(p[1]);

    int r = read_bounded(p[0], out, sizeof(*out), CHILD_REPORT_TIMEOUT_MS);
    close(p[0]);

    int wstatus = 0;
    for (int i = 0; i < 20; i++) {
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid || w < 0)
            break;
        struct pollfd nothing = { .fd = -1, .events = 0 };
        poll(&nothing, 1, 100);
    }

    if (r != 1) {
        printf("  (reader child produced no report within %d ms)\n",
               CHILD_REPORT_TIMEOUT_MS);
        return 0;
    }
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    printf("=== test_syssvc_qio_terminal (public sys$assign/sys$qio to the executive's terminal) ===\n");

    /*
     * sys$assign/sys$qio/sys$dassgn are channel-table operations on the
     * PER-PROCESS PCB (src/vmsprocess/vms_pcb.c) -- a normally activated
     * VMS image gets one for free from IMGACT/crt0 (see
     * src/ovmx_init/ovmx_init.c, src/vmsdcl/dcl_main.c). This is a plain
     * gcc-built test program, not an activated image, so vms_pcb_get()
     * would otherwise return NULL and every sys$assign call in this file
     * would fail at its very first line with SS$_BADPARAM -- before ever
     * reaching resolve_vms_device() or vms_kif_assign(), which would look
     * exactly like (and be mistaken for) the property under test. Full
     * privilege mask: this program does not exercise privilege checks and
     * needs none withheld, matching ovmx_init.c's own PID 1 initialization.
     */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (bootstrap("parent") < 0) {
        /*
         * NO-FABRICATED-SUCCESS PROOF, executed against the running
         * artifact -- see the file header's NEGATIVE CONTROL RIG note.
         * This branch is reached ONLY inside the CI negative-control rig
         * (vms.ko never insmod'ed); it is not a product state (vms-0ff).
         * No status VALUE is pinned here on purpose: what must hold
         * regardless of what an unreachable executive "means" is that
         * $ASSIGN to a terminal never reports success and never hands out
         * a channel when it did not reach the executive -- there is no
         * per-process fallback terminal identity (CLAUDE.md Rule 11).
         */
        $DESCRIPTOR(devnam_tt, "TT:");
        uint16_t chan_absent = 0xFFFF;
        uint32_t st = sys$assign(&devnam_tt, &chan_absent, 0, NULL);
        printf("  INFO: sys$assign(\"TT:\") with no executive returned status %u, chan=%u\n",
               st, (unsigned)chan_absent);
        CHECK(!(st & 1),
              "parent: sys$assign(\"TT:\") does NOT report success when the executive was never reached (public API, real returned status)");
        CHECK(chan_absent == 0xFFFF,
              "parent: sys$assign(\"TT:\") did not write the channel variable on failure (no fabricated channel)");

        printf("=== test_syssvc_qio_terminal: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process A-writes/B-reads scenario not exercised, but the no-fabricated-success check above WAS) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* --- Baseline: OPA0:'s reference count before this suite touches it --- */
    struct vms_devinfo info0;
    uint32_t bst = vms_kif_getdvi_devnam("OPA0:", &info0);
    CHECK(bst & 1, "parent: baseline read of the executive's OPA0: row succeeded");
    uint32_t baseline_refcnt = (bst & 1) ? info0.refcnt : 0;

    /* --- 1. The PUBLIC sys$assign, not vms_kif_assign() directly --- */
    $DESCRIPTOR(devnam_tt, "TT:");
    uint16_t chan = 0;
    uint32_t st = sys$assign(&devnam_tt, &chan, 0, NULL);
    printf("  INFO: sys$assign(\"TT:\") returned status=%u chan=%u errno=%d\n", st, chan, errno);
    CHECK((st & 1) && chan != 0,
          "parent: sys$assign(\"TT:\") succeeded and returned a channel (public API)");

    /* --- 2. A-WRITES / B-READS: a fresh, unrelated process reads OPA0: --- */
    struct child_msg after_assign;
    int got1 = read_via_fresh_child(&after_assign);
    CHECK(got1 && (after_assign.status & 1),
          "parent: a fresh child process could read the executive's OPA0: row");
    /* negctl: assign-terminal-bypasses-executive */
    CHECK(got1 && (after_assign.status & 1) &&
          after_assign.refcnt == baseline_refcnt + 1,
          "A-WRITES/B-READS: a fresh child sees the reference sys$assign(\"TT:\") added to OPA0: in the executive (public API, cross-process)");

    /* --- 3. Real I/O through the PUBLIC sys$qiow on that channel --- */
    static const char msg[] = "OVMX vms-1c57: $QIO to a terminal now reaches the executive's device table\n";
    struct _iosb iosb = {0};
    uint32_t qst = sys$qiow(0, chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                             (void *)msg, (uint32_t)(sizeof(msg) - 1), 0, 0, 0, 0);
    CHECK(qst & 1,
          "parent: sys$qiow WRITEVBLK on the executive-backed terminal channel succeeded (real I/O)");
    CHECK((iosb.iosb$w_status & 1) && iosb.iosb$w_bcnt == (uint16_t)(sizeof(msg) - 1),
          "parent: the IOSB reports the real byte count written to the terminal");

    /* --- 4. The PUBLIC sys$dassgn, then a SECOND fresh reader --- */
    uint32_t dst = sys$dassgn(chan);
    CHECK(dst & 1, "parent: sys$dassgn(\"TT:\" channel) succeeded (public API)");

    struct child_msg after_dassgn;
    int got2 = read_via_fresh_child(&after_dassgn);
    CHECK(got2 && (after_dassgn.status & 1),
          "parent: a second fresh child process could read the executive's OPA0: row");
    CHECK(got2 && (after_dassgn.status & 1) &&
          after_dassgn.refcnt == baseline_refcnt,
          "A-WRITES/B-READS: a second fresh child sees OPA0:'s reference count back at baseline after sys$dassgn (the release reached the executive, not just local bookkeeping)");

    /* --- 5. The channel is the identity: once gone, $QIO refuses it --- */
    struct _iosb iosb2 = {0};
    uint32_t qst2 = sys$qio(0, chan, IO$_WRITEVBLK, &iosb2, NULL, 0,
                             (void *)msg, (uint32_t)(sizeof(msg) - 1), 0, 0, 0, 0);
    CHECK(!(qst2 & 1) && qst2 == SS$_IVCHAN,
          "parent: sys$qio on the now-deassigned channel is refused with SS$_IVCHAN, not silently allowed to keep writing");

    printf("=== test_syssvc_qio_terminal: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
