/*
 * test_kmod_bind.c - The product reaches the executive, and only processes the
 * executive created do (vms-9fc, rd vms-2a5).
 *
 * There is no registration. A process's PCB is made by its creator before it
 * runs (VMS_IOCTL_CREPRC_PCB; this suite's by the harness boot task, its
 * children's by this suite), so the first vms_kif_* call a process makes just
 * opens its own /dev/vms descriptor (kif_bind) and reaches its row. A task the
 * executive did not create has no row, and every service refuses it with
 * SS$_NONEXPR. Each property below is checked against a real /dev/vms:
 *
 *   0 setident binds   (vms-fb9 r6) vms_kif_setident() goes through KIF_CALL
 *   1 first call binds no explicit vms_kif_open() before a facility
 *   2 $ENQ/$DEQ        the lock manager through the same path
 *   3 subprocess       a child its creator made reaches its OWN row
 *   3b not created     a plain fork() child is refused SS$_NONEXPR and is in
 *                      no $PROCESS_SCAN (negctl unknown-task-gets-pcb)
 *   4 post-exec        an image activated in a process is the same VMS
 *                      process: no call needed, identity and PID intact
 *   6 errno mapping    vms_kif_kerr_to_ss(): any one arm
 *   7 one PCB/process  threads share the process's row (negctl pcb-per-thread)
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
 *   --reexec <pipe-wfd>       the freshly activated image (suite 4)
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
#include "ovmx_test_fork.h"

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
#define SS_NONEXPR      2280

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

/* Suite 0's identity. The probe is a subprocess of this suite, which the boot
 * task made SYSTEM, so it holds CMKRNL/SETPRV and VMS_IOCTL_SETIDENT does not
 * refuse the stamp on privilege grounds -- the property under test is purely
 * "did the ioctl reach the process's row at all", not identity authorization
 * (that is test_kmod_ident.c's job). */
#define BARE_SETIDENT_USER   "VMS$BAREID"
#define BARE_SETIDENT_UIC    (((uint32_t)401 << 16) | 402u)
#define BARE_SETIDENT_PRIVS  VMS_PRV_M_TMPMBX

/*
 * The identity suite 4 establishes before the image is thrown away, and which
 * the image that replaces it must still have: the user name, UIC, privilege
 * mask, process name and VMS process ID. An image activation does not create
 * a process (VMS), so nothing is asked for and nothing is re-derived.
 */
#define ADOPT_USER      "VMS$ADOPT"
#define ADOPT_UIC       (((uint32_t)301 << 16) | 302u)
/* TMPMBX|NETMBX|OPER -- a mask different from SYSTEM's, so a match after exec
 * proves the image kept the identity SETIDENT established. */
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
    uint32_t getjpi_status;
    uint32_t linux_pid;
    uint32_t vms_pid;           /* VMS process ID after the exec */
    uint64_t cur_privs;         /* privileges after the exec */
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
 * The re-executed image (suite 4)
 *
 * Its predecessor, in the same process, established an identity and closed
 * its channel before exec. This image asks for nothing: on VMS an image
 * activation does not create a process, so the row is simply still there.
 * ================================================================ */
static int reexeced_image(int wfd)
{
    struct exec_report rep;
    struct vms_procinfo info;
    uint8_t mode = 0;
    uint64_t cur = 0, perm = 0;

    memset(&rep, 0, sizeof(rep));

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
 * The child (suites 3 and 3b)
 *
 * It inherited the parent's thread-local state wholesale: the parent's
 * /dev/vms descriptor number AND the parent's "bound" mark. The executive
 * keys its process table by process: made by its creator (suite 3), the child
 * reaches its OWN row; a plain fork() (suite 3b) has none and is refused. It
 * calls ONE public entry point and nothing else.
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
 * (no explicit open: kif_bind runs inside it) and
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
static int raw_ioctl_errno(unsigned long req, void *arg)
{
    int fd = open("/dev/vms", O_RDWR);
    int rc;

    if (fd < 0)
        return 0;

    errno = 0;
    rc = ioctl(fd, req, arg);
    rc = (rc < 0) ? errno : 0;
    close(fd);
    return rc;
}

/*
 * unknown_task_errno - what vms.ko answers a task it has no entry for: a plain
 * fork() child, which the executive did not create (rd vms-2a5).
 */
static int unknown_task_errno(void)
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
        ce = raw_ioctl_errno(VMS_IOCTL_SETEF, &efa);
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
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    int pipefd[2];
    pid_t child;
    uint32_t status, state;
    uint32_t lkid = 0;
    struct vms_procinfo info;
    char wfd_arg[16];

    if (argc >= 3 && strcmp(argv[1], "--reexec") == 0)
        return reexeced_image(atoi(argv[2]));

    printf("=== Executive binding (vms-9fc) ===\n");

    if (access("/dev/vms", F_OK) != 0) {
        printf("  FAIL: /dev/vms absent (executive not loaded)\n");
        printf("=== test_kmod_bind: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* ------------------------------------------------------------
     * SUITE 0 -- vms_kif_setident() BINDS TOO.
     *
     * Until vms-fb9 r6, vms_kif_setident() issued a raw vms_sys_ioctl()
     * instead of going through KIF_CALL, so it never ran kif_bind(): measured
     * before r6, a vms_kif_open() followed by a BARE vms_kif_setident()
     * returned SS$_BADPARAM because the ioctl never reached the process's
     * row. Run in a SUBPROCESS this suite creates (its row exists before it
     * runs), as the first vms_kif_* call that process makes after open.
     * ------------------------------------------------------------ */
    printf("--- 0. vms_kif_setident() binds as its first call ---\n");

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

        c = ovmx_fork_subprocess();
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
              "process's row");
        CHECK(s != SS_BADPARAM,
              "... and is not rejected as the caller's own bad parameter "
              "(the pre-r6 raw-ioctl failure path)");
    }

    /* ------------------------------------------------------------
     * SUITE 1 -- THE FIRST CALL BINDS.
     *
     * No vms_kif_open(). The first thing this process does to the executive
     * is use a facility, exactly as libvms does. Its row was made by its
     * creator (the harness boot task) before it ran.
     * ------------------------------------------------------------ */
    printf("--- 1. a facility used as the process's first executive call ---\n");

    status = vms_kif_setef(BIND_EFN);
    CHECK(status == SS_WASCLR || status == SS_WASSET,
          "$SETEF reaches the executive with no explicit open");
    CHECK(status != SS_BADPARAM,
          "$SETEF does not report the caller's parameters as bad");

    state = 0;
    status = vms_kif_readef(BIND_EFN, &state);
    /* negctl-knockon: eflag-readef-status-inverted */
    CHECK(status == SS_WASSET, "$READEF sees the flag this process just set");
    CHECK((state & (1u << (BIND_EFN % 32))) != 0,
          "cluster state carries the flag bit");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL, "$GETJPI(self) resolves this process");
    CHECK(info.linux_pid == (uint32_t)getpid(),
          "the executive entry is this process's own");

    /* ------------------------------------------------------------
     * SUITE 2 -- sys$ENQ / sys$DEQ, through the same path.
     * ------------------------------------------------------------ */
    printf("--- 2. lock manager reached through the same path ---\n");

    status = vms_kif_enq(0, 5 /* LCK$K_EXMODE */, 0, BIND_RESNAM, 0,
                         0, 0, 0, &lkid, NULL);
    CHECK(status == SS_NORMAL, "$ENQ EX granted through /dev/vms");
    CHECK(lkid != 0, "$ENQ returned a lock id");

    status = vms_kif_deq(lkid, NULL, 0);
    CHECK(status == SS_NORMAL, "$DEQ released the lock");

    /* ------------------------------------------------------------
     * SUITE 3 -- a subprocess its creator made reaches its OWN row.
     *
     * The child's thread-local state says "already bound" and names a
     * descriptor it inherited. Both are about the parent. kif_bind() must
     * notice the new process and take its own descriptor.
     * ------------------------------------------------------------ */
    printf("--- 3. a subprocess binds as itself ---\n");

    if (pipe(pipefd) < 0) {
        printf("  FAIL: pipe()\n");
        fail++;
        goto done;
    }

    child = ovmx_fork_subprocess();
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

        CHECK(n == (ssize_t)sizeof(frep), "subprocess reported back");
        CHECK(frep.setef_status == SS_WASCLR || frep.setef_status == SS_WASSET,
              "subprocess's $SETEF reaches the executive");
        CHECK(frep.getjpi_status == SS_NORMAL,
              "subprocess resolves itself in the process table");
        CHECK(frep.linux_pid == (uint32_t)child,
              "subprocess's executive entry is the CHILD's, not the parent's");
    }

    /* ------------------------------------------------------------
     * SUITE 3b -- A TASK THE EXECUTIVE DID NOT CREATE IS NOT A VMS PROCESS
     * (rd vms-2a5, design note 3.1).
     *
     * A plain fork() child calls the same public entry points. There is no
     * registration: no service makes it a PCB. Every call is refused
     * SS$_NONEXPR, and $PROCESS_SCAN, run by this suite, never lists it.
     * ------------------------------------------------------------ */
    printf("--- 3b. a plain fork() child is refused and is in no $PROCESS_SCAN ---\n");

    {
        int up[2], go[2];

        if (pipe(up) < 0 || pipe(go) < 0) {
            printf("  FAIL: pipe()\n");
            fail++;
            goto done;
        }
        child = fork();
        if (child < 0) {
            printf("  FAIL: fork()\n");
            fail++;
            goto done;
        }
        if (child == 0) {
            struct fork_report rep;
            struct vms_procinfo cinfo;
            char b;

            close(up[0]); close(go[1]);
            memset(&rep, 0, sizeof(rep));
            rep.setef_status = vms_kif_setef(BIND_EFN);
            memset(&cinfo, 0, sizeof(cinfo));
            rep.getjpi_status = vms_kif_getjpi_self(&cinfo);
            rep.linux_pid = cinfo.linux_pid;
            (void)!write(up[1], &rep, sizeof(rep));
            (void)!read(go[0], &b, 1);          /* alive while the scan runs */
            _exit(0);
        }
        close(up[1]); close(go[0]);
        {
            struct fork_report urep;
            struct vms_procinfo row;
            uint32_t index = 0, listed = 0, scanned = 0;
            ssize_t n;

            memset(&urep, 0, sizeof(urep));
            n = read(up[0], &urep, sizeof(urep));
            CHECK(n == (ssize_t)sizeof(urep), "the plain fork() child reported back");
            /* negctl: unknown-task-gets-pcb */
            CHECK(urep.setef_status == SS_NONEXPR,
                  "a task the executive did not create is refused $SETEF with SS$_NONEXPR");
            /* negctl: unknown-task-gets-pcb */
            CHECK(urep.getjpi_status == SS_NONEXPR,
                  "... and $GETJPI(self) with SS$_NONEXPR: it has no row");

            for (;;) {
                memset(&row, 0, sizeof(row));
                status = vms_kif_procscan(&index, &row);
                if (status != SS_NORMAL)
                    break;
                scanned++;
                if (row.linux_pid == (uint32_t)child)
                    listed++;
                if (scanned > 4096)
                    break;
            }
            CHECK(scanned > 0, "$PROCESS_SCAN listed this system's processes");
            /* negctl: unknown-task-gets-pcb */
            CHECK(listed == 0, "the task the executive did not create is in no $PROCESS_SCAN");
        }
        (void)!write(go[1], "g", 1);
        close(up[0]); close(go[1]);
        waitpid(child, NULL, 0);
    }

    /* ------------------------------------------------------------
     * SUITE 4 -- execve(): the activated image is the same VMS process.
     *
     * ORACLE PIN (VAX1, OpenVMS VAX V7.3, 2026-07-30): activating an image
     * inside an existing process does not recreate the process. SHOW
     * PROCESS/ACCOUNTING across two more image activations:
     *     Process ID: 2020021D  name "SYSTEM"  Images activated: 19
     *     Process ID: 2020021D  name "SYSTEM"  Images activated: 21
     * The image after exec asks for nothing and finds its identity, process
     * name and VMS process ID as the previous image left them.
     * ------------------------------------------------------------ */
    printf("--- 4. the image activated after execve() is the same VMS process ---\n");

    if (pipe(pipefd) < 0) {
        printf("  FAIL: pipe()\n");
        fail++;
        goto done;
    }

    child = ovmx_fork_subprocess();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        fail++;
        close(pipefd[0]);
        close(pipefd[1]);
        goto done;
    }
    if (child == 0) {
        close(pipefd[0]);

        /* Establish the pre-exec identity on the child's own row, then throw
         * the image away. Nothing is carried across: the channel is closed,
         * no environment variable, no descriptor. */
        {
            struct preexec_note note;
            struct vms_procinfo pinfo;

            memset(&note, 0, sizeof(note));
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
              "the pre-exec image reported its VMS process ID");

        memset(&erep, 0, sizeof(erep));
        n = read(pipefd[0], &erep, sizeof(erep));
        close(pipefd[0]);
        waitpid(child, NULL, 0);

        CHECK(n == (ssize_t)sizeof(erep), "re-execed image reported back");
        CHECK(erep.getjpi_status == SS_NORMAL,
              "the activated image resolves in the process table without asking");
        CHECK(erep.linux_pid == (uint32_t)child,
              "it is the same executive entry, not a new one");
        CHECK(strcmp(erep.prcnam, REEXEC_NAME "2") == 0,
              "the process name survived the image activation");
        /* negctl-knockon: getmode-buffer-not-written */
        CHECK(erep.cur_privs == ADOPT_PRIVS,
              "the privilege mask SETIDENT established survived");
        CHECK(erep.uic == ADOPT_UIC, "the UIC survived");
        CHECK(strcmp(erep.username, ADOPT_USER) == 0,
              "the authenticated user name survived");
        CHECK(erep.vms_pid == note.vms_pid,
              "the VMS process ID is the SAME one, not a fresh one");
    }

    /* ------------------------------------------------------------
     * SUITE 6 -- honest statuses for real kernel errnos.
     *
     * Each errno below is what vms.ko actually returned to a raw ioctl
     * issued here, and the mapping is then checked against it.
     * ------------------------------------------------------------ */
    printf("--- 6. ioctl failures get honest statuses ---\n");

    {
        struct vms_ef_args efa;
        int e;

        /* -ENOTTY: a command number the module does not implement. */
        memset(&efa, 0, sizeof(efa));
        e = raw_ioctl_errno(_IOWR(VMS_IOC_MAGIC, 0x7E, struct vms_ef_args), &efa);
        CHECK(e == ENOTTY, "module rejects an unknown ioctl with -ENOTTY");
        CHECK(vms_kif_kerr_to_ss(-e) == SS_ILLIOFUNC,
              "-ENOTTY -> SS$_ILLIOFUNC (oracle 244), not SS$_BADPARAM");

        /* -ESRCH: the module's answer to a task it did not create, provoked
         * in a FRESH process (a plain fork): this process's own second
         * descriptor would reach this process's row. */
        e = unknown_task_errno();
        CHECK(e == ESRCH, "module rejects a task it did not create with -ESRCH");
        CHECK(vms_kif_kerr_to_ss(-e) == SS_NONEXPR,
              "-ESRCH -> SS$_NONEXPR: not a VMS process");

        /* -EFAULT: a pointer the module cannot copy from. */
        e = raw_ioctl_errno(VMS_IOCTL_SETEF, (void *)(unsigned long)0x1);
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
     * it (suites 0-6 all pass either way, because tgid == pid for a
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

            /* negctl: pcb-per-thread */
            CHECK(trep.getjpi_status == SS_NORMAL,
                  "sibling thread resolves in the process table");
            /* negctl-knockon: pcb-per-thread */
            CHECK(trep.linux_pid == (uint32_t)getpid(),
                  "sibling thread got the PROCESS's entry, not one of its own");
            /* negctl: pcb-per-thread */
            CHECK(strcmp(trep.prcnam, THREAD_NAME) == 0,
                  "sibling thread sees the process name the main thread set");
            /* negctl: pcb-per-thread */
            /* negctl-knockon: eflag-readef-status-inverted */
            CHECK(trep.readef_status == SS_WASSET,
                  "sibling thread sees the event flag the main thread set");
            /* negctl-knockon: pcb-per-thread */
            CHECK((trep.readef_state & (1u << (THREAD_EFN % 32))) != 0,
                  "sibling thread's cluster state carries the flag bit");
            /* negctl: pcb-per-thread */
            CHECK(trep.deq_status == SS_NORMAL,
                  "sibling thread can $DEQ the lock the main thread took");
        }
    }

done:
    printf("=== test_kmod_bind: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
