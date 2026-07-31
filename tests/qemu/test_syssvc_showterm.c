/*
 * test_syssvc_showterm.c - SHOW TERMINAL names the terminal THIS JOB is on by
 * reading the executive, and reports characteristics ANOTHER process changed
 * (vms-d0b).
 *
 * WHY THIS IS NOT tests/qemu/test_kmod_setterm.c AGAIN. That suite proves the
 * BINDING at the ioctl layer: process A records its terminal, process B reads
 * it out of A's row. What it cannot see is whether the USER-VISIBLE COMMAND is
 * a reader of that binding or is answering from somewhere else -- which is the
 * whole of this half of vms-d0b. SHOW TERMINAL used to print DCL's own
 * in-process copy of a terminal, whose name arrived in a VMS_TERMINAL
 * environment variable. That output looked perfectly plausible in a single
 * process, which is exactly why single-process coverage proves nothing about a
 * system facility (CLAUDE.md rule 11).
 *
 * So this program makes no assertion through any sys$ or vms_kif_ reader. It
 * EXECUTES DCL.EXE -- the shipped image, the one a user types at -- feeds it
 * SHOW TERMINAL, and reads what the user would see.
 *
 * THE DISCRIMINATOR IS THAT DCL IS RUN BOTH WAYS, from the same binary in the
 * same boot:
 *
 *   BOUND    the forking process takes a channel to the console and records
 *            it as its terminal BEFORE exec'ing DCL -- which is exactly what
 *            PID 1's login child does (src/ovmx_init/ovmx_init.c). DCL must
 *            name _OPA0:.
 *   UNBOUND  the same fork/exec with no binding. DCL must name NOTHING.
 *
 * Every fabrication this item deletes would pass the first and fail the
 * second: getenv, ttyname(), isatty(), a compiled-in "_FTA0:", and the one
 * that looks most like a reader -- "OPA0: is the only terminal in the device
 * table, so it must be mine". All of them answer identically in both runs.
 *
 * AND THE CHARACTERISTIC BITS ARE A-WRITES / B-READS. A second process assigns
 * the console and changes two characteristic bits (Echo, Pasthru) through the
 * executive (IO$_SETMODE); DCL -- a third process, which changed nothing --
 * must report the new values, and must report the old ones again once they are
 * put back. Both directions are asserted: a reader that always printed the new
 * value would pass the first, and one that always printed the default would
 * pass the second. The same process also sets Width through the identical
 * IO$_SETMODE call, but SHOW TERMINAL prints no Width/Page line at all
 * (vms-d0b deleted an invented one-line layout for it -- see
 * show_terminal_render() in src/vmsdcl/dcl_cmd_show.c), so what this suite
 * asserts about Width is its ABSENCE, in every one of the three states below;
 * its A-writes/B-reads proof lives at the kernel layer instead
 * (tests/qemu/test_kmod_devtab.c).
 *
 * ORACLE-PINNED OUTPUT, docs/oracle/vax73-terminal-device.md:
 *   section 1  -- the physical name form "_OPA0:" in the header.
 *   section 2  -- the header's column positions, and the four-column
 *                 characteristic grid. Five of the grid's rows are asserted
 *                 BYTE FOR BYTE against the V7.3 capture: OVMX's console
 *                 defaults agree with the lab console on those rows, so they
 *                 are lifted from it verbatim rather than recomputed here.
 *   section 3  -- "Unknown" is what VMS displays for a terminal whose device
 *                 type is not identified, which OVMX's serial console is.
 *
 * NEGATIVE CONTROL RIG (NEGATIVE_CONTROL=1 in tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko): the executive is absent, which is not a product
 * state (vms-0ff -- PID 1 refuses to boot without one). This program then
 * asserts the property that survives regardless: DCL must print no terminal it
 * could not have read. It exits EXIT_SKIP, never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/* Where tests/qemu/Dockerfile stages the shipped DCL image. */
#define DCL_PATH_DEFAULT "/tests/DCL.EXE"

/*
 * Failure bound on one DCL command, not pacing. Generous by three orders of
 * magnitude (SHOW TERMINAL returns in milliseconds even under TCG) and well
 * inside run_tests.sh's 120s QEMU timeout, so a wedged DCL produces a NAMED
 * failure line rather than an unattributable VM timeout.
 */
#define DCL_TIMEOUT_MS 30000

#define CONSOLE_DEVNAM "OPA0:"

/*
 * THE BASELINE THIS SUITE ESTABLISHES, AND WHY IT ESTABLISHES ONE.
 *
 * The console's characteristics are EXECUTIVE STATE, shared by everything on
 * the node for the life of the module -- which is the property this whole
 * epic exists to build, and it has a consequence for testing: an earlier
 * suite in the same boot legitimately leaves the device changed.
 * tests/qemu/test_kmod_devtab.c does exactly that (it proves A-writes/B-reads
 * by setting width and clearing Echo), and it runs first in the alphabet. A
 * first draft of this file asserted the module's power-on defaults and failed
 * on Width and on Echo for precisely that reason -- the values were right, the
 * assumption that nothing had touched them was wrong.
 *
 * So this suite does not assume a state: it SETS the whole mask, through the
 * executive, from a second process, and then reads it back through DCL. Every
 * value asserted below has a writer in this file.
 *
 * The set itself is the console's power-on characteristic set
 * (src/kernel/vms_devtab.c, VMS_CONSOLE_DEVCHAR), which is oracle-derived --
 * section 3, the characteristics V7.3 reports set for a terminal whose device
 * type is Unknown, less Hardcopy (the lab console is a physical LA36 and OVMX's
 * is not). Choosing it rather than an arbitrary mask is what lets five rows of
 * the section 2 capture be asserted byte for byte.
 */
#define TTC_BASELINE (VMS_TTC_INTERACTIVE    | \
                      VMS_TTC_ECHO           | \
                      VMS_TTC_TYPEAHEAD      | \
                      VMS_TTC_TTSYNC         | \
                      VMS_TTC_LOWERCASE      | \
                      VMS_TTC_WRAP           | \
                      VMS_TTC_BROADCAST      | \
                      VMS_TTC_FULLDUP        | \
                      VMS_TTC_SET_SPEED      | \
                      VMS_TTC_INSERT_EDITING | \
                      VMS_TTC_NUMERIC_KEYPAD | \
                      VMS_TTC_VMS_STYLE_INPUT)

#define TTSET_ALL (VMS_TTSET_CHAR | VMS_TTSET_WIDTH | VMS_TTSET_PAGE)

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *dcl_path(void)
{
    const char *p = getenv("OVMX_DCL");
    return (p && p[0]) ? p : DCL_PATH_DEFAULT;
}

/*
 * run_dcl - feed one DCL command line to the real DCL.EXE and capture its
 * stdout.
 *
 * `bind` decides whether the forked process records the console as its
 * terminal BEFORE exec'ing DCL. That is the only difference between the two
 * runs, and it is made in the CHILD, after fork and before exec -- the same
 * place and the same two calls as src/ovmx_init/ovmx_init.c's login child.
 * The parent never binds anything, so nothing leaks between runs.
 */
static int run_dcl(const char *cmdline, int bind, char *out, size_t outsz)
{
    char script[] = "/tmp/showterm_in.XXXXXX";
    char capture[] = "/tmp/showterm_out.XXXXXX";
    char text[512];
    int sfd, cfd, wstatus, len;
    pid_t pid;
    ssize_t n;

    out[0] = '\0';

    sfd = mkstemp(script);
    if (sfd < 0)
        return -1;
    /* The marker proves DCL ran at all, so an empty capture can never be
     * mistaken for "the command printed nothing". */
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

        if (bind) {
            uint32_t chan = 0;
            if (vms_kif_assign(CONSOLE_DEVNAM, &chan) != SS$_NORMAL)
                _exit(126);
            if (vms_kif_setterm(chan) != SS$_NORMAL)
                _exit(125);
        }
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

/*
 * Whole-line equality, so an assertion cannot be satisfied by a substring
 * appearing inside some other line with different spacing. The column layout
 * IS the pinned property here.
 */
static int has_line(const char *out, const char *want)
{
    const char *p = out;
    size_t wlen = strlen(want);

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);

        if (n == wlen && strncmp(p, want, wlen) == 0)
            return 1;
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

/* Does any line START with this prefix? Used for the header, whose Owner
 * field is not pinned (see below). */
static int has_line_prefix(const char *out, const char *want)
{
    const char *p = out;
    size_t wlen = strlen(want);

    while (p && *p) {
        if (strncmp(p, want, wlen) == 0)
            return 1;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return 0;
}

static void show_capture(const char *label, const char *out)
{
    printf("  INFO: %s stdout:\n", label);
    printf("----8<----\n%s----8<----\n", out);
}

/*
 * PROCESS A -- the writer. A distinct Linux process, therefore a distinct
 * process to the executive, so anything it changes reaches the parent's DCL
 * only if the device table is genuinely shared.
 *
 * It walks three states, one per go-ahead from the parent, acking each:
 *   1. the baseline (see TTC_BASELINE)
 *   2. width 80, Echo cleared, Pasthru set
 *   3. the baseline again
 * Synchronisation is on observed events (pipe reads), never on sleeps.
 */
static void writer_child(int wfd, int gfd)
{
    static const struct {
        uint64_t set;
        uint64_t clr;
        uint32_t width;
    } states[3] = {
        { TTC_BASELINE,     ~(uint64_t)TTC_BASELINE, 132 },
        { VMS_TTC_PASTHRU,  VMS_TTC_ECHO,             80 },
        { TTC_BASELINE,     ~(uint64_t)TTC_BASELINE, 132 },
    };
    uint32_t chan = 0;
    int i;
    char c;

    if (vms_kif_assign(CONSOLE_DEVNAM, &chan) != SS$_NORMAL) {
        if (write(wfd, "N", 1) != 1)
            _exit(2);
        _exit(2);
    }

    for (i = 0; i < 3; i++) {
        uint32_t st = vms_kif_ttsetmode(chan, TTSET_ALL,
                                        states[i].set, states[i].clr,
                                        states[i].width, 24);
        if (write(wfd, (st == SS$_NORMAL) ? "Y" : "N", 1) != 1)
            _exit(3);
        while (read(gfd, &c, 1) < 0)
            ;
    }
    _exit(0);
}

/* Wait for A's one-byte ack and assert it succeeded. */
static void expect_ack(int rfd, const char *msg)
{
    struct pollfd pfd;
    char verdict = '?';

    pfd.fd = rfd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, DCL_TIMEOUT_MS) > 0 && read(rfd, &verdict, 1) == 1)
        CHECK(verdict == 'Y', msg);
    else
        CHECK(0, "the second process never reported whether IO$_SETMODE succeeded");
}

/*
 * Five rows of the section 2 capture, verbatim. OVMX's console defaults
 * (src/kernel/vms_devtab.c, VMS_CONSOLE_DEVCHAR) agree with the lab console on
 * exactly these rows, so they are the oracle's bytes and not a restatement of
 * what OVMX happens to print. The rows that differ are NOT asserted here and
 * the differences are deliberate and recorded at the device: Hardcopy (the lab
 * console is a physical LA36, OVMX's is not) and Line Editing (section 3: V7.3
 * clears it when the device type is Unknown, which OVMX's is).
 */
static const char *const ORACLE_ROW_1 =
    "   Interactive        Echo               Type_ahead         No Escape";
static const char *const ORACLE_ROW_2 =
    "   No Hostsync        TTsync             Lowercase          No Tab";
static const char *const ORACLE_ROW_4 =
    "   Broadcast          No Readsync        No Form            Fulldup";
static const char *const ORACLE_ROW_10 =
    "   Numeric Keypad     No ANSI_CRT        No Regis           No Block_mode";
static const char *const ORACLE_ROW_13 =
    "   VMS Style Input";

/* Header through the two fields that ARE pinned. Owner is deliberately not:
 * the executive has a user name for a process only once an identity has been
 * stamped on it (VMS_IOCTL_SETIDENT), and OVMX's LOGINOUT does not do that yet
 * (vms-2b8), so the field is legitimately empty today. Asserting "SYSTEM"
 * there would be asserting a fabrication. */
static const char *const HEADER_PREFIX =
    "Terminal: _OPA0:      Device_Type: Unknown       Owner:";

int main(void)
{
    char out[16384];
    int pipefd[2], stopfd[2];
    pid_t child;
    struct stat st;

    /*
     * A BROKEN PIPE MUST BE A NAMED FAILURE, NOT A DEAD TEST, and this line
     * is here because it was neither until the full negative-control sweep
     * showed it. Under the bind-client-no-register control the writer child
     * cannot register with the executive, so it reports its failure and
     * exits -- and the parent's next write to the gate pipe then killed the
     * whole program with SIGPIPE, rc=141. The suite's verdict became a
     * signal number, which attributes nothing: the sweep could not tell a
     * suite that FOUND the defect from one that fell over. With the signal
     * ignored, every one of those writes falls into the checked
     * `if (write(...) != 1)` branches below and prints a FAIL line naming
     * what it was doing.
     */
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_showterm (DCL SHOW TERMINAL reads the executive) ===\n");

    if (stat(dcl_path(), &st) != 0) {
        printf("  FAIL: %s is not staged in the initramfs -- this suite cannot\n",
               dcl_path());
        printf("        run the command it exists to test\n");
        printf("=== test_syssvc_showterm: 0 passed, 1 failed ===\n");
        return 1;
    }

    if (vms_kif_open() < 0) {
        /*
         * NEGATIVE-CONTROL RIG ONLY (no vms.ko). Not a product state. The
         * property asserted is the one that holds regardless: DCL must not
         * name a terminal it could not have read. No binding is possible
         * here, so this is the UNBOUND run and nothing else.
         */
        if (run_dcl("SHOW TERMINAL", 0, out, sizeof(out)) < 0) {
            printf("  FAIL: DCL.EXE could not be run\n");
            printf("=== test_syssvc_showterm: 0 passed, 1 failed ===\n");
            return 1;
        }
        show_capture("SHOW TERMINAL with no executive", out);
        CHECK(strcmp(out, "OVMX-PROBE-ALIVE\n") == 0,
              "SHOW TERMINAL names NO terminal when there was no executive to read one from (whole of stdout is the liveness marker)");

        printf("=== test_syssvc_showterm: %d passed, %d failed (SKIPPED: no /dev/vms -- the bound case and the A-writes/B-reads case were not exercised, the no-fabricated-terminal check WAS) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ---- 1. UNBOUND: nothing recorded this job's terminal ------------ */
    if (run_dcl("SHOW TERMINAL", 0, out, sizeof(out)) < 0) {
        printf("  FAIL: DCL.EXE could not be run\n");
        printf("=== test_syssvc_showterm: 0 passed, 1 failed ===\n");
        return 1;
    }
    show_capture("SHOW TERMINAL (no binding)", out);
    CHECK(strcmp(out, "OVMX-PROBE-ALIVE\n") == 0,
          "with no binding in the executive, SHOW TERMINAL names NOTHING -- it does not fall back to the environment, to ttyname(), or to the only terminal in the device table");

    /* ---- 2. A SECOND PROCESS puts the console in a known state ------ */
    /*
     * Two pipes. `pipefd` carries A's acks up; `stopfd` is how the parent
     * tells A to move to the next state. Nothing here sleeps waiting for
     * something to become true.
     */
    if (pipe(pipefd) != 0 || pipe(stopfd) != 0) {
        printf("  FAIL: pipe() failed\n");
        printf("=== test_syssvc_showterm: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    child = fork();
    if (child == 0) {
        close(pipefd[0]);
        close(stopfd[1]);
        writer_child(pipefd[1], stopfd[0]);
        _exit(0);
    }
    if (child < 0) {
        printf("  FAIL: fork() failed\n");
        printf("=== test_syssvc_showterm: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    close(pipefd[1]);
    close(stopfd[0]);

    expect_ack(pipefd[0],
               "a second process put the console in a known state through the executive (IO$_SETMODE)");

    /* ---- 3. BOUND: the session recorded it, exactly as PID 1 does ---- */
    if (run_dcl("SHOW TERMINAL", 1, out, sizeof(out)) < 0) {
        printf("  FAIL: DCL.EXE could not be run for the bound case\n");
        printf("=== test_syssvc_showterm: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    show_capture("SHOW TERMINAL (terminal bound before exec)", out);
    CHECK(has_line_prefix(out, HEADER_PREFIX),
          "SHOW TERMINAL names _OPA0: once the executive holds the binding -- the SAME BINARY that named nothing a moment ago");
    CHECK(has_line(out, "Terminal Characteristics:"),
          "the characteristics heading is printed (oracle section 2)");
    CHECK(has_line(out, ORACLE_ROW_1),
          "grid row 1 is byte-for-byte the V7.3 capture");
    CHECK(has_line(out, ORACLE_ROW_2),
          "grid row 2 is byte-for-byte the V7.3 capture");
    CHECK(has_line(out, ORACLE_ROW_4),
          "grid row 4 is byte-for-byte the V7.3 capture");
    CHECK(has_line(out, ORACLE_ROW_10),
          "grid row 10 is byte-for-byte the V7.3 capture");
    CHECK(has_line(out, ORACLE_ROW_13),
          "the last row carries the single remaining characteristic, unpadded");
    CHECK(!has_line_prefix(out, "   Width:") && !has_line_prefix(out, "   Page:"),
          "SHOW TERMINAL prints no Width/Page line -- the oracle shows them "
          "only inside a block with Input/Output speed, LFfill/CRfill and "
          "Parity that OVMX cannot source, and a renderer that printed just "
          "the two fields it has would be the invented layout vms-d0b "
          "deleted (docs/oracle/vax73-terminal-device.md section 2)");

    /* ---- 4. A changes the device; DCL, a third process, sees it ------ */
    if (write(stopfd[1], "g", 1) != 1) {
        CHECK(0, "could not tell the second process to change the console");
    } else {
        expect_ack(pipefd[0],
                   "the second process changed width, Echo and Pasthru through the executive");
    }

    if (run_dcl("SHOW TERMINAL", 1, out, sizeof(out)) == 0) {
        show_capture("SHOW TERMINAL (while another process holds the change)", out);
        CHECK(!has_line_prefix(out, "   Width:") && !has_line_prefix(out, "   Page:"),
              "...still no Width/Page line while the width IS 80 at the executive -- not printing it is a display choice, not a value the reader lost");
        CHECK(has_line(out,
              "   Interactive        No Echo            Type_ahead         No Escape"),
              "...and the cleared Echo bit, in the grid cell the oracle prints it in");
        CHECK(has_line(out,
              "   No Dialup          No Secure server   No Disconnect      Pasthru"),
              "...and the set Pasthru bit, so both directions of one IO$_SETMODE are read back");
    } else {
        CHECK(0, "SHOW TERMINAL could not be run while the console was changed");
    }

    /* ---- 5. AFTER: the change is undone and DCL observes THAT ------- */
    if (write(stopfd[1], "g", 1) != 1) {
        CHECK(0, "could not tell the second process to restore the console");
    } else {
        expect_ack(pipefd[0], "the second process restored the console");
    }

    if (run_dcl("SHOW TERMINAL", 1, out, sizeof(out)) == 0) {
        show_capture("SHOW TERMINAL (after the change was undone)", out);
        CHECK(!has_line_prefix(out, "   Width:") && !has_line_prefix(out, "   Page:"),
              "...and still no Width/Page line once the width is back to 132 -- the absence does not track the value either");
        CHECK(has_line(out, ORACLE_ROW_1),
              "...and grid row 1 is the oracle's bytes again, so neither is the grid");
    } else {
        CHECK(0, "SHOW TERMINAL could not be run after the change was undone");
    }

    close(stopfd[1]);
    waitpid(child, NULL, 0);
    close(pipefd[0]);

    /* ---- 6. the bindings did not leak into this process -------------- */
    {
        struct vms_procinfo pinfo;

        memset(&pinfo, 0, sizeof(pinfo));
        CHECK(vms_kif_getjpi_self(&pinfo) == SS$_NORMAL &&
              pinfo.terminal[0] == '\0',
              "this process, which bound nothing, still has no terminal -- the bindings belonged to the DCL jobs, not to the device or to the system");
    }

    printf("=== test_syssvc_showterm: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
