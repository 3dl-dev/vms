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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>
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

int main(void)
{
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
    /* negctl: spawn-arm-gone-subprocess-not-completed */
    CHECK(lost == 0, "every /NOWAIT lib$spawn of an instantly-finishing command set its completion event flag");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
