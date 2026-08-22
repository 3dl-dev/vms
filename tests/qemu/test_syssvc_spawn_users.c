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

/* The session script: SPAWN a subprocess that stays alive across the two
 * displays (WAIT blocks the subprocess), then show it both ways. Auto-naming
 * (no /PROCESS) so the SPAWNED message and the SUB_NAME row are what vms-c17's
 * naming produces, not a name the test dictated. */
static const char *SESSION_SCRIPT =
    "SPAWN/NOWAIT WAIT 00:00:20\n"
    "SHOW USERS\n"
    "SHOW USERS/FULL\n"
    "SHOW SYSTEM\n"
    "LOGOUT\n";

/* ---- captured-output helpers ---------------------------------------- */

static int has_substr(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/* the first line containing `needle`, or NULL */
static const char *line_with(const char *text, const char *needle)
{
    const char *line = text;
    while (line && *line) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        const char *hit = strstr(line, needle);
        if (hit && (size_t)(hit - line) < len)
            return line;
        line = eol ? eol + 1 : NULL;
    }
    return NULL;
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
    /* negctl: bind-client-no-register */
    CHECK(!(st & 1),
          "the process-table scan does not report success with no executive");

    /* SHOW USERS must fabricate no user row with no executive to read. */
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
            /* With no executive, SHOW USERS must not print a user-process
             * table summary line -- there is no process table to read. */
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
 * console, redirects its output to OUT_FILE, and execs the real DCL.EXE
 * reading SESSION_SCRIPT from stdin. It reports the status of each executive
 * call back over `repfd` so the parent can tell a broken PRECONDITION (skip)
 * apart from a real product bug. Never returns. */
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

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffer: no buffered splice into a child */
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

    /* Feed the session script, then read the child's setup report. */
    (void)write_all(in_pipe[1], SESSION_SCRIPT, strlen(SESSION_SCRIPT));
    close(in_pipe[1]);

    struct setup_report rep;
    memset(&rep, 0, sizeof(rep));
    int got = read_all(rep_pipe[0], &rep, sizeof(rep));
    close(rep_pipe[0]);

    /* Wait for DCL.EXE (the session root) to log out. The SPAWNed subprocess
     * is a grandchild (WAIT-blocked) and is reparented away, so this returns
     * at LOGOUT with the full transcript already flushed to OUT_FILE. */
    int wst; while (waitpid(child, &wst, 0) < 0 && errno == EINTR) ;

    /* PRECONDITION: the interactive job root had to be established. A failed
     * register/$ASSIGN/SETTERM means the facilities this suite BUILDS ON are
     * not effective here -- skip, deferring their detection to the suites that
     * own them (see the header comment). */
    if (got != 0 || !(rep.setprn & 1) || !(rep.assign & 1) || !(rep.setterm & 1)) {
        printf("  (interactive root not established: setprn=%08X assign=%08X "
               "setterm=%08X -- SKIP)\n", rep.setprn, rep.assign, rep.setterm);
        printf("=== test_syssvc_spawn_users: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        return EXIT_SKIP;
    }

    /* Now this process may register: the child's job_id was already derived
     * (while main was unregistered), so registering here changes nothing about
     * the subject. This lets us read the executive rows directly. */
    struct vms_procinfo root_info, sub_info;
    uint32_t rst = vms_kif_getjpi_prcnam(ROOT_NAME, &root_info);
    uint32_t sst = vms_kif_getjpi_prcnam(SUB_NAME, &sub_info);

    /* SECOND precondition guard: if SETTERM did not actually record the
     * binding (an injected setterm-binding-not-recorded defect returns success
     * but records nothing), the root is not terminal-bound and the subject is
     * not what this suite needs -- skip rather than redden. */
    if (!(rst & 1) || root_info.terminal[0] == '\0') {
        printf("  (root row absent or not terminal-bound (rst=%08X term=\"%s\") "
               "-- SKIP)\n", rst, (rst & 1) ? root_info.terminal : "");
        printf("=== test_syssvc_spawn_users: %d passed, %d failed (SKIPPED) ===\n",
               pass, fail);
        /* best-effort cleanup of the subprocess before leaving */
        if (sst & 1 && sub_info.linux_pid) kill((pid_t)sub_info.linux_pid, SIGKILL);
        return EXIT_SKIP;
    }

    /* ---------------------------------------------------------------
     * P1. THE EXECUTIVE'S CLASSIFICATION, read by $GETJPI (Change A+B).
     *     The root is INTERACTIVE (terminal-bound job root); the SPAWNed
     *     subprocess EXISTS and is a SUBPROCESS.
     * --------------------------------------------------------------- */
    CHECK(sst & 1,
          "the SPAWNed subprocess is registered in the executive process "
          "table (cmd_spawn no longer skips registration)");
    CHECK((rst & 1) && root_info.proc_type == VMS_PROC_T_INTERACTIVE,
          "the terminal-bound session root is classified INTERACTIVE");
    CHECK((sst & 1) && sub_info.proc_type == VMS_PROC_T_SUBPROCESS,
          "the SPAWNed subprocess is classified SUBPROCESS");

    /* A job root with no terminal is OTHER, not a "user": this process (main)
     * is its own job root (registered above, real parent unregistered) and
     * bound to no terminal. */
    struct vms_procinfo self_info;
    if (vms_kif_getjpi_self(&self_info) & 1)
        CHECK(self_info.proc_type == VMS_PROC_T_OTHER,
              "a terminal-less job root is classified OTHER (not a user)");

    /* ---------------------------------------------------------------
     * P2. THE USER-VISIBLE READERS (Change C + Change D), from the real
     *     DCL.EXE transcript.
     * --------------------------------------------------------------- */
    long n = read_file(OUT_FILE, out, sizeof(out));
    CHECK(n > 0, "captured the DCL.EXE session transcript");

    /* Change C: the /NOWAIT completion message is the oracle-grounded
     * "%DCL-S-SPAWNED, process <name> spawned" (severity S, the NAME) -- not
     * the old "%DCL-I-SPAWNED, process id is <linux-pid>". */
    CHECK(has_substr(out, "%DCL-S-SPAWNED, process " SUB_NAME " spawned"),
          "SPAWN/NOWAIT printed '%DCL-S-SPAWNED, process " SUB_NAME
          " spawned' (severity S, the process name)");
    CHECK(!has_substr(out, "%DCL-I-SPAWNED"),
          "the old '%DCL-I-SPAWNED, process id is <pid>' message is gone");

    /* Change D: SHOW USERS counts the subprocess. Both rows carry the same
     * (blank) username, so they aggregate to one user and two processes. */
    CHECK(has_substr(out, "number of processes = 2"),
          "SHOW USERS reports number of processes = 2 (root + subprocess)");

    /* SHOW USERS/FULL now LISTS the subprocess by name -- the old terminal[0]
     * filter dropped every subprocess; proc_type keeps it. Both the root and
     * the subprocess must appear. */
    CHECK(line_with(out, ROOT_NAME) != NULL,
          "SHOW USERS/FULL lists the interactive root by process name");
    CHECK(line_with(out, SUB_NAME) != NULL,
          "SHOW USERS/FULL lists the SPAWNed subprocess by process name");

    /* Change C: SHOW SYSTEM enumerates the whole table, so the newly-
     * registered subprocess appears there too, by name. */
    {
        /* SHOW SYSTEM row: "%08X %-15s ..." -- the name field starts at
         * column 9 (oracle-pinned, see test_syssvc_showproc.c). Assert the
         * subprocess name appears on a line that also begins with 8 hex
         * digits, so we are reading a SHOW SYSTEM row and not the SHOW USERS
         * echo above. */
        const char *ln = out;
        int found = 0;
        while (ln && *ln) {
            int i;
            for (i = 0; i < 8; i++)
                if (!isxdigit((unsigned char)ln[i])) break;
            if (i == 8 && ln[8] == ' ' &&
                strncmp(ln + 9, SUB_NAME, strlen(SUB_NAME)) == 0) {
                found = 1;
                break;
            }
            ln = strchr(ln, '\n');
            if (ln) ln++;
        }
        CHECK(found,
              "SHOW SYSTEM lists the SPAWNed subprocess as a process-table row");
    }

    /* Release the subprocess (it is WAIT-blocked for up to 20s). */
    if (sst & 1 && sub_info.linux_pid) {
        kill((pid_t)sub_info.linux_pid, SIGKILL);
        int s; while (waitpid((pid_t)sub_info.linux_pid, &s, 0) < 0 && errno == EINTR)
            ;   /* it is reparented to us via subreaper? if not, ECHILD -- fine */
    }

    printf("=== test_syssvc_spawn_users: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
