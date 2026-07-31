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
 * AND WHAT THE COMMAND REFUSES (P7, P8). RUN's process qualifiers are
 * documented OpenVMS syntax with documented meanings -- quoted verbatim,
 * from the reference lab's own HELP, in src/libvms/include/ovmx_status.h.
 * OVMX honours the /DETACHED form and cannot honour /UIC (the executive
 * derives a process's UIC from Linux credentials, vms-afd) or the
 * subprocess form (OVMX's RUN has none). CLAUDE.md Rule 10 leaves two
 * answers, and a qualifier a user can type cannot be made unreachable, so
 * it must be REFUSED. Those phases assert the refusal is real: the
 * diagnostic names what could not be done, no process is created, the
 * image does not run behind it -- and, as the control that keeps the
 * refusal honest, plain RUN still runs the image.
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

/* The names used by the REFUSAL phases (P7, P8). Nothing may ever be
 * created under either of them -- that is what those phases assert. */
#define UIC_NAME        "OVMX47BUIC"
#define SUBP_NAME       "OVMX47BSUB"

/* P10's names. ABBR_NAME is asked for with an ABBREVIATED qualifier and
 * must be refused (nothing may exist under it); DETABBR_NAME is asked
 * for with the abbreviated spelling real VMS software uses and MUST
 * exist; DETPRIO_NAME is created alongside a qualifier nobody reads. */
#define ABBR_NAME       "OVMX47BABR"
#define DETABBR_NAME    "OVMX47BDTA"
#define DETPRIO_NAME    "OVMX47BDTP"

#define HOLD_SCRIPT     "/tmp/ovmx47b_hold.sh"
#define SVC_LOG         "/tmp/ovmx47b_svc.log"
#define SVC_STARTUP_COM "/tmp/OVMX47B_SVC_STARTUP.COM"
#define SYSTARTUP_COM   "/tmp/OVMX47B_SYSTARTUP.COM"
#define UIC_COM         "/tmp/OVMX47B_UIC.COM"
#define SUBP_COM        "/tmp/OVMX47B_SUBP.COM"
#define PRIO_COM        "/tmp/OVMX47B_PRIO.COM"
#define PLAIN_COM       "/tmp/OVMX47B_PLAIN.COM"
#define NODBG_COM       "/tmp/OVMX47B_NODBG.COM"
#define DEBUG_COM       "/tmp/OVMX47B_DEBUG.COM"
#define ABPRIO_COM      "/tmp/OVMX47B_ABPRIO.COM"
#define ABPROC_COM      "/tmp/OVMX47B_ABPROC.COM"
#define ABNODBG_COM     "/tmp/OVMX47B_ABNODBG.COM"
#define ABDET_COM       "/tmp/OVMX47B_ABDET.COM"
#define DETPRIO_COM     "/tmp/OVMX47B_DETPRIO.COM"
#define AMBIG_COM       "/tmp/OVMX47B_AMBIG.COM"
/* A "did the image run at all?" witness: the script's only job is to
 * leave a file behind, so a refusal that still ran the image cannot hide
 * behind having produced no output. */
#define TOUCH_SCRIPT    "/tmp/ovmx47b_touch.sh"
#define TOUCH_MARK      "/tmp/ovmx47b_touched"
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

    /* The witness image for the refusal phases: it does nothing but
     * prove it ran. */
    if (write_file(TOUCH_SCRIPT,
                   "#!/bin/sh\n"
                   "touch " TOUCH_MARK "\n", 0755) != 0)
        return -1;

    /* P7's procedure: RUN/DETACHED with a UIC the executive cannot be
     * given. Written exactly as an operator would type it, INCLUDING the
     * comma, because the comma is half the defect this refuses. */
    if (write_file(UIC_COM,
                   "$! /UIC refusal (test fixture, vms-47b)\n"
                   "$ RUN/DETACHED/UIC=[300,1]"
                   "/PROCESS_NAME=" UIC_NAME
                   "/INPUT=\"" HOLD_SCRIPT "\""
                   " \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* P8's procedure: process qualifiers WITHOUT /DETACHED. On OpenVMS
     * this asks for a subprocess (HELP RUN Process, quoted in
     * src/libvms/include/ovmx_status.h); OVMX has no subprocess form. */
    if (write_file(SUBP_COM,
                   "$! subprocess-qualifier refusal (test fixture, vms-47b)\n"
                   "$ RUN/PROCESS_NAME=" SUBP_NAME
                   " \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* P8, second case: a process qualifier that is NOT one of the four
     * RUN/DETACHED honours. The oracle's rule is "any of the qualifiers
     * except /UIC or /DETACHED", so refusing a list of four would leave
     * this one silently discarded -- the same defect, narrower. */
    if (write_file(PRIO_COM,
                   "$! non-enumerated process qualifier (test fixture, vms-47b)\n"
                   "$ RUN/PRIORITY=4"
                   " \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* P8's positive control: the SAME image, run by the SAME DCL, with
     * no process qualifier at all. This must still work -- a refusal
     * that swallowed plain RUN would pass every assertion above. */
    if (write_file(PLAIN_COM,
                   "$! plain RUN still runs the image (test fixture, vms-47b)\n"
                   "$ RUN \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* P9's procedures: the OTHER RUN topic. /NODEBUG and /DEBUG are RUN
     * (Image) qualifiers -- `HELP/NOPROMPT RUN Image Qualifier` on the
     * reference lab (VAX1, OpenVMS VAX V7.3, 2026-07-31) lists those
     * two and nothing else -- so neither of them asks for a subprocess,
     * whatever the RUN (Process) topic says about "any of the
     * qualifiers". */
    if (write_file(NODBG_COM,
                   "$! RUN (Image) /NODEBUG still runs the image (vms-47b)\n"
                   "$ RUN/NODEBUG \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    if (write_file(DEBUG_COM,
                   "$! RUN (Image) /DEBUG names the absent debugger (vms-47b)\n"
                   "$ RUN/DEBUG \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* ---------------------------------------------------------------
     * P10's procedures. Every qualifier here is spelled the way an
     * operator spells it and the way real VMS software in this repo
     * spells it -- tests/corpus/tier4-mx/kit/mx_start.com builds
     *   "RUN/AST_LIMIT=100/BUFFER=.../DETACH/PRIV=ALL/PRIO=4/UIC=[1,4]"
     * -- i.e. ABBREVIATED. See P10's own comment for the oracle.
     * --------------------------------------------------------------- */
    if (write_file(ABPRIO_COM,
                   "$! abbreviated process qualifier, no /DETACHED (vms-47b)\n"
                   "$ RUN/PRIO=4"
                   " \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    if (write_file(ABPROC_COM,
                   "$! abbreviated /PROCESS_NAME, no /DETACHED (vms-47b)\n"
                   "$ RUN/PROC=" ABBR_NAME
                   " \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    if (write_file(ABNODBG_COM,
                   "$! abbreviated RUN (Image) qualifier still runs (vms-47b)\n"
                   "$ RUN/NODEB \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* mx_start.com's own spelling of the one form OVMX honours.
     *
     * ALL THREE of /INPUT /OUTPUT /ERROR are given, as the service
     * fixture above gives them, and that is load-bearing rather than
     * tidy: $CREPRC attaches an UNSPECIFIED stream to the null device
     * only on the DETACHED path (src/libvms/syssvc/sys_process.c). The
     * negative control run-detached-not-detached takes that path away,
     * so a stream left unspecified here would be inherited from the
     * DCL -- and the created process holds that pipe open for its
     * whole 600-second life, which would make run_dcl() block instead
     * of returning a verdict. A control that hangs is a flaky gate. */
    if (write_file(ABDET_COM,
                   "$! abbreviated /DETACHED + /PROCESS_NAME (vms-47b)\n"
                   "$ RUN/DETACH"
                   "/PROC=" DETABBR_NAME
                   "/INPUT=\"" HOLD_SCRIPT "\""
                   "/OUTPUT=\"" SVC_LOG "\""
                   "/ERROR=\"" SVC_LOG "\""
                   " \"" SUBJECT_IMAGE "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* The vms-69e case: /DETACHED plus a process qualifier NOTHING in
     * run_detached() reads. */
    if (write_file(DETPRIO_COM,
                   "$! /DETACHED + a qualifier nobody reads (vms-47b/vms-69e)\n"
                   "$ RUN/DETACHED/PRIORITY=4"
                   "/PROCESS_NAME=" DETPRIO_NAME
                   "/INPUT=\"" HOLD_SCRIPT "\""
                   "/OUTPUT=\"" SVC_LOG "\""
                   "/ERROR=\"" SVC_LOG "\""
                   " \"" SUBJECT_IMAGE "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    /* An abbreviation that resolves to more than one RUN qualifier:
     * /PR is PRIORITY, PRIVILEGES and PROCESS_NAME. */
    if (write_file(AMBIG_COM,
                   "$! ambiguous abbreviation (vms-47b)\n"
                   "$ RUN/PR=4"
                   " \"" TOUCH_SCRIPT "\"\n"
                   "$ EXIT\n", 0644) != 0)
        return -1;

    return 0;
}

/*
 * touched - has the witness image run since the last clear_touch()?
 */
static int touched(void)
{
    struct stat st;
    return stat(TOUCH_MARK, &st) == 0;
}

static void clear_touch(void)
{
    unlink(TOUCH_MARK);
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
 * ROW GEOMETRY, ORACLE-PINNED (vms-6a7): docs/oracle/vax73-show-system-process.md
 * Section 1.1. A row is "%08X %-15s %s" -- the pid starts at COLUMN ZERO,
 * because that is where OpenVMS VAX V7.3 puts it (counted through `cat -A`
 * on VAX1). This helper used to expect " %08X ...", the one-column-indented
 * format OVMX printed before the geometry was measured; landing vms-6a7 on
 * main (this suite predates it) shifted the real output out from under this
 * reader, which is why the rebase's positive control failed here: the row
 * WAS present ("1000003D OVMX47BSVC ...") and this helper's leading-space
 * anchor simply stopped matching column 0. Shifted, not loosened -- still
 * anchors on an exact column, matching tests/qemu/test_syssvc_procnam.c's
 * already-fixed reader.
 */
#define ROW_COL_NAME 9          /* Process Name field start */

static const char *row_for(const char *text, const char *name)
{
    const char *line = text;
    size_t namelen = strlen(name);

    while (line && *line) {
        int i;
        for (i = 0; i < 8; i++)
            if (!isxdigit((unsigned char)line[i]))
                break;
        if (i == 8 && line[8] == ' ' &&
            strncmp(line + ROW_COL_NAME, name, namelen) == 0)
            return line;
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

        /*
         * DETACHED -- and which of these actually discriminates it.
         *
         * "ppid == 1" is a NECESSARY consequence of detachment and NOT a
         * test of it. Linux reparents ANY orphan to init, and the
         * creating DCL has already exited and been reaped above, so a
         * $CREPRC that accepted PRC$M_DETACH and discarded it would show
         * ppid 1 here too. That is measured, not argued: the
         * run-detached-not-detached negative control in
         * tests/qemu/facility_defects.sh is exactly that mutation, and
         * this assertion stays green under it. It is kept because it
         * would still catch a service left hanging off a live creator,
         * but it must never be quoted as evidence of detachment.
         *
         * The SESSION is evidence. $CREPRC's PRC$M_DETACH path calls
         * setsid() inside the created task before forking the process
         * that runs the image, so the service is in a session created
         * for it -- not the session this test and the creating DCL share.
         * setsid() is called nowhere else in OVMX (grep: the only other
         * occurrence is in that function's own comment), so a service
         * outside this process's session can only have got there by
         * PRC$M_DETACH being honoured. Under the mutation, this fails.
         *
         * The other discriminator is P5: the creator cannot wait for it.
         */
        {
            long ppid = ppid_of(svc_lpid);
            long svc_sid  = (long)getsid((pid_t)svc_lpid);
            long self_sid = (long)getsid(0);

            printf("  (service linux pid %u, ppid %ld, session %ld; "
                   "its creator was pid %ld, this process is in session %ld)\n",
                   (unsigned)svc_lpid, ppid, svc_sid,
                   (long)creator, self_sid);

            /* self_sid is compared, not required to be positive: in this
             * rig PID 1 is a shell the kernel started directly and never
             * called setsid(), so the whole harness runs in session 0.
             * That is the point -- the service must not be in it. */
            CHECK(svc_sid > 0 && self_sid >= 0 && svc_sid != self_sid,
                  "the service left the session its creator ran in");
            CHECK(ppid == 1 && ppid != (long)creator,
                  "the service's parent is init, not the DCL that created it");
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

    /* ---------------------------------------------------------------
     * P7. A QUALIFIER OVMX CANNOT HONOUR IS REFUSED, NOT DISCARDED.
     *
     * ORACLE-PINNED. HELP RUN Process /UIC on the reference lab (VAX2,
     * OpenVMS VAX V7.3, 2026-07-31) says /UIC "Specifies that the
     * created process be a detached process and assigns it a user
     * identification code (UIC)", and HELP RUN Process /PROCESS_NAME
     * says the process name "is implicitly qualified by the group
     * number of the process's user identification code (UIC)". So /UIC
     * chooses the scope in which the process NAME is unique -- the
     * whole subject of the lab transcript in
     * tests/qemu/test_kmod_procnam.c.
     *
     * OVMX cannot deliver that: the executive derives a process's UIC
     * from Linux credentials, so a UIC handed to $CREPRC changes
     * nothing another process can observe (vms-afd). CLAUDE.md Rule 10
     * gives two answers and the qualifier cannot be made unreachable,
     * so it is REFUSED. What is being tested is that the refusal
     * happens -- that the user is not told a UIC was accepted.
     *
     * The procedure is written with the comma an operator would type.
     * The DCL lexer splits on it, so before this refusal the command
     * reported "%DCL-E-IVIMAGE, image not found - 1]" -- a fragment of
     * the UIC named as a missing image, with the UIC itself never
     * mentioned. That specific symptom is asserted absent.
     * --------------------------------------------------------------- */
    {
        char out7[65536];
        int n7 = 0;

        clear_touch();
        run_dcl(UIC_COM, out7, sizeof(out7), &exit_st);
        printf("  (RUN/DETACHED/UIC=[300,1])\n%s\n", out7);

        CHECK(strstr(out7, "%RUN-F-CREPRC, process creation failed") != NULL &&
              strstr(out7, "-OVMX-F-NOPRCUIC,") != NULL,
              "RUN refuses /UIC with %RUN-F-CREPRC / -OVMX-F-NOPRCUIC");
        CHECK(strstr(out7, "IVIMAGE") == NULL,
              "the refusal names the UIC, not a fragment of it mistaken for an image");

        (void)proc_id_of(out7, &n7);
        CHECK(n7 == 0, "the refused /UIC start announced no process");

        memset(&info, 0, sizeof(info));
        CHECK(vms_kif_getjpi_prcnam(UIC_NAME, &info) != SS$_NORMAL,
              "no process exists in the executive under the refused name");
        CHECK(!touched(),
              "the image was not run behind the refusal");
    }

    /* ---------------------------------------------------------------
     * P8. PROCESS QUALIFIERS WITHOUT /DETACHED ARE REFUSED TOO.
     *
     * ORACLE-PINNED. HELP RUN Process (VAX2, OpenVMS VAX V7.3,
     * 2026-07-31): "A subprocess is created if any of the qualifiers
     * except the /UIC or the /DETACHED qualifier is specified." So
     * RUN/PROCESS_NAME without /DETACHED asks OpenVMS for a SUBPROCESS
     * -- a second process, named in the executive's table. OVMX's RUN
     * has no subprocess form; before this refusal it read the qualifier
     * and threw it away, ran the image in a plain fork()ed child, and
     * told the user nothing.
     *
     * The positive control matters as much as the refusal: the SAME
     * image, run by the SAME DCL with no process qualifier, must still
     * run. A refusal that had swallowed plain RUN would satisfy every
     * "nothing was created" assertion here.
     * --------------------------------------------------------------- */
    {
        char out8[65536];
        int n8 = 0;

        clear_touch();
        run_dcl(SUBP_COM, out8, sizeof(out8), &exit_st);
        printf("  (RUN/PROCESS_NAME with no /DETACHED)\n%s\n", out8);

        CHECK(strstr(out8, "%RUN-F-CREPRC, process creation failed") != NULL &&
              strstr(out8, "-OVMX-F-NOSUBPRC,") != NULL,
              "RUN refuses /PROCESS_NAME without /DETACHED with %RUN-F-CREPRC / -OVMX-F-NOSUBPRC");

        (void)proc_id_of(out8, &n8);
        CHECK(n8 == 0, "the refused subprocess start announced no process");

        memset(&info, 0, sizeof(info));
        CHECK(vms_kif_getjpi_prcnam(SUBP_NAME, &info) != SS$_NORMAL,
              "no process exists in the executive under the refused subprocess name");
        CHECK(!touched(),
              "the image was not run behind the subprocess refusal");

        /* A process qualifier RUN/DETACHED would not have read either.
         * The refusal is on the oracle's set, as the oracle states it,
         * not on a list this code happened to enumerate.
         *
         * NOTE WHAT THIS CASE IS NOT. It carries no /DETACHED, so it
         * says nothing about what happens to /PRIORITY on the DETACHED
         * path -- where it is still read by nobody. That is P10(e),
         * and it is a different case; an adversary correctly pointed
         * out that this assertion's old wording read as if it covered
         * both. */
        clear_touch();
        run_dcl(PRIO_COM, out8, sizeof(out8), &exit_st);
        printf("  (RUN/PRIORITY with no /DETACHED)\n%s\n", out8);
        CHECK(strstr(out8, "-OVMX-F-NOSUBPRC,") != NULL && !touched(),
              "RUN/PRIORITY without /DETACHED is refused too: the rule is the "
              "oracle's set, not a list this code enumerated");

        /* Positive control. */
        clear_touch();
        run_dcl(PLAIN_COM, out8, sizeof(out8), &exit_st);
        CHECK(touched(),
              "plain RUN, with no process qualifier, still runs the image");
    }

    /* ---------------------------------------------------------------
     * P9. THE SUBPROCESS REFUSAL IS SCOPED TO THE PROCESS QUALIFIERS.
     *
     * WHY THIS PHASE EXISTS. P8's refusal was first written over "any
     * qualifier at all", on the strength of the RUN (Process) sentence
     * "A subprocess is created if any of the qualifiers except the /UIC
     * or the /DETACHED qualifier is specified". That sentence is
     * scoped to its OWN topic. The oracle's HELP tree carries a
     * SEPARATE RUN (Image) topic, and `HELP/NOPROMPT RUN Image
     * Qualifier` (VAX1, OpenVMS VAX V7.3, 2026-07-31) lists exactly
     * /DEBUG and /NODEBUG -- qualifiers of the form that "Executes an
     * image within the context of your process", creating no process
     * at all. The unscoped test refused RUN/NODEBUG as a subprocess
     * request and the image did not run: a functional regression, and
     * a message asserting a VMS semantic the oracle contradicts.
     *
     * Both P8 cases are genuine process qualifiers, so nothing in P8
     * could catch it. That is what this phase is for.
     *
     * /NODEBUG matches VMS by doing what VMS does -- running the image.
     * /DEBUG cannot: OVMX has no debugger. It is refused under its own
     * OVMX condition value naming the debugger, and specifically NOT
     * under the subprocess message, because the RUN (Image) form
     * creates no process for a "process creation failed" to describe.
     * --------------------------------------------------------------- */
    {
        char out9[65536];

        clear_touch();
        run_dcl(NODBG_COM, out9, sizeof(out9), &exit_st);
        printf("  (RUN/NODEBUG -- a RUN (Image) qualifier)\n%s\n", out9);
        CHECK(touched() && strstr(out9, "NOSUBPRC") == NULL,
              "RUN/NODEBUG runs the image: it is not a subprocess request");

        clear_touch();
        run_dcl(DEBUG_COM, out9, sizeof(out9), &exit_st);
        printf("  (RUN/DEBUG -- a debugger OVMX has not got)\n%s\n", out9);
        CHECK(strstr(out9, "%OVMX-F-NODEBUGGER,") != NULL &&
              strstr(out9, "NOSUBPRC") == NULL &&
              strstr(out9, "CREPRC") == NULL &&
              !touched(),
              "RUN/DEBUG is refused naming the debugger, not process creation");
    }

    /* ---------------------------------------------------------------
     * P10. AN ABBREVIATED QUALIFIER IS THE SAME QUALIFIER.
     *
     * WHY THIS PHASE EXISTS. P7-P9 spell every qualifier out in full,
     * and nobody types them that way. DCL resolves a qualifier by
     * SHORTEST UNIQUE PREFIX, so /PRIO IS /PRIORITY and /DETACH IS
     * /DETACHED -- and a refusal keyed on exact names is therefore not
     * a strict version of the refusal, it is a refusal a user can walk
     * straight past. An adversary measured exactly that on the previous
     * round: `RUN/PRIO=4 <image>` ran the image, exit 0, no diagnostic,
     * with the priority discarded -- the round-2 defect restored for
     * every abbreviated spelling, at the same time as the full spelling
     * was correctly refused.
     *
     * ORACLE-PINNED (reference lab VAX1, OpenVMS VAX V7.3, 2026-07-31;
     * transcript in the lab as
     * captures/run-qualifier-abbrev-vax1-2026-07-31.txt). Each probe
     * named an image that does not exist, so DCL's verdict on the
     * qualifier is visible without creating anything -- a resolved
     * qualifier reaches RUN and fails on the image, an unresolved one
     * never gets there:
     *
     *   RUN/PRIO=4    %RUN-F-PARSEFAIL / -RMS-E-FNF   -> /PRIORITY
     *   RUN/PROC=FOO  %RUN-F-PARSEFAIL / -RMS-E-FNF   -> /PROCESS_NAME
     *   RUN/DETACH    %RUN-F-PARSEFAIL / -RMS-E-FNF   -> /DETACHED
     *   RUN/AST=100   %RUN-F-PARSEFAIL / -RMS-E-FNF   -> /AST_LIMIT
     *   RUN/PRIV=ALL  %RUN-F-PARSEFAIL / -RMS-E-FNF   -> /PRIVILEGES
     *   RUN/P=4       %DCL-W-ABKEYW, ambiguous qualifier or keyword
     *
     * This is not a hypothetical spelling: mx_start.com in this repo's
     * own VMS corpus writes /DETACH, /PRIO, /PRIV and /AST_LIMIT.
     *
     * THE PHASE MEASURES BOTH DIRECTIONS, because resolving
     * abbreviations only where the command REFUSES would create a new
     * silent discard in the half where it OBEYS: /DETACH must still
     * detach and /PROC= must still name.
     * --------------------------------------------------------------- */
    {
        char outA[65536];
        int nA = 0;
        uint32_t ab_lpid = 0;

        /* (a) The adversary's exact repro: the abbreviation must reach
         * the same refusal the full spelling reaches. */
        clear_touch();
        run_dcl(ABPRIO_COM, outA, sizeof(outA), &exit_st);
        printf("  (RUN/PRIO=4 -- /PRIORITY, abbreviated)\n%s\n", outA);
        CHECK(strstr(outA, "%RUN-F-CREPRC, process creation failed") != NULL &&
              strstr(outA, "-OVMX-F-NOSUBPRC,") != NULL &&
              !touched(),
              "RUN/PRIO is /PRIORITY: the abbreviation is refused and the image does not run");

        /* (b) The same, for a qualifier whose whole point is a name in
         * the executive: nothing may exist under it. */
        clear_touch();
        run_dcl(ABPROC_COM, outA, sizeof(outA), &exit_st);
        printf("  (RUN/PROC=%s -- /PROCESS_NAME, abbreviated)\n%s\n",
               ABBR_NAME, outA);
        memset(&info, 0, sizeof(info));
        CHECK(strstr(outA, "-OVMX-F-NOSUBPRC,") != NULL &&
              !touched() &&
              vms_kif_getjpi_prcnam(ABBR_NAME, &info) != SS$_NORMAL,
              "RUN/PROC is /PROCESS_NAME: refused, image not run, nothing named in the executive");

        /* (c) THE CONTROL AGAINST OVER-REFUSING, in abbreviated form --
         * because the last two rounds of this item were an over-refusal
         * and then an under-refusal, and only a case on each side can
         * tell them apart.
         *
         * This is COMPOSED from two pinned facts, not a third guess:
         * DCL resolves a qualifier by unique prefix (the capture above),
         * and /NODEBUG runs the image (P9's capture, `HELP/NOPROMPT RUN
         * Image Qualifier`). DEB is unique -- of the oracle's 34-name
         * process index only DELAY and DETACHED begin with DE, and
         * neither survives a third character B -- so /NODEB is /NODEBUG
         * and must reach the image exactly as the full spelling does. */
        clear_touch();
        run_dcl(ABNODBG_COM, outA, sizeof(outA), &exit_st);
        printf("  (RUN/NODEB -- a RUN (Image) qualifier, abbreviated)\n%s\n",
               outA);
        CHECK(touched() && strstr(outA, "NOSUBPRC") == NULL,
              "RUN/NODEB is not refused as a subprocess request: the image runs");

        /* (d) THE OBEYING HALF. mx_start.com's own spelling of the one
         * form OVMX honours. This is A-writes/B-reads (Rule 11): the
         * DCL that created it has exited, and THIS process resolves the
         * name out of the executive's table. */
        run_dcl(ABDET_COM, outA, sizeof(outA), &exit_st);
        printf("  (RUN/DETACH/PROC=%s -- the spelling mx_start.com uses)\n%s\n",
               DETABBR_NAME, outA);
        uint32_t ab_announced = proc_id_of(outA, &nA);
        memset(&info, 0, sizeof(info));
        st = vms_kif_getjpi_prcnam(DETABBR_NAME, &info);
        ab_lpid = info.linux_pid;
        CHECK(nA == 1 && st == SS$_NORMAL && ab_lpid != 0,
              "RUN/DETACH/PROC= creates a detached process the executive knows by name");
        CHECK(st == SS$_NORMAL && ab_announced != 0 &&
              ab_announced == info.vms_pid,
              "the abbreviated form announces the process ID the executive assigned");
        if (ab_lpid) kill((pid_t)ab_lpid, SIGKILL);

        /* (e) KNOWN GAP, TRACKED AS vms-69e -- THIS ASSERTION PINS WHAT
         * OVMX DOES TODAY, NOT WHAT VMS DOES.
         *
         * On the /DETACHED path a process qualifier outside the four
         * run_detached() reads is passed to $CREPRC as a bare literal
         * (baspri 0, prvadr NULL, quota NULL), so /PRIORITY, /PRIVILEGES
         * and the whole quota set are parsed, never read, and dropped --
         * under a SUCCESS message. That is Rule 10's illegal third
         * answer, and it is what an adversary found on the previous
         * round by booting this harness and driving
         * RUN/DETACHED/PROCESS_NAME=.../PRIORITY=4.
         *
         * The fix is NOT this item's to make: settling it needs either a
         * third OVMX condition value or real quota/privilege propagation
         * to the executive, and it is filed as vms-69e. What IS this
         * item's to do is make the gap VISIBLE, because until this
         * assertion existed the suite could not tell "refused" from
         * "silently discarded" on this branch -- the same blindness that
         * let round 2's overshoot ship. When vms-69e settles, this
         * assertion MUST go red and be rewritten; that is its job.
         *
         * There is deliberately NO negative control for this one. A
         * mutation that reddened it would have to be the vms-69e FIX,
         * and the manifest in tests/qemu/facility_defects.sh injects
         * defects, not improvements. */
        {
            uint32_t dp_lpid = 0;
            int nP = 0;

            run_dcl(DETPRIO_COM, outA, sizeof(outA), &exit_st);
            printf("  (RUN/DETACHED/PRIORITY=4 -- vms-69e: /PRIORITY is read "
                   "by nobody)\n%s\n", outA);
            (void)proc_id_of(outA, &nP);
            memset(&info, 0, sizeof(info));
            st = vms_kif_getjpi_prcnam(DETPRIO_NAME, &info);
            dp_lpid = info.linux_pid;

            CHECK(nP == 1 && st == SS$_NORMAL,
                  "vms-69e: /DETACHED with /PRIORITY still creates the process "
                  "and announces success");
            CHECK(strstr(outA, "PRIORITY") == NULL &&
                  strstr(outA, "NOSUBPRC") == NULL &&
                  strstr(outA, "CREPRC") == NULL,
                  "vms-69e: and says NOTHING about the priority it discarded "
                  "(this is the defect, asserted so it cannot be fixed silently)");
            if (dp_lpid) kill((pid_t)dp_lpid, SIGKILL);
        }

        /* (f) An abbreviation DCL cannot resolve. On the oracle this is
         * refused by DCL before RUN is entered:
         *   %DCL-W-ABKEYW, ambiguous qualifier or keyword - supply more characters
         *    \PR\
         * OVMX's parser validates no qualifier against any command's
         * table, for ANY command, so neither that refusal nor the
         * %DCL-W-IVQUAL one for an unknown qualifier exists anywhere in
         * DCL. Answering it inside RUN alone would answer for one
         * command a question VMS answers for the whole language, so the
         * gap is left where it is -- and asserted here, as it BEHAVES,
         * so that it is a measured fact rather than a remark in a commit
         * message. */
        clear_touch();
        run_dcl(AMBIG_COM, outA, sizeof(outA), &exit_st);
        printf("  (RUN/PR=4 -- ambiguous: PRIORITY, PRIVILEGES, "
               "PROCESS_NAME)\n%s\n", outA);
        CHECK(touched() && strstr(outA, "ABKEYW") == NULL,
              "parser-wide gap: an ambiguous abbreviation is not resolved, and "
              "OVMX has no %DCL-W-ABKEYW to refuse it with");
    }

    clear_touch();
    unlink(HOLD_SCRIPT);
    unlink(TOUCH_SCRIPT);
    unlink(UIC_COM);
    unlink(SUBP_COM);
    unlink(PRIO_COM);
    unlink(PLAIN_COM);
    unlink(NODBG_COM);
    unlink(DEBUG_COM);
    unlink(ABPRIO_COM);
    unlink(ABPROC_COM);
    unlink(ABNODBG_COM);
    unlink(ABDET_COM);
    unlink(DETPRIO_COM);
    unlink(AMBIG_COM);
    unlink(SVC_STARTUP_COM);
    unlink(SYSTARTUP_COM);
    unlink(SVC_LOG);

    printf("=== test_syssvc_startup_service: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
