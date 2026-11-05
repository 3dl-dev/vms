/*
 * test_syssvc_creprc_nopipe_audit.c - $CREPRC creates a process with NO host
 * pipe: creator and created task synchronise through the executive's creation
 * record (VMS_IOCTL_CREPRC_SYNC, rd vms-416d), never pipe(2)/pipe2(2).
 *
 * WHY. Every $CREPRC used to make three pipes in the creator (the creation
 * report, the PCB request and its answer) and one more in a detached process's
 * intermediate (its release). Found by the networking lane's S3 STARTNET audit
 * (STARTNET -> NCP -> NETACP -> FAL, each a $CREPRC): a VMS process is created
 * by the executive, and the two halves of OVMX's fork-based creation now meet
 * in an executive object, so the host IPC must be gone from the trace.
 *
 * WHAT IS AUDITED. The suite makes a VMS subprocess (the CREATOR) that runs
 * under ptrace (PTRACE_TRACEME) with PTRACE_O_TRACEFORK/VFORK/CLONE/EXEC, so
 * every task $CREPRC makes -- the created task, a detached process's
 * intermediate and grandchild -- is traced from birth through its image. The
 * creator issues
 *
 *   1. $CREPRC of a SUBPROCESS running this binary in subject mode, and
 *   2. $CREPRC of a DETACHED process (PRC$M_DETACH: setsid + the intermediate
 *      that holds until the grandchild's PCB exists) running the same,
 *   3. ovmx_subprocess_fork() (src/libvms/include/ovmx_subprocess.h, the in-tree
 *      forkers' $CREPRC: ovmx_init's OPCOM, rd vms-ff3), whose child records
 *      that it ran,
 *
 * waits (by polling the subjects' marker files, no host IPC) until both ran,
 * and exits. Every system-call entry of every traced task is classified, and
 * the suite asserts NONE is pipe / pipe2 / socketpair.
 *
 * DCL'S FORK-PATH IMAGE ACTIVATION (rd vms-89bb / vms-d8e1). A second traced
 * subject is the shipped DCL.EXE itself, a VMS subprocess reading
 *
 *   $ DEFINE AUDDIR "/tmp/c416d/"
 *   $ RUN AUDDIR:DCLRUN.EXE
 *
 * from SYS$INPUT, where DCLRUN.EXE is this binary in subject mode: DCL forks
 * the image task, makes it its own VMS process (VMS_CREPRC_PCB_CONTINUE)
 * through the creation record (dcl_image_fork), and the task execs the image.
 * The suite asserts no pipe / pipe2 in DCL or the image task. (DCL's CLI
 * callback socketpair is counted and printed, not asserted: rd vms-6d9.)
 *
 * NOT VACUOUS. The trace must have seen the creation record in use (the
 * VMS_IOCTL_CREPRC_SYNC ioctls of both creations), both subject images
 * exec'd and ran, and both $CREPRC calls returned success with a process ID.
 * The classifier is proven on a control child that makes exactly the
 * forbidden calls (pipe and pipe2): each must be flagged.
 *
 * NEGATIVE CONTROLS (product mutations that must redden this suite):
 * tests/qemu/defects.d/creprc-handshake-host-pipe.defect ($CREPRC) and
 * subprocess-fork-host-pipe.defect (ovmx_subprocess_fork).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#if defined(__alpha__)
#include <asm/ptrace.h>
#else
#include <sys/user.h>
#endif
#include <elf.h>

#include "starlet.h"
#include "descrip.h"
#include "prcdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_subprocess.h"   /* ovmx_subprocess_fork (rd vms-ff3) */

#define EXIT_SKIP 77
#define DIR_      "/tmp/c416d"
#define RESULTS   DIR_ "/creator.results"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* ---- the per-arch syscall entry: number + first three arguments -------- */
struct sc { long nr; unsigned long a0, a1, a2; };

static int sc_read(pid_t pid, struct sc *s)
{
    struct iovec iov;
#if defined(__alpha__)
    /* No PTRACE_GETREGSET on Linux/Alpha: v0 ($0) is the syscall number at
     * the entry stop, a0..a2 are $16..$18 (as strace reads them there). */
    static const int regno[4] = { 0, 16, 17, 18 };
    unsigned long val[4];
    (void)iov;
    for (int i = 0; i < 4; i++) {
        errno = 0;
        val[i] = (unsigned long)ptrace(PTRACE_PEEKUSER, pid, (void *)(long)regno[i], 0);
        if (errno != 0) return -1;
    }
    s->nr = (long)val[0]; s->a0 = val[1]; s->a1 = val[2]; s->a2 = val[3];
    return 0;
#else
    struct user_regs_struct r;
    memset(&r, 0, sizeof r);
    iov.iov_base = &r; iov.iov_len = sizeof r;
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
#if defined(__x86_64__)
    s->nr = (long)r.orig_rax; s->a0 = r.rdi; s->a1 = r.rsi; s->a2 = r.rdx;
    return 0;
#elif defined(__aarch64__)
    s->nr = (long)r.regs[8]; s->a0 = r.regs[0]; s->a1 = r.regs[1]; s->a2 = r.regs[2];
    return 0;
#else
    (void)s;
    return -1;
#endif
#endif
}

/* ---- the classifier ------------------------------------------------------ */
struct verdict {
    unsigned long total;       /* syscall entries seen */
    unsigned long sync_ioctls; /* VMS_IOCTL_CREPRC_SYNC: the creation record */
    unsigned execs;            /* PTRACE_EVENT_EXEC stops */
    unsigned bad_pipe;         /* pipe / pipe2 */
    unsigned bad_sockpair;     /* socketpair */
    unsigned tasks;            /* tasks the tracee made (traced from birth) */
    char first_bad[160];
};

static void classify(struct verdict *v, pid_t who, const struct sc *s)
{
    const char *what = NULL;

    v->total++;
    if (s->nr == SYS_ioctl) {
        if ((s->a1 & 0xFFFFFFFFul) == ((unsigned long)VMS_IOCTL_CREPRC_SYNC & 0xFFFFFFFFul))
            v->sync_ioctls++;
        return;
    }
#ifdef SYS_pipe
    if (s->nr == SYS_pipe) what = "pipe";
#endif
#ifdef SYS_pipe2
    if (s->nr == SYS_pipe2) what = "pipe2";
#endif
#ifdef SYS_socketpair
    if (s->nr == SYS_socketpair) {
        v->bad_sockpair++;
        if (!v->first_bad[0])
            snprintf(v->first_bad, sizeof v->first_bad, "socketpair in task %d", (int)who);
        return;
    }
#endif
    if (what) {
        v->bad_pipe++;
        if (!v->first_bad[0])
            snprintf(v->first_bad, sizeof v->first_bad, "%s in task %d", what, (int)who);
    }
}

static volatile pid_t g_root;
static void on_bound(int sig) { (void)sig; if (g_root > 0) kill(g_root, SIGKILL); }

/*
 * trace_all - run the tracee (stopped at its SIGSTOP after PTRACE_TRACEME) and
 * every task it makes to completion, feeding each syscall-ENTRY stop to
 * classify(). Returns the root's exit status (waitpid form), or -1.
 */
static int trace_all(pid_t root, struct verdict *v, int bound_s)
{
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
    enum { MAXT = 64 };
    pid_t tids[MAXT]; int insys[MAXT]; int newborn[MAXT]; int nt = 0;
    int root_status = -1, live = 1;

    if (ptrace(PTRACE_SETOPTIONS, root, 0, (void *)opts) != 0)
        return -1;
    g_root = root;
    signal(SIGALRM, on_bound);
    alarm((unsigned)bound_s);         /* a wedged tracee is killed, not waited on */
    tids[nt] = root; insys[nt] = 0; newborn[nt] = 0; nt++;
    if (ptrace(PTRACE_SYSCALL, root, 0, 0) != 0)
        return -1;

    while (live > 0) {
        int st = 0;
        pid_t p = waitpid(-1, &st, __WALL);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int k;
        for (k = 0; k < nt && tids[k] != p; k++)
            ;
        if (k == nt && nt < MAXT) { tids[nt] = p; insys[nt] = 0; newborn[nt] = 1; nt++; live++; v->tasks++; }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (p == root) root_status = st;
            live--;
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;
        int sig = WSTOPSIG(st);
        int deliver = 0;
        if (sig == (SIGTRAP | 0x80)) {
            if (k < nt) {
                if (!insys[k]) {
                    struct sc s;
                    if (sc_read(p, &s) == 0)
                        classify(v, p, &s);
                }
                insys[k] = !insys[k];
            }
        } else if (sig == SIGTRAP && (st >> 16)) {
            if ((st >> 16) == PTRACE_EVENT_EXEC)
                v->execs++;
        } else if (sig == SIGSTOP && k < nt && newborn[k]) {
            newborn[k] = 0;            /* a new task's birth stop: swallow it */
        } else {
            deliver = sig;
        }
        (void)ptrace(PTRACE_SYSCALL, p, 0, (void *)(long)deliver);
    }
    alarm(0);
    return root_status;
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int ran(const char *role)
{
    char p[128];
    snprintf(p, sizeof(p), DIR_ "/%s.ran", role);
    return access(p, F_OK) == 0;
}

/* SUBJECT MODE (argv[0] = DIR_/<role>): record that this subject ran. */
static int subject_main(const char *role)
{
    char p[128];
    snprintf(p, sizeof(p), DIR_ "/%s.ran", role);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    return 0;
}

/*
 * CREATOR (traced): the two $CREPRC calls. Its exit status carries the
 * verdict bits; the statuses and process IDs go to RESULTS for the log.
 */
#define R_SUB_OK  0x01
#define R_SUB_RAN 0x02
#define R_DET_OK  0x04
#define R_DET_RAN 0x08
#define R_OF_OK   0x10  /* ovmx_subprocess_fork (ovmx_subprocess.h, rd vms-ff3) */
#define R_OF_RAN  0x20
static void creator_main(void)
{
    struct timespec tick = { 0, 20 * 1000 * 1000 };
    uint32_t spid = 0, dpid = 0, sst, dst;
    int bits = 0;

    if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
        _exit(100);
    raise(SIGSTOP);                         /* the tracer sets its options here */
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);

    struct dsc$descriptor_s si = mkdsc(DIR_ "/SUBPROC");
    sst = sys$creprc(&spid, &si, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0);
    if ((sst & 1) && spid != 0) bits |= R_SUB_OK;

    struct dsc$descriptor_s di = mkdsc(DIR_ "/DETACHED");
    struct dsc$descriptor_s nl = mkdsc("NL:");
    dst = sys$creprc(&dpid, &di, NULL, &nl, &nl, NULL, NULL, NULL, 0, 0, 0, PRC$M_DETACH);
    if ((dst & 1) && dpid != 0) bits |= R_DET_OK;

    /* The in-tree forkers' path (ovmx_subprocess.h: ovmx_init's OPCOM, DCL):
     * the same creation record, no pipe either (rd vms-ff3). */
    uint32_t ost = 0, opid = 0;
    pid_t of = ovmx_subprocess_fork(&ost, &opid);
    if (of == 0)
        _exit(subject_main("OVMXFORK"));
    if (of > 0 && (ost & 1) && opid != 0) bits |= R_OF_OK;

    for (int waited = 0; waited < 1500; waited++) {   /* <= 30 s */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        if ((!(bits & R_SUB_OK) || ran("SUBPROC")) && (!(bits & R_DET_OK) || ran("DETACHED")) &&
            (!(bits & R_OF_OK) || ran("OVMXFORK")))
            break;
        nanosleep(&tick, NULL);
    }
    if (ran("SUBPROC"))  bits |= R_SUB_RAN;
    if (ran("DETACHED")) bits |= R_DET_RAN;
    if (ran("OVMXFORK")) bits |= R_OF_RAN;
    for (int i = 0; i < 50 && waitpid(-1, NULL, WNOHANG) >= 0; i++)
        nanosleep(&tick, NULL);             /* reap what is ours to reap */

    int fd = open(RESULTS, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dprintf(fd, "subprocess status %08X pid %08X; detached status %08X pid %08X; "
                "ovmx_subprocess_fork status %08X pid %08X\n",
                (unsigned)sst, (unsigned)spid, (unsigned)dst, (unsigned)dpid,
                (unsigned)ost, (unsigned)opid);
        close(fd);
    }
    _exit(bits);
}

/* The DCL.EXE under trace: the harness's staged copy, or OVMX_DCL. */
static const char *pick_dcl(void)
{
    const char *e = getenv("OVMX_DCL");
    if (e && *e) return e;
    static const char *cand[] = {
        "/run/ovmx-boot/DCL.EXE", "/tests/DCL.EXE", "/bin/DCL.EXE",
    };
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++)
        if (access(cand[i], X_OK) == 0) return cand[i];
    return cand[0];
}

/* The control child: it makes exactly the forbidden calls. */
static void control_child(void)
{
    int p[2];
    if (pipe(p) == 0) { close(p[0]); close(p[1]); }
    if (pipe2(p, O_CLOEXEC) == 0) { close(p[0]); close(p[1]); }
    _exit(0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    {
        /* The image DCL activates is named by its environment, so the subject
         * is chosen whatever argv[0] DCL's path resolution hands it. */
        const char *sub = getenv("OVMX_NOPIPE_SUBJECT");
        if (sub && *sub)
            return subject_main(sub);
    }
    if (argc >= 1 && strncmp(argv[0], DIR_ "/", strlen(DIR_) + 1) == 0)
        return subject_main(basename(argv[0]));

    printf("=== test_syssvc_creprc_nopipe_audit ($CREPRC makes no host pipe, rd vms-416d) ===\n");

    /* ---- 0. the classifier works (anti-vacuity) ------------------------- */
    {
        struct verdict v;
        memset(&v, 0, sizeof v);
        pid_t c = fork();
        if (c == 0) {
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            control_child();
        }
        int st = 0;
        waitpid(c, &st, 0);
        int rs = trace_all(c, &v, 30);
        CHECK(rs >= 0 && v.bad_pipe >= 2,
              "control: the audit flags a pipe() and a pipe2()");
    }

    {
        int fd = vms_kif_open();
        if (fd < 0) {
            printf("=== test_syssvc_creprc_nopipe_audit: %d passed, %d failed (SKIPPED: no"
                   " /dev/vms -- $CREPRC's creation record is executive-resident) ===\n",
                   pass, fail);
            return fail > 0 ? 1 : EXIT_SKIP;
        }
        vms_kif_close();
    }

    /* ---- 1. stage the subjects (this binary, in subject mode) ----------- */
    char self[512];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) {
        printf("  FAIL: cannot resolve this test's own image\n");
        return 1;
    }
    self[sl] = '\0';
    mkdir(DIR_, 0777);
    int staged = 1;
    static const char *const roles[] = { "SUBPROC", "DETACHED" };
    for (int i = 0; i < 2; i++) {
        char p[128];
        snprintf(p, sizeof(p), DIR_ "/%s", roles[i]);
        unlink(p);
        if (symlink(self, p) != 0) staged = 0;
        snprintf(p, sizeof(p), DIR_ "/%s.ran", roles[i]);
        unlink(p);
    }
    unlink(RESULTS);
    CHECK(staged, "staged the subject images (this binary, in subject mode)");

    /* ---- 2. trace a creator through both $CREPRC calls ------------------ */
    uint32_t cst = 0;
    pid_t c = ovmx_fork_subprocess_st(&cst);
    if (c == 0)
        creator_main();
    CHECK(c > 0 && (cst & 1), "the suite makes a VMS subprocess to be the traced creator");
    if (c <= 0) {
        printf("=== test_syssvc_creprc_nopipe_audit: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }
    int st = 0;
    waitpid(c, &st, 0);
    struct verdict v;
    memset(&v, 0, sizeof v);
    int rs = trace_all(c, &v, 90);
    int bits = (rs >= 0 && WIFEXITED(rs)) ? WEXITSTATUS(rs) : -1;

    char res[256] = "";
    int rf = open(RESULTS, O_RDONLY);
    if (rf >= 0) {
        ssize_t n = read(rf, res, sizeof(res) - 1);
        if (n > 0) res[n] = '\0';
        close(rf);
    }
    printf("  (creator exit %d; %s", bits, res[0] ? res : "no results file\n");
    printf("  (syscalls seen %lu, creation-record ioctls %lu, execs %u; host pipes %u%s%s)\n",
           v.total, v.sync_ioctls, v.execs, v.bad_pipe,
           v.first_bad[0] ? "; first: " : "", v.first_bad);

    CHECK(bits >= 0 && (bits & R_SUB_OK),
          "$CREPRC of a subprocess succeeded with a process ID, under the trace");
    CHECK(bits >= 0 && (bits & R_SUB_RAN), "the subprocess ran its image");
    CHECK(bits >= 0 && (bits & R_DET_OK),
          "$CREPRC of a DETACHED process succeeded with a process ID, under the trace");
    CHECK(bits >= 0 && (bits & R_DET_RAN), "the detached process ran its image");
    CHECK(bits >= 0 && (bits & R_OF_OK),
          "ovmx_subprocess_fork made a VMS subprocess (PCB status success, a VMS PID), under the trace");
    CHECK(bits >= 0 && (bits & R_OF_RAN), "the ovmx_subprocess_fork child ran");
    CHECK(v.execs >= 2 && v.total > 50,
          "the trace is not vacuous: it followed both created tasks into their images");
    CHECK(v.sync_ioctls >= 14,
          "creator and created tasks met in the executive's creation record (VMS_IOCTL_CREPRC_SYNC)");
    /* negctl: creprc-handshake-host-pipe */
    /* negctl: subprocess-fork-host-pipe */
    CHECK(v.bad_pipe == 0 && v.bad_sockpair == 0,
          "NO host pipe: no pipe/pipe2/socketpair across $CREPRC and the created processes' runs");

    /* ---- 3. DCL's fork-path image activation (rd vms-89bb, vms-d8e1) ---- */
    {
        const char *dcl = pick_dcl();
        char img[128];
        snprintf(img, sizeof img, DIR_ "/DCLRUN.EXE");
        unlink(img);
        unlink(DIR_ "/DCLRUN.ran");
        int ok = symlink(self, img) == 0;
        int cf = open(DIR_ "/RUN.COM", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (cf >= 0) {
            dprintf(cf, "$ DEFINE AUDDIR \"" DIR_ "/\"\n$ RUN AUDDIR:DCLRUN.EXE\n");
            close(cf);
        } else {
            ok = 0;
        }
        CHECK(ok && access(dcl, X_OK) == 0,
              "staged DCL's subject (AUDDIR:DCLRUN.EXE, this binary) and found DCL.EXE");

        uint32_t dst = 0;
        pid_t d = ovmx_fork_subprocess_st(&dst);
        if (d == 0) {
            int in = open(DIR_ "/RUN.COM", O_RDONLY);
            if (in >= 0) { dup2(in, 0); close(in); }
            unsetenv("OVMX$CLI_FD");    /* the subject is its own DCL, not a callback */
            setenv("OVMX_NOPIPE_SUBJECT", "DCLRUN", 1);
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            raise(SIGSTOP);
            execl(dcl, "DCL.EXE", (char *)NULL);
            _exit(127);
        }
        CHECK(d > 0 && (dst & 1), "the suite makes a VMS subprocess to run the traced DCL");
        if (d > 0) {
            int dstat = 0;
            waitpid(d, &dstat, 0);
            struct verdict dv;
            memset(&dv, 0, sizeof dv);
            int drs = trace_all(d, &dv, 60);
            printf("  (DCL exit %s%d; syscalls %lu, creation-record ioctls %lu, tasks %u,"
                   " execs %u; pipe %u, socketpair %u (vms-6d9)%s%s)\n",
                   (drs >= 0 && WIFEXITED(drs)) ? "" : "signal/broken ",
                   (drs >= 0 && WIFEXITED(drs)) ? WEXITSTATUS(drs) : -1,
                   dv.total, dv.sync_ioctls, dv.tasks, dv.execs, dv.bad_pipe,
                   dv.bad_sockpair, dv.first_bad[0] ? "; first: " : "", dv.first_bad);
            CHECK(ran("DCLRUN"), "DCL's RUN activated the image and it ran");
            CHECK(dv.tasks >= 1 && dv.execs >= 2,
                  "the trace is not vacuous: DCL took the fork path (an image task, exec'd)");
            CHECK(dv.sync_ioctls >= 7,
                  "DCL and its image task met in the executive's creation record (VMS_IOCTL_CREPRC_SYNC)");
            /* negctl: dcl-image-fork-host-pipe */
            CHECK(dv.bad_pipe == 0,
                  "DCL's fork-path RUN made NO host pipe (pipe/pipe2) -- DCL or its image task");
        }
    }

    printf("=== test_syssvc_creprc_nopipe_audit: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
