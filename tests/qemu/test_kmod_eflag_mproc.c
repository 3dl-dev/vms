/*
 * test_kmod_eflag_mproc.c - Multi-process common event-flag-cluster test
 *
 * vms-ef1 (executive retrofit Phase 1): userspace never called the
 * common event-flag-cluster ioctls (0x20-0x27 in src/kernel/vms_ioctl.h),
 * so src/libvms/syssvc/sys_event.c stored ALL flags (including the
 * "common" range 64-127) in the per-process PCB -- two OVMX processes
 * could never actually share a cluster. This test uses a real fork()
 * to create a second Linux process, each with its own /dev/vms
 * registration, and drives the RAW ioctl layer that
 * src/libvms/syssvc/sys_event.c's vms_kif_ascefc/setef/waitfr/readef/
 * dacefc wrappers now call, to prove -- against the real kernel
 * module, not a per-process fake -- that:
 *
 *   1. Before associating, a common-range flag is unusable
 *      (SS__UNASEFC) -- the kernel refuses to let an unassociated
 *      process touch cluster state.
 *   2. Two independently-registered processes that ASCEFC the same
 *      cluster NAME see the SAME kernel-side flag state: a flag the
 *      parent sets before the child even associates is visible to the
 *      child immediately on read (proves it is shared kernel state,
 *      not a coincidence of local initialization).
 *   3. A child genuinely BLOCKED in WAITFR on a still-clear common
 *      flag is woken when the PARENT (a different process) sets that
 *      flag -- the actual cross-process wake, not a level check that
 *      happened to already be true.
 *   4. DACEFC has a real effect: after the child disassociates, the
 *      same efn is unusable again for the CHILD (SS__UNASEFC) -- but
 *      the PARENT's association (and the cluster) survive, proving
 *      DACEFC/refcounting only tears down the calling process's own
 *      membership, not the cluster itself.
 *
 * NOTE on scope (same rationale as test_kmod_lock_mproc.c): this tests
 * the *kernel* common-EF-cluster mechanism via the same raw /dev/vms
 * ioctls that sys_event.c's vms_kif_* wrappers use -- not the public
 * sys$setef/sys$waitfr/sys$ascefc API itself. Testing the public API
 * in QEMU requires libvms (glibc, dynamically linked, depends on
 * vmsprocess/vmsfs/vmslnm) to be present in the initramfs, which is
 * blocked on the same fat-initramfs / image-activation work noted by
 * test_kmod_lock_mproc.c (vms-841 / vms-913). The host-side ctest
 * conformance test (tests/conformance/vms_programs/test_event_flags.c)
 * and the vmsprocess unit test only exercise LOCAL flags (0-63), which
 * never leave the PCB and are unaffected by this change -- so they do
 * not, and cannot, cover the common-cluster path this test proves.
 *
 * KNOWN RACE WINDOW (documented, not hidden): step 3 above depends on
 * the child actually being inside the blocking WAITFR ioctl before the
 * parent calls SETEF. The child signals "about to wait" over a pipe
 * immediately before issuing the ioctl, and the parent waits for that
 * message plus a short settle delay before setting the flag. This is
 * the same practical synchronization pattern used elsewhere for
 * kernel-blocking tests; it is not airtight against a pathologically
 * delayed scheduler, but is standard practice and was observed stable
 * across repeated local runs. If it ever flakes in CI, that is a
 * test-timing defect to fix (widen the settle delay or add a kernel-
 * visible "waiter count"), not evidence the underlying mechanism is
 * broken -- the OTHER three checks (1, 2, 4) do not depend on this
 * window at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include "vms_ioctl.h"

/* Raw kernel SS__xxx values (src/kernel/vms_internal.h) -- NOT the
 * public ssdef.h SS$_xxx numbering (sys_event.c's ef_kstat_to_ss()
 * translates between the two at the userspace/public-API boundary;
 * this test operates below that boundary, at the raw ioctl layer,
 * same as test_kmod_eflag.c and test_kmod_lock_mproc.c). */
#define SS_NORMAL    1
#define SS_WASSET    9
#define SS_WASCLR    5
#define SS_UNASEFC   48

#define CLUSTER_NAME "MPROCEFLAG1"
#define EFN_COMMON_BASE 64   /* cluster index 0 (efn 64-95) */
#define EFN_CHILD_SETS  70   /* bit 6 of the common cluster */
#define EFN_PARENT_SETS 71   /* bit 7 of the common cluster */

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct child_msg {
    uint32_t stage;   /* 1 = about to block in WAITFR, 2 = final report */
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
    reg.init_privs = 0xFFFFFFFFFFFFFFFFULL;
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    if (reg.status != SS_NORMAL) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t do_setef(int fd, uint32_t efn)
{
    struct vms_ef_args args = {0};
    args.efn = efn;
    ioctl(fd, VMS_IOCTL_SETEF, &args);
    return args.status;
}

static uint32_t do_readef(int fd, uint32_t efn, uint32_t *state)
{
    struct vms_ef_read_args args = {0};
    args.efn = efn;
    ioctl(fd, VMS_IOCTL_READEF, &args);
    if (state) *state = args.state;
    return args.status;
}

static uint32_t do_waitfr(int fd, uint32_t efn)
{
    struct vms_ef_args args = {0};
    args.efn = efn;
    ioctl(fd, VMS_IOCTL_WAITFR, &args);
    return args.status;
}

static uint32_t do_ascefc(int fd, uint32_t efn, const char *name)
{
    struct vms_ef_common_args args = {0};
    args.efn = efn;
    strncpy(args.name, name, sizeof(args.name) - 1);
    ioctl(fd, VMS_IOCTL_ASCEFC, &args);
    return args.status;
}

static uint32_t do_dacefc(int fd, uint32_t efn)
{
    struct vms_ef_args args = {0};
    args.efn = efn;
    ioctl(fd, VMS_IOCTL_DACEFC, &args);
    return args.status;
}

/* ================================================================
 * Child process
 * ================================================================ */
static int run_child(int cfd, int c2p_write)
{
    struct child_msg msg;

    /* --- Join the cluster the parent already created --- */
    uint32_t st = do_ascefc(cfd, EFN_COMMON_BASE, CLUSTER_NAME);
    CHECK(st == SS_NORMAL,
          "child: ASCEFC joins the cluster the parent already created");

    /* --- Cross-process visibility: parent set EFN_PARENT_SETS (71)
     * before this child even associated. If the child sees it, the
     * state came from the KERNEL's shared cluster, not from any local
     * initialization the child could have done on its own. --- */
    uint32_t state = 0;
    st = do_readef(cfd, EFN_PARENT_SETS, &state);
    CHECK(st == SS_WASSET,
          "child: sees flag 71 (set by parent pre-association) -- shared kernel state");

    /* --- Tell the parent we're about to block, then immediately
     * issue the blocking WAITFR (see file banner: documented race
     * window between the write() returning and the ioctl() actually
     * parking the task). --- */
    msg.stage = 1;
    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        fail++;

    st = do_waitfr(cfd, EFN_CHILD_SETS);
    CHECK(st == SS_NORMAL,
          "child: WAITFR(70) returns after being woken by parent's SETEF");

    st = do_readef(cfd, EFN_CHILD_SETS, &state);
    CHECK(st == SS_WASSET, "child: READEF(70) confirms flag is set post-wake");

    /* --- DACEFC has a real effect: disassociate, then prove the efn
     * is unusable again for THIS process. --- */
    st = do_dacefc(cfd, EFN_COMMON_BASE);
    CHECK(st == SS_NORMAL, "child: DACEFC disassociates from the cluster");

    st = do_setef(cfd, EFN_CHILD_SETS);
    CHECK(st == SS_UNASEFC,
          "child: SETEF(70) fails SS__UNASEFC after DACEFC (real effect, not a no-op)");

    msg.stage = 2;
    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        fail++;

    close(cfd);
    return fail > 0 ? 1 : 0;
}

/* ================================================================
 * Parent process (drives the test, prints combined results)
 * ================================================================ */
int main(void)
{
    printf("=== test_kmod_eflag_mproc ===\n");

    int pfd = open_and_register();
    CHECK(pfd >= 0, "parent: open + register /dev/vms");
    if (pfd < 0) {
        printf("=== test_kmod_eflag_mproc: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* --- Negative check BEFORE associating: the common range is
     * unusable until ASCEFC. --- */
    uint32_t st = do_setef(pfd, EFN_CHILD_SETS);
    CHECK(st == SS_UNASEFC,
          "parent: SETEF(70) fails SS__UNASEFC before ASCEFC (kernel enforces association)");

    /* --- Create the named common cluster --- */
    st = do_ascefc(pfd, EFN_COMMON_BASE, CLUSTER_NAME);
    CHECK(st == SS_NORMAL, "parent: ASCEFC creates common cluster 'MPROCEFLAG1'");

    /* Set a flag the child will observe as already-set at ASCEFC time
     * (proves the "already set" persistence path, distinct from the
     * "blocked, then woken" path exercised via EFN_CHILD_SETS below). */
    st = do_setef(pfd, EFN_PARENT_SETS);
    CHECK(st == SS_WASCLR, "parent: SETEF(71) before child associates");

    int c2p[2];
    if (pipe(c2p) < 0) {
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
        close(pfd);

        int cfd = open_and_register();
        if (cfd < 0) {
            printf("  FAIL: child register\n");
            _exit(1);
        }
        int rc = run_child(cfd, c2p[1]);
        close(c2p[1]);
        _exit(rc);
    }

    /* Parent continues */
    close(c2p[1]);

    /* --- Receive child's stage-1 report (about to block in WAITFR) --- */
    struct child_msg msg = {0};
    ssize_t n = read(c2p[0], &msg, sizeof(msg));
    CHECK(n == (ssize_t)sizeof(msg) && msg.stage == 1,
          "parent: received child's pre-wait report");

    /* Settle delay -- see file banner's documented race-window note. */
    usleep(150000);

    /* --- The actual cross-process wake: child is (almost certainly)
     * now blocked in WAITFR(70); this SETEF must wake it. --- */
    st = do_setef(pfd, EFN_CHILD_SETS);
    CHECK(st == SS_WASCLR,
          "parent: SETEF(70) wakes child blocked in WAITFR (cross-process wake)");

    /* --- Receive child's stage-2 report (post-wake + DACEFC checks) --- */
    n = read(c2p[0], &msg, sizeof(msg));
    CHECK(n == (ssize_t)sizeof(msg) && msg.stage == 2,
          "parent: received child's final report");

    /* --- Refcounting: child's DACEFC must NOT have torn down the
     * cluster out from under the parent's own association. --- */
    uint32_t state = 0;
    st = do_readef(pfd, EFN_CHILD_SETS, &state);
    CHECK(st == SS_WASSET,
          "parent: still associated and sees flag 70 after child's DACEFC (cluster survives)");

    st = do_setef(pfd, EFN_CHILD_SETS);
    CHECK(st == SS_WASSET,
          "parent: SETEF(70) still works after child's DACEFC (parent's membership intact)");

    int wstatus = 0;
    waitpid(child_pid, &wstatus, 0);
    int child_exit_failed = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;

    /* Clean up parent's own association. */
    do_dacefc(pfd, EFN_COMMON_BASE);
    close(pfd);
    close(c2p[0]);

    int total_pass = pass + (int)msg.pass;
    int total_fail = fail + (int)msg.fail;
    if (child_exit_failed)
        total_fail++;

    printf("=== test_kmod_eflag_mproc: %d passed, %d failed ===\n",
           total_pass, total_fail);
    return total_fail > 0 ? 1 : 0;
}
