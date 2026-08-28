/*
 * test_syssvc_rundown_ff8.c - process rundown timing is governed by the
 *                             /dev/vms struct-file lifetime (vms-ff8)
 *
 * vms-ff8 asked: "does the executive run down a $CREPRC subject's process
 * table entry -- and the LOCKS/EF/CHANNELS it holds -- when its Linux backing
 * dies?" Reading src/kernel/{vms_proctab.c,vms_module.c,vms_lock.c} and
 * src/libvmssys/vms_kif.c, then measuring against a real /dev/vms, the answer
 * is: YES, and WHEN it happens is decided by who holds the dying task's
 * /dev/vms struct file:
 *
 *   - vms_dev_release() (vms_module.c:796) frees a PCB -- and via
 *     vms_proc_free_claimed() -> vms_proc_release_locks() its locks/EF/channels
 *     -- SYNCHRONOUSLY, but only when the OWNING task's exit closes the LAST
 *     reference to that struct file (PF_EXITING && thread_group_empty && the
 *     file refcount reaching zero).
 *   - If another task still holds a dup of that fd, .release does not fire on
 *     the owner's exit, and the PCB + its still-granted locks are reclaimed
 *     only LAZILY, by vms_proc_reap_dead() (vms_proctab.c:95), which runs ONLY
 *     at the top of a process-table op (REGISTER/GETJPI/PROCSCAN/SETPRN/
 *     SETIDENT) -- never from $ENQ/$DEQ/$SETEF/$QIO. lock_compatible()
 *     (vms_lock.c) does not check whether a granting process is still alive.
 *
 * WHY PRODUCTION MOSTLY AVOIDS THE LAZY WINDOW: a forked child that touches
 * any facility binds through kif_bind() (vms_kif.c:214), and image activation
 * through vms_kif_register_continue() -- BOTH detect the fd was dup'd across
 * fork (vms_bound_pid != getpid()) and DROP it for a fresh one, so a normal
 * $CREPRC/RUN subject owns its own struct file and is run down synchronously
 * at death. The lazy path is the executive's DESIGNED fallback for the
 * remaining case: a registered task that dies while another task still holds a
 * dup of its /dev/vms fd (e.g. a plain fork() that never re-binds).
 *
 * This program proves both halves against a real /dev/vms:
 *
 *   Part A - FRESH fd (models the production kif_bind/register_continue path):
 *     the subject drops the inherited fd (vms_kif_close) and opens its own,
 *     takes an EX lock, and exits cleanly. It is the sole holder of its struct
 *     file, so .release fires and releases the lock SYNCHRONOUSLY: the parent's
 *     later EX+NOQUEUE $ENQ is granted with NO intervening process-table op.
 *
 *   Part B - SHARED fd (the lazy-reaper fallback / shared-fd death mode):
 *     the subject keeps the parent's inherited fd (explicit vms_kif_open reuse,
 *     the ONE path that does not drop it), takes an EX lock, and exits. The
 *     parent still holds that struct file, so .release does NOT fire for the
 *     subject; its lock stays granted. From the live parent, with NO table op:
 *       B1: conflicting EX+NOQUEUE $ENQ -> SS$_NOTQUEUED (dead owner's lock
 *           still held -- the resource-rundown-latency the item is about).
 *       (reap) one $GETJPI runs the lazy reaper.
 *       B2: the same $ENQ retried -> granted (the reap released it).
 *
 * Net: rundown is not leaked; its TIMING tracks the fd lifetime. Synchronous
 * for a task that owns its channel (the production norm), lazy for the
 * shared-fd death mode. Whether the lazy window should be closed for strict
 * VMS fidelity (event-driven rundown at task death) is the vms-ff8 fix
 * question; this suite pins the current, measured behavior of both modes.
 *
 * Requires a real /dev/vms; without it, exercises the no-fabricated-success
 * checks and exits EXIT_SKIP (77), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/wait.h>
#include <stdint.h>

/* starlet.h only, NOT lckdef.h -- see test_syssvc_lock.c:65 (vms-5bd). */
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

/* Caller-allocated Lock Status Block; mirrors sys_lock.c's private layout,
 * exactly as test_syssvc_lock.c declares it (no public lksdef.h, Rule 8). */
struct lksb_caller {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[16];
};

#define EXIT_SKIP 77
#define REPORT_TIMEOUT_MS 20000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* subject -> parent report */
struct subj_msg {
    uint32_t granted;   /* 1 = subject's EX $ENQ was granted */
    uint32_t lkid;
};

static int bootstrap(const char *who)
{
    if (vms_kif_open() < 0) {
        printf("  FAIL: %s: cannot open /dev/vms\n", who);
        return -1;
    }
    uint32_t st = vms_kif_register(NULL);   /* NO privilege arg (vms-2b8) */
    if (!(st & 1)) {
        printf("  FAIL: %s: vms_kif_register status=%u\n", who, st);
        return -1;
    }
    return 0;
}

/* Build a CLASS_S string descriptor with the SAME dtype/class the $DESCRIPTOR
 * macro (descrip.h:225) produces, so it matches what sys_lock.c expects. */
static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d = {
        (uint16_t)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)s
    };
    return d;
}

/* Bounded read in the PARENT (never the child); see test_syssvc_lock.c:152. */
static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 0;
        if (pr < 0)  return -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0)  return -1;
        got += (size_t)n;
    }
    return 1;
}

/*
 * subject_take - in a forked child: (optionally) drop the parent's inherited
 * /dev/vms fd so we own our OWN struct file, register, take EX on `resource`,
 * report the grant to the parent, and RETURN (the caller then _exit()s).
 *
 *   fresh_fd != 0 : drop the dup'd parent fd and open our own -- models the
 *                   production kif_bind()/register_continue() path, so our
 *                   later exit is the last reference and fires .release.
 *   fresh_fd == 0 : keep the inherited fd (vms_kif_open reuses it) -- the
 *                   parent still holds this struct file, so our exit will NOT
 *                   fire .release; the lazy reaper is the only path left.
 */
static uint32_t subject_take(const char *who, const char *resource,
                             int report_fd, int fresh_fd)
{
    struct subj_msg m = {0, 0};
    if (fresh_fd)
        vms_kif_close();   /* drop the fd dup'd across fork, take our own below */
    if (bootstrap(who) < 0) {
        (void)!write(report_fd, &m, sizeof(m));
        return 0;
    }
    struct dsc$descriptor_s resnam = mkdsc(resource);
    struct lksb_caller lksb = {0};
    uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb, 0,
                           &resnam, 0, NULL, 0, NULL, 0, 0);
    m.granted = ((st & 1) && lksb.lksb$l_lkid != 0) ? 1 : 0;
    m.lkid = lksb.lksb$l_lkid;
    (void)!write(report_fd, &m, sizeof(m));
    return m.granted ? m.lkid : 0;
}

/* Fork a subject that takes EX on `resource` and exits; wait for its grant
 * report and reap its zombie. Returns 1 if the subject reported a grant. */
static int spawn_dead_holder(const char *who, const char *resource, int fresh_fd)
{
    int c2p[2];
    if (pipe(c2p) < 0) { printf("  FAIL: pipe(%s)\n", who); return 0; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork(%s)\n", who); return 0; }
    if (pid == 0) {
        close(c2p[0]);
        subject_take(who, resource, c2p[1], fresh_fd);
        close(c2p[1]);
        _exit(0);
    }
    close(c2p[1]);

    struct subj_msg m = {0, 0};
    int r = read_bounded(c2p[0], &m, sizeof(m), REPORT_TIMEOUT_MS);
    close(c2p[0]);

    int ws = 0;
    waitpid(pid, &ws, 0);   /* reap so the subject's task is truly dead */
    return (r == 1 && m.granted);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* vms-b5b */
    printf("=== test_syssvc_rundown_ff8 (rundown timing vs /dev/vms fd lifetime, vms-ff8) ===\n");

    if (bootstrap("parent") < 0) {
        /* No-fabricated-success proof (CI negative control only); see
         * test_syssvc_lock.c:266. */
        struct dsc$descriptor_s resnam = mkdsc("FF8_RUNDOWN_ABSENT_EXE");
        struct lksb_caller lksb = {0};
        uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb, 0,
                               &resnam, 0, NULL, 0, NULL, 0, 0);
        printf("  INFO: sys$enqw with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$enqw does NOT report success when the executive was never reached");
        CHECK(lksb.lksb$l_lkid == 0,
              "parent: sys$enqw fabricates no lock ID when the executive was never reached");
        printf("=== test_syssvc_rundown_ff8: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ============================================================
     * Part A - FRESH fd: clean exit runs the lock down SYNCHRONOUSLY.
     * The parent performs NO process-table op between the subject's death
     * and its $ENQ, so a grant proves .release freed the lock at exit.
     * ============================================================ */
    {
        const char *RES = "FF8_FRESH_FD";
        int held = spawn_dead_holder("freshfd-subject", RES, /*fresh_fd=*/1);
        CHECK(held, "A: fresh-fd subject took EX, then exited (sole holder of its /dev/vms channel)");

        struct dsc$descriptor_s resnam = mkdsc(RES);
        struct lksb_caller lk = {0};
        uint32_t st = sys$enq(0, LCK$K_EXMODE, &lk, LCK$M_NOQUEUE,
                              &resnam, 0, NULL, 0, NULL, 0, 0);
        printf("  INFO: A $ENQ after fresh-fd subject's exit (no table op) returned status %u\n", st);
        /* negctl: proc-rundown-locks-not-released */
        CHECK((st & 1) && lk.lksb$l_lkid != 0,
              "A: after a fresh-fd subject's exit, EX+NOQUEUE granted with NO table op (SYNCHRONOUS .release rundown)");
        if (st & 1) sys$deq(lk.lksb$l_lkid, NULL, 0, 0);
    }

    /* ============================================================
     * Part B - SHARED fd: the dead subject's lock is held until a reap.
     * The subject keeps the parent's inherited fd, so the parent still
     * references that struct file and .release never fires for the subject.
     * ============================================================ */
    {
        const char *RES = "FF8_SHARED_FD";
        int held = spawn_dead_holder("sharedfd-subject", RES, /*fresh_fd=*/0);
        CHECK(held, "B: shared-fd subject took EX, then died with the parent still holding its /dev/vms channel");

        struct dsc$descriptor_s resnam = mkdsc(RES);

        /* B1: conflicting EX+NOQUEUE with NO intervening table op. */
        struct lksb_caller lk1 = {0};
        uint32_t st1 = sys$enq(0, LCK$K_EXMODE, &lk1, LCK$M_NOQUEUE,
                               &resnam, 0, NULL, 0, NULL, 0, 0);
        printf("  INFO: B1 $ENQ before any reap returned status %u (SS$_NOTQUEUED=%u, SS$_NORMAL=%u)\n",
               st1, (unsigned)SS$_NOTQUEUED, (unsigned)SS$_NORMAL);
        CHECK(st1 == SS$_NOTQUEUED,
              "B1 [vms-ff8]: a shared-fd dead subject's EX lock STILL BLOCKS a conflicting $ENQ before any process-table op reaps it");
        if ((st1 & 1) && lk1.lksb$l_lkid) sys$deq(lk1.lksb$l_lkid, NULL, 0, 0);

        /* Trigger the lazy reaper: a single $GETJPI is a process-table op. */
        struct vms_procinfo info; memset(&info, 0, sizeof(info));
        uint32_t gst = vms_kif_getjpi_self(&info);
        CHECK(gst & 1, "B: $GETJPI (a process-table op) succeeded -- runs the lazy reaper");

        /* B2: retry the conflicting $ENQ; the reap should have freed it. */
        struct lksb_caller lk2 = {0};
        uint32_t st2 = sys$enq(0, LCK$K_EXMODE, &lk2, LCK$M_NOQUEUE,
                               &resnam, 0, NULL, 0, NULL, 0, 0);
        printf("  INFO: B2 $ENQ after a $GETJPI reap returned status %u\n", st2);
        CHECK((st2 & 1) && lk2.lksb$l_lkid != 0,
              "B2: after an unrelated process-table op reaps the dead subject, the SAME $ENQ is granted (held only until the reap)");
        if (st2 & 1) sys$deq(lk2.lksb$l_lkid, NULL, 0, 0);
    }

    printf("=== test_syssvc_rundown_ff8: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
