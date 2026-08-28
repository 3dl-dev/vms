/*
 * test_syssvc_showdev.c - SHOW DEVICE is a READER of the executive's device
 * table, proven by driving the REAL DCL.EXE against a real /dev/vms inside
 * QEMU (vms-fb9).
 *
 * WHY THIS IS NOT ANOTHER test_kmod_devtab.c. tests/qemu/test_kmod_devtab.c
 * already proves the executive's device table itself, A-writes/B-reads, at
 * the ioctl layer. What it cannot see is whether the USER-VISIBLE COMMAND is
 * a reader of that table or is answering from somewhere else -- which is the
 * whole of vms-fb9. SHOW DEVICE used to walk /proc/mounts, invent "$1$DGAn:"
 * names for Linux mount points, add a process-local table MOUNT kept in its
 * own memory, and print a hardcoded stub row when all of that produced
 * nothing. Every one of those rows looked perfectly plausible in a single
 * process, which is exactly why single-process coverage proves nothing about
 * a system facility (CLAUDE.md rule 11).
 *
 * So this program does not call any sys$ or vms_kif_ entry point to make its
 * assertions. It EXECUTES DCL.EXE -- the shipped image, the one a user types
 * at -- feeds it SHOW DEVICE, and reads what the user would see.
 *
 * THE DECISIVE TEST IS A-WRITES / B-READS, and it is the reason for the
 * fork() below. Reading OPA0: out of the table from one process proves only
 * that the row exists; a devinfo_fill() patched to return a per-process view
 * would satisfy it perfectly. So a SECOND process (the child) allocates the
 * console through the executive, and then DCL -- a THIRD process, which did
 * nothing and knows nothing about the child -- must observe the change:
 *
 *   1. Before:  SHOW DEVICE prints "OPA0: ... Online"        (no "alloc")
 *   2. Child $ALLOCs OPA0: and stays alive holding it
 *   3. During:  SHOW DEVICE prints "OPA0: ... Online alloc"  <-- B reads A
 *   4. Child exits; the executive releases the allocation with it
 *   5. After:   SHOW DEVICE prints "OPA0: ... Online"        again
 *
 * Steps 3 and 5 are both required: a reader that always printed "alloc"
 * would pass 3, and one that never did would pass 1 and 5.
 *
 * "Online" / "Online alloc" AND THEIR COLUMNS ARE ORACLE-MEASURED, not
 * chosen: docs/oracle/vax73-terminal-device.md section 4.1, captured on the
 * ~/vax OpenVMS VAX V7.3 lab for this test, including the same
 * A-writes/B-reads shape driven with RUN/DETACHED. Section 4.1 also records
 * that ownership alone does NOT appear in this column -- only allocation --
 * which is why the child allocates rather than merely assigning a channel.
 *
 * NEGATIVE CONTROL RIG (NEGATIVE_CONTROL=1 in tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko): the executive is absent, which is not a
 * product state (vms-0ff -- PID 1 refuses to boot without one). This program
 * then asserts the property that survives regardless: DCL must print NO
 * device row it could not have read. It exits EXIT_SKIP, never a fake pass,
 * and exits 1 if that property is violated.
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
#include "tcg_deadline.h"

#define EXIT_SKIP 77

/*
 * Where the harness stages the shipped DCL image. tests/qemu/Dockerfile
 * copies build-static/bin/DCL.EXE here; the environment variable exists so
 * the same program can be pointed at a host build by hand.
 */
#define DCL_PATH_DEFAULT "/tests/DCL.EXE"

/*
 * Bound on how long DCL may take to answer one SHOW DEVICE. Generous by
 * three orders of magnitude (the command returns in milliseconds even under
 * TCG) and well inside run_tests.sh's 120s QEMU timeout, so a wedged DCL
 * still produces a NAMED failure line for CI to attribute rather than an
 * unattributable VM timeout. This is a failure bound, not pacing: nothing
 * here sleeps waiting for something to become true.
 */
#define DCL_TIMEOUT_MS 30000

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
 * stdout into `out`. Returns 0 on success, -1 if DCL could not be run or did
 * not finish in time.
 *
 * stdout only, deliberately: device rows are printed to stdout and
 * diagnostics to stderr, and the two are buffered differently, so a merged
 * capture would interleave non-deterministically. A test paced by luck is a
 * flaky test. stderr is captured separately by run_dcl_err() when a
 * diagnostic is what is being asserted.
 */
static int run_dcl_streams(const char *cmdline, char *out, size_t outsz,
                           int want_stderr)
{
    char script[] = "/tmp/showdev_in.XXXXXX";
    char capture[] = "/tmp/showdev_out.XXXXXX";
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
        dup2(sfd, 0);
        dup2(cfd, want_stderr ? 2 : 1);
        /* Send the stream we are NOT capturing to /dev/null so it cannot
         * land in the capture file. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, want_stderr ? 1 : 2);
        execl(dcl_path(), dcl_path(), (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        close(sfd); close(cfd);
        unlink(script); unlink(capture);
        return -1;
    }

    /* Bounded wait: poll waitpid rather than blocking forever, so a hung
     * DCL becomes a named FAIL instead of killing the whole VM run. */
    long dcl_deadline_ms = ovmx_tcg_ms(DCL_TIMEOUT_MS);
    for (long waited = 0; waited < dcl_deadline_ms; waited += 20) {
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

static int run_dcl(const char *cmdline, char *out, size_t outsz)
{
    return run_dcl_streams(cmdline, out, outsz, 0);
}

static int run_dcl_err(const char *cmdline, char *out, size_t outsz)
{
    return run_dcl_streams(cmdline, out, outsz, 1);
}

/* The console row as the oracle prints it: name in columns 0-23, status from
 * column 24. Matching on the leading name anchors to a row rather than to
 * the word OPA0: appearing anywhere. */
static const char *console_row(const char *out)
{
    const char *p = out;

    while (p && *p) {
        if (strncmp(p, "OPA0:", 5) == 0)
            return p;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return NULL;
}

/*
 * Scoped to the console's OWN row, not the whole capture: the word must come
 * from the row's Device Status column, so an assertion satisfiable by the
 * word appearing anywhere else in the output would be vacuous.
 */
static int row_says_alloc(const char *out)
{
    const char *row = console_row(out);
    const char *nl;
    char line[256];
    size_t n;

    if (!row)
        return 0;
    nl = strchr(row, '\n');
    n = nl ? (size_t)(nl - row) : strlen(row);
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    memcpy(line, row, n);
    line[n] = '\0';
    return strstr(line, "alloc") != NULL;
}

static void show_capture(const char *label, const char *out)
{
    printf("  INFO: %s stdout:\n", label);
    printf("----8<----\n%s----8<----\n", out);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    char out[8192];
    int pipefd[2], stopfd[2];
    pid_t child;
    struct stat st;

    printf("=== test_syssvc_showdev (DCL SHOW DEVICE reads the executive table) ===\n");

    if (stat(dcl_path(), &st) != 0) {
        printf("  FAIL: %s is not staged in the initramfs -- this suite cannot\n",
               dcl_path());
        printf("        run the command it exists to test\n");
        printf("=== test_syssvc_showdev: 0 passed, 1 failed ===\n");
        return 1;
    }

    if (vms_kif_open() < 0) {
        /*
         * NEGATIVE-CONTROL RIG ONLY (no vms.ko). Not a product state. The
         * property asserted here is the one that holds regardless of what an
         * absent executive would "mean": DCL must not print a device row it
         * could not have read. Deliberately pins no status value.
         */
        if (run_dcl("SHOW DEVICE", out, sizeof(out)) < 0) {
            printf("  FAIL: DCL.EXE could not be run\n");
            printf("=== test_syssvc_showdev: 0 passed, 1 failed ===\n");
            return 1;
        }
        show_capture("SHOW DEVICE with no executive", out);
        CHECK(strcmp(out, "OVMX-PROBE-ALIVE\n") == 0,
              "SHOW DEVICE prints NO device row when there was no executive to read one from (whole of stdout is the liveness marker)");

        if (run_dcl_err("SHOW DEVICE", out, sizeof(out)) == 0)
            CHECK(strstr(out, "NOSUCHDEV") == NULL,
                  "an unanswered read is NOT reported with the oracle's NOSUCHDEV, which was measured for a device the executive said does not exist");

        printf("=== test_syssvc_showdev: %d passed, %d failed (SKIPPED: no /dev/vms -- the A-writes/B-reads case was not exercised, the no-fabricated-row checks WERE) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ---- 1. BEFORE: the console is listed, and it is not allocated ---- */
    if (run_dcl("SHOW DEVICE", out, sizeof(out)) < 0) {
        printf("  FAIL: DCL.EXE could not be run\n");
        printf("=== test_syssvc_showdev: 0 passed, 1 failed ===\n");
        return 1;
    }
    show_capture("SHOW DEVICE (before)", out);
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: devtab-devscan-found-status-wrong */
    CHECK(console_row(out) != NULL,
          "bare SHOW DEVICE lists OPA0: -- a device DCL has no other way to know about, read from the executive's table");
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: devtab-devscan-found-status-wrong */
    CHECK(strstr(out, "Device                  Device           Error") != NULL,
          "the listing carries the oracle's column header (section 4)");
    CHECK(!row_says_alloc(out),
          "the console row does NOT say 'alloc' while nobody has allocated it");

    if (run_dcl("SHOW DEVICE OPA0:", out, sizeof(out)) == 0) {
        show_capture("SHOW DEVICE OPA0: (before)", out);
        /* negctl-knockon: bind-client-no-register */
        CHECK(console_row(out) != NULL,
              "SHOW DEVICE OPA0: resolves the name through the executive and prints its row");
    } else {
        CHECK(0, "SHOW DEVICE OPA0: could not be run");
    }

    /* ---- 1b. SHOW DEVICE lists the NIC face ETH0: (vms-9d2, epic vms-67f L0).
     * The guest has one virtio-net NIC (run_tests.sh); the executive enters
     * ETH0: from it via the generic netdev abstraction, so SHOW DEVICE -- a pure
     * reader of that table -- lists it beside OPA0:, and SHOW DEVICE ETH0:
     * resolves the name through the executive. On a node with no NIC there would
     * be no ETH0: row and SHOW DEVICE ETH0: would print the oracle's NOSUCHDEV;
     * the name is keyed on VMS_NIC_DEVNAM in src/kernel-core/vms_devtab.c. */
    if (run_dcl("SHOW DEVICE ETH0:", out, sizeof(out)) == 0) {
        show_capture("SHOW DEVICE ETH0:", out);
        CHECK(strstr(out, "ETH0:") != NULL && strstr(out, "NOSUCHDEV") == NULL,
              "SHOW DEVICE ETH0: resolves the NIC name through the executive and prints its row");
    } else {
        CHECK(0, "SHOW DEVICE ETH0: could not be run");
    }

    /* ---- 2. A SECOND PROCESS allocates the console ------------------- */
    /*
     * Two pipes, not one. `report` carries the child's verdict up; `stop`
     * is how the parent tells the child to exit, by CLOSING its write end.
     * The child blocks in read() on stop[0] until then -- synchronising on
     * an observed event, never on a sleep, because a test paced by fixed
     * sleeps against an emulated guest is a flaky test.
     */
    if (pipe(pipefd) != 0 || pipe(stopfd) != 0) {
        printf("  FAIL: pipe() failed\n");
        printf("=== test_syssvc_showdev: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    child = fork();
    if (child == 0) {
        /*
         * PROCESS A. A distinct Linux process, therefore a distinct process
         * to the executive (vms_kif.c keys its registration on the tgid, and
         * detects the descriptor it inherited through fork as belonging to
         * another process), so anything it changes is only visible to the
         * parent's DCL if the device table is genuinely shared. It holds the
         * allocation by staying alive: the executive releases a dying
         * process's allocations (src/kernel/vms_devtab.c,
         * vms_proc_release_channels), which is what step 5 relies on.
         */
        uint32_t st;
        char c;

        close(pipefd[0]);
        close(stopfd[1]);
        st = vms_kif_alloc("OPA0:");
        if (write(pipefd[1], (st & 1) ? "Y" : "N", 1) != 1)
            _exit(2);
        while (read(stopfd[0], &c, 1) < 0)
            ;
        _exit(0);
    }
    if (child < 0) {
        printf("  FAIL: fork() failed\n");
        printf("=== test_syssvc_showdev: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    close(pipefd[1]);
    close(stopfd[0]);

    {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        char verdict = '?';
        int pr = poll(&pfd, 1, (int)ovmx_tcg_ms(DCL_TIMEOUT_MS));

        /* negctl-knockon: bind-client-no-register */
        if (pr > 0 && read(pipefd[0], &verdict, 1) == 1)
            CHECK(verdict == 'Y',
                  "the second process allocated OPA0: through the executive ($ALLOC)");
        else
            CHECK(0, "the second process never reported whether $ALLOC succeeded");
    }

    /* ---- 3. DURING: DCL, a third process, observes the change -------- */
    if (run_dcl("SHOW DEVICE OPA0:", out, sizeof(out)) == 0) {
        show_capture("SHOW DEVICE OPA0: (while another process holds it)", out);
        /* negctl: devtab-alloc-not-recorded */
        /* negctl-knockon: bind-client-no-register */
        CHECK(row_says_alloc(out),
              "A-WRITES/B-READS: DCL's SHOW DEVICE reports the console allocated -- a change made by a DIFFERENT process, which a per-process device view could not show");
    } else {
        CHECK(0, "SHOW DEVICE OPA0: could not be run while the console was allocated");
    }

    /* negctl: devtab-alloc-not-recorded */
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: devtab-devscan-found-status-wrong */
    if (run_dcl("SHOW DEVICE", out, sizeof(out)) == 0)
        CHECK(row_says_alloc(out),
              "the bare listing shows it too, so both row sources ($DEVICE_SCAN and $GETDVI) read the same shared table");

    /* ---- 4/5. AFTER: the allocation ends and DCL observes THAT ------- */
    close(stopfd[1]);
    waitpid(child, NULL, 0);
    close(pipefd[0]);

    if (run_dcl("SHOW DEVICE OPA0:", out, sizeof(out)) == 0) {
        show_capture("SHOW DEVICE OPA0: (after the holder exited)", out);
        /* negctl-knockon: bind-client-no-register */
        CHECK(console_row(out) != NULL,
              "the console is still listed once the other process is gone");
        CHECK(!row_says_alloc(out),
              "...and no longer reports 'alloc' -- the disappearance is observed too, so the reader is not simply printing a constant");
    } else {
        CHECK(0, "SHOW DEVICE OPA0: could not be run after the holder exited");
    }

    /* ---- 6. A device the executive does not have is refused as VMS does */
    if (run_dcl("SHOW DEVICE ZZA0:", out, sizeof(out)) == 0)
        CHECK(strcmp(out, "OVMX-PROBE-ALIVE\n") == 0,
              "SHOW DEVICE ZZA0: prints no row for a device that does not exist");
    /* negctl-knockon: bind-client-no-register */
    if (run_dcl_err("SHOW DEVICE ZZA0:", out, sizeof(out)) == 0)
        CHECK(strstr(out, "NOSUCHDEV, no such device available") != NULL,
              "...and refuses it with the oracle's own message (section 6)");

    printf("=== test_syssvc_showdev: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
