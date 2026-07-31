/*
 * test_syssvc_startup_service.c - a STARTUP PROCEDURE creates a named
 * DETACHED process, and a DIFFERENT process finds it in SHOW SYSTEM
 * (vms-47b).
 *
 * WHAT THIS SUITE IS FOR, AND WHAT IT REFUSES TO ACCEPT AS PROOF.
 *
 * tests/qemu/test_syssvc_procnam.c proves $CREPRC can name a SUBPROCESS in
 * the executive's table. This suite proves the thing a system actually does
 * with that: a service, started the VMS way -- STARTUP.COM invokes
 * SYSTARTUP_VMS.COM, which invokes a service startup procedure, which runs
 * RUN/DETACHED/PROCESS_NAME -- exists as a real detached process that
 * OUTLIVES the DCL that created it and that ANY other process can find BY
 * NAME.
 *
 * Every assertion below is A-WRITES / B-READS (CLAUDE.md Rule 11):
 *
 *   A = a DCL.EXE running the startup procedure. It EXITS before anything
 *       is observed, so nothing it says about itself can be the evidence.
 *   B = a SECOND DCL.EXE running the user-visible command SHOW SYSTEM,
 *       plus this test process reading the executive through the public
 *       sys$/vms_kif API.
 *
 * The refutation this arrangement exists to perform: a process name carried
 * in the environment, or in the creating process's own memory, passes every
 * single-process test and is invisible to B. That was the rejected
 * VMS_PRCNAM "fix" (CLAUDE.md Rule 10, worked example 2). The service here
 * is /bin/sh -- busybox, which knows nothing about VMS and never opens
 * /dev/vms -- so a name B can see can only have come from the executive.
 *
 * WHAT IS NOT ASSERTED HERE: that OVMX ships a service. It does not, and
 * SYS$MANAGER:SYSTARTUP_VMS.COM deliberately starts none (shipping a
 * procedure for an image that is never built would be its own facade). The
 * procedures below are this test's own fixtures, in exactly the shape the
 * shipped SYSTARTUP_VMS.COM documents.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms, and DCL.EXE staged at
 * /bin/DCL.EXE by tests/qemu/Dockerfile. With no /dev/vms -- which happens
 * ONLY in the CI negative-control rig, never in the product (vms-0ff: PID 1
 * refuses to boot without the executive) -- it checks that nothing reports
 * a fabricated success and exits EXIT_SKIP (77), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "prcdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                                 \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }  \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }  \
    } while (0)

/* The service's VMS process name. 11 characters, inside the 1-15 VMS
 * range pinned in src/kernel/vms_ioctl.h. */
#define SVC_NAME        "OVMX47BSVC"
/* The name used by the wait()-behaviour probe, which calls $CREPRC
 * directly rather than through DCL. Distinct so the two cannot be
 * confused for one another in the table. */
#define PROBE_NAME      "OVMX47BPRB"

#define HOLD_SCRIPT     "/tmp/ovmx47b_hold.sh"
#define SVC_LOG         "/tmp/ovmx47b_svc.log"
#define SVC_STARTUP_COM "/tmp/OVMX47B_SVC_STARTUP.COM"
#define SYSTARTUP_COM   "/tmp/OVMX47B_SYSTARTUP.COM"
#define SUBJECT_IMAGE   "/bin/sh"
#define DCL_IMAGE       "/bin/DCL.EXE"

/* ------------------------------------------------------------------ */

static struct dsc$descriptor_s str_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int write_file(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fputs(text, f);
    fclose(f);
    chmod(path, mode);
    return 0;
}

/*
 * The fixtures, in the shape SYS$MANAGER:SYSTARTUP_VMS.COM documents.
 *
 * The image paths are quoted because a bare Linux path would be lexed as a
 * DCL qualifier -- on the product's system disk these are VMS filespecs
 * (SYS$SYSTEM:<image>.EXE) and need no quoting. That is a property of this
 * rig's initramfs, which has no mounted system disk, not of RUN/DETACHED.
 */
static int write_fixtures(void)
{
    /* The service's "work": block, so it can be observed. `sleep` is a
     * busybox applet installed by init.sh. Nothing here is paced by a
     * fixed sleep in the TEST -- this is the subject staying alive, and
     * every observation below is of an event that has already happened. */
    if (write_file(HOLD_SCRIPT, "sleep 600\n", 0644) != 0)
        return -1;

    if (write_file(SVC_STARTUP_COM,
                   "$! Service startup procedure (test fixture, vms-47b)\n"
                   "$ RUN/DETACHED"
                   "/PROCESS_NAME=" SVC_NAME
                   "/INPUT=\"" HOLD_SCRIPT "\""
                   "/OUTPUT=\"" SVC_LOG "\""
                   "/ERROR=\"" SVC_LOG "\""
                   " \"" SUBJECT_IMAGE "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* Stands in for SYS$MANAGER:SYSTARTUP_VMS.COM: the site procedure
     * that invokes each service's own startup procedure. */
    if (write_file(SYSTARTUP_COM,
                   "$! Site-specific startup (test fixture, vms-47b)\n"
                   "$ @\"" SVC_STARTUP_COM "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    return 0;
}

/*
 * run_dcl - run a DCL command procedure to completion in its OWN process,
 * capturing everything it wrote.
 *
 * This is process A. It is a separate image activation of the real
 * DCL.EXE, exactly as STARTUP.EXE runs STARTUP.COM, and this function does
 * not return until it has EXITED -- so every later observation is of a
 * process whose creator is gone.
 *
 * Returns the creator's pid (already reaped) or -1; its exit status goes
 * in *exit_st.
 */
static pid_t run_dcl(const char *procedure, char *out, size_t outsz,
                     int *exit_st)
{
    int pfd[2];

    out[0] = '\0';
    if (exit_st) *exit_st = -1;
    if (pipe(pfd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }

    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execl(DCL_IMAGE, "DCL.EXE", procedure, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(pfd[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(pfd[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (exit_st) *exit_st = st;
    return pid;
}

/*
 * run_show_system - run the REAL "SHOW SYSTEM" through the REAL DCL.EXE.
 *
 * This is process B. SHOW SYSTEM is a READER of the executive's process
 * table, and this is the only environment where that can be proven: ctest
 * never runs anywhere a process table exists (CLAUDE.md Rule 9).
 */
static int run_show_system(char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(DCL_IMAGE, "DCL.EXE", (char *)NULL);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    const char *script = "SHOW SYSTEM\nLOGOUT\n";
    ssize_t w = write(in_pipe[1], script, strlen(script));
    (void)w;
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/*
 * row_for - the SHOW SYSTEM row naming `name`, or NULL.
 *
 * A row is " %08X %-15s  %s": pid, process name, CPU. Matching is anchored
 * on the name column so a name appearing in a banner or in another column
 * cannot be mistaken for a row. Same reader as
 * tests/qemu/test_syssvc_procnam.c.
 */
static const char *row_for(const char *text, const char *name)
{
    const char *line = text;
    size_t namelen = strlen(name);

    while (line && *line) {
        if (line[0] == ' ') {
            int i;
            for (i = 1; i < 9; i++)
                if (!isxdigit((unsigned char)line[i]))
                    break;
            if (i == 9 && line[9] == ' ' &&
                strncmp(line + 10, name, namelen) == 0)
                return line;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

/*
 * proc_id_of - the process ID a %RUN-S-PROC_ID line reports.
 *
 * ORACLE-PINNED (reference lab VAX1, OpenVMS VAX V7.3): RUN/DETACHED
 * announces the created process as
 *   %RUN-S-PROC_ID, identification of created process is 20200214
 * Returns 0 if no such line is present, and sets *count to how many were.
 */
#define PROC_ID_PREFIX "%RUN-S-PROC_ID, identification of created process is "

static uint32_t proc_id_of(const char *text, int *count)
{
    uint32_t first = 0;
    int n = 0;
    const char *p = text;

    while ((p = strstr(p, PROC_ID_PREFIX)) != NULL) {
        p += strlen(PROC_ID_PREFIX);
        if (n == 0)
            first = (uint32_t)strtoul(p, NULL, 16);
        n++;
    }
    if (count) *count = n;
    return first;
}

/*
 * ppid_of - the parent pid Linux records for a task, or -1.
 *
 * A detached process is reparented away from its creator, so this is how
 * "the creator is not its parent" is observed from OUTSIDE both of them.
 */
static long ppid_of(uint32_t lpid)
{
    char path[64], line[256];
    FILE *f;
    long ppid = -1;

    snprintf(path, sizeof(path), "/proc/%u/status", (unsigned)lpid);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            ppid = strtol(line + 5, NULL, 10);
            break;
        }
    }
    fclose(f);
    return ppid;
}

/*
 * The wait()-behaviour probe.
 *
 * "Detached" is not a decoration on the process table: the creator is not
 * the created process's parent and CANNOT wait on it. That is only
 * observable from inside the creator, so this runs as its own child of the
 * test -- a process with no other children at all, so wait() has exactly
 * one thing to say.
 *
 * It calls $CREPRC directly rather than through DCL because the property
 * belongs to $CREPRC's PRC$M_DETACH contract, not to the command.
 */
struct probe_report {
    uint32_t status;      /* what $CREPRC returned */
    uint32_t vms_pid;     /* the executive-assigned process ID */
    int      wait_rc;     /* wait(2) return value */
    int      wait_errno;
};

static void detach_probe(int fd)
{
    struct probe_report rep;
    struct dsc$descriptor_s img = str_dsc(SUBJECT_IMAGE);
    struct dsc$descriptor_s in  = str_dsc(HOLD_SCRIPT);
    struct dsc$descriptor_s nd  = str_dsc(PROBE_NAME);

    memset(&rep, 0, sizeof(rep));
    rep.status = sys$creprc(&rep.vms_pid, &img, &in, NULL, NULL, NULL, NULL,
                            &nd, 0, 0, 0, PRC$M_DETACH);

    /* The whole question: after creating a DETACHED process, does this
     * process have a child to wait for? WNOHANG is deliberate -- a
     * blocking wait() would hang the suite for the service's lifetime on
     * exactly the defect this is looking for, and a hang is not a
     * verdict. ECHILD is the answer that means "detached". */
    errno = 0;
    rep.wait_rc = (int)waitpid(-1, NULL, WNOHANG);
    rep.wait_errno = errno;

    ssize_t w = write(fd, &rep, sizeof(rep));
    (void)w;
    _exit(0);
}

/*
 * Checks that run when there is NO executive. Nothing may report success.
 */
static int device_absent_checks(void)
{
    struct vms_procinfo info;
    uint32_t st;

    printf("  (no /dev/vms: checking that nothing fabricates a success)\n");

    st = vms_kif_getjpi_prcnam(SVC_NAME, &info);
    CHECK(!(st & 1),
          "no process resolves by name with no executive");

    {
        struct dsc$descriptor_s img = str_dsc(SUBJECT_IMAGE);
        struct dsc$descriptor_s nd  = str_dsc(SVC_NAME);
        uint32_t pid = 0;
        st = sys$creprc(&pid, &img, NULL, NULL, NULL, NULL, NULL, &nd,
                        0, 0, 0, PRC$M_DETACH);
        CHECK(!(st & 1),
              "sys$creprc PRC$M_DETACH does not report success with no executive");
        CHECK(pid == 0,
              "a failed detached creation hands back no process ID");
    }

    printf("=== test_syssvc_startup_service: %d passed, %d failed "
           "(SKIPPED: no /dev/vms) ===\n", pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    char out[65536];
    char show[65536];
    struct vms_procinfo info;
    uint32_t svc_vms_pid = 0, svc_lpid = 0;
    uint32_t announced = 0;
    int announced_n = 0;
    int row_found = 0;
    pid_t creator = -1;
    int exit_st = 0;
    uint32_t st;

    printf("=== test_syssvc_startup_service: a startup procedure creates a "
           "named detached process (vms-47b) ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    if (access(DCL_IMAGE, X_OK) != 0) {
        printf("  FAIL: %s is not present -- the user-visible command "
               "cannot be exercised\n", DCL_IMAGE);
        printf("=== test_syssvc_startup_service: 0 passed, 1 failed ===\n");
        return 1;
    }

    if (write_fixtures() != 0) {
        printf("  FAIL: cannot write the startup-procedure fixtures\n");
        printf("=== test_syssvc_startup_service: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* ---------------------------------------------------------------
     * P1. THE STARTUP PROCEDURE RUNS, AND RETURNS.
     *
     * A site procedure invokes a service startup procedure, which runs
     * RUN/DETACHED. DCL must NOT wait for the service -- if it did, this
     * call would not come back for ten minutes -- and it must report the
     * created process the way VMS does.
     * --------------------------------------------------------------- */
    creator = run_dcl(SYSTARTUP_COM, out, sizeof(out), &exit_st);
    CHECK(creator > 0 && WIFEXITED(exit_st),
          "the startup procedure ran to completion and its DCL exited");
    if (out[0])
        printf("  (startup procedure output)\n%s\n", out);

    announced = proc_id_of(out, &announced_n);
    CHECK(announced_n == 1 && announced != 0,
          "the startup procedure announced the created process with %RUN-S-PROC_ID");

    /* ---------------------------------------------------------------
     * P2. THE SERVICE OUTLIVED ITS CREATOR, AND THE EXECUTIVE HAS IT.
     *
     * The DCL that created it has already exited and been reaped above.
     * This lookup is by NAME, from a process that did not create the
     * service and shares no memory with the one that did.
     * --------------------------------------------------------------- */
    memset(&info, 0, sizeof(info));
    st = vms_kif_getjpi_prcnam(SVC_NAME, &info);
    CHECK(st == SS$_NORMAL,
          "the executive resolves the service BY NAME after its creator exited");

    if (st == SS$_NORMAL) {
        svc_vms_pid = info.vms_pid;
        svc_lpid = info.linux_pid;

        /* The process ID the startup procedure printed is the
         * EXECUTIVE's, not a number DCL invented: they are the same
         * value, and only the executive assigns it. */
        CHECK(svc_vms_pid == announced,
              "the process ID the startup procedure printed is the executive's own");

        /* It is a live process, not a stale row. */
        CHECK(svc_lpid != 0 && kill((pid_t)svc_lpid, 0) == 0,
              "the service is still running after the procedure that created it exited");

        /* Detached: reparented away from its creator. The creator is
         * gone, so the parent must be init -- and in particular must NOT
         * be the DCL that created it. */
        {
            long ppid = ppid_of(svc_lpid);
            printf("  (service linux pid %u, ppid %ld; its creator was pid %ld)\n",
                   (unsigned)svc_lpid, ppid, (long)creator);
            CHECK(ppid == 1 && ppid != (long)creator,
                  "the service was reparented away from the DCL that created it");
        }
    }

    /* ---------------------------------------------------------------
     * P3. THE USER-VISIBLE COMMAND, IN A THIRD PROCESS.
     *
     * THIS IS THE ITEM'S DONE CONDITION. A separate DCL.EXE runs SHOW
     * SYSTEM and lists the service by the name the startup procedure
     * asked for. A per-process name -- one carried in the environment or
     * in the creator's memory -- cannot appear here.
     * --------------------------------------------------------------- */
    if (run_show_system(show, sizeof(show)) != 0) {
        CHECK(0, "SHOW SYSTEM ran under DCL.EXE");
    } else {
        const char *row = row_for(show, SVC_NAME);
        row_found = (row != NULL);
        if (!row_found)
            printf("  (SHOW SYSTEM output follows)\n%s\n", show);
        CHECK(row_found,
              "SHOW SYSTEM, in a different process, lists the service by its VMS process name");
    }

    /* ---------------------------------------------------------------
     * P4. A SECOND START OF THE SAME SERVICE IS REFUSED TO THE CALLER.
     *
     * ORACLE-PINNED (reference lab VAX1, OpenVMS VAX V7.3, 2026-07-30;
     * transcript quoted in tests/qemu/test_kmod_procnam.c): a third
     * detached process taking a PROCESS_NAME already held in the same
     * UIC group is refused with
     *
     *     %RUN-F-CREPRC, process creation failed
     *     -SYSTEM-F-DUPLNAM, duplicate name
     *
     * This is what makes the name an identity rather than a label: a
     * startup procedure run twice cannot produce two services under one
     * name, and the SECOND caller is the one that is told.
     * --------------------------------------------------------------- */
    {
        char out2[65536];
        int n2 = 0;
        run_dcl(SYSTARTUP_COM, out2, sizeof(out2), &exit_st);
        printf("  (second start of the same service)\n%s\n", out2);

        int refused = (strstr(out2, "%RUN-F-CREPRC, process creation failed")
                       != NULL) &&
                      (strstr(out2, "-SYSTEM-F-DUPLNAM, duplicate name")
                       != NULL);
        CHECK(refused,
              "starting the same named service twice is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM");

        if (refused) {
            (void)proc_id_of(out2, &n2);
            CHECK(n2 == 0,
                  "the refused start announced no process");
        }
    }

    /* ---------------------------------------------------------------
     * P5. THE CREATOR CANNOT WAIT ON A DETACHED PROCESS.
     *
     * The difference between a detached process and a subprocess, seen
     * from the only place it is visible: inside the creator. After
     * $CREPRC returns, a creator with no other children has NOTHING to
     * wait for -- the created process is not in its job tree.
     *
     * The probe reports the process ID the executive gave it, and THIS
     * process (which did not create it) then resolves that row by name.
     * A creator that "detached" a process by simply forgetting about it
     * would satisfy neither half.
     * --------------------------------------------------------------- */
    {
        int pfd[2];
        struct probe_report rep;

        memset(&rep, 0, sizeof(rep));
        if (pipe(pfd) < 0) {
            CHECK(0, "the detached-creation probe could be started");
        } else {
            pid_t probe = fork();
            if (probe < 0) {
                close(pfd[0]); close(pfd[1]);
                CHECK(0, "the detached-creation probe could be started");
            } else if (probe == 0) {
                close(pfd[0]);
                detach_probe(pfd[1]);
                _exit(0);   /* not reached */
            } else {
                ssize_t r;
                int pst;

                close(pfd[1]);
                r = read(pfd[0], &rep, sizeof(rep));
                close(pfd[0]);
                while (waitpid(probe, &pst, 0) < 0 && errno == EINTR)
                    ;

                CHECK(r == (ssize_t)sizeof(rep) && (rep.status & 1),
                      "sys$creprc PRC$M_DETACH created the probe's process");

                if (r == (ssize_t)sizeof(rep) && (rep.status & 1)) {
                    printf("  (probe: wait() = %d, errno = %d; vms pid %08X)\n",
                           rep.wait_rc, rep.wait_errno,
                           (unsigned)rep.vms_pid);
                    CHECK(rep.wait_rc == -1 && rep.wait_errno == ECHILD,
                          "the creator of a detached process has no child to wait for");

                    memset(&info, 0, sizeof(info));
                    st = vms_kif_getjpi_prcnam(PROBE_NAME, &info);
                    CHECK(st == SS$_NORMAL && info.vms_pid == rep.vms_pid,
                          "a process that did NOT create it resolves the detached process by name");

                    if (st == SS$_NORMAL && info.linux_pid)
                        kill((pid_t)info.linux_pid, SIGKILL);
                }
            }
        }
    }

    /* ---------------------------------------------------------------
     * P6. THE NAME BELONGS TO THE LIVE PROCESS, NOT TO THE TABLE.
     *
     * Killing the service releases its name -- so what SHOW SYSTEM
     * listed was a running process, not a record left behind by the
     * procedure that created it.
     * --------------------------------------------------------------- */
    if (svc_lpid) {
        kill((pid_t)svc_lpid, SIGKILL);

        /* Wait for the row to go, bounded, without a fixed sleep pacing
         * the test: the executive reaps a dead row when it is next
         * asked, so this polls the OBSERVABLE rather than guessing how
         * long an emulated guest takes to reap a task. */
        int gone = 0;
        for (int i = 0; i < 2000; i++) {
            memset(&info, 0, sizeof(info));
            if (vms_kif_getjpi_prcnam(SVC_NAME, &info) == SS$_NONEXPR) {
                gone = 1;
                break;
            }
            usleep(1000);
        }
        CHECK(gone,
              "the service's name is released when the service dies");
    }

    unlink(HOLD_SCRIPT);
    unlink(SVC_STARTUP_COM);
    unlink(SYSTARTUP_COM);
    unlink(SVC_LOG);

    printf("=== test_syssvc_startup_service: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
