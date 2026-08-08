/*
 * test_kmod_lock_mproc.c - Multi-process lock manager test
 *
 * The kernel lock manager (src/kernel/vms_lock.c) keys both lock
 * ownership and AST delivery off current->pid, and a single-process
 * test (test_kmod_lock.c) can exercise the mode-compatibility matrix
 * but cannot exercise cross-process blocking ASTs, since those require
 * two distinct registered vms_proc structures. This test uses a real
 * fork() to create a second Linux process, each with its own /dev/vms
 * registration, to prove -- via the raw ioctl layer that sys$enq /
 * sys$enqw / sys$deq (src/libvms/syssvc/sys_lock.c) now delegate to --
 * that this behavior actually works across process boundaries:
 *
 *   1. Mode compatibility across processes:
 *        - parent holds EX; child's EX+NOQUEUE and CR+NOQUEUE are both
 *          denied (SS$_NOTQUEUED) -- EX blocks everything.
 *        - on a second resource, parent and child each hold CR
 *          concurrently -- CR+CR is compatible.
 *   2. Blocking ASTs across processes:
 *        - parent's EX lock carries a blkastadr sentinel.
 *        - child's incompatible CR request (queued, not NOQUEUE) causes
 *          the kernel to queue a blocking AST for parent.
 *        - parent calls DELIVERAST and observes the queued AST, with
 *          astadr == the sentinel and astprm == parent's own lock ID.
 *
 * NOTE on scope: this deliberately tests the *kernel* lock manager via
 * the same raw /dev/vms ioctls that src/libvms/syssvc/sys_lock.c's
 * vms_kif_enq/deq/convert wrappers use -- not the public sys$enq/
 * sys$enqw/sys$deq API itself. Testing the public API in QEMU requires
 * libvms (glibc, dynamically linked, depends on vmsprocess/vmsfs/
 * vmslnm) to be present in the initramfs, which needs the "fat
 * initramfs with dynamic binaries" work (bead vms-841) and/or image
 * activation (vms-913) -- neither exists yet. A follow-up bead tracks
 * the public-API QEMU integration test, blocked on those two.
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
/* ssdef.h SS$_NOTQUEUED -- see vms-82a; the private 40 is gone. */
#define SS_NOTQUEUED    2488

#define AST_SENTINEL    0xABCD1234ULL

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Message passed from child to parent over a pipe. */
struct child_msg {
    uint32_t stage;   /* 1 = queued-lock report, 2 = final counts */
    uint32_t lkid;    /* child's queued lock ID (stage 1 only) */
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

/* ================================================================
 * Child process
 * ================================================================ */
static int run_child(int cfd, int c2p_write, int p2c_read)
{
    struct child_msg msg;

    /* --- Phase 1: EX blocks everything (mode compatibility) --- */
    struct vms_enq_args enq_ex_nq = {0};
    enq_ex_nq.lkmode = LCK_K_EXMODE;
    enq_ex_nq.flags = LCK_M_NOQUEUE;
    strncpy(enq_ex_nq.resnam, "MPROCLOCK1", sizeof(enq_ex_nq.resnam));
    ioctl(cfd, VMS_IOCTL_ENQ, &enq_ex_nq);
    CHECK(enq_ex_nq.status == SS_NOTQUEUED,
          "child: EX+NOQUEUE denied while parent holds EX (EX blocks EX)");

    struct vms_enq_args enq_cr_nq = {0};
    enq_cr_nq.lkmode = LCK_K_CRMODE;
    enq_cr_nq.flags = LCK_M_NOQUEUE;
    strncpy(enq_cr_nq.resnam, "MPROCLOCK1", sizeof(enq_cr_nq.resnam));
    ioctl(cfd, VMS_IOCTL_ENQ, &enq_cr_nq);
    /* negctl: lock-compat-cr-ex */
    CHECK(enq_cr_nq.status == SS_NOTQUEUED,
          "child: CR+NOQUEUE denied while parent holds EX (EX blocks CR)");

    /* --- Phase 2: queue a CR request (no NOQUEUE) to trigger a
     * blocking AST on the parent's granted EX lock. --- */
    struct vms_enq_args enq_cr = {0};
    enq_cr.lkmode = LCK_K_CRMODE;
    strncpy(enq_cr.resnam, "MPROCLOCK1", sizeof(enq_cr.resnam));
    ioctl(cfd, VMS_IOCTL_ENQ, &enq_cr);
    CHECK(enq_cr.status == SS_NORMAL && enq_cr.lkid != 0,
          "child: CR request queued behind parent's EX");

    /* CONVERTED (vms-290): was raw ioctl(cfd, VMS_IOCTL_GETLKI, &lki). Now
     * exercises vms_kif_getlki() (src/libvmssys/vms_kif.c), which had zero
     * callers anywhere in the checkout before this. */
    uint32_t granted_mode = 0;
    uint32_t getlki_st = vms_kif_getlki(enq_cr.lkid, &granted_mode, NULL, NULL, NULL);
    /* negctl-knockon: lock-compat-cr-ex */
    CHECK(getlki_st == SS_NORMAL && granted_mode == LCK_K_NLMODE,
          "child: queued CR request still ungranted (NL) per GETLKI");

    /* Report the queued lock to the parent so it can verify the
     * blocking AST and cross-process GETLKI visibility. */
    msg.stage = 1;
    msg.lkid = enq_cr.lkid;
    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        fail++;

    /* Wait for the parent's go-ahead (it has verified the AST). */
    char go;
    if (read(p2c_read, &go, 1) != 1)
        fail++;

    /* Clean up the queued request. */
    struct vms_deq_args deq = {0};
    deq.lkid = enq_cr.lkid;
    ioctl(cfd, VMS_IOCTL_DEQ, &deq);
    CHECK(deq.status == SS_NORMAL, "child: DEQ queued CR request");

    /* --- Phase 3: CR+CR compatibility across processes --- */
    struct vms_enq_args enq_cr2 = {0};
    enq_cr2.lkmode = LCK_K_CRMODE;
    strncpy(enq_cr2.resnam, "MPROCLOCK2", sizeof(enq_cr2.resnam));
    ioctl(cfd, VMS_IOCTL_ENQ, &enq_cr2);
    CHECK(enq_cr2.status == SS_NORMAL,
          "child: CR granted on MPROCLOCK2 (own request)");

    msg.stage = 2;
    msg.lkid = 0;
    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        fail++;

    /* Wait for parent's second go-ahead (after it confirms its own CR
     * grant on MPROCLOCK2 is concurrent with this one), then clean up. */
    if (read(p2c_read, &go, 1) != 1)
        fail++;

    struct vms_deq_args deq2 = {0};
    deq2.lkid = enq_cr2.lkid;
    ioctl(cfd, VMS_IOCTL_DEQ, &deq2);

    close(cfd);
    return fail > 0 ? 1 : 0;
}

/* ================================================================
 * Parent process (drives the test, prints combined results)
 * ================================================================ */
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so an unflushed fork() cannot splice output */
    printf("=== test_kmod_lock_mproc ===\n");

    int pfd = open_and_register();
    CHECK(pfd >= 0, "parent: open + register /dev/vms");
    if (pfd < 0) {
        printf("=== test_kmod_lock_mproc: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* Parent takes EX on MPROCLOCK1 with a blocking-AST sentinel. */
    struct vms_enq_args enq_ex = {0};
    enq_ex.lkmode = LCK_K_EXMODE;
    enq_ex.blkastadr = AST_SENTINEL;
    strncpy(enq_ex.resnam, "MPROCLOCK1", sizeof(enq_ex.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &enq_ex);
    CHECK(enq_ex.status == SS_NORMAL && enq_ex.lkid != 0,
          "parent: EX granted on MPROCLOCK1 with blkastadr set");
    uint32_t parent_lkid = enq_ex.lkid;

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
        /* Child process */
        close(c2p[0]);
        close(p2c[1]);
        close(pfd);

        int cfd = open_and_register();
        if (cfd < 0) {
            printf("  FAIL: child register\n");
            _exit(1);
        }
        int rc = run_child(cfd, c2p[1], p2c[0]);
        close(c2p[1]);
        close(p2c[0]);
        _exit(rc);
    }

    /* Parent process continues here */
    close(c2p[1]);
    close(p2c[0]);

    /* --- Receive child's stage-1 report (queued CR request) --- */
    struct child_msg msg = {0};
    ssize_t n = read(c2p[0], &msg, sizeof(msg));
    CHECK(n == (ssize_t)sizeof(msg) && msg.stage == 1 && msg.lkid != 0,
          "parent: received child's queued-lock report");

    /*
     * Cross-process GETLKI: parent can query the child's lock ID.
     *
     * CONVERTED (vms-290): both calls below used to be raw ioctls
     * (VMS_IOCTL_GETLKI, VMS_IOCTL_DELIVERAST) against `pfd`. They now go
     * through vms_kif_getlki()/vms_kif_deliverast() (src/libvmssys/
     * vms_kif.c), which had zero callers anywhere in the checkout before
     * this.
     */
    uint32_t p_granted_mode = 0, p_requested_mode = 0;
    uint32_t getlki_st = vms_kif_getlki(msg.lkid, &p_granted_mode,
                                         &p_requested_mode, NULL, NULL);
    /* negctl-knockon: lock-compat-cr-ex */
    CHECK(getlki_st == SS_NORMAL && p_granted_mode == LCK_K_NLMODE
              && p_requested_mode == LCK_K_CRMODE,
          "parent: cross-process GETLKI sees child's queued CR request");

    /* --- Blocking AST: parent should now have a queued AST from the
     * kernel notifying it that child's request is blocked on its EX
     * lock. --- */
    uint64_t d_astadr = 0, d_astprm = 0;
    uint8_t d_acmode = 0;
    int ar = vms_kif_deliverast(&d_astadr, &d_astprm, &d_acmode);
    /* negctl-knockon: lock-compat-cr-ex */
    CHECK(ar == 0 && d_astadr == AST_SENTINEL && d_astprm == parent_lkid,
          "parent: received blocking AST (astadr=sentinel, astprm=own lkid)");

    /* Let the child dequeue its blocked request. */
    char go = 'g';
    if (write(p2c[1], &go, 1) != 1)
        fail++;

    /* --- Receive child's stage-2 report and do the CR+CR check --- */
    n = read(c2p[0], &msg, sizeof(msg));
    CHECK(n == (ssize_t)sizeof(msg) && msg.stage == 2,
          "parent: received child's stage-2 report");

    struct vms_enq_args enq_cr2 = {0};
    enq_cr2.lkmode = LCK_K_CRMODE;
    strncpy(enq_cr2.resnam, "MPROCLOCK2", sizeof(enq_cr2.resnam));
    ioctl(pfd, VMS_IOCTL_ENQ, &enq_cr2);
    CHECK(enq_cr2.status == SS_NORMAL,
          "parent: CR granted on MPROCLOCK2 concurrently with child's CR (CR+CR compatible)");

    if (write(p2c[1], &go, 1) != 1)
        fail++;

    int wstatus = 0;
    waitpid(child_pid, &wstatus, 0);

    /* Clean up parent's own locks. */
    struct vms_deq_args deq_ex = {0};
    deq_ex.lkid = parent_lkid;
    ioctl(pfd, VMS_IOCTL_DEQ, &deq_ex);
    CHECK(deq_ex.status == SS_NORMAL, "parent: DEQ own EX lock");

    struct vms_deq_args deq_cr2 = {0};
    deq_cr2.lkid = enq_cr2.lkid;
    ioctl(pfd, VMS_IOCTL_DEQ, &deq_cr2);

    close(pfd);
    close(c2p[0]);
    close(p2c[1]);

    int total_pass = pass + (int)msg.pass;
    int total_fail = fail + (int)msg.fail;
    int child_exit_failed = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
    if (child_exit_failed)
        total_fail++;

    printf("=== test_kmod_lock_mproc: %d passed, %d failed ===\n",
           total_pass, total_fail);
    return total_fail > 0 ? 1 : 0;
}
