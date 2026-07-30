/*
 * test_kmod_bind.c - The product actually reaches the executive (vms-9fc)
 *
 * vms_kif.h documented a two-step protocol from the day it was written:
 * open /dev/vms, then vms_kif_register(). vms_kif_register() was defined
 * and had ZERO CALLERS product-wide. src/kernel/vms_module.c routes every
 * ioctl except REGISTER through vms_proc_find_or_err(), which returns
 * -ESRCH for an unregistered task -- so EVERY /dev/vms call OVMX made was
 * rejected, including sys$ENQ/$DEQ, the facility the executive-retrofit
 * design named as "already wired" and told every implementer to copy.
 *
 * A test that opens and registers by hand cannot see that defect: it
 * supplies the very step the product forgets. So NOTHING in this file's
 * positive path calls vms_kif_open() or vms_kif_register() before using a
 * facility. It uses the public entry points exactly the way libvms uses
 * them, against a real /dev/vms, and each property has a minimal mutation
 * that turns it -- and only it -- red:
 *
 *   1 auto-bind        remove the vms_kif_register() call in kif_bind()
 *   2 $ENQ/$DEQ        (same mutation reaches it; see suite 2 note)
 *   3 fork re-bind     kif_bind(): `vms_bound_pid == pid` -> `!= 0`
 *   4 post-exec adopt  vms_module.c: adopt branch -> old 0x1C status
 *   5 adopt keeps privs vms_module.c: adopt branch -> re-apply init_privs
 *   6 errno mapping    vms_kif_kerr_to_ss(): any one arm
 *   7 one PCB/process  vms_module.c: current->tgid -> current->pid
 *
 * Status values are ORACLE-PINNED on the reference lab node VAX1, OpenVMS
 * VAX V7.3 (2026-07-30), by two independent documented-tool observations:
 *
 *   LIBRARY/EXTRACT=$SSDEF/OUTPUT=... SYS$LIBRARY:STARLET.MLB + SEARCH
 *       $EQU  SS$_NORMAL       1      $EQU  SS$_ACCVIO      12
 *       $EQU  SS$_BADPARAM    20      $EQU  SS$_ILLIOFUNC  244
 *       $EQU  SS$_INSFMEM    292      $EQU  SS$_BUGCHECK   676
 *   F$MESSAGE round-trip
 *       1   -> %SYSTEM-S-NORMAL,    normal successful completion
 *       12  -> %SYSTEM-F-ACCVIO,    access violation, ...
 *       20  -> %SYSTEM-F-BADPARAM,  bad parameter value
 *       244 -> %SYSTEM-F-ILLIOFUNC, illegal I/O function code
 *       292 -> %SYSTEM-F-INSFMEM,   insufficient dynamic memory
 *       676 -> %SYSTEM-F-BUGCHECK,  internal consistency failure
 *
 * NOTE 244: the in-tree SS$_ILLIOFUNC was 580, which the same oracle run
 * shows is %SYSTEM-F-VASFULL. Corrected in ssdef.h and vms_errno.h.
 *
 * Modes:
 *   (no args)                 the test
 *   --reexec <pipe-wfd>       the freshly activated image (suites 4/5)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "vms_kif.h"

/* Oracle-pinned; see the file header. */
#define SS_NORMAL       1
#define SS_WASCLR       5
#define SS_WASSET       9
#define SS_ACCVIO       12
#define SS_BADPARAM     20
#define SS_ILLIOFUNC    244
#define SS_INSFMEM      292
#define SS_BUGCHECK     676

/* Local cluster 1 (EFN 32-63). Deliberately NOT a common-cluster flag:
 * EFN 64+ answers SS$_UNASEFC until $ASCEFC associates the cluster, which
 * would put a second, unrelated reason for failure in front of the thing
 * these suites are measuring. */
#define BIND_EFN        40
#define BIND_RESNAM     "VMS$BINDPROBE"
#define REEXEC_NAME     "VMS$REBOUND"

/* Suite 7 uses its own flag, resource and name so that a failure there
 * cannot be a leftover of an earlier suite's state. */
#define THREAD_EFN      41
#define THREAD_RESNAM   "VMS$THREADPROBE"
#define THREAD_NAME     "VMS$THREADED"

/*
 * A privilege mask with no meaning other than "not the default, and not
 * the one the second registration asks for". Suite 5 only cares that the
 * value the executive holds does not follow the second request.
 */
#define PRIV_FIRST      0x0000000000000042ULL
#define PRIV_SECOND     0x0000000000005500ULL

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What the forked child reports back. */
struct fork_report {
    uint32_t getjpi_status;
    uint32_t linux_pid;
    uint32_t setef_status;
};

/* What the re-execed image reports back. */
struct exec_report {
    uint32_t register_status;   /* REGISTER issued by a process that has an
                                 * executive entry already -- the adopt case */
    uint32_t getjpi_status;
    uint32_t linux_pid;
    uint64_t cur_privs;         /* privileges AFTER the second REGISTER */
    char     prcnam[VMS_PRCNAM_SIZE];
};

/* ================================================================
 * The re-executed image (suites 4 and 5)
 *
 * This image was never registered. Its predecessor was, in the same
 * process, and closed the channel before exec. It re-issues REGISTER --
 * which on VMS terms is a second image activation inside one process, and
 * so must ADOPT the surviving entry, not fail and not reset it.
 * ================================================================ */
static int reexeced_image(int wfd)
{
    struct exec_report rep;
    struct vms_procinfo info;
    uint8_t mode = 0;
    uint64_t cur = 0, perm = 0;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0) {
        rep.register_status = 0;   /* 0 is not a VMS status: never got there */
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }

    /* The adopt case, asking for DIFFERENT privileges than the process
     * already holds. Adoption must ignore the request. */
    rep.register_status = vms_kif_register((uint32_t)getpid(), PRIV_SECOND);

    memset(&info, 0, sizeof(info));
    rep.getjpi_status = vms_kif_getjpi_self(&info);
    rep.linux_pid = info.linux_pid;
    memcpy(rep.prcnam, info.prcnam, VMS_PRCNAM_SIZE);

    if (vms_kif_getmode(&mode, &cur, &perm) == SS_NORMAL)
        rep.cur_privs = cur;

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        return 1;
    close(wfd);
    return 0;
}

/* ================================================================
 * The forked child (suite 3)
 *
 * It inherited the parent's thread-local state wholesale: the parent's
 * /dev/vms descriptor number AND the parent's "bound" mark. The executive
 * keys its process table by process, and a fork makes a new one, so the
 * child is NOT registered no matter what its TLS claims. It calls ONE
 * public entry point and nothing else.
 * ================================================================ */
static void forked_child(int wfd)
{
    struct fork_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    rep.setef_status = vms_kif_setef(BIND_EFN);

    memset(&info, 0, sizeof(info));
    rep.getjpi_status = vms_kif_getjpi_self(&info);
    rep.linux_pid = info.linux_pid;

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        _exit(70);
    close(wfd);
    _exit(0);
}

/* ================================================================
 * The sibling thread (suite 7)
 *
 * On OpenVMS a process has ONE PCB and its kernel threads SHARE it.
 * That shared residency is the entire meaning of a process-wide event
 * flag cluster, a process name and a process's lock ids -- two threads
 * of one image cannot disagree about who they are.
 *
 * This thread therefore uses the public entry points and nothing else
 * (no explicit open, no explicit register: kif_bind runs inside it) and
 * reports what the executive tells it. Every value it reports was
 * established by the MAIN thread before this one existed.
 * ================================================================ */
struct thread_report {
    uint32_t tid;               /* this thread's own Linux task id */
    uint32_t getjpi_status;
    uint32_t linux_pid;         /* whose entry the executive handed back */
    uint32_t readef_status;
    uint32_t readef_state;
    uint32_t deq_status;        /* releasing a lock the MAIN thread took */
    char     prcnam[VMS_PRCNAM_SIZE];
};

static uint32_t thread_lkid;            /* lock id taken by the main thread */
static struct thread_report trep;

static void *sibling_thread(void *arg)
{
    struct vms_procinfo info;

    (void)arg;

    trep.tid = (uint32_t)syscall(SYS_gettid);

    memset(&info, 0, sizeof(info));
    trep.getjpi_status = vms_kif_getjpi_self(&info);
    trep.linux_pid = info.linux_pid;
    memcpy(trep.prcnam, info.prcnam, VMS_PRCNAM_SIZE);

    trep.readef_status = vms_kif_readef(THREAD_EFN, &trep.readef_state);
    trep.deq_status = vms_kif_deq(thread_lkid, NULL, 0);

    return NULL;
}

/* ================================================================
 * Suite 6 helper: provoke a REAL errno from the REAL module.
 *
 * The inputs to the mapping are not invented here -- each one is what
 * vms.ko actually returned to a raw ioctl on a raw descriptor. The raw
 * descriptor is deliberate: it is the only way to reach the failure
 * modes, because the bound client no longer produces them.
 * ================================================================ */
static int raw_ioctl_errno(unsigned long req, void *arg, int reg)
{
    int fd = open("/dev/vms", O_RDWR);
    int rc;

    if (fd < 0)
        return 0;

    if (reg) {
        struct vms_register_args ra;
        memset(&ra, 0, sizeof(ra));
        ra.vms_pid = (uint32_t)getpid();
        if (ioctl(fd, VMS_IOCTL_REGISTER, &ra) < 0) {
            close(fd);
            return 0;
        }
    }

    errno = 0;
    rc = ioctl(fd, req, arg);
    rc = (rc < 0) ? errno : 0;
    close(fd);
    return rc;
}

/*
 * unregistered_task_errno - what vms.ko answers a task it has no entry for.
 *
 * Forked, because the parent has an entry by now and the executive's
 * lookup is by task. The child touches NO vms_kif_* function -- calling
 * one would bind it and destroy the condition being measured.
 */
static int unregistered_task_errno(void)
{
    int p[2];
    pid_t c;
    int e = 0;

    if (pipe(p) < 0)
        return 0;

    c = fork();
    if (c < 0) {
        close(p[0]);
        close(p[1]);
        return 0;
    }
    if (c == 0) {
        struct vms_ef_args efa;
        int ce;

        close(p[0]);
        memset(&efa, 0, sizeof(efa));
        efa.efn = BIND_EFN;
        ce = raw_ioctl_errno(VMS_IOCTL_SETEF, &efa, 0);
        (void)!write(p[1], &ce, sizeof(ce));
        _exit(0);
    }
    close(p[1]);
    if (read(p[0], &e, sizeof(e)) != (ssize_t)sizeof(e))
        e = 0;
    close(p[0]);
    waitpid(c, NULL, 0);
    return e;
}

int main(int argc, char **argv)
{
    int pipefd[2];
    pid_t child;
    uint32_t status, state;
    uint32_t lkid = 0;
    struct vms_procinfo info;
    uint8_t mode = 0;
    uint64_t cur = 0, perm = 0;
    char wfd_arg[16];

    if (argc >= 3 && strcmp(argv[1], "--reexec") == 0)
        return reexeced_image(atoi(argv[2]));

    printf("=== Executive binding (vms-9fc) ===\n");

    if (access("/dev/vms", F_OK) != 0) {
        printf("  FAIL: /dev/vms absent (executive not loaded)\n");
        printf("=== RESULTS: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* ------------------------------------------------------------
     * SUITE 1 -- AUTO-BIND.
     *
     * No vms_kif_open(). No vms_kif_register(). The first thing this
     * process ever does to the executive is use a facility, exactly as
     * libvms does. Before this change every one of these returned
     * SS$_BADPARAM, because the ioctl was rejected with -ESRCH and every
     * errno was reported as "your parameters were bad".
     * ------------------------------------------------------------ */
    printf("--- 1. auto-bind: a facility used with no explicit register ---\n");

    status = vms_kif_setef(BIND_EFN);
    CHECK(status == SS_WASCLR || status == SS_WASSET,
          "$SETEF reaches the executive with no explicit register");
    CHECK(status != SS_BADPARAM,
          "$SETEF does not report the caller's parameters as bad");

    state = 0;
    status = vms_kif_readef(BIND_EFN, &state);
    CHECK(status == SS_WASSET, "$READEF sees the flag this process just set");
    CHECK((state & (1u << (BIND_EFN % 32))) != 0,
          "cluster state carries the flag bit");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL, "$GETJPI(self) resolves the auto-bound process");
    CHECK(info.linux_pid == (uint32_t)getpid(),
          "the executive entry is this process's own");

    /* ------------------------------------------------------------
     * SUITE 2 -- sys$ENQ / sys$DEQ.
     *
     * The item's done-condition. src/libvms/syssvc/sys_lock.c calls
     * vms_kif_enq()/vms_kif_deq() and adds only status translation on top,
     * and libvms cannot yet be built into this initramfs (vms-1d9), so the
     * ioctl path is exercised here at the layer sys_lock.c calls. Same
     * auto-bind rule: nothing registers first.
     * ------------------------------------------------------------ */
    printf("--- 2. lock manager reached through the same auto-bind ---\n");

    status = vms_kif_enq(0, 5 /* LCK$K_EXMODE */, 0, BIND_RESNAM, 0,
                         0, 0, 0, &lkid, NULL);
    CHECK(status == SS_NORMAL, "$ENQ EX granted through /dev/vms");
    CHECK(lkid != 0, "$ENQ returned a lock id");

    status = vms_kif_deq(lkid, NULL, 0);
    CHECK(status == SS_NORMAL, "$DEQ released the lock");

    /* ------------------------------------------------------------
     * SUITE 3 -- fork().
     *
     * The child's thread-local state says "already bound" and names a
     * descriptor it inherited. Both are lies about the child. If the bind
     * is guarded by a plain boolean the child never registers and every
     * call it makes is rejected forever.
     * ------------------------------------------------------------ */
    printf("--- 3. a forked child binds as itself ---\n");

    if (pipe(pipefd) < 0) {
        printf("  FAIL: pipe()\n");
        fail++;
        goto done;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        close(pipefd[0]);
        close(pipefd[1]);
        goto done;
    }
    if (child == 0) {
        close(pipefd[0]);
        forked_child(pipefd[1]);
        _exit(71);
    }
    close(pipefd[1]);

    {
        struct fork_report frep;
        ssize_t n;

        memset(&frep, 0, sizeof(frep));
        n = read(pipefd[0], &frep, sizeof(frep));
        close(pipefd[0]);
        waitpid(child, NULL, 0);

        CHECK(n == (ssize_t)sizeof(frep), "forked child reported back");
        CHECK(frep.setef_status == SS_WASCLR || frep.setef_status == SS_WASSET,
              "forked child's $SETEF reaches the executive");
        CHECK(frep.setef_status != SS_BUGCHECK,
              "forked child is not rejected as an unregistered task");
        CHECK(frep.getjpi_status == SS_NORMAL,
              "forked child resolves itself in the process table");
        CHECK(frep.linux_pid == (uint32_t)child,
              "child's executive entry is the CHILD's, not the parent's");
    }

    /* ------------------------------------------------------------
     * SUITES 4 and 5 -- execve(), and what REGISTER does afterwards.
     *
     * ORACLE PIN (VAX1, OpenVMS VAX V7.3, 2026-07-30): activating an image
     * inside an existing process does not recreate the process and is not
     * an error. SHOW PROCESS/ACCOUNTING across two more image activations:
     *     Process ID: 2020021D  name "SYSTEM"  Images activated: 19
     *     Process ID: 2020021D  name "SYSTEM"  Images activated: 21
     * So a second REGISTER must ADOPT: SS$_NORMAL, same identity, and --
     * because an image activation never changes a process's privileges --
     * the privilege mask must NOT follow the second request.
     * ------------------------------------------------------------ */
    printf("--- 4/5. post-exec REGISTER adopts the surviving process ---\n");

    /* Give this process an identity and a privilege mask to preserve. The
     * explicit register here is the SUBJECT of the test, not a setup step
     * the product would otherwise need: it is what establishes the
     * privileges that adoption must leave alone. */
    status = vms_kif_register((uint32_t)getpid(), PRIV_FIRST);
    CHECK(status == SS_NORMAL,
          "re-registering an already-bound process is accepted (adopt)");

    status = vms_kif_setprn(REEXEC_NAME);
    CHECK(status == SS_NORMAL, "process named before image activation");

    if (vms_kif_getmode(&mode, &cur, &perm) == SS_NORMAL)
        CHECK(cur == 0 || cur == PRIV_FIRST,
              "privileges after adoption are not the second request");

    if (pipe(pipefd) < 0) {
        printf("  FAIL: pipe()\n");
        fail++;
        goto done;
    }

    child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        close(pipefd[0]);
        close(pipefd[1]);
        goto done;
    }
    if (child == 0) {
        close(pipefd[0]);

        /* Establish the pre-exec identity in the CHILD's own executive
         * entry, then throw the image away. Nothing is carried across:
         * the channel is closed, no environment variable, no descriptor. */
        if (vms_kif_register((uint32_t)getpid(), PRIV_FIRST) != SS_NORMAL)
            _exit(72);
        if (vms_kif_setprn(REEXEC_NAME "2") != SS_NORMAL)
            _exit(73);
        vms_kif_close();

        snprintf(wfd_arg, sizeof(wfd_arg), "%d", pipefd[1]);
        execl(argv[0], argv[0], "--reexec", wfd_arg, (char *)NULL);
        _exit(74);
    }
    close(pipefd[1]);

    {
        struct exec_report erep;
        ssize_t n;

        memset(&erep, 0, sizeof(erep));
        n = read(pipefd[0], &erep, sizeof(erep));
        close(pipefd[0]);
        waitpid(child, NULL, 0);

        CHECK(n == (ssize_t)sizeof(erep), "re-execed image reported back");
        CHECK(erep.register_status == SS_NORMAL,
              "REGISTER after execve() adopts the process (SS$_NORMAL)");
        CHECK(erep.register_status != 0x1C,
              "REGISTER after execve() is not reported as a duplicate");
        CHECK(erep.getjpi_status == SS_NORMAL,
              "adopted process still resolves in the process table");
        CHECK(erep.linux_pid == (uint32_t)child,
              "adoption kept the same executive entry, not a new one");
        CHECK(strcmp(erep.prcnam, REEXEC_NAME "2") == 0,
              "adoption did not reset the process name");
        CHECK(erep.cur_privs != PRIV_SECOND,
              "adoption did not let the image re-declare its privileges");
    }

    /* ------------------------------------------------------------
     * SUITE 6 -- honest statuses for real kernel errnos.
     *
     * Every failure used to be SS$_BADPARAM, i.e. "your parameters were
     * bad", said to callers whose parameters were fine. The errnos below
     * are not invented: each is what vms.ko actually returned to a raw
     * ioctl issued here, and the mapping is then checked against it.
     * ------------------------------------------------------------ */
    printf("--- 6. ioctl failures get honest statuses ---\n");

    {
        struct vms_ef_args efa;
        int e;

        /* -ENOTTY: a command number the module does not implement. */
        memset(&efa, 0, sizeof(efa));
        e = raw_ioctl_errno(_IOWR(VMS_IOC_MAGIC, 0x7E, struct vms_ef_args),
                            &efa, 1);
        CHECK(e == ENOTTY, "module rejects an unknown ioctl with -ENOTTY");
        CHECK(vms_kif_kerr_to_ss(-e) == SS_ILLIOFUNC,
              "-ENOTTY -> SS$_ILLIOFUNC (oracle 244), not SS$_BADPARAM");

        /* -ESRCH: the module's own answer to an unregistered task. This
         * is the condition auto-bind makes unreachable for the product;
         * it is provoked here only to show what it maps to if the
         * executive ever loses a PCB it must hold.
         *
         * It has to be provoked in a FRESH PROCESS. The executive keys
         * its process table by process, not by channel, so a raw second
         * descriptor opened by THIS process reaches this process's entry
         * and the ioctl simply succeeds -- which is exactly what the
         * first version of this check measured, and why it read green on
         * a claim it was not testing. A second THREAD would not do
         * either: it shares this process's PCB by design (suite 7). */
        e = unregistered_task_errno();
        CHECK(e == ESRCH, "module rejects an unregistered task with -ESRCH");
        CHECK(vms_kif_kerr_to_ss(-e) == SS_BUGCHECK,
              "-ESRCH -> SS$_BUGCHECK (oracle 676), not SS$_BADPARAM");

        /* -EFAULT: a pointer the module cannot copy from. */
        e = raw_ioctl_errno(VMS_IOCTL_SETEF, (void *)(unsigned long)0x1, 1);
        CHECK(e == EFAULT, "module rejects an unreadable argument with -EFAULT");
        CHECK(vms_kif_kerr_to_ss(-e) == SS_ACCVIO,
              "-EFAULT -> SS$_ACCVIO (oracle 12), not SS$_BADPARAM");

        /* -ENOMEM cannot be provoked on demand -- allocation failure is not
         * a caller-reachable state -- so only the mapping is checked. */
        CHECK(vms_kif_kerr_to_ss(-ENOMEM) == SS_INSFMEM,
              "-ENOMEM -> SS$_INSFMEM (oracle 292), not SS$_BADPARAM");
    }

    /* ------------------------------------------------------------
     * SUITE 7 -- ONE PCB PER PROCESS, SHARED BY ITS THREADS.
     *
     * On OpenVMS a process has exactly one PCB and its kernel threads
     * share it; the process name, the local event flag clusters and the
     * process's lock ids are all properties of the PROCESS. Keying the
     * executive's table on the Linux TID instead of the thread-group id
     * minted one VMS process per thread -- a sibling thread saw a
     * different linux_pid from getpid(), an empty process name after the
     * main thread had named the process, its event flags clear after the
     * main thread had set them, and could not release the process's own
     * lock. That is Rule 11's facade inverted: per-thread state
     * pretending to be per-process, and NO single-threaded test can see
     * it (suites 1-6 all pass either way, because tgid == pid for a
     * single-threaded process).
     *
     * Minimal mutation for this property and no other:
     *     vms_module.c: current->tgid -> current->pid
     * ------------------------------------------------------------ */
    printf("--- 7. threads of one image share ONE executive process ---\n");

    {
        pthread_t th;

        status = vms_kif_setprn(THREAD_NAME);
        CHECK(status == SS_NORMAL, "main thread names the process");

        status = vms_kif_setef(THREAD_EFN);
        CHECK(status == SS_WASCLR || status == SS_WASSET,
              "main thread sets a local event flag");

        thread_lkid = 0;
        status = vms_kif_enq(0, 5 /* LCK$K_EXMODE */, 0, THREAD_RESNAM, 0,
                             0, 0, 0, &thread_lkid, NULL);
        CHECK(status == SS_NORMAL, "main thread takes a lock");
        CHECK(thread_lkid != 0, "main thread holds a lock id");

        memset(&trep, 0, sizeof(trep));
        if (pthread_create(&th, NULL, sibling_thread, NULL) != 0) {
            printf("  FAIL: pthread_create()\n");
            fail++;
        } else {
            pthread_join(th, NULL);

            /* Guard: if this were not really a second Linux task, every
             * assertion below would be trivially true. */
            CHECK(trep.tid != 0 && trep.tid != (uint32_t)getpid(),
                  "the sibling really is a second Linux task");

            CHECK(trep.getjpi_status == SS_NORMAL,
                  "sibling thread resolves in the process table");
            CHECK(trep.linux_pid == (uint32_t)getpid(),
                  "sibling thread got the PROCESS's entry, not one of its own");
            CHECK(strcmp(trep.prcnam, THREAD_NAME) == 0,
                  "sibling thread sees the process name the main thread set");
            CHECK(trep.readef_status == SS_WASSET,
                  "sibling thread sees the event flag the main thread set");
            CHECK((trep.readef_state & (1u << (THREAD_EFN % 32))) != 0,
                  "sibling thread's cluster state carries the flag bit");
            CHECK(trep.deq_status == SS_NORMAL,
                  "sibling thread can $DEQ the lock the main thread took");
        }
    }

done:
    printf("=== RESULTS: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
