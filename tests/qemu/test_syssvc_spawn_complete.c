/*
 * test_syssvc_spawn_complete.c - a /NOWAIT lib$spawn completion is NEVER lost, even
 * when the subprocess finishes (and is reclaimed) before the arm runs (vms-f45).
 *
 * THE BUG. lib$spawn(CLI$M_NOWAIT, efn) arms the completion in the executive AFTER
 * $CREPRC returns. A subprocess whose command ends at once (a DCL that reads an
 * empty/unreadable SYS$INPUT and exits) can be dead AND reclaimed by the executive's
 * lazy reaper before the arm ioctl looks it up; the ioctl then answered SS$_NONEXPR,
 * lib$spawn ignored it, and the caller's $WAITFR on its completion event flag hung
 * forever (corpus sys_forcex: rc=124 on ~half the boots of a loaded rail).
 *
 * THE PROPERTY. Whatever the subprocess's lifetime, the caller's completion event flag
 * is set. This suite spawns a command that ends immediately, N times, and requires the
 * flag to be set within a bounded wait every time. It can fail: against the unfixed
 * lib$spawn, any iteration that loses the race leaves the flag clear.
 *
 * Requires a real, insmod'd vms.ko; without one it SKIPs (77), never a fake pass.
 */
#define _GNU_SOURCE 1   /* sched_setaffinity, CPU_SET */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <strings.h>
#include <errno.h>
#include <pthread.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "clidef.h"
#include "starlet.h"
#include "vms_kif.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

#define EXIT_SKIP 77
/* 200: a lost completion was seen ONCE in 30 when $CREPRC's forked child took a
 * userspace lock before exec (rd vms-003b); the loop has to be long enough to
 * catch a 1-in-30 race with near certainty. */
#define ITERATIONS 200
#define COMPLETION_EFN 11
#define WAIT_MS 20000

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static struct dsc$descriptor_s dsc(const char *s)
{
    struct dsc$descriptor_s d = { (unsigned short)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)s };
    return d;
}

/*
 * THE LEADER EXITS FIRST (rd vms-003b). A multithreaded subprocess whose main
 * thread ends before another thread -- DCL with its SYS$INPUT reader thread --
 * leaves its group leader a zombie while the LAST thread drops /dev/vms. The
 * executive must still see the process end and deliver the creator's armed
 * completion. Deterministic: the child's main thread pthread_exit()s at once and
 * a second thread ends the process 200 ms later.
 */
static void *leader_gone_tail(void *v)
{
    (void)v;
    struct pollfd nothing = { .fd = -1, .events = 0 };
    poll(&nothing, 1, 200);
    exit(0);
}

/* Fork a registered child process whose leader exits first; arm the completion
 * on it, release it, and wait for the flag. Returns 1 set, 0 lost, -1 setup. */
static int leader_first_completion(uint32_t efn)
{
    int up[2], go[2];
    if (pipe(up) != 0 || pipe(go) != 0) return -1;
    pid_t lp = fork();
    if (lp < 0) return -1;
    if (lp == 0) {
        close(up[0]); close(go[1]);
        /* Its OWN /dev/vms (not the creator's shared one) and its own PCB. */
        vms_kif_close();
        uint32_t me = 0;
        if (vms_kif_open() < 0 || !(vms_kif_register(&me) & 1)) me = 0;
        (void)write(up[1], &me, sizeof me);
        char c;
        (void)read(go[0], &c, 1);
        pthread_t t;
        if (pthread_create(&t, NULL, leader_gone_tail, NULL) != 0) _exit(2);
        pthread_exit(NULL);               /* the leader goes first */
    }
    close(up[1]); close(go[0]);
    uint32_t child = 0;
    int ok = read(up[0], &child, sizeof child) == (ssize_t)sizeof child && child != 0;
    int done = 0;
    (void)sys$clref(efn);
    if (ok)
        ok = (vms_kif_spawn_notify(child, efn, 0, 0, &done) & 1) != 0;
    (void)write(go[1], "g", 1);
    close(up[0]); close(go[1]);
    int set = -1;
    if (ok) {
        set = 0;
        for (int waited = 0; waited < 10000 && !set; waited += 50) {
            set = (sys$readef(efn, &(uint32_t){0}) == SS$_WASSET);
            if (!set) { struct pollfd n = { .fd = -1, .events = 0 }; poll(&n, 1, 50); }
        }
    }
    int ws;
    while (waitpid(lp, &ws, 0) < 0 && errno == EINTR)
        ;
    return set;
}

/*
 * THE ARM FINDS THE SUBPROCESS ALREADY GONE (vms-f45), made deterministic. The
 * CLI lib$spawn runs is SYS$SYSTEM:DCL.EXE; here SYS$SYSTEM names a directory
 * whose DCL.EXE is this program, which -- started under that name -- opens its
 * own /dev/vms and exits at once. The creator runs SCHED_FIFO on the guest's one
 * CPU, so once it blocks in $CREPRC the subprocess runs to its end (and the
 * executive deletes it) before the creator is scheduled again to arm the
 * completion: the arm always answers SS$_NONEXPR, and lib$spawn must complete
 * the request itself.
 */
#define STUB_DIR "/tmp/ovmxstub"
static int stub_cli_main(void)
{
    struct vms_procinfo me;
    if (vms_kif_open() >= 0)
        (void)vms_kif_getjpi_self(&me);      /* its own /dev/vms file */
    return 0;
}

static int gone_before_arm(lnm_manager_t *mgr, const char *self_exe, uint32_t efn)
{
    char link[256];
    (void)mkdir(STUB_DIR, 0755);
    snprintf(link, sizeof link, "%s/DCL.EXE", STUB_DIR);
    (void)unlink(link);
    if (symlink(self_exe, link) != 0) { printf("  INFO: symlink %s -> %s failed\n", link, self_exe); return -1; }
    snprintf(link, sizeof link, "%s/dcl.exe", STUB_DIR);
    (void)unlink(link);
    (void)symlink(self_exe, link);
    if (!(lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM", STUB_DIR, 0, LNM_MODE_SUPER) & 1)) {
        printf("  INFO: redefining SYS$SYSTEM failed\n");
        return -1;
    }
    struct sched_param sp = { .sched_priority = 1 };
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(0, &one);
    /* musl's sched_setscheduler() is a stub (ENOSYS, by design); the policy is
     * set with the system call itself. */
    int rt = sched_setaffinity(0, sizeof one, &one) == 0 &&
             syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &sp) == 0;
    int set = -1;
    if (!rt)
        printf("  INFO: SCHED_FIFO/affinity refused (errno %d)\n", errno);
    if (rt) {
        struct dsc$descriptor_s cmd = dsc("EXIT");
        uint32_t flags = CLI$M_NOWAIT, e = efn, pid = 0;
        (void)sys$clref(efn);
        uint32_t r = lib$spawn(&cmd, NULL, NULL, &flags, NULL, &pid, NULL, &e,
                               NULL, NULL, NULL, NULL, NULL);
        struct sched_param np = { .sched_priority = 0 };
        (void)syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &np);
        if (!(r & 1))
            printf("  INFO: lib$spawn of the stub CLI returned %08x\n", (unsigned)r);
        if (r & 1) {
            set = 0;
            for (int waited = 0; waited < 5000 && !set; waited += 50) {
                set = (sys$readef(efn, &(uint32_t){0}) == SS$_WASSET);
                if (!set) { struct pollfd n = { .fd = -1, .events = 0 }; poll(&n, 1, 50); }
            }
            struct vms_procinfo pi;
            if (set && (vms_kif_getjpi_pid(pid, &pi) & 1))
                set = 2;                   /* not gone after all: not this case */
        }
    }
    (void)lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM", "/bin", 0, LNM_MODE_SUPER);
    return set;
}

/*
 * AN IMAGE THAT NEVER TOUCHES THE EXECUTIVE STILL ENDS AS A VMS PROCESS (rd
 * vms-d9ab / vms-9f32). A registered child exec()s an image that never opens
 * /dev/vms (its registration's descriptor is close-on-exec, so the image holds
 * none) and exits. The executive must still delete the process and complete the
 * creator's armed /NOWAIT flag, and the PID must be gone ($GETJPI SS$_NONEXPR)
 * BEFORE the creator reaps the zombie -- a deleted process cannot be read, as
 * on VMS. Deterministic: the arm happens before the child is let go.
 */
static int noexec_image_completion(uint32_t efn, uint32_t *after_jpi)
{
    int up[2], go[2];
    *after_jpi = 0;
    if (pipe(up) != 0 || pipe(go) != 0) return -1;
    pid_t lp = fork();
    if (lp < 0) return -1;
    if (lp == 0) {
        close(up[0]); close(go[1]);
        vms_kif_close();
        uint32_t me = 0;
        if (vms_kif_open() < 0 || !(vms_kif_register(&me) & 1)) me = 0;
        (void)write(up[1], &me, sizeof me);
        char c;
        (void)read(go[0], &c, 1);
        execl("/proc/self/exe", "d9ab-noexec-image", (char *)NULL);
        _exit(3);
    }
    close(up[1]); close(go[0]);
    uint32_t child = 0;
    int ok = read(up[0], &child, sizeof child) == (ssize_t)sizeof child && child != 0;
    int done = 0;
    (void)sys$clref(efn);
    if (ok)
        ok = (vms_kif_spawn_notify(child, efn, 0, 0, &done) & 1) != 0;
    (void)write(go[1], "g", 1);
    close(up[0]); close(go[1]);
    int set = -1;
    if (ok) {
        set = 0;
        for (int waited = 0; waited < 10000 && !set; waited += 50) {
            set = (sys$readef(efn, &(uint32_t){0}) == SS$_WASSET);
            if (!set) { struct pollfd n = { .fd = -1, .events = 0 }; poll(&n, 1, 50); }
        }
        struct vms_procinfo pi;
        memset(&pi, 0, sizeof pi);
        *after_jpi = vms_kif_getjpi_pid(child, &pi);   /* before waitpid */
    }
    int ws;
    while (waitpid(lp, &ws, 0) < 0 && errno == EINTR)
        ;
    return set;
}

/*
 * ARMED AFTER THE SUBPROCESS IS ALREADY GONE (rd vms-f45 / vms-9f32). A
 * registered child ends and is deleted (its PID already answers SS$_NONEXPR)
 * before the creator arms its /NOWAIT completion. The executive kept the
 * child's termination record in the creator's PCB, so the arm completes at
 * once -- the creator's flag is set, never a lost completion.
 */
static int gone_child_arm(uint32_t efn)
{
    int up[2];
    if (pipe(up) != 0) return -1;
    pid_t lp = fork();
    if (lp < 0) return -1;
    if (lp == 0) {
        close(up[0]);
        vms_kif_close();
        uint32_t me = 0;
        if (vms_kif_open() < 0 || !(vms_kif_register(&me) & 1)) me = 0;
        (void)write(up[1], &me, sizeof me);
        _exit(0);
    }
    close(up[1]);
    uint32_t child = 0;
    int ok = read(up[0], &child, sizeof child) == (ssize_t)sizeof child && child != 0;
    close(up[0]);
    int gone = 0;
    for (int waited = 0; ok && waited < 10000 && !gone; waited += 20) {
        struct vms_procinfo pi;
        gone = vms_kif_getjpi_pid(child, &pi) == SS$_NONEXPR;
        if (!gone) { struct pollfd n = { .fd = -1, .events = 0 }; poll(&n, 1, 20); }
    }
    int set = -1;
    if (gone) {
        int done = 0;
        (void)sys$clref(efn);
        uint32_t st = vms_kif_spawn_notify(child, efn, 0, 0, &done);
        set = (st & 1) && done && sys$readef(efn, &(uint32_t){0}) == SS$_WASSET;
    }
    int ws;
    while (waitpid(lp, &ws, 0) < 0 && errno == EINTR)
        ;
    return set;
}

int main(int argc, char **argv)
{
    {
        const char *b = strrchr(argv[0], '/');
        b = b ? b + 1 : argv[0];
        if (argc >= 1 && strcasecmp(b, "DCL.EXE") == 0)
            return stub_cli_main();       /* started by lib$spawn as the CLI */
        if (strcmp(b, "d9ab-noexec-image") == 0)
            return 0;                     /* an image that never opens /dev/vms */
    }
    printf("=== test_syssvc_spawn_complete (vms-f45: /NOWAIT completion is never lost) ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    close(devfd);
    if (access("/bin/DCL.EXE", X_OK) != 0) { printf("  SKIP: no /bin/DCL.EXE staged\n"); return EXIT_SKIP; }

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  SKIP: no lnm manager\n"); return EXIT_SKIP; }
    CHECK(lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM", "/bin", 0, LNM_MODE_SUPER) & 1,
          "define SYS$SYSTEM -> /bin (stage DCL.EXE for lib$spawn)");
    if (!(vms_kif_open() >= 0 && (vms_kif_register(NULL) & 1))) { printf("  FAIL: register\n"); return 1; }

    {
        char self[256];
        ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
        int r = -1;
        if (sl > 0) { self[sl] = 0; r = gone_before_arm(mgr, self, COMPLETION_EFN); }
        CHECK(r == 0 || r == 1, "a SCHED_FIFO creator spawns an instantly-ending CLI on one CPU");
        CHECK(r != 2, "the instantly-ending subprocess is already deleted when its completion is armed");
        CHECK(r == 1, "a /NOWAIT lib$spawn whose subprocess is already gone when the arm runs"
                      " still sets the creator's completion event flag");
    }
    {
        int r = gone_child_arm(COMPLETION_EFN);
        CHECK(r != -1, "a registered child ends and its PID is gone before the creator arms");
        /* negctl: spawn-arm-gone-subprocess-not-completed */
        CHECK(r == 1, "arming the completion of an already-deleted subprocess completes at once:"
                      " the creator's flag is set from the termination record the executive kept");
    }
    {
        uint32_t jpi = 0;
        int r = noexec_image_completion(COMPLETION_EFN, &jpi);
        CHECK(r != -1, "a registered child is armed and then exec()s an image that never opens /dev/vms");
        /* negctl: process-exit-deletion-needs-vms-fd */
        CHECK(r == 1, "that image ending completes the creator's armed /NOWAIT flag: the executive"
                      " deletes the process whatever the image did (vms-d9ab)");
        /* negctl-knockon: process-exit-deletion-needs-vms-fd */
        CHECK(jpi == SS$_NONEXPR, "$GETJPI of the ended process is SS$_NONEXPR before its creator"
                                  " reaps it: a deleted process cannot be read");
    }
    {
        int r = leader_first_completion(COMPLETION_EFN);
        CHECK(r != -1, "a registered child process with its own /dev/vms is created and armed");
        /* negctl: release-leader-zombie-pcb-kept */
        CHECK(r == 1, "a subprocess whose main thread exits before its last thread still completes:"
                      " the creator's armed completion event flag is set");
    }

    int lost = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        struct dsc$descriptor_s cmd = dsc("EXIT");
        uint32_t flags = CLI$M_NOWAIT, efn = COMPLETION_EFN, pid = 0;
        (void)sys$clref(efn);
        uint32_t r = lib$spawn(&cmd, NULL, NULL, &flags, NULL, &pid, NULL, &efn,
                               NULL, NULL, NULL, NULL, NULL);
        if (!(r & 1)) { printf("  FAIL: iteration %d: lib$spawn returned %08x\n", i, r); fail++; continue; }
        int set = 0;
        for (int waited = 0; waited < WAIT_MS && !set; waited += 50) {
            uint32_t st = sys$readef(efn, &(uint32_t){0});
            set = (st == SS$_WASSET);
            if (!set) { struct pollfd nothing = { .fd = -1, .events = 0 }; poll(&nothing, 1, 50); }
        }
        if (!set) {
            lost++;
            printf("  INFO: iteration %d: completion event flag never set (pid %08x)\n", i, pid);
            break;          /* one lost completion fails the property; stop waiting 20 s each */
        }
    }
    CHECK(lost == 0, "every /NOWAIT lib$spawn of an instantly-finishing command set its completion event flag");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
