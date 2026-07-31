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
 * supplies the very step the product forgets. This file's positive path
 * uses the public entry points exactly the way libvms uses them, against
 * a real /dev/vms, WITHOUT calling vms_kif_open() or vms_kif_register()
 * by hand first to reach the facility under test -- except where the
 * manual call IS the subject (suite 0's bare-setident probe below, and
 * suites 4/5's post-exec register-adopt, each explained at its own
 * definition, not here). Each property below has a minimal mutation
 * described at its own definition:
 *
 *   0 setident binds   (vms-fb9 r6) src/libvmssys/vms_kif.c:
 *                      vms_kif_setident() -> KIF_CALL(...) back to a raw
 *                      vms_sys_ioctl() with no kif_bind()
 *   1 auto-bind        remove the vms_kif_register() call in kif_bind()
 *   2 $ENQ/$DEQ        (same mutation reaches it; see suite 2 note)
 *   3 fork re-bind     kif_bind(): `vms_bound_pid == pid` -> `!= 0`
 *   4 post-exec adopt  vms_module.c: adopt branch -> old 0x1C status
 *   5 adopt keeps privs vms_module.c: adopt branch -> re-apply init_privs
 *   6 errno mapping    vms_kif_kerr_to_ss(): any one arm
 *   7 one PCB/process  vms_module.c: current->tgid -> current->pid
 *
 * Suite 0 is a NARROWER property than suite 1: it exists because
 * vms_kif_setident() was, until r6, the ONE entry point in
 * src/libvmssys/vms_kif.c that bypassed kif_bind() entirely (a raw
 * vms_sys_ioctl(), not KIF_CALL) -- so restoring JUST that one bypass
 * (putting the raw ioctl back) turns suite 0 red while suites 1-7 stay
 * green, and restoring the vms-9fc defect in kif_bind() itself (suite 1's
 * mutation) turns suite 0 red TOO, because setident now reaches kif_bind()
 * exactly like every other entry point.
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
/* Was 5. ORACLE-PINNED to 1 (vms-68c), docs/oracle/vax73-event-flags.md --
 * SS$_WASCLR and SS$_NORMAL are both 1 in $SSDEF on VAX V7.3. */
#define SS_WASCLR       1
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

/* Suite 0's identity. This process is root under QEMU (PID 1 runs init.sh
 * as uid 0), so it holds SETPRV from registration (VMS_PRV_M_ENFORCED) and
 * VMS_IOCTL_SETIDENT does not refuse the stamp on privilege grounds -- the
 * property under test is purely "did the ioctl reach a registered process
 * at all", not identity authorization (that is test_kmod_ident.c's job). */
#define BARE_SETIDENT_USER   "VMS$BAREID"
#define BARE_SETIDENT_UIC    (((uint32_t)401 << 16) | 402u)
#define BARE_SETIDENT_PRIVS  VMS_PRV_M_TMPMBX

/*
 * The identity suite 5 establishes before the image is thrown away, and
 * which adoption must hand back to the image that replaces it.
 *
 * REWRITTEN FOR vms-2b8 ROUND 3, and the rewrite makes the suite STRONGER
 * rather than weaker. It used to pass PRIV_FIRST to the first REGISTER and
 * PRIV_SECOND to the second, and assert that the executive's mask did not
 * follow the second request. VMS_IOCTL_REGISTER no longer HAS a privilege
 * argument -- a process cannot ask for privileges at registration at all,
 * and cannot ask for a VMS process ID either -- so the old probe would now
 * be asserting that an argument which does not exist was ignored.
 *
 * The identity is therefore established the way a process really acquires
 * one, through VMS_IOCTL_SETIDENT while holding SETPRV, and adoption has
 * to preserve ALL of it: the user name, the UIC, the privilege mask, the
 * process name AND the VMS process ID. The last of those is new coverage
 * that the old form could not express, because the pid used to be
 * whatever userspace passed.
 */
#define ADOPT_USER      "VMS$ADOPT"
#define ADOPT_UIC       (((uint32_t)301 << 16) | 302u)
/* TMPMBX|NETMBX|OPER -- OPER is not in the mask registration derives, so
 * establishing it proves the identity came from SETIDENT and not from
 * registration re-deriving a default. */
#define ADOPT_PRIVS     ((1ULL << 15) | (1ULL << 20) | (1ULL << 18))

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
    uint32_t vms_pid;           /* VMS process ID AFTER the second REGISTER */
    uint64_t cur_privs;         /* privileges AFTER the second REGISTER */
    uint32_t uic;
    char     username[VMS_USERNAME_SIZE];
    char     prcnam[VMS_PRCNAM_SIZE];
};

/* What the pre-exec image tells the parent before it throws itself away,
 * so the parent can check that adoption handed the SAME row back. */
struct preexec_note {
    uint32_t vms_pid;
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

    /* The adopt case. There is nothing left to ask FOR (vms-2b8: neither
     * a privilege mask nor a process ID), so what adoption must do is
     * hand back the identity the previous image established -- whole. */
    rep.register_status = vms_kif_register(NULL);

    memset(&info, 0, sizeof(info));
    rep.getjpi_status = vms_kif_getjpi_self(&info);
    rep.linux_pid = info.linux_pid;
    rep.vms_pid   = info.vms_pid;
    rep.uic       = info.uic;
    memcpy(rep.username, info.username, VMS_USERNAME_SIZE);
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
     * SUITE 0 -- vms_kif_setident() NOW BINDS TOO.
     *
     * Until vms-fb9 r6, vms_kif_setident() issued a raw vms_sys_ioctl()
     * instead of going through kif_call()/KIF_CALL -- so it never ran
     * kif_bind() and reached an unregistered process's entry, the same
     * class of defect suite 1 below exists to prove fixed for the
     * facilities it drives ($SETEF/$READEF/$GETJPI). Measured against a
     * real /dev/vms before the r6 fix:
     * vms_kif_open() followed by a BARE vms_kif_setident() (no
     * vms_kif_register(), no other vms_kif_* call first) returned
     * status=20 (SS$_BADPARAM) -- not because the parameters were bad,
     * but because the unbound ioctl was rejected -ESRCH and the old
     * failure path hard-coded SS$_BADPARAM for every failure.
     *
     * Run in a FORKED CHILD, and before suite 1: the probe has to be the
     * FIRST vms_kif_* call this task ever makes, or the process would
     * already be bound (by suite 1's own calls) and the defect would be
     * invisible -- exactly the trap suite 1's own comment warns about.
     * ------------------------------------------------------------ */
    printf("--- 0. vms_kif_setident() binds with no prior register ---\n");

    {
        int p[2];
        pid_t c;
        uint32_t s = 0xFFFFFFFFu;
        ssize_t n;

        if (pipe(p) < 0) {
            printf("  FAIL: pipe()\n");
            fail++;
            goto done;
        }

        c = fork();
        if (c < 0) {
            printf("  FAIL: fork()\n");
            fail++;
            close(p[0]);
            close(p[1]);
            goto done;
        }
        if (c == 0) {
            uint32_t st;

            close(p[0]);
            if (vms_kif_open() < 0)
                st = 0; /* 0 is not a VMS status: never got there */
            else
                st = vms_kif_setident(BARE_SETIDENT_USER, BARE_SETIDENT_UIC,
                                       BARE_SETIDENT_PRIVS);
            (void)!write(p[1], &st, sizeof(st));
            _exit(0);
        }
        close(p[1]);
        n = read(p[0], &s, sizeof(s));
        close(p[0]);
        waitpid(c, NULL, 0);

        CHECK(n == (ssize_t)sizeof(s), "bare-setident child reported back");
        CHECK(s == SS_NORMAL,
              "vms_kif_open() then a BARE vms_kif_setident() reaches the "
              "executive with no explicit register");
        CHECK(s != SS_BADPARAM,
              "... and is not rejected as the caller's own bad parameter "
              "(the pre-r6 raw-ioctl failure path)");
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
     * the privilege mask the previous image established must survive.
     * ------------------------------------------------------------ */
    printf("--- 4/5. post-exec REGISTER adopts the surviving process ---\n");

    /* Re-registering an already-bound process is the SUBJECT of the test,
     * not a setup step the product would otherwise need. It carries no
     * arguments at all now (vms-2b8), so the only thing it can do wrong is
     * disturb the process it found. */
    status = vms_kif_register(NULL);
    CHECK(status == SS_NORMAL,
          "re-registering an already-bound process is accepted (adopt)");

    status = vms_kif_setprn(REEXEC_NAME);
    CHECK(status == SS_NORMAL, "process named before image activation");

    if (vms_kif_getmode(&mode, &cur, &perm) == SS_NORMAL)
        CHECK((cur & (1ULL << 18)) == 0,
              "adoption did not conjure a privilege registration never grants");

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
         * the channel is closed, no environment variable, no descriptor.
         *
         * The identity is established through SETIDENT, the way a process
         * really acquires one -- registration carries no privilege mask
         * to establish it with any more (vms-2b8). This image is still
         * root here, so it holds SETPRV and the stamp is legitimate. */
        {
            struct preexec_note note;
            struct vms_procinfo pinfo;

            memset(&note, 0, sizeof(note));
            if (vms_kif_register(NULL) != SS_NORMAL)
                _exit(72);
            if (vms_kif_setprn(REEXEC_NAME "2") != SS_NORMAL)
                _exit(73);
            if (vms_kif_setident(ADOPT_USER, ADOPT_UIC, ADOPT_PRIVS) != SS_NORMAL)
                _exit(75);

            memset(&pinfo, 0, sizeof(pinfo));
            if (vms_kif_getjpi_self(&pinfo) != SS_NORMAL)
                _exit(76);
            note.vms_pid = pinfo.vms_pid;
            if (write(pipefd[1], &note, sizeof(note)) != (ssize_t)sizeof(note))
                _exit(77);
        }
        vms_kif_close();

        snprintf(wfd_arg, sizeof(wfd_arg), "%d", pipefd[1]);
        execl(argv[0], argv[0], "--reexec", wfd_arg, (char *)NULL);
        _exit(74);
    }
    close(pipefd[1]);

    {
        struct exec_report erep;
        struct preexec_note note;
        ssize_t n;

        memset(&note, 0, sizeof(note));
        n = read(pipefd[0], &note, sizeof(note));
        CHECK(n == (ssize_t)sizeof(note) && note.vms_pid != 0,
              "the pre-exec image reported the VMS process ID it was assigned");

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
        /*
         * The identity established before the image was thrown away has to
         * come back whole. Registration can no longer be handed a mask or a
         * pid to disturb it with (vms-2b8), so anything different here is
         * the executive re-deriving state it was supposed to preserve.
         */
        CHECK(erep.cur_privs == ADOPT_PRIVS,
              "adoption preserved the privilege mask SETIDENT established");
        CHECK(erep.uic == ADOPT_UIC,
              "adoption preserved the UIC");
        CHECK(strcmp(erep.username, ADOPT_USER) == 0,
              "adoption preserved the authenticated user name");
        CHECK(erep.vms_pid == note.vms_pid,
              "adoption returned the SAME VMS process ID, not a fresh one");
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
