/*
 * test_kmod_spawn_notify.c - Executive /NOWAIT subprocess-exit completion
 *                            (vms-e9a B1, docs/design-libspawn-ovmx.md §3b)
 *
 * WHAT THIS PROVES
 *
 * This is the executive-resident half of LIB$SPAWN's efn/astadr/astprm
 * completion contract. A /NOWAIT LIB$SPAWN/$CREPRC returns immediately; the
 * caller is later told its subprocess finished because the EXECUTIVE set the
 * caller's event flag and/or queued the caller's completion AST when the
 * subprocess recorded its exit status ($EXIT -> VMS_IOCTL_SETEXIT). That is a
 * genuine cross-process signal: PROCESS A (the child) exiting lands an event
 * flag set + an AST in PROCESS B's (the parent's) executive state, which B then
 * observes with $WAITFR / $READEF / DELIVERAST. A per-process userspace fake
 * living in B's own address space could not carry it -- exactly the property
 * (Rule 9 / INV-6) that makes this a real executive facility.
 *
 * Two paths, both against a real /dev/vms with raw ioctl(2):
 *
 *   1. ARM-BEFORE-EXIT (the async path): B arms VMS_IOCTL_SPAWN_NOTIFY on A's
 *      VMS PID while A is still running, then $WAITFR on the completion flag.
 *      A records its exit status; the executive sets B's flag (unblocking the
 *      $WAITFR) and queues B's completion AST. completed == 0 at arm time.
 *
 *   2. ARM-AFTER-EXIT (the race the design closes): A records its exit status
 *      FIRST, then B arms. The executive sees the child already exited and
 *      delivers the completion IMMEDIATELY -- flag set + AST queued -- with
 *      completed == 1, so a fast subprocess never drops the notification.
 *
 * DEVICE-ABSENT CONTRACT (ci.yml kernel-executive-negative-control): a
 * test_kmod_* other than test_kmod_vmsfs* must exit NONZERO when /dev/vms is
 * absent -- it needs the executive and fails honestly without it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"

#define SS_NORMAL   1u

/* SS$_WASSET (ssdef.h == 9): $READEF returns this as its status when the queried
 * event-flag bit was set. Repeated here since this raw-ioctl test does not link
 * libvms. */
#define SS_WASSET_V 9u

/* Completion condition values the children record (any legal $STATUS works;
 * these are the ssdef SS$_NORMAL / SS$_ABORT vectors used by test_kmod_exit). */
#define COND_A      1u          /* SS$_NORMAL  */
#define COND_B      44u         /* SS$_ABORT   */

/* Completion event flags (local cluster 1, 32-63 -- avoids the reserved low
 * flags and the common clusters) and AST cookies. */
#define EFN_A       40u
#define EFN_B       45u
#define EFN_C       50u         /* arm-then-SIGKILL completion flag (vms-2a4) */
#define COOKIE_A    0xA5A5A5A5A5A5A5A5ull
#define PRM_A       0x1111222233334444ull
#define COOKIE_B    0x5A5A5A5A5A5A5A5Aull
#define PRM_B       0x4444333322221111ull
#define COOKIE_C    0xC0FFEE00C0FFEE00ull
#define PRM_C       0xDEADBEEFCAFEF00Dull

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Watchdog: a real bug in the delivery path could hang $WAITFR forever, which
 * would stall the whole CI suite. Bound the run and fail honestly if it fires. */
static void on_alarm(int sig)
{
    (void)sig;
    const char *m = "  FAIL: test_kmod_spawn_notify timed out (completion never delivered)\n";
    (void)!write(STDOUT_FILENO, m, strlen(m));
    _exit(3);
}

/* Register the calling task and return its assigned VMS PID, or 0 on error. */
static uint32_t do_register(int fd)
{
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) != 0 || reg.status != SS_NORMAL)
        return 0;
    return reg.vms_pid;
}

/* SETEXIT(cond) on the calling process. Returns 0 on success. */
static int do_setexit(int fd, uint32_t cond)
{
    struct vms_exit_args a;
    memset(&a, 0, sizeof(a));
    a.condition = cond;
    if (ioctl(fd, VMS_IOCTL_SETEXIT, &a) != 0 || a.status != SS_NORMAL)
        return -1;
    return 0;
}

/* Arm a /NOWAIT completion on child_pid. Fills *completed. Returns SS$ status. */
static uint32_t do_spawn_notify(int fd, uint32_t child_pid, uint32_t efn,
                                uint64_t astadr, uint64_t astprm, int *completed)
{
    struct vms_spawn_notify_args a;
    memset(&a, 0, sizeof(a));
    a.child_vms_pid = child_pid;
    a.efn = efn;
    a.astadr = astadr;
    a.astprm = astprm;
    if (ioctl(fd, VMS_IOCTL_SPAWN_NOTIFY, &a) != 0)
        return 0;   /* transport failure -> not a VMS status; caller treats as fail */
    if (completed)
        *completed = a.completed;
    return a.status;
}

/* $WAITFR(efn): block until the flag is set. Returns SS$ status. */
static uint32_t do_waitfr(int fd, uint32_t efn)
{
    struct vms_ef_args a;
    memset(&a, 0, sizeof(a));
    a.efn = efn;
    if (ioctl(fd, VMS_IOCTL_WAITFR, &a) != 0)
        return 0;
    return a.status;
}

/* $READEF(efn): returns 1 iff the flag bit is currently set. */
static int do_readef_set(int fd, uint32_t efn)
{
    struct vms_ef_read_args a;
    memset(&a, 0, sizeof(a));
    a.efn = efn;
    if (ioctl(fd, VMS_IOCTL_READEF, &a) != 0)
        return 0;
    /* status is SS$_WASSET when the queried bit was set. */
    return a.status == SS_WASSET_V;
}

/* DELIVERAST: dequeue the head deliverable AST, returning its astadr/astprm.
 * Returns 0 iff one was delivered. */
static int do_deliverast(int fd, uint64_t *astadr, uint64_t *astprm)
{
    struct vms_ast_args a;
    memset(&a, 0, sizeof(a));
    if (ioctl(fd, VMS_IOCTL_DELIVERAST, &a) != 0)
        return -1;
    if (astadr) *astadr = a.astadr;
    if (astprm) *astprm = a.astprm;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_spawn_notify ===\n");

    signal(SIGALRM, on_alarm);
    alarm(30);

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        /* Fail honest -- this suite needs the executive (INV-6). */
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        return 1;
    }

    uint32_t self_pid = do_register(fd);
    CHECK(self_pid != 0, "parent registers with the executive");
    if (self_pid == 0) {
        printf("=== test_kmod_spawn_notify: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* --- Path 1: ARM-BEFORE-EXIT (async completion) -------------------- */
    printf("--- arm-before-exit: EF set + AST queued when the child $EXITs ---\n");
    {
        int c2p[2], p2c[2];
        if (pipe(c2p) != 0 || pipe(p2c) != 0) {
            CHECK(0, "pipe() for arm-before-exit test");
        } else {
            pid_t kid = fork();
            if (kid == 0) {
                /* CHILD (process A): register, hand its VMS PID to the parent,
                 * wait until the parent has ARMED, then record its exit. */
                close(c2p[0]); close(p2c[1]);
                int cfd = open("/dev/vms", O_RDWR);
                uint32_t cpid = (cfd >= 0) ? do_register(cfd) : 0;
                (void)!write(c2p[1], &cpid, sizeof cpid);
                char go;
                (void)!read(p2c[0], &go, 1);           /* wait for "armed" */
                if (cfd >= 0) {
                    (void)do_setexit(cfd, COND_A);     /* fires the notification */
                    close(cfd);
                }
                _exit(0);
            }
            close(c2p[1]); close(p2c[0]);
            uint32_t cpid = 0;
            ssize_t n = read(c2p[0], &cpid, sizeof cpid);
            CHECK(n == (ssize_t)sizeof cpid && cpid != 0,
                  "child (process A) registered and reported its VMS PID");

            int completed = -1;
            uint32_t st = do_spawn_notify(fd, cpid, EFN_A, COOKIE_A, PRM_A, &completed);
            CHECK(st == SS_NORMAL, "parent arms VMS_IOCTL_SPAWN_NOTIFY on the child");
            CHECK(completed == 0, "arm reports completed == 0 (child still running)");

            (void)!write(p2c[1], "g", 1);              /* release the child to $EXIT */

            uint32_t wst = do_waitfr(fd, EFN_A);
            CHECK(wst == SS_NORMAL,
                  "$WAITFR on the completion flag returns once the child $EXITs");
            CHECK(do_readef_set(fd, EFN_A) == 1,
                  "the executive SET the parent's completion event flag");

            uint64_t da = 0, dp = 0;
            int drc = do_deliverast(fd, &da, &dp);
            CHECK(drc == 0, "a completion AST was queued into the parent's AST queue");
            CHECK(drc == 0 && da == COOKIE_A,
                  "the queued AST carries the astadr the parent armed");
            CHECK(drc == 0 && dp == PRM_A,
                  "the queued AST carries the astprm the parent armed");

            waitpid(kid, NULL, 0);
            close(c2p[0]); close(p2c[1]);
        }
    }

    /* --- Path 2: ARM-AFTER-EXIT (immediate completion, race closed) ---- */
    printf("--- arm-after-exit: immediate delivery when the child already $EXITed ---\n");
    {
        int c2p[2], p2c[2];
        if (pipe(c2p) != 0 || pipe(p2c) != 0) {
            CHECK(0, "pipe() for arm-after-exit test");
        } else {
            pid_t kid = fork();
            if (kid == 0) {
                /* CHILD (process A): register, record its exit FIRST, then block
                 * (keeping its PCB alive) until the parent has armed + read. */
                close(c2p[0]); close(p2c[1]);
                int cfd = open("/dev/vms", O_RDWR);
                uint32_t cpid = 0;
                if (cfd >= 0) {
                    cpid = do_register(cfd);
                    if (cpid != 0 && do_setexit(cfd, COND_B) != 0)
                        cpid = 0;
                }
                (void)!write(c2p[1], &cpid, sizeof cpid);
                char go;
                (void)!read(p2c[0], &go, 1);           /* stay alive until released */
                if (cfd >= 0) close(cfd);
                _exit(0);
            }
            close(c2p[1]); close(p2c[0]);
            uint32_t cpid = 0;
            ssize_t n = read(c2p[0], &cpid, sizeof cpid);
            CHECK(n == (ssize_t)sizeof cpid && cpid != 0,
                  "child (process A) registered and recorded its exit before the arm");

            int completed = -1;
            uint32_t st = do_spawn_notify(fd, cpid, EFN_B, COOKIE_B, PRM_B, &completed);
            CHECK(st == SS_NORMAL, "parent arms on an already-exited child");
            CHECK(completed == 1,
                  "arm reports completed == 1 (delivered immediately, race closed)");
            CHECK(do_readef_set(fd, EFN_B) == 1,
                  "the executive SET the completion flag immediately");

            uint64_t da = 0, dp = 0;
            int drc = do_deliverast(fd, &da, &dp);
            CHECK(drc == 0 && da == COOKIE_B && dp == PRM_B,
                  "the immediate completion AST carries the armed astadr/astprm");

            (void)!write(p2c[1], "g", 1);              /* release the child */
            waitpid(kid, NULL, 0);
            close(c2p[0]); close(p2c[1]);
        }
    }

    /* --- Path 3: ARM-then-SIGKILL (abnormal deletion, vms-2a4) --------- */
    printf("--- arm-then-SIGKILL: completion fires on a subprocess killed WITHOUT $EXIT ---\n");
    {
        /*
         * The vms-e9a B1 edge: vms_ioctl_setexit delivers an armed completion
         * only when the subprocess records its exit via $EXIT. A subprocess
         * KILLED before that (SIGKILL here; equally a crash / segfault) never
         * reaches SETEXIT, so B1 alone would leave compl_armed set on the dead
         * PCB and the parent's $WAITFR / completion AST would hang forever. The
         * executive must instead deliver the completion when it reclaims the
         * dead child -- with a synthesized abnormal $STATUS (SS$_ABORT), as real
         * VMS notifies the creator on abnormal subprocess deletion. This proves
         * the EF is set and the AST is queued even though the child $EXITed
         * nothing.
         */
        int c2p[2];
        if (pipe(c2p) != 0) {
            CHECK(0, "pipe() for arm-then-SIGKILL test");
        } else {
            pid_t kid = fork();
            if (kid == 0) {
                /* CHILD (process A): register, hand its VMS PID to the parent,
                 * then block FOREVER -- it must be KILLED, never $EXIT. */
                close(c2p[0]);
                int cfd = open("/dev/vms", O_RDWR);
                uint32_t cpid = (cfd >= 0) ? do_register(cfd) : 0;
                (void)!write(c2p[1], &cpid, sizeof cpid);
                for (;;)
                    pause();               /* wait to be SIGKILLed; no SETEXIT */
                _exit(0);                  /* not reached */
            }
            close(c2p[1]);
            uint32_t cpid = 0;
            ssize_t n = read(c2p[0], &cpid, sizeof cpid);
            CHECK(n == (ssize_t)sizeof cpid && cpid != 0,
                  "child (process A) registered and reported its VMS PID");

            int completed = -1;
            uint32_t st = do_spawn_notify(fd, cpid, EFN_C, COOKIE_C, PRM_C, &completed);
            CHECK(st == SS_NORMAL, "parent arms VMS_IOCTL_SPAWN_NOTIFY on the child");
            CHECK(completed == 0, "arm reports completed == 0 (child still running)");

            /* Kill the child WITHOUT letting it record an exit, then reap the
             * Linux zombie so its PCB is reclaimed by the executive. */
            kill(kid, SIGKILL);
            waitpid(kid, NULL, 0);

            /* The executive delivered the completion when it reclaimed the dead
             * PCB (abnormal-deletion path). $WAITFR must therefore return -- if
             * the fix is missing, this hangs and the watchdog fails the test. */
            uint32_t wst = do_waitfr(fd, EFN_C);
            CHECK(wst == SS_NORMAL,
                  "$WAITFR returns after the child is KILLED without $EXIT");
            CHECK(do_readef_set(fd, EFN_C) == 1,
                  "the executive SET the parent's completion flag on abnormal deletion");

            uint64_t da = 0, dp = 0;
            int drc = do_deliverast(fd, &da, &dp);
            CHECK(drc == 0, "a completion AST was queued despite the child never $EXITing");
            CHECK(drc == 0 && da == COOKIE_C && dp == PRM_C,
                  "the completion AST carries the armed astadr/astprm");

            close(c2p[0]);
        }
    }

    alarm(0);
    printf("=== test_kmod_spawn_notify: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
