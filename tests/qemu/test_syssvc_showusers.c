/*
 * test_syssvc_showusers.c - SHOW USERS is a READER of the executive's process
 * table, proven A-WRITES/B-READS by driving the REAL DCL.EXE against a real
 * /dev/vms inside QEMU (vms-150).
 *
 * WHY THIS EXISTS, AND IT IS A MEASUREMENT, NOT A HUNCH.
 *
 * vms-72c's claim is that SHOW USERS reads the executive's process table
 * instead of fabricating a row from the calling process's own DCL context and
 * getpid(). Before this suite, the tests that assert anything about SHOW
 * USERS were these, and both are SINGLE-PROCESS:
 *
 *   tests/dcl/test_show_users_terminal.sh  no executive at all -- asserts only
 *                                          that NO row is printed.
 *   tests/uat/vms_session_qemu.sh          one logged-in console session
 *                                          asserting its OWN pid and
 *                                          "Total number of users = 1".
 *
 * (tests/uat/vms_session_test.sh runs the command too, but its checks are
 * whole-transcript greps for 'SYSTEM' and a Unix-leak blacklist, so nothing in
 * it is an assertion about the table SHOW USERS printed.)
 *
 * A SHOW USERS that could only ever see its own process passes both of those
 * perfectly -- which is the exact facade shape CLAUDE.md Rule 11 names, and
 * the exact shape vms-72c was fixing. The gap was proven by execution rather
 * than argued: a per-process-facade mutation injected into vms.ko's
 * proc_fill_info() -- keeping the shared table, the shared lock and every
 * write, and restricting ONLY the terminal field to the process that recorded
 * it, which is precisely the field cmd_show_users() filters rows on --
 * reddened exactly ONE assertion in the entire harness, in
 * tests/qemu/test_kmod_setterm.c, at the raw ioctl layer. Nothing at the
 * public sys$ layer and nothing through DCL noticed that a second process's
 * terminal binding had become invisible. The same shape restricted to the
 * user name reddened test_kmod_ident, test_syssvc_ident and
 * test_syssvc_procnam -- the raw and public sys$ layers -- and again nothing
 * through DCL.
 *
 * With this suite present both mutations are caught THROUGH DCL as well:
 * measured, the terminal one reddens its six A-writes/B-reads assertions and
 * the user-name one reddens exactly the Username assertion.
 *
 * So this program makes NO assertion through any sys$ or vms_kif_ reader on
 * the observing side. It EXECUTES DCL.EXE -- the shipped image, the one a
 * user types at -- feeds it SHOW USERS, and reads what the user would see.
 *
 * THE THREE-PROCESS SHAPE, and why each process is needed:
 *
 *   SUBJECT  a forked child that shares nothing with the observer but the
 *            executive. It takes a channel to the console, records it as its
 *            terminal (VMS_IOCTL_SETTERM), takes a process name
 *            (VMS_IOCTL_SETPRN) and stamps an identity on itself
 *            (VMS_IOCTL_SETIDENT) -- the same calls OVMX's own login path
 *            makes, split across PID 1 (src/ovmx_init/ovmx_init.c: $ASSIGN
 *            the console, SETTERM, SETIDENT) and LOGINOUT (tools/vms_login.c:
 *            SETPRN, SETIDENT). It then blocks, holding all of it, until
 *            released.
 *   THIS     the observer. It creates the subject but asserts nothing about
 *            it from its own memory: every expected value is what the SUBJECT
 *            read back out of the executive about ITSELF and reported over a
 *            pipe. So the A side of A-writes/B-reads is sourced from A, never
 *            from the observer's assumptions.
 *   DCL      a THIRD process, exec'd fresh, which did not create the subject,
 *            shares no memory with it, and cannot know it exists except by
 *            reading the executive.
 *
 * BOTH DIRECTIONS ARE ASSERTED. The row must APPEAR while the subject holds
 * its binding and DISAPPEAR once it exits -- a reader that always printed a
 * row would pass the first, and one that never did would pass the last.
 *
 * THE PID CHECK IS THE vms-72c REGRESSION ITSELF, NAMED. The defect that item
 * fixed printed getpid() -- a LINUX pid -- in the PID column while the same
 * session's SHOW PROCESS printed the executive-assigned VMS pid. So this
 * suite asserts BOTH that the column carries the subject's VMS pid AND that
 * it does not carry the subject's Linux pid: the first alone would be
 * satisfied by any correct-looking hex, and the second alone by any wrong
 * one.
 *
 * NOTHING HERE PINS A LAYOUT. The four column offsets are DERIVED from the
 * header line SHOW USERS itself prints, by locating its four labels -- so
 * this suite asserts what the columns CONTAIN and never re-certifies where
 * they sit. Column positions are a display question with its own oracle
 * (docs/oracle/), and a test that hard-coded them here would be self-
 * certifying a layout (CLAUDE.md Rule 10). The only structural requirement is
 * that the header carries the four labels in order, which is asserted, and
 * without which no reader -- human or program -- could tell the columns apart.
 *
 * NEGATIVE CONTROL RIG (NEGATIVE_CONTROL=1 in tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko): the executive is absent, which is not a product
 * state (vms-0ff -- PID 1 refuses to boot without one). This program then
 * asserts the property that holds regardless: SHOW USERS must print NO user
 * row it could not have read. It exits EXIT_SKIP, never a fake pass, and
 * exits 1 if that property is violated.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/* Where the harness stages the shipped DCL image (tests/qemu/Dockerfile).
 * The environment variable exists so the same program can be pointed at a
 * host build by hand. */
#define DCL_PATH_DEFAULT "/tests/DCL.EXE"

/*
 * Bound on how long DCL may take to answer one SHOW USERS. Generous by three
 * orders of magnitude (the command returns in milliseconds even under TCG)
 * and well inside run_tests.sh's 120s QEMU timeout, so a wedged DCL still
 * produces a NAMED failure line for CI to attribute rather than an
 * unattributable VM timeout. A failure bound, not pacing: nothing here sleeps
 * waiting for something to become true.
 */
#define DCL_TIMEOUT_MS 30000

/*
 * The subject's name, process name and user name. Chosen to be findable
 * (nothing else in the harness uses them) and to fit the columns SHOW USERS
 * prints: the user name field is 12 wide and the process name field 16, so a
 * longer name would shift the columns this suite derives from the header and
 * turn a display question into a spurious failure.
 */
#define SUBJ_PRCNAM   "SHOWUSRSUBJ"
#define SUBJ_USERNAME "SHOWUSR"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What the SUBJECT read back about ITSELF, out of the executive. Every
 * expectation the observer checks DCL against comes from here -- never from
 * the observer's own idea of what the subject did. */
struct subject_report {
    uint32_t register_status;
    uint32_t assign_status;
    uint32_t setterm_status;
    uint32_t setprn_status;
    uint32_t setident_status;
    uint32_t getjpi_status;
    uint32_t vms_pid;
    uint32_t linux_pid;
    char     prcnam[VMS_PRCNAM_SIZE];
    char     username[VMS_USERNAME_SIZE];
    char     terminal[VMS_DEVNAM_SIZE];
};

static const char *dcl_path(void)
{
    const char *p = getenv("OVMX_DCL");
    return (p && p[0]) ? p : DCL_PATH_DEFAULT;
}

/*
 * run_dcl - feed one DCL command line to the real DCL.EXE and capture its
 * stdout. Returns 0 on success, -1 if DCL could not be run or did not finish
 * in time.
 *
 * stdout only, deliberately: user rows are printed to stdout and diagnostics
 * to stderr, and the two are buffered differently, so a merged capture would
 * interleave non-deterministically. A test paced by luck is a flaky test.
 */
static int run_dcl(const char *cmdline, char *out, size_t outsz)
{
    char script[] = "/tmp/showusers_in.XXXXXX";
    char capture[] = "/tmp/showusers_out.XXXXXX";
    char text[512];
    int sfd, cfd, wstatus, len;
    pid_t pid;
    ssize_t n;

    out[0] = '\0';

    sfd = mkstemp(script);
    if (sfd < 0)
        return -1;
    /* The marker proves DCL ran at all, so an empty capture can never be
     * mistaken for "the command printed no row". */
    len = snprintf(text, sizeof(text),
                   "%s\nWRITE SYS$OUTPUT \"OVMX-PROBE-ALIVE\"\n", cmdline);
    if (len < 0 || write(sfd, text, (size_t)len) != len) {
        close(sfd);
        unlink(script);
        return -1;
    }
    lseek(sfd, 0, SEEK_SET);

    cfd = mkstemp(capture);
    if (cfd < 0) {
        close(sfd);
        unlink(script);
        return -1;
    }

    pid = fork();
    if (pid == 0) {
        int devnull;
        dup2(sfd, 0);
        dup2(cfd, 1);
        devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, 2);
        execl(dcl_path(), dcl_path(), (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        close(sfd); close(cfd);
        unlink(script); unlink(capture);
        return -1;
    }

    /* Bounded wait: poll waitpid rather than blocking forever, so a hung DCL
     * becomes a named FAIL instead of killing the whole VM run. */
    for (int waited = 0; waited < DCL_TIMEOUT_MS; waited += 20) {
        pid_t r = waitpid(pid, &wstatus, WNOHANG);
        if (r == pid)
            goto reaped;
        if (r < 0)
            break;
        usleep(20000);
    }
    kill(pid, 9);
    waitpid(pid, &wstatus, 0);
    close(sfd); close(cfd);
    unlink(script); unlink(capture);
    return -1;

reaped:
    lseek(cfd, 0, SEEK_SET);
    n = read(cfd, out, outsz - 1);
    out[(n > 0) ? (size_t)n : 0] = '\0';
    close(sfd); close(cfd);
    unlink(script); unlink(capture);
    return 0;
}

/* ------------------------------------------------------------------------
 * Reading the table SHOW USERS prints, WITHOUT pinning where its columns sit.
 * ---------------------------------------------------------------------- */

/* The four labels, in the order the header carries them. Their POSITIONS are
 * read off the header at run time; only their presence and order are required
 * here, because a table whose columns cannot be told apart cannot be read by
 * anyone. */
static const char *const COLUMN_LABEL[4] = {
    "Username", "Process Name", "PID", "Terminal"
};

struct table {
    int   have_header;
    size_t off[4];          /* column start offsets, derived from the header */
    const char *body;       /* first line after the header */
};

/* Locate the header line and derive the four column offsets from it. */
static int table_open(const char *out, struct table *t)
{
    const char *line = out;

    memset(t, 0, sizeof(*t));

    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char buf[512];
        size_t i;
        const char *p;
        size_t prev = 0;
        int ok = 1;

        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, line, len);
        buf[len] = '\0';

        for (i = 0; i < 4; i++) {
            p = strstr(buf, COLUMN_LABEL[i]);
            if (!p || (size_t)(p - buf) < prev) { ok = 0; break; }
            t->off[i] = (size_t)(p - buf);
            prev = t->off[i];
        }
        if (ok) {
            t->have_header = 1;
            t->body = nl ? nl + 1 : line + len;
            return 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    return -1;
}

/* Copy column `col` out of `line`, trimmed of surrounding spaces. */
static void cell(const struct table *t, const char *line, size_t linelen,
                 int col, char *outbuf, size_t outsz)
{
    size_t start = t->off[col];
    size_t end   = (col < 3) ? t->off[col + 1] : linelen;
    size_t n;

    outbuf[0] = '\0';
    if (start >= linelen)
        return;
    if (end > linelen)
        end = linelen;
    while (start < end && line[start] == ' ')
        start++;
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\r'))
        end--;
    n = end - start;
    if (n >= outsz)
        n = outsz - 1;
    memcpy(outbuf, line + start, n);
    outbuf[n] = '\0';
}

/*
 * Find the row whose PROCESS NAME cell equals `prcnam`, and copy its four
 * cells out. Returns 1 if found.
 *
 * Matched on the CELL, not on the name appearing anywhere in the capture: an
 * assertion satisfiable by the string turning up in a banner would be vacuous.
 */
static int table_find(const struct table *t, const char *prcnam,
                      char user[64], char name[64], char pid[64], char term[64])
{
    const char *line = t->body;

    if (!t->have_header || !line)
        return 0;

    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char c_user[64], c_name[64], c_pid[64], c_term[64];

        cell(t, line, len, 0, c_user, sizeof(c_user));
        cell(t, line, len, 1, c_name, sizeof(c_name));
        cell(t, line, len, 2, c_pid,  sizeof(c_pid));
        cell(t, line, len, 3, c_term, sizeof(c_term));

        if (strcmp(c_name, prcnam) == 0) {
            snprintf(user, 64, "%s", c_user);
            snprintf(name, 64, "%s", c_name);
            snprintf(pid,  64, "%s", c_pid);
            snprintf(term, 64, "%s", c_term);
            return 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

/*
 * The "Total number of users = N" figure SHOW USERS prints. Returns -1 when
 * the line is absent, which is itself an answer this suite asserts on.
 */
static int table_total(const char *out)
{
    const char *p = strstr(out, "Total number of users = ");
    int n = -1;

    if (!p)
        return -1;
    p += strlen("Total number of users = ");
    if (sscanf(p, "%d", &n) != 1)
        return -1;
    return n;
}

/* Count every data row under the header -- used to prove the "Total" figure
 * is not a constant divorced from the rows beneath it. */
static int table_rows(const struct table *t)
{
    const char *line = t->body;
    int n = 0;

    if (!t->have_header || !line)
        return 0;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        char c_name[64];

        cell(t, line, len, 1, c_name, sizeof(c_name));
        if (c_name[0] != '\0' && strcmp(c_name, "OVMX-PROBE-ALIVE") != 0)
            n++;
        line = nl ? nl + 1 : NULL;
    }
    return n;
}

static void show_capture(const char *label, const char *out)
{
    printf("  INFO: %s stdout:\n", label);
    printf("----8<----\n%s----8<----\n", out);
}

/* ------------------------------------------------------------------------
 * The subject: a process that shares nothing with the observer but /dev/vms.
 * ---------------------------------------------------------------------- */
static int subject_main(int wfd, int rfd)
{
    struct subject_report rep;
    struct vms_procinfo info;
    uint32_t chan = 0;
    char byte;

    memset(&rep, 0, sizeof(rep));
    rep.linux_pid = (uint32_t)getpid();

    if (vms_kif_open() < 0)
        _exit(80);

    rep.register_status = vms_kif_register(NULL);
    rep.assign_status   = vms_kif_assign("OPA0:", &chan);
    rep.setterm_status  = vms_kif_setterm(chan);
    rep.setprn_status   = vms_kif_setprn(SUBJ_PRCNAM);

    /*
     * Stamp an identity the way LOGINOUT does. The UIC and the authorized
     * mask are the ones this process ALREADY has -- read back from the
     * executive, not invented here -- so the only thing this call changes is
     * the user name, and the SETPRV guard in vms_ioctl_setident() is being
     * satisfied honestly rather than worked around.
     */
    memset(&info, 0, sizeof(info));
    if (vms_kif_getjpi_self(&info) == SS$_NORMAL)
        rep.setident_status = vms_kif_setident(SUBJ_USERNAME, info.uic,
                                               info.perm_privs);

    memset(&info, 0, sizeof(info));
    rep.getjpi_status = vms_kif_getjpi_self(&info);
    rep.vms_pid = info.vms_pid;
    memcpy(rep.prcnam,   info.prcnam,   sizeof(rep.prcnam));
    memcpy(rep.username, info.username, sizeof(rep.username));
    memcpy(rep.terminal, info.terminal, sizeof(rep.terminal));
    rep.prcnam[sizeof(rep.prcnam) - 1] = '\0';
    rep.username[sizeof(rep.username) - 1] = '\0';
    rep.terminal[sizeof(rep.terminal) - 1] = '\0';

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        _exit(81);

    /* Hold everything -- channel, binding, name, identity -- until released.
     * Synchronised on the pipe, never on a timer. */
    if (read(rfd, &byte, 1) != 1)
        _exit(82);

    vms_kif_close();
    return 0;
}

int main(void)
{
    char out[16384];
    struct subject_report rep;
    struct table tbl;
    struct stat st;
    int a2b[2], b2a[2];
    pid_t child;
    int baseline_total = -1;
    char c_user[64], c_name[64], c_pid[64], c_term[64];
    char want_vms_pid[64], want_linux_pid[64];

    printf("=== test_syssvc_showusers (DCL SHOW USERS reads the executive process table, A writes / B reads) ===\n");

    if (stat(dcl_path(), &st) != 0) {
        printf("  FAIL: %s is not staged in the initramfs -- this suite cannot\n",
               dcl_path());
        printf("        run the command it exists to test\n");
        printf("=== test_syssvc_showusers: 0 passed, 1 failed ===\n");
        return 1;
    }

    if (vms_kif_open() < 0) {
        /*
         * NEGATIVE-CONTROL RIG ONLY (no vms.ko). Not a product state. The
         * property asserted here is the one that holds regardless: SHOW USERS
         * must not print a user row it could not have read. Deliberately pins
         * no status value.
         */
        if (run_dcl("SHOW USERS", out, sizeof(out)) < 0) {
            printf("  FAIL: DCL.EXE could not be run\n");
            printf("=== test_syssvc_showusers: 0 passed, 1 failed ===\n");
            return 1;
        }
        show_capture("SHOW USERS with no executive", out);
        CHECK(table_open(out, &tbl) != 0 || table_rows(&tbl) == 0,
              "SHOW USERS prints NO user row when there was no executive to read one from");
        CHECK(strstr(out, "OVMX-PROBE-ALIVE") != NULL,
              "DCL ran at all (the liveness marker is present), so the empty table is an answer and not a dead binary");
        printf("=== test_syssvc_showusers: %d passed, %d failed (SKIPPED: no /dev/vms -- the A-writes/B-reads case was not exercised, the no-fabricated-row check WAS) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ---- 1. BEFORE: the subject does not exist yet ---------------------- */
    if (run_dcl("SHOW USERS", out, sizeof(out)) < 0) {
        printf("  FAIL: DCL.EXE could not be run\n");
        printf("=== test_syssvc_showusers: 0 passed, 1 failed ===\n");
        return 1;
    }
    show_capture("SHOW USERS (before)", out);

    CHECK(table_open(out, &tbl) == 0,
          "SHOW USERS prints a header carrying Username, Process Name, PID and Terminal in that order");
    if (tbl.have_header) {
        CHECK(!table_find(&tbl, SUBJ_PRCNAM, c_user, c_name, c_pid, c_term),
              "no row names the subject before the subject exists");
        baseline_total = table_total(out);
        CHECK(baseline_total >= 0,
              "SHOW USERS prints its \"Total number of users\" line");
        CHECK(baseline_total == table_rows(&tbl),
              "the total it prints EQUALS the number of rows it prints (the figure is counted from the table, not carried separately)");
    } else {
        CHECK(0, "no row names the subject before the subject exists (header unreadable)");
        CHECK(0, "SHOW USERS prints its \"Total number of users\" line (header unreadable)");
        CHECK(0, "the total it prints EQUALS the number of rows it prints (header unreadable)");
    }

    /* ---- 2. A SECOND PROCESS becomes a terminal session ------------------ */
    if (pipe(a2b) != 0 || pipe(b2a) != 0) {
        printf("  FAIL: pipe() failed\n");
        printf("=== test_syssvc_showusers: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    child = fork();
    if (child == 0) {
        /* a2b is observer -> subject (the release), b2a is subject ->
         * observer (the report). Each side closes the ends it does not use,
         * so a read here can never be satisfied by this process's own copy
         * of the write end. */
        close(a2b[1]);
        close(b2a[0]);
        return subject_main(b2a[1], a2b[0]);
    }
    if (child < 0) {
        printf("  FAIL: fork() failed\n");
        printf("=== test_syssvc_showusers: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    close(b2a[1]);
    close(a2b[0]);

    memset(&rep, 0, sizeof(rep));
    if (read(b2a[0], &rep, sizeof(rep)) != (ssize_t)sizeof(rep)) {
        printf("  FAIL: the subject process never reported what the executive holds for it\n");
        fail++;
        kill(child, 9);
        waitpid(child, NULL, 0);
        printf("=== test_syssvc_showusers: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /*
     * The subject's own writes must have SUCCEEDED, or this suite is asserting
     * about a session that was never established -- a green run would then
     * mean nothing. Reported as its own failure so the cause is named rather
     * than showing up as a mysteriously absent row.
     */
    CHECK((rep.assign_status & 1) && (rep.setterm_status & 1) &&
          (rep.setprn_status & 1) && (rep.setident_status & 1) &&
          (rep.getjpi_status & 1),
          "the subject established a real session in the executive ($ASSIGN, SETTERM, SETPRN, SETIDENT all succeeded)");
    printf("  INFO: subject vms_pid=%08X linux_pid=%u prcnam=\"%s\" username=\"%s\" terminal=\"%s\"\n",
           (unsigned)rep.vms_pid, (unsigned)rep.linux_pid,
           rep.prcnam, rep.username, rep.terminal);

    /* ---- 3. DURING: a THIRD process reads the subject out of the table --- */
    if (run_dcl("SHOW USERS", out, sizeof(out)) < 0) {
        printf("  FAIL: SHOW USERS could not be run while the subject held its session\n");
        fail++;
    } else {
        int found;

        show_capture("SHOW USERS (during)", out);
        found = (table_open(out, &tbl) == 0) &&
                table_find(&tbl, SUBJ_PRCNAM, c_user, c_name, c_pid, c_term);

        CHECK(found,
              "A-WRITES/B-READS: DCL's SHOW USERS lists a process it did not create and which is not the caller -- a row only the executive's process table could supply");

        if (found) {
            snprintf(want_vms_pid, sizeof(want_vms_pid), "%08X",
                     (unsigned)rep.vms_pid);
            snprintf(want_linux_pid, sizeof(want_linux_pid), "%08X",
                     (unsigned)rep.linux_pid);

            CHECK(strcmp(c_user, rep.username) == 0,
                  "that row's Username is the name the SUBJECT stamped on itself in the executive -- identity read across a process boundary, through DCL");
            CHECK(strcmp(c_pid, want_vms_pid) == 0,
                  "that row's PID is the VMS process ID the executive assigned the subject");
            CHECK(strcmp(c_pid, want_linux_pid) != 0,
                  "...and is NOT the subject's Linux pid, which is what the fabricated row printed before vms-72c");
            CHECK(strcmp(c_term, rep.terminal) == 0,
                  "that row's Terminal is the binding the SUBJECT made (VMS_IOCTL_SETTERM) -- a job-to-terminal binding read by a process that did not make it");

            if (baseline_total >= 0)
                CHECK(table_total(out) == baseline_total + 1,
                      "the \"Total number of users\" figure went up by exactly one when the subject appeared");
            else
                CHECK(0, "the \"Total number of users\" figure went up by exactly one when the subject appeared (no baseline)");
        } else {
            CHECK(0, "that row's Username is the name the SUBJECT stamped on itself in the executive -- identity read across a process boundary, through DCL");
            CHECK(0, "that row's PID is the VMS process ID the executive assigned the subject");
            CHECK(0, "...and is NOT the subject's Linux pid, which is what the fabricated row printed before vms-72c");
            CHECK(0, "that row's Terminal is the binding the SUBJECT made (VMS_IOCTL_SETTERM) -- a job-to-terminal binding read by a process that did not make it");
            CHECK(0, "the \"Total number of users\" figure went up by exactly one when the subject appeared");
        }
    }

    /* ---- 4. AFTER: the session ends and DCL observes THAT too ------------ */
    if (write(a2b[1], "g", 1) != 1) {
        printf("  FAIL: could not release the subject process\n");
        fail++;
    }
    close(a2b[1]);
    waitpid(child, NULL, 0);

    if (run_dcl("SHOW USERS", out, sizeof(out)) < 0) {
        printf("  FAIL: SHOW USERS could not be run after the subject exited\n");
        fail++;
    } else {
        show_capture("SHOW USERS (after)", out);
        CHECK(table_open(out, &tbl) != 0 ||
              !table_find(&tbl, SUBJ_PRCNAM, c_user, c_name, c_pid, c_term),
              "the row is GONE once the subject exits -- the disappearance is observed too, so the reader is not printing a constant");
        if (baseline_total >= 0)
            CHECK(table_total(out) == baseline_total,
                  "and the \"Total number of users\" figure came back down to what it was");
        else
            CHECK(0, "and the \"Total number of users\" figure came back down to what it was (no baseline)");
    }

    close(b2a[0]);
    vms_kif_close();

    printf("=== test_syssvc_showusers: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
