/*
 * test_syssvc_spawn_users.c - a SPAWNed subprocess is VISIBLE and CLASSIFIED,
 * through the real DCL.EXE against a real executive (vms-c17).
 *
 * WHAT THIS SUITE GUARDS. Before vms-c17 two independent defects hid a
 * SPAWNed subprocess from SHOW USERS / SHOW SYSTEM:
 *
 *   (1) cmd_spawn() (src/vmsdcl/dcl_cmd_process.c) did a bare fork()+execl()
 *       and never entered the child in the executive's process table, so
 *       vms_kif_procscan() never returned it -- the "reports success while
 *       sharing nothing" fabrication class (INV-6). It now runs a $CREPRC-style
 *       creation handshake: the child registers, auto-names itself
 *       "<creator>_N", and reports back before exec, so the subprocess EXISTS
 *       (named, in the executive) the moment SPAWN returns.
 *
 *   (2) the interactive/subprocess/batch classification was not on the wire.
 *       struct vms_procinfo now carries proc_type (VMS_PROC_T_*), set by the
 *       executive from the PCB's job_id (proc_fill_info, src/kernel-core/
 *       vms_proctab.c), and cmd_show_users() filters/counts on it instead of
 *       the old terminal[0] filter that dropped every subprocess.
 *
 * THE PROOF IS MULTI-LAYER, and every positive assertion is A-CREATES /
 * B-OBSERVES: an interactive job root is established, it SPAWNs a subprocess,
 * and a DIFFERENT reader (the real DCL.EXE and, independently, $GETJPI from
 * this process) reports the classification the executive holds -- never a value
 * the subprocess wrote about itself.
 *
 * WHY THE ROOT MUST BE A JOB ROOT, AND HOW. proc_type is derived from job_id,
 * which the executive derives from task ancestry AT REGISTRATION
 * (vms_proc_parent_job_id): a task whose real parent is already a registered
 * VMS process INHERITS that parent's job (a subprocess); a task with no
 * registered parent becomes its own job root. So this process (main) does NOT
 * register until AFTER it has forked the session child -- it opens /dev/vms
 * with a raw open() for the presence check only. The child's real parent is
 * therefore an UNREGISTERED task, so when the child registers it becomes a job
 * root; bound to the console it is INTERACTIVE, and its SPAWN child inherits
 * its job and is a SUBPROCESS.
 *
 * WHY THE SESSION IS KEPT ALIVE FOR THE $GETJPI READS, THEN LOGGED OUT FOR THE
 * TRANSCRIPT. proc_type is computed LIVE: a subprocess is SUBPROCESS only while
 * its job root is still in the table (proc_fill_info looks the root up by
 * job_id). So the session script issues its SHOW commands but sends NO LOGOUT,
 * and this process holds the command pipe OPEN, leaving DCL blocked -- root and
 * subprocess both alive -- while the $GETJPI reads run. ONLY AFTER those reads
 * does the test LOGOUT the root. That ordering is also what makes the transcript
 * capture work: DCL's stdout is a fully-buffered file and a non-interactive DCL
 * prints no prompt, so nothing reaches OUT_FILE until DCL exits -- the SHOW
 * output (produced earlier, while the subprocess was alive) is flushed as one
 * block on LOGOUT, and the transcript is read only after the child is reaped.
 *
 * PRECONDITION-NOT-ESTABLISHED IS A SKIP, NOT A FAIL (the blind/skip
 * discipline this tree already uses). This suite's property is the
 * CLASSIFICATION; establishing a terminal-bound job root depends on the
 * register + $ASSIGN + SETTERM facilities, whose own faithfulness is proven by
 * test_kmod_bind / test_kmod_setterm / test_syssvc_showterm. If those
 * facilities are not effective here (e.g. under a facility negative-control
 * that injects a SETTERM/register defect) this suite cannot build its subject
 * and SKIPs, leaving detection to the suites that own those defects, rather
 * than reddening a control it is not registered for. On a healthy build the
 * preconditions hold and the full proof runs.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With none -- only the CI
 * negative-control rig, never the product (PID 1 refuses to boot without the
 * executive) -- it runs device_absent_checks() and exits EXIT_SKIP (77).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                                 \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }   \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }   \
    } while (0)

/* Unique across the whole initramfs run: every suite shares one booted
 * executive and one process table. The subprocess auto-name is the creator's
 * process name plus "_1" (OpenVMS DCL Dictionary, SPAWN; oracle VAX1 OpenVMS
 * VAX V7.3 "%DCL-S-SPAWNED, process SYSTEM_1 spawned"). */
#define ROOT_NAME   "C17ROOT"
#define SUB_NAME    "C17ROOT_1"
#define CONSOLE     "OPA0:"
#define OUT_FILE    "/tmp/ovmx_c17_dcl.out"

/* SPAWN a subprocess that outlives the displays and the reads (WAIT blocks it),
 * show it both ways, then STOP -- no LOGOUT in the script, so DCL blocks and the
 * job stays alive for the $GETJPI reads. The test sends LOGOUT itself, after the
 * reads, to flush the transcript (see the header comment). Auto-naming (no
 * /PROCESS) so the SPAWNED message and the SUB_NAME row are what vms-c17's
 * naming produces. */
static const char *SESSION_SCRIPT =
    "SPAWN/NOWAIT WAIT 00:02:00\n"
    "SHOW USERS\n"
    "SHOW USERS/FULL\n"
    "SHOW SYSTEM\n";

/* ---- captured-output helpers ---------------------------------------- */

static int has_substr(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* a SHOW SYSTEM row for `name`: 8 hex digits at col 0, a space, then the name
 * at col 9 (oracle-pinned geometry, see test_syssvc_showproc.c). */
static int has_sys_row(const char *text, const char *name)
{
    const char *ln = text;
    size_t nl = strlen(name);
    while (ln && *ln) {
        int i;
        for (i = 0; i < 8; i++)
            if (!isxdigit((unsigned char)ln[i])) break;
        if (i == 8 && ln[8] == ' ' && strncmp(ln + 9, name, nl) == 0)
            return 1;
        ln = strchr(ln, '\n');
        if (ln) ln++;
    }
    return 0;
}

/* a NON-SPAWNED-message line that names `name` -- i.e. an actual table row
 * (SHOW USERS/FULL or SHOW SYSTEM), not the "%DCL-S-SPAWNED, process X spawned"
 * echo, which also contains the name and would make a bare substring match
 * pass vacuously. */
static int has_name_row(const char *text, const char *name)
{
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *hit = strstr(line, name);
        if (hit && (size_t)(hit - line) < len &&
            !strstr(line, "SPAWNED") && !strstr(line, "spawned"))
            return 1;
        line = eol ? eol + 1 : NULL;
    }
    return 0;
}

static long read_file(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, bufsz - 1 - used);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        used += (size_t)n;
        if (used >= bufsz - 1) break;
    }
    buf[used] = '\0';
    close(fd);
    return (long)used;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; left -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;
    while (left) {
        ssize_t r = read(fd, p, left);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        p += r; left -= (size_t)r;
    }
    return left == 0 ? 0 : -1;
}

/* ---- device-absent negative control -------------------------------- */

static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    struct vms_procinfo info;
    uint32_t index = 0;
    uint32_t st = vms_kif_procscan(&index, &info);
    CHECK(!(st & 1),
          "the process-table scan does not report success with no executive");

    static char out[65536];
    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  (no /bin/DCL.EXE -- the SHOW USERS assertion cannot run here)\n");
    } else {
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) == 0 && pipe(out_pipe) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                dup2(in_pipe[0], STDIN_FILENO);
                dup2(out_pipe[1], STDOUT_FILENO);
                dup2(out_pipe[1], STDERR_FILENO);
                close(in_pipe[0]); close(in_pipe[1]);
                close(out_pipe[0]); close(out_pipe[1]);
                execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
                _exit(127);
            }
            close(in_pipe[0]); close(out_pipe[1]);
            (void)write_all(in_pipe[1], "SHOW USERS\nLOGOUT\n",
                            strlen("SHOW USERS\nLOGOUT\n"));
            close(in_pipe[1]);
            size_t used = 0;
            for (;;) {
                ssize_t n = read(out_pipe[0], out + used, sizeof(out) - 1 - used);
                if (n <= 0) break;
                used += (size_t)n;
                if (used >= sizeof(out) - 1) break;
            }
            out[used] = '\0';
            close(out_pipe[0]);
            int wst; while (waitpid(pid, &wst, 0) < 0 && errno == EINTR) ;
            CHECK(!has_substr(out, "number of processes"),
                  "SHOW USERS fabricates no user-process summary with no "
                  "executive");
        }
    }

    printf("=== test_syssvc_spawn_users: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

/* ---- the session subject ------------------------------------------- */

/* The child becomes the interactive job root: it names itself, binds the
 * console, redirects its output to OUT_FILE, and execs the real DCL.EXE reading
 * SESSION_SCRIPT from stdin. It reports the status of each executive call back
 * over `repfd` so the parent can tell a broken PRECONDITION (skip) apart from a
 * real product bug. Never returns. */
struct setup_report { uint32_t setprn; uint32_t assign; uint32_t setterm; };

static void session_child(int in_fd, int repfd)
{
    struct setup_report rep;
    uint32_t chan = 0;
    memset(&rep, 0, sizeof(rep));

    /* FIRST vms_kif_* call binds + registers this task (kif_bind). Because
     * main is still unregistered, this task's real parent has no PCB, so the
     * executive makes it a job root (job_id == its own vms_pid). */
    rep.setprn  = vms_kif_setprn(ROOT_NAME);
    rep.assign  = vms_kif_assign(CONSOLE, &chan);
    rep.setterm = (rep.assign & 1) ? vms_kif_setterm(chan) : rep.assign;
    (void)write_all(repfd, &rep, sizeof(rep));
    close(repfd);

    int fd = open(OUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    dup2(in_fd, STDIN_FILENO);
    close(in_fd);

    execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
    _exit(127);
}

/* ---- vms-01f: a REGISTERED creator's terminal-binding login is INTERACTIVE ---
 *
 * The scenario above keeps its creator (main) UNREGISTERED so the session child
 * becomes a job root for free -- which is NOT the condition the real boot meets.
 * On the real runtime JOB_CONTROL is a registered, detached, terminal-LESS
 * process (test_job_control_console.sh proves SHOW SYSTEM lists it) that fork()s
 * the console login. Under the inherit-the-parent's-job rule the login then took
 * JOB_CONTROL's job_id and was classified a SUBPROCESS of a terminal-less root
 * -- i.e. OTHER -- so SHOW USERS reported "Total number of users = 0" on a live
 * console login (vms-01f).
 *
 * This reproduces THAT: a registered creator forks a child that binds the
 * console terminal, and a DIFFERENT process ($GETJPI from main) reads the
 * child's classification. proc_type == INTERACTIVE is only reachable through
 * proc_fill_info()'s job-ROOT branch with a terminal set; the subprocess branch
 * yields SUBPROCESS or OTHER, never INTERACTIVE. So this assertion fails on the
 * pre-fix executive (the login would read OTHER) and passes once SETTERM
 * promotes a terminal owner to a job root. */
#define JC_LOGIN_NAME  "C01FLOGIN"

struct jc_report { uint32_t setprn; uint32_t assign; uint32_t setterm; };

static void registered_creator_login_scenario(void)
{
    int rep_pipe[2];
    if (pipe(rep_pipe) != 0) {
        printf("  (vms-01f: pipe() failed -- SKIP)\n");
        return;
    }

    pid_t creator = fork();
    if (creator < 0) {
        printf("  (vms-01f: fork() failed -- SKIP)\n");
        close(rep_pipe[0]); close(rep_pipe[1]);
        return;
    }
    if (creator == 0) {
        /* CREATOR: register (first vms_kif_* call binds a PCB), so this task
         * -- the login child's real parent -- is a REGISTERED process, exactly
         * as JOB_CONTROL is on the real boot. It binds NO terminal. */
        close(rep_pipe[0]);
        struct vms_procinfo ci;
        (void)vms_kif_getjpi_self(&ci);          /* bind + register the creator */

        pid_t login = fork();
        if (login == 0) {
            /* LOGIN CHILD: name itself, bind the console terminal, report, then
             * stay alive (blocked) for the read below. This is the LOGINOUT/DCL
             * session's PCB in miniature. */
            struct jc_report rep;
            uint32_t chan = 0;
            memset(&rep, 0, sizeof(rep));
            rep.setprn  = vms_kif_setprn(JC_LOGIN_NAME);
            rep.assign  = vms_kif_assign(CONSOLE, &chan);
            rep.setterm = (rep.assign & 1) ? vms_kif_setterm(chan) : rep.assign;
            (void)write_all(rep_pipe[1], &rep, sizeof(rep));
            close(rep_pipe[1]);
            for (;;) pause();
            _exit(0);
        }
        /* Creator keeps the login child as its live child and stays registered
         * and alive until main kills the subtree. */
        close(rep_pipe[1]);
        for (;;) pause();
        _exit(0);
    }
    close(rep_pipe[1]);

    struct jc_report rep;
    memset(&rep, 0, sizeof(rep));
    int got = read_all(rep_pipe[0], &rep, sizeof(rep));
    close(rep_pipe[0]);

    /* Precondition: the login child had to establish its name + terminal. A
     * failure here means the facilities this scenario BUILDS ON are not
     * effective (see the header comment's blind/skip discipline) -- SKIP. */
    if (got != 0 || !(rep.setprn & 1) || !(rep.assign & 1) || !(rep.setterm & 1)) {
        printf("  (vms-01f: login not established: setprn=%08X assign=%08X "
               "setterm=%08X -- SKIP)\n", rep.setprn, rep.assign, rep.setterm);
        kill(creator, SIGKILL);
        int s; while (waitpid(creator, &s, 0) < 0 && errno == EINTR) ;
        return;
    }

    struct vms_procinfo login_info;
    uint32_t lst = 0;
    for (int i = 0; i < 500; i++) {   /* up to ~5s */
        lst = vms_kif_getjpi_prcnam(JC_LOGIN_NAME, &login_info);
        if (lst & 1) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    printf("  (vms-01f diag: lst=%08X login_type=%u login_term=%s)\n",
           lst, (lst & 1) ? login_info.proc_type : 0xffu,
           (lst & 1) ? login_info.terminal : "?");

    CHECK((lst & 1) && login_info.proc_type == VMS_PROC_T_INTERACTIVE,
          "a REGISTERED creator's terminal-binding login is classified "
          "INTERACTIVE, not a subprocess of a terminal-less root (vms-01f)");

    /* Tear down the whole subtree (login grandchild is reparented to the
     * creator; killing the creator is not enough, so kill the login row's task
     * too when we know it). */
    if (lst & 1 && login_info.linux_pid)
        kill((pid_t)login_info.linux_pid, SIGKILL);
    kill(creator, SIGKILL);
    int s; while (waitpid(creator, &s, 0) < 0 && errno == EINTR) ;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    static char out[131072];

    printf("=== test_syssvc_spawn_users: SPAWN visibility + interactive/"
           "subprocess classification ===\n");

    /* Presence check ONLY -- a raw open(), not a vms_kif_* call, so this
     * process stays UNREGISTERED and the session child below becomes a job
     * root (see the header comment). */
    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  (no /bin/DCL.EXE staged -- cannot drive SHOW USERS/SYSTEM)\n");
        printf("=== test_syssvc_spawn_users: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        return EXIT_SKIP;
    }

    unlink(OUT_FILE);

    int in_pipe[2], rep_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(rep_pipe) != 0) {
        printf("  FAIL: pipe()\n");
        printf("=== test_syssvc_spawn_users: %d passed, %d failed ===\n",
               pass, fail + 1);
        return 1;
    }

    pid_t child = fork();
    if (child < 0) {
        printf("  FAIL: fork()\n");
        printf("=== test_syssvc_spawn_users: %d passed, %d failed ===\n",
               pass, fail + 1);
        return 1;
    }
    if (child == 0) {
        close(in_pipe[1]); close(rep_pipe[0]);
        session_child(in_pipe[0], rep_pipe[1]);
        _exit(127);
    }
    close(in_pipe[0]); close(rep_pipe[1]);

    /* Feed the session script, but KEEP in_pipe[1] OPEN so DCL blocks at its
     * prompt after SHOW SYSTEM and the job stays alive for the reads below. */
    (void)write_all(in_pipe[1], SESSION_SCRIPT, strlen(SESSION_SCRIPT));

    struct setup_report rep;
    memset(&rep, 0, sizeof(rep));
    int got = read_all(rep_pipe[0], &rep, sizeof(rep));
    close(rep_pipe[0]);

    /* PRECONDITION: the interactive job root had to be established. A failed
     * register/$ASSIGN/SETTERM means the facilities this suite BUILDS ON are
     * not effective here -- skip, deferring their detection to the suites that
     * own them (see the header comment). */
    if (got != 0 || !(rep.setprn & 1) || !(rep.assign & 1) || !(rep.setterm & 1)) {
        printf("  (interactive root not established: setprn=%08X assign=%08X "
               "setterm=%08X -- SKIP)\n", rep.setprn, rep.assign, rep.setterm);
        close(in_pipe[1]);
        int s; while (waitpid(child, &s, 0) < 0 && errno == EINTR) ;
        printf("=== test_syssvc_spawn_users: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        return EXIT_SKIP;
    }

    /* The transcript is captured below, AFTER the session logs out. A non-
     * interactive DCL prints no prompt, and its stdout is a fully-buffered file,
     * so nothing is flushed until DCL exits -- reading OUT_FILE now would see an
     * empty file. The $GETJPI reads that follow need the whole job ALIVE, so the
     * order is: read the executive rows here, then LOGOUT (flush + exit), then
     * read the flushed transcript. The SHOW commands already ran while the
     * subprocess was alive, so the flushed transcript still shows both rows. */
    int have_transcript = 0;

    /* Now this process may register (its first vms_kif_* call): the child's
     * job_id was already derived while main was unregistered, so this changes
     * nothing about the subject. Read the executive rows while root and
     * subprocess are BOTH still alive. */
    struct vms_procinfo root_info, sub_info, self_info;
    /* The root registered synchronously (session_child reported setprn/$ASSIGN/
     * SETTERM back over rep_pipe before exec'ing DCL), but the SPAWN/NOWAIT
     * subprocess registers ASYNCHRONOUSLY -- DCL forks it and it makes its first
     * vms_kif call only after it starts. Poll for its row to APPEAR (an observed
     * condition, not a fixed sleep) before reading the classification; the
     * transcript's "number of processes = 2" proves it does register. */
    uint32_t rst = vms_kif_getjpi_prcnam(ROOT_NAME, &root_info);
    uint32_t sst = 0;
    for (int i = 0; i < 3000; i++) {   /* up to ~30s */
        sst = vms_kif_getjpi_prcnam(SUB_NAME, &sub_info);
        if (sst & 1) break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };  /* 10ms */
        nanosleep(&ts, NULL);
    }
    uint32_t sfst = vms_kif_getjpi_self(&self_info);

    printf("  (diag: transcript=%d rst=%08X root_type=%u sst=%08X sub_type=%u "
           "sfst=%08X self_type=%u)\n",
           have_transcript, rst, (rst & 1) ? root_info.proc_type : 0xffu,
           sst, (sst & 1) ? sub_info.proc_type : 0xffu,
           sfst, (sfst & 1) ? self_info.proc_type : 0xffu);

    /* If the root has no row at all, registration/bind is not effective here
     * (e.g. an injected bind-client-no-register): SKIP, deferring that
     * detection to the bind suites (see the header comment). */
    if (!(rst & 1)) {
        printf("  (root row absent (rst=%08X) -- SKIP)\n", rst);
        if (sst & 1 && sub_info.linux_pid) kill((pid_t)sub_info.linux_pid, SIGKILL);
        close(in_pipe[1]);
        int s; while (waitpid(child, &s, 0) < 0 && errno == EINTR) ;
        printf("=== test_syssvc_spawn_users: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        return EXIT_SKIP;
    }

    /* The root registered but its terminal is not recorded => SETTERM returned
     * success without writing the binding into the executive row. That is the
     * setterm-binding-not-recorded facade, and this suite reads it back through
     * $GETJPI on a row a DIFFERENT process bound -- so it DETECTS that defect.
     * Report it and stop before the classification assertions (which all depend
     * on the binding and would cascade), so the injection reddens this one
     * assertion, not a shower of them. */
    /* negctl: setterm-binding-not-recorded */
    CHECK(root_info.terminal[0] != '\0',
          "the interactive session root is terminal-bound (SETTERM recorded the binding)");
    if (root_info.terminal[0] == '\0') {
        if (sst & 1 && sub_info.linux_pid) kill((pid_t)sub_info.linux_pid, SIGKILL);
        close(in_pipe[1]);
        int s; while (waitpid(child, &s, 0) < 0 && errno == EINTR) ;
        printf("=== test_syssvc_spawn_users: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* ---------------------------------------------------------------
     * P1. THE EXECUTIVE'S CLASSIFICATION, read by $GETJPI (Change A+B),
     *     while the whole job is alive.
     * --------------------------------------------------------------- */
    CHECK(sst & 1,
          "the SPAWNed subprocess is registered in the executive process "
          "table (cmd_spawn no longer skips registration)");
    CHECK(root_info.proc_type == VMS_PROC_T_INTERACTIVE,
          "the terminal-bound session root is classified INTERACTIVE");
    CHECK((sst & 1) && sub_info.proc_type == VMS_PROC_T_SUBPROCESS,
          "the SPAWNed subprocess is classified SUBPROCESS");
    /* A job root with no terminal is OTHER, not a "user": this process is its
     * own job root and bound to no terminal. */
    CHECK((sfst & 1) && self_info.proc_type == VMS_PROC_T_OTHER,
          "a terminal-less job root is classified OTHER (not a user)");

    /* The live $GETJPI reads are done, but the SUBPROCESS MUST STAY ALIVE until
     * the DCL session has run ALL THREE of SHOW USERS / SHOW USERS/FULL / SHOW
     * SYSTEM. DCL executes the script asynchronously from this process, so the
     * kill must not be paced by THIS process's read progress: an earlier
     * ordering killed the subprocess right after the $GETJPI reads, and under a
     * slow/contended SHOW path (a full-suite TCG run) the last two SHOWs then
     * ran against a table the subprocess had already left -- a race that read as
     * "SHOW USERS/FULL and SHOW SYSTEM drop the subprocess", not a product bug
     * (the executive ADOPTs the name+job_id across the subprocess's execve, so
     * the row is correct the whole time it exists).
     *
     * So LOGOUT FIRST -- the root runs its remaining SHOWs with the subprocess
     * still present, then exits, flushing OUT_FILE -- reap it, read the now-
     * complete transcript, and only THEN release the WAIT-blocked subprocess.
     * Killing after the reap cannot race the SHOWs: the root cannot have exited
     * before it processed every command ahead of LOGOUT in the pipe. */
    (void)write_all(in_pipe[1], "LOGOUT\n", strlen("LOGOUT\n"));
    close(in_pipe[1]);
    { int s; while (waitpid(child, &s, 0) < 0 && errno == EINTR) ; }
    have_transcript = (read_file(OUT_FILE, out, sizeof(out)) > 0 &&
                       has_substr(out, "number of processes"));
    if (sst & 1 && sub_info.linux_pid)
        kill((pid_t)sub_info.linux_pid, SIGKILL);

    /* ---------------------------------------------------------------
     * P2. THE USER-VISIBLE READERS (Change C + Change D), from the real
     *     DCL.EXE transcript.
     * --------------------------------------------------------------- */
    CHECK(have_transcript, "captured the DCL.EXE session transcript "
                           "(SHOW USERS + SHOW SYSTEM)");

    /* Change C: the /NOWAIT completion message is the oracle-grounded
     * "%DCL-S-SPAWNED, process <name> spawned" (severity S, the NAME) -- not
     * the old "%DCL-I-SPAWNED, process id is <linux-pid>". */
    CHECK(has_substr(out, "%DCL-S-SPAWNED, process " SUB_NAME " spawned"),
          "SPAWN/NOWAIT printed '%DCL-S-SPAWNED, process " SUB_NAME
          " spawned' (severity S, the process name)");
    CHECK(!has_substr(out, "%DCL-I-SPAWNED"),
          "the old '%DCL-I-SPAWNED, process id is <pid>' message is gone");

    /* Change D: SHOW USERS counts the subprocess. Both rows carry the same
     * (blank) username, so they aggregate to one user and two processes -- the
     * headline bug (was "number of processes = 1"). */
    CHECK(has_substr(out, "number of processes = 2"),
          "SHOW USERS reports number of processes = 2 (root + subprocess)");

    /* SHOW USERS/FULL now LISTS the subprocess by name -- the old terminal[0]
     * filter dropped every subprocess; proc_type keeps it. Both the root and
     * the subprocess must appear. */
    CHECK(has_name_row(out, ROOT_NAME),
          "SHOW USERS/FULL lists the interactive root by process name");
    CHECK(has_name_row(out, SUB_NAME),
          "SHOW USERS/FULL lists the SPAWNed subprocess by process name");

    /* Change C: SHOW SYSTEM enumerates the whole table, so the newly-
     * registered subprocess appears there too, as a process-table row. */
    CHECK(has_sys_row(out, SUB_NAME),
          "SHOW SYSTEM lists the SPAWNed subprocess as a process-table row");

    /* ---------------------------------------------------------------
     * P3. vms-01f: the REAL-boot shape -- a REGISTERED creator's
     *     terminal-binding login must be INTERACTIVE (a job root), not a
     *     subprocess of a terminal-less root filtered out of SHOW USERS.
     * --------------------------------------------------------------- */
    registered_creator_login_scenario();

    if (fail > 0) {
        printf("  (diag: captured transcript follows)\n---8<---\n%.4000s\n---8<---\n",
               out);
    }
    printf("=== test_syssvc_spawn_users: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
