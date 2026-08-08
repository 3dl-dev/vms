/*
 * test_syssvc_ef_mproc.c - common event flag clusters through the PUBLIC
 * sys$ API, A-writes / B-reads (vms-f1f).
 *
 * ============================================================
 * WRITTEN RED (vms-f1f). MADE GREEN BY FIXING THE PRODUCT (vms-2a8).
 * NOT ONE ASSERTION WAS CHANGED TO GET THERE.
 *
 * ROUND 2 ADDED THE INTERRUPTED-WAIT BLOCK, and it was written red too:
 * wiring sys_event.c to the executive made src/kernel/vms_eflag.c's
 * "interrupted, but still return normally" line LIVE, so $WAITFR reported
 * SS$_NORMAL over a flag that was still clear. That defect is now a standing
 * negative control -- tests/qemu/facility_defects.sh
 * eflag-waitfr-eintr-normal -- which restores it and requires this suite to
 * go red on exactly the two assertions the manifest names.
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
 * failure is a named FAIL line rather than a harness-wide QEMU timeout. The
 * interrupted-wait block added by round 2 arms an interval timer, but nothing
 * in the test is PACED by it: every step still advances on a byte the waiter
 * actually wrote, and the interval only decides how long a bounded poll sits
 * there. See the block comment above run_wait_child().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/time.h>
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

/* Common cluster 2 flag used only by the interrupted-wait block (vms-2a8
 * round 2). A flag NOBODY sets until the parent decides to -- that is the
 * whole measurement. */
#define WAIT_EFN        80

/* Common cluster 3 (flags 96-127), used only by the vms-2a8 lifetime block
 * at the end of main() so it cannot collide with the cluster-2 flags the
 * A-writes/B-reads measurement above uses. PERM_EFN is deliberately NOT the
 * base number 96 -- see the oracle note at that block. */
#define COMMON_BASE_3   96
#define PERM_EFN        100

/* Common cluster 3, a SECOND pair of flags distinct from PERM_EFN (vms-2ed).
 * Bit positions within the cluster word are WFLAND_EFN_A - COMMON_BASE_3 = 9
 * and WFLAND_EFN_B - COMMON_BASE_3 = 10. */
#define WFLAND_EFN_A    105
#define WFLAND_EFN_B    106

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
    /* negctl: bind-client-no-register */
    CHECK(st_local & 1,
          "child: sys$setef on a LOCAL flag succeeds (baseline: the event flag facility is operative in this process at all)");

    st = sys$setef(CLUSTER_EFN_A);
    /* negctl-knockon: bind-client-no-register */
    CHECK((st_local & 1) && !(st & 1),
          "child: sys$setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')");

    /*
     * Wait for A to have created the cluster first. Without this, A and B's
     * own sys$ascefc calls below race to be the FIRST caller on this same
     * cluster name (which CREATES it) versus the SECOND (which FINDS it and
     * re-associates) -- unordered, so which process lands on which branch
     * is scheduler-dependent. Same race, same fix, as test_kmod_eflag_mproc.c
     * one layer down (vms-400): a token handshake decides the winner, A
     * always creates, B always re-associates.
     */
    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'P') {
        printf("  FAIL: child: never saw the parent's cluster-created token\n");
        fail++;
    }

    st = sys$ascefc(COMMON_BASE, &clusnam, 0, 0);
    printf("  INFO: child: sys$ascefc returned status %u\n", st);
    /* negctl-knockon: bind-client-no-register */
    /* negctl: eflag-ascefc-reassoc-status-wrong */
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
        /* negctl-knockon: bind-client-no-register */
        CHECK((st & 1) && (state & (1u << (CLUSTER_EFN_A - COMMON_BASE))),
              "child: a common flag SET BY THE PARENT via sys$setef is visible here (A writes, B reads, public API)");
    }

    st = sys$setef(CLUSTER_EFN_B);
    /* negctl-knockon: bind-client-no-register */
    CHECK(st & 1, "child: sys$setef on the associated common cluster reported success");
    if (send_token(c2p_write, 'C') < 0)
        fail++;

    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'D') {
        printf("  FAIL: child: never saw the parent's post-$CLREF token\n");
        fail++;
    } else {
        state = 0;
        st = sys$readef(CLUSTER_EFN_A, &state);
        /* negctl: eflag-clref-noop */
        /* negctl-knockon: bind-client-no-register */
        CHECK((st & 1) && !(state & (1u << (CLUSTER_EFN_A - COMMON_BASE))),
              "child: a common flag CLEARED BY THE PARENT via sys$clref reads clear here (A clears, B reads, public API)");

        /* Discriminator: local clusters are per-process on VMS too. */
        state = 0;
        st = sys$readef(LOCAL_EFN, &state);
        /* negctl-knockon: bind-client-no-register */
        CHECK((st & 1) && !(state & (1u << LOCAL_EFN)),
              "child: a LOCAL flag set by the parent is NOT visible here (local clusters stay per-process)");
    }

    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return fail > 0 ? 1 : 0;
}

/* ======================================================================
 * INTERRUPTED WAITS (added by vms-2a8 round 2).
 *
 * WHAT THIS MEASURES. src/kernel/vms_eflag.c's WAITFR/WFLOR/WFLAND treated
 * a wait_event_interruptible() return as terminal and answered SS$_NORMAL
 * for it -- "the flag is set" about a flag that was still clear -- with
 * rc=0/errno=0 so the caller could not even detect it. That is a public
 * sys$ entry point fabricating success, which is the exact defect class
 * this whole suite exists to catch, and it went LIVE when sys_event.c was
 * wired to the executive: before that, nothing called those handlers.
 *
 * WHY THERE IS NO OTHER LEGAL ANSWER, and it is oracle-pinned, not argued
 * (docs/oracle/vax73-event-flags.md §4, VAX1 OpenVMS VAX V7.3):
 *   HELP $WAITFR -- "the process is placed in a wait state UNTIL THE EVENT
 *     FLAG IS SET", and the online help has no Condition Values topic for it.
 *   HELP $HIBER  -- a waiting process "remains known to the system so that
 *     it can be interrupted; for example, to receive ASTs": a VMS wait IS
 *     interruptible, the AST runs, and the wait continues. The caller of the
 *     wait never learns it happened.
 *   SEARCH of $SSDEF for WAIT/INTERRUPT/ABORTED -- four unrelated symbols.
 *     VMS has no "wait was interrupted" status at all.
 * So under CLAUDE.md Rule 10 the condition is made UNREACHABLE, not handled.
 *
 * HOW IT IS MEASURED, AND WHY IT IS NOT PACED BY SLEEPS. The waiter child
 * arms a REPEATING interval timer (not alarm(), not a one-shot) whose
 * SIGALRM handler is installed WITHOUT SA_RESTART, then blocks in
 * sys$waitfr() on a flag nobody has set. Because the timer repeats and the
 * child has nowhere else to be, delivery INSIDE the wait is guaranteed
 * rather than raced for: at most the first tick can land before the ioctl
 * is entered, and every later one cannot. Each handler run writes one byte
 * to the pipe, so the parent advances on OBSERVED OUTPUT, never on elapsed
 * time; the interval only decides how long the parent's bounded poll waits.
 *
 * THE DISCRIMINATOR IS THE CHILD'S FINAL BYTE:
 *   'S' -- $WAITFR returned AND $READEF confirms the flag is genuinely set.
 *   'X' -- $WAITFR returned while $READEF still shows the flag clear, or it
 *          returned a failure status. Either way it did not wait.
 * The parent only sets the flag after WAIT_SIGNAL_ROUNDS handler bytes, so
 * an 'S' cannot be produced early by accident.
 *
 * IT REQUIRES BOTH HALVES OF THE FIX AND CATCHES EITHER ONE MISSING:
 *   - executive still fabricating SS$_NORMAL  -> 'X' on the first tick.
 *   - executive fixed but libvmssys not re-entering the wait -> the ioctl
 *     surfaces -EINTR, which vms_kif_kerr_to_ss maps to SS$_BUGCHECK (even,
 *     so !(st & 1)) -> 'X'.
 * ====================================================================== */

#define WAIT_SIGNAL_ROUNDS  3
#define WAIT_TICK_USEC      100000

static volatile int wait_sig_fd = -1;

static void wait_sig_handler(int sig)
{
    char tok = 'A';
    (void)sig;
    /* write(2) is async-signal-safe. The child is parked in the executive's
     * wait when this runs, so there is no reentrancy to worry about, and the
     * parent is draining the pipe. */
    if (write(wait_sig_fd, &tok, 1) != 1)
        _exit(1);
}

/*
 * The waiter child. Returns nothing to the parent except bytes on the pipe:
 * 'A' per handler run (written by the handler itself), then exactly one
 * verdict byte -- 'S', 'X', or 'E' if it could not even set the scenario up.
 */
static int run_wait_child(int c2p_write, const struct dsc$descriptor_s *nam)
{
    struct sigaction sa;
    struct itimerval it;
    uint32_t st, state = 0;
    char verdict;

    st = sys$ascefc(COMMON_BASE, nam, 0, 0);
    if (!(st & 1)) {
        printf("  INFO: waiter: sys$ascefc failed, status %u\n", st);
        (void)send_token(c2p_write, 'E');
        return 1;
    }

    wait_sig_fd = c2p_write;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = wait_sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* NO SA_RESTART, DELIBERATELY: this is what forces
                        * the interrupted ioctl out to userspace instead of
                        * letting the host kernel restart it silently. */
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        (void)send_token(c2p_write, 'E');
        return 1;
    }

    it.it_interval.tv_sec  = 0;
    it.it_interval.tv_usec = WAIT_TICK_USEC;
    it.it_value = it.it_interval;
    if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
        (void)send_token(c2p_write, 'E');
        return 1;
    }

    /* Nobody has set WAIT_EFN. This must not return until somebody does. */
    st = sys$waitfr(WAIT_EFN);

    memset(&it, 0, sizeof(it));
    (void)setitimer(ITIMER_REAL, &it, NULL);

    state = 0;
    (void)sys$readef(WAIT_EFN, &state);
    printf("  INFO: waiter: sys$waitfr(%d) returned status %u; sys$readef "
           "state=0x%08x (bit %d %s)\n",
           WAIT_EFN, st, state, WAIT_EFN - COMMON_BASE,
           (state & (1u << (WAIT_EFN - COMMON_BASE))) ? "SET" : "STILL CLEAR");

    verdict = ((st & 1) && (state & (1u << (WAIT_EFN - COMMON_BASE)))) ? 'S' : 'X';
    (void)send_token(c2p_write, verdict);
    return verdict == 'S' ? 0 : 1;
}

/* ========================================================================
 * $WFLAND -- waits for ALL of a mask, not any one of it (vms-2ed).
 *
 * ORACLE (docs/oracle/vax73-event-flags.md section 4.4, OpenVMS System
 * Services Reference Manual): "$WFLAND ... The process is put in a wait
 * state until all specified event flags are set." Condition values are the
 * same three ($SS_NORMAL/$SS_ILLEFC/$SS_UNASEFC) $WAITFR already carries, so
 * this scenario -- like the interrupted-wait one above -- asserts the
 * behaviour, not an invented status.
 *
 * THE CHILD blocks in $WFLAND on a TWO-flag mask (WFLAND_EFN_A |
 * WFLAND_EFN_B) and reports 'R' the instant before the blocking call, so the
 * parent knows the call has actually been made.
 *
 * THE PARENT sets ONLY WFLAND_EFN_A, then does a SHORT bounded read
 * expecting SILENCE: if $WFLAND were actually implemented as $WFLOR (any
 * ONE flag suffices, the exact defect class this scenario exists to catch),
 * the child would return and write its verdict almost immediately. A short
 * bound is enough to catch "returned essentially at once" without being a
 * tight race -- this is not testing timing, it is testing which of two
 * categorically different code paths ran. Only after that silence is
 * confirmed does the parent set WFLAND_EFN_B, and the child is expected to
 * unblock and report success within the ordinary PEER_TIMEOUT_MS.
 * ======================================================================== */

#define WFLAND_SILENCE_MS   500

static int run_wfland_child(int c2p_write, const struct dsc$descriptor_s *nam)
{
    uint32_t st, state = 0;
    uint32_t mask = (1u << (WFLAND_EFN_A - COMMON_BASE_3)) |
                    (1u << (WFLAND_EFN_B - COMMON_BASE_3));
    char verdict;

    st = sys$ascefc(COMMON_BASE_3, nam, 0, 0);
    if (!(st & 1)) {
        printf("  INFO: wfland-child: sys$ascefc failed, status %u\n", st);
        (void)send_token(c2p_write, 'E');
        return 1;
    }

    if (send_token(c2p_write, 'R') < 0)
        return 1;

    st = sys$wfland(WFLAND_EFN_A, mask);

    (void)sys$readef(WFLAND_EFN_A, &state);
    printf("  INFO: wfland-child: sys$wfland(%d, 0x%x) returned status %u; "
           "state=0x%08x\n", WFLAND_EFN_A, mask, st, state);

    verdict = ((st & 1) && ((state & mask) == mask)) ? 'S' : 'X';
    (void)send_token(c2p_write, verdict);
    return verdict == 'S' ? 0 : 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
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
    /* negctl: bind-client-no-register */
    CHECK(st_local & 1,
          "parent: sys$setef on a LOCAL flag succeeds (baseline: the event flag facility is operative in this process at all)");

    st = sys$setef(CLUSTER_EFN_A);
    printf("  INFO: sys$setef(%d) [COMMON, not yet associated] returned status %u\n",
           CLUSTER_EFN_A, st);
    /* negctl-knockon: bind-client-no-register */
    CHECK((st_local & 1) && !(st & 1),
          "parent: sys$setef on an UNASSOCIATED common flag is refused WHILE a local flag succeeds (not merely 'every call fails')");

    st = sys$ascefc(COMMON_BASE, &clusnam, 0, 0);
    printf("  INFO: sys$ascefc(%d, \"OVMX$F1F_SVC\") returned status %u\n",
           COMMON_BASE, st);
    /* negctl-knockon: bind-client-no-register */
    CHECK(st & 1, "parent: sys$ascefc created/joined the named common cluster");

    /* Tell B the cluster now exists, so B's own sys$ascefc (below, via the
     * matching wait on its side) deterministically re-associates rather
     * than racing A to create it -- see B's matching comment. */
    if (send_token(p2c[1], 'P') < 0)
        fail++;

    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'A') {
        printf("  FAIL: parent: child never reported that it associated\n");
        fail++;
    }

    st = sys$setef(CLUSTER_EFN_A);
    printf("  INFO: sys$setef(%d) [COMMON, after $ASCEFC] returned status %u\n",
           CLUSTER_EFN_A, st);
    /* negctl-knockon: bind-client-no-register */
    CHECK(st & 1, "parent: sys$setef on the associated common cluster reported success");
    if (send_token(p2c[1], 'B') < 0)
        fail++;

    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'C') {
        printf("  FAIL: parent: child never reported its own sys$setef\n");
        fail++;
    } else {
        uint32_t state = 0;
        st = sys$readef(CLUSTER_EFN_B, &state);
        /* negctl-knockon: bind-client-no-register */
        CHECK((st & 1) && (state & (1u << (CLUSTER_EFN_B - COMMON_BASE))),
              "parent: a common flag SET BY THE CHILD via sys$setef is visible here (B writes, A reads, public API)");
    }

    st = sys$clref(CLUSTER_EFN_A);
    printf("  INFO: sys$clref(%d) [COMMON, after $ASCEFC] returned status %u\n",
           CLUSTER_EFN_A, st);
    /* negctl-knockon: bind-client-no-register */
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
    /* negctl-knockon: bind-client-no-register */
    /* negctl: eflag-dacefc-status-wrong */
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
        /* negctl-knockon: bind-client-no-register */
        CHECK(st & 1, "parent: sys$ascefc created a PERMANENT common cluster");

        st = sys$setef(PERM_EFN);
        /* negctl-knockon: bind-client-no-register */
        CHECK(st & 1, "parent: sys$setef on the permanent cluster reported success");

        st = sys$dacefc(PERM_EFN);
        /* negctl-knockon: bind-client-no-register */
        /* negctl-knockon: eflag-dacefc-status-wrong */
        CHECK(st & 1, "parent: sys$dacefc released the last association to the permanent cluster");

        st = sys$ascefc(PERM_EFN, &permnam, 0, 0);
        /* negctl-knockon: bind-client-no-register */
        /* negctl-knockon: eflag-ascefc-reassoc-status-wrong */
        CHECK(st & 1, "parent: sys$ascefc re-joined the permanent cluster by name");
        state = 0;
        st = sys$readef(PERM_EFN, &state);
        printf("  INFO: after re-join, sys$readef(%d) status=%u state=0x%08x\n",
               PERM_EFN, st, state);
        /* negctl-knockon: bind-client-no-register */
        CHECK((st & 1) && (state & (1u << (PERM_EFN - COMMON_BASE_3))),
              "parent: a PERMANENT cluster survived losing its last association (its flags are still set)");

        st = sys$dlcefc(&permnam);
        printf("  INFO: sys$dlcefc(\"OVMX$2A8_PERM\") returned status %u\n", st);
        /* negctl-knockon: bind-client-no-register */
        /* negctl: eflag-dlcefc-status-wrong */
        CHECK(st & 1, "parent: sys$dlcefc accepted the permanent cluster by name");

        st = sys$dacefc(PERM_EFN);
        /* negctl-knockon: bind-client-no-register */
        /* negctl-knockon: eflag-dacefc-status-wrong */
        CHECK(st & 1, "parent: sys$dacefc released the marked cluster");

        st = sys$ascefc(PERM_EFN, &permnam, 0, 0);
        /* negctl-knockon: bind-client-no-register */
        CHECK(st & 1, "parent: sys$ascefc after the deletion created a cluster of that name again");
        state = 0;
        st = sys$readef(PERM_EFN, &state);
        printf("  INFO: after $DLCEFC + re-create, sys$readef(%d) status=%u state=0x%08x (expect 0)\n",
               PERM_EFN, st, state);
        /* negctl-knockon: bind-client-no-register */
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

    /* =====================================================================
     * INTERRUPTED WAITS (vms-2a8 round 2). See the block comment above
     * run_wait_child() for the oracle pins and the measurement design.
     * ===================================================================== */
    {
        $DESCRIPTOR(waitnam, "OVMX$2A8_WAIT");
        int w2p[2];
        pid_t wpid;
        int alarms = 0;
        char verdict = 0;

        st = sys$ascefc(COMMON_BASE, &waitnam, 0, 0);
        printf("  INFO: sys$ascefc(%d, \"OVMX$2A8_WAIT\") returned status %u\n",
               COMMON_BASE, st);
        /* negctl-knockon: bind-client-no-register */
        CHECK(st & 1, "parent: sys$ascefc joined the cluster the interrupted-wait measurement uses");

        if (pipe(w2p) < 0) {
            printf("  FAIL: parent: pipe() for the waiter failed\n");
            fail++;
        } else if ((wpid = fork()) < 0) {
            printf("  FAIL: parent: fork() for the waiter failed\n");
            fail++;
        } else if (wpid == 0) {
            close(w2p[0]);
            _exit(run_wait_child(w2p[1], &waitnam));
        } else {
            int wst = 0;
            int silent = 0;
            uint32_t sst;
            close(w2p[1]);

            /*
             * PHASE 1 -- collect interrupts. Advance ONLY on bytes the waiter
             * actually produced; stop early if it hands back a verdict, which
             * means it stopped waiting before it was allowed to.
             */
            while (alarms < WAIT_SIGNAL_ROUNDS && verdict == 0) {
                char t;
                if (read_bounded(w2p[0], &t, 1, PEER_TIMEOUT_MS) != 1) {
                    silent = 1;
                    break;
                }
                if (t == 'A')
                    alarms++;
                else
                    verdict = t;
            }

            if (silent) {
                printf("  FAIL: parent: the waiter went silent (no handler byte, no verdict)\n");
                fail++;
            }

            printf("  INFO: parent: waiter reported %d signal interrupt(s) before the flag was set\n",
                   alarms);
            /* negctl-knockon: bind-client-no-register */
            /* negctl-knockon: eflag-waitfr-eintr-normal */
            /* negctl-knockon: eflag-ascefc-reassoc-status-wrong */
            CHECK(alarms >= WAIT_SIGNAL_ROUNDS,
                  "parent: the waiter was interrupted by a signal repeatedly WHILE blocked in sys$waitfr (the condition under test is reachable, not hypothetical)");

            /*
             * Release the flag UNCONDITIONALLY, even when the waiter already
             * gave up: the assertion below must always be reached, and a
             * waiter still parked in the executive must never be left there.
             */
            sst = sys$setef(WAIT_EFN);
            printf("  INFO: parent: sys$setef(%d) returned status %u\n", WAIT_EFN, sst);
            /* negctl-knockon: bind-client-no-register */
            CHECK(sst & 1, "parent: sys$setef released the waiter's flag");

            /* PHASE 2 -- the verdict, if the waiter has not produced one. */
            while (verdict == 0) {
                char t;
                if (read_bounded(w2p[0], &t, 1, PEER_TIMEOUT_MS) != 1)
                    break;
                if (t != 'A')
                    verdict = t;
            }

            waitpid(wpid, &wst, 0);

            printf("  INFO: waiter verdict '%c' ('S' = it waited for the flag, 'X' = it returned over a clear flag)\n",
                   verdict ? verdict : '?');

            /* negctl: eflag-waitfr-eintr-normal */
            /* negctl-knockon: bind-client-no-register */
            /* negctl-knockon: eflag-ascefc-reassoc-status-wrong */
            CHECK(verdict == 'S',
                  "parent: sys$waitfr did NOT return until the flag was really set -- an interrupted wait is re-entered, never reported as SS$_NORMAL over a clear flag");

            close(w2p[0]);
        }

        (void)sys$clref(WAIT_EFN);
        (void)sys$dacefc(WAIT_EFN);
    }

    /* =====================================================================
     * $WFLOR / $WFLAND -- OR vs. AND (vms-2ed). See docs/oracle/
     * vax73-event-flags.md section 4.4 for the pin and run_wfland_child()
     * above for the design.
     * ===================================================================== */
    {
        $DESCRIPTOR(wfnam, "OVMX$2ED_WF");
        uint32_t mask = (1u << (WFLAND_EFN_A - COMMON_BASE_3)) |
                        (1u << (WFLAND_EFN_B - COMMON_BASE_3));
        uint32_t state = 0;
        int c2p2[2];
        pid_t cpid;

        /* --- $WFLOR: ANY one flag in the mask suffices --------------------
         * Done in THIS process, not forked: with WFLAND_EFN_A already set,
         * the predicate is true at call time, so $WFLOR cannot genuinely
         * block here -- there is nothing to wait FOR. That is the point:
         * an implementation that (wrongly) required ALL flags, like
         * $WFLAND, would block forever on this exact call. */
        st = sys$ascefc(COMMON_BASE_3, &wfnam, 0, 0);
        printf("  INFO: parent: sys$ascefc(%d, \"OVMX$2ED_WF\") returned status %u\n",
               COMMON_BASE_3, st);
        /* negctl-knockon: bind-client-no-register */
        CHECK(st & 1, "parent: sys$ascefc joined the cluster the WFLOR/WFLAND measurement uses");

        (void)sys$clref(WFLAND_EFN_A);
        (void)sys$clref(WFLAND_EFN_B);

        st = sys$setef(WFLAND_EFN_A);
        printf("  INFO: parent: sys$setef(%d) [WFLOR setup] returned status %u\n",
               WFLAND_EFN_A, st);
        /* negctl-knockon: bind-client-no-register */
        CHECK(st & 1, "parent: sys$setef sets the flag $WFLOR will find already satisfied");

        state = 0;
        st = sys$wflor(WFLAND_EFN_A, mask);
        (void)sys$readef(WFLAND_EFN_A, &state);
        printf("  INFO: parent: sys$wflor(%d, 0x%x) returned status %u; state=0x%08x "
               "(only bit %d of the mask is set)\n",
               WFLAND_EFN_A, mask, st, state, WFLAND_EFN_A - COMMON_BASE_3);
        /* negctl: eflag-wflor-status-wrong */
        /* negctl-knockon: bind-client-no-register */
        CHECK((st & 1) && (state & (1u << (WFLAND_EFN_A - COMMON_BASE_3))),
              "parent: sys$wflor returned with only ONE of the two mask flags set -- OR, not AND");

        (void)sys$clref(WFLAND_EFN_A);

        /* --- $WFLAND: ALL flags in the mask are required ------------------ */
        if (pipe(c2p2) < 0) {
            printf("  FAIL: parent: pipe() for the wfland child failed\n");
            fail++;
        } else if ((cpid = fork()) < 0) {
            printf("  FAIL: parent: fork() for the wfland child failed\n");
            fail++;
            close(c2p2[0]);
            close(c2p2[1]);
        } else if (cpid == 0) {
            close(c2p2[0]);
            _exit(run_wfland_child(c2p2[1], &wfnam));
        } else {
            char tok = 0;
            char verdict = 0;
            int wst = 0;

            close(c2p2[1]);

            if (read_bounded(c2p2[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'R') {
                /* negctl-knockon: bind-client-no-register */
                /* negctl-knockon: eflag-ascefc-reassoc-status-wrong */
                printf("  FAIL: parent: wfland child never reported it was ready to block\n");
                fail++;
            } else {
                st = sys$setef(WFLAND_EFN_A);
                printf("  INFO: parent: sys$setef(%d) [only ONE of the WFLAND mask] returned status %u\n",
                       WFLAND_EFN_A, st);
                CHECK(st & 1, "parent: sys$setef sets one of the two flags $WFLAND is waiting on");

                /* Expect SILENCE: the child must still be blocked, because
                 * only one of the two required flags is set. */
                if (read_bounded(c2p2[0], &verdict, 1, WFLAND_SILENCE_MS) == 1) {
                    printf("  FAIL: parent: wfland child returned '%c' with only ONE flag set -- it did not wait for ALL of them\n",
                           verdict);
                    fail++;
                } else {
                    printf("  PASS: parent: wfland child stayed blocked with only one of two required flags set\n");
                    pass++;

                    st = sys$setef(WFLAND_EFN_B);
                    printf("  INFO: parent: sys$setef(%d) [the second WFLAND mask flag] returned status %u\n",
                           WFLAND_EFN_B, st);
                    CHECK(st & 1, "parent: sys$setef sets the second, completing flag");

                    if (read_bounded(c2p2[0], &verdict, 1, PEER_TIMEOUT_MS) != 1) {
                        printf("  FAIL: parent: wfland child never reported a verdict after both flags were set\n");
                        fail++;
                    } else {
                        /* negctl: eflag-wfland-status-wrong */
                        CHECK(verdict == 'S',
                              "parent: sys$wfland unblocked only once BOTH mask flags were set (AND, not OR)");
                    }
                }
            }

            waitpid(cpid, &wst, 0);
            close(c2p2[0]);
        }

        (void)sys$clref(WFLAND_EFN_A);
        (void)sys$clref(WFLAND_EFN_B);
        (void)sys$dacefc(WFLAND_EFN_A);
    }

    printf("=== test_syssvc_ef_mproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
