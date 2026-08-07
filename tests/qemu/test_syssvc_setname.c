/*
 * test_syssvc_setname.c - DCL's "SET PROCESS/NAME=" writes the executive's
 * process table, not per-DCL-process memory (vms-fbe).
 *
 * WHY THIS SUITE EXISTS, AND WHAT IT IS GUARDING AGAINST.
 *
 * tests/qemu/test_syssvc_procnam.c and tests/qemu/test_syssvc_showproc.c
 * already prove that the executive's process table is real and that SHOW
 * SYSTEM / $GETJPI read it -- but every name in those suites arrives
 * through sys$creprc's prcnam argument or a raw vms_kif_setprn() call from
 * a test helper. NEITHER exercises the DCL command a user actually types.
 *
 * Before this item, "SET PROCESS/NAME=" (src/vmsdcl/dcl_cmd_set.c,
 * cmd_set_process()) wrote ONLY ctx->process_name, a struct private to the
 * DCL process that ran the command. `nm -D` on the built DCL.EXE shows the
 * defect directly: it imported vms_kif_getjpi_self, vms_kif_open and
 * vms_kif_procscan, and NO vms_kif_setprn SYMBOL AT ALL, even though
 * VMS_IOCTL_SETPRN already existed in the kernel and $CREPRC already used
 * it. SET PROCESS/NAME=X then F$PROCESS() read "X" back out of the same
 * private struct it had just written -- a single-process test that passes
 * perfectly while sharing nothing with any other process. That is the
 * facade class CLAUDE.md Rule 11 names: the decisive test is A-writes /
 * B-reads, so every positive assertion below runs the SET command in one
 * DCL.EXE process and reads the result back through a COMPLETELY SEPARATE
 * DCL.EXE process's SHOW SYSTEM.
 *
 * P1-P2: the executive's own row for the SET-ing process carries the name
 *        it set, read through vms_kif_procscan() -- the same reader SHOW
 *        SYSTEM uses -- from THIS test process, a third party to the SET.
 * P3:    a SECOND, independent DCL.EXE's SHOW SYSTEM names the process that
 *        ran SET PROCESS/NAME, at the pid the executive assigned it. This
 *        is the property the item is actually about.
 * P4:    a THIRD DCL.EXE's SET PROCESS/NAME=<same name>, while the first is
 *        still alive, is refused through the DCL command layer with the
 *        oracle-pinned two-line %SET-E-NOTSET / -SYSTEM-F-DUPLNAM shape
 *        (src/kernel/vms_ioctl.h:653-658, F$MESSAGE(148) on real OpenVMS
 *        VAX V7.3, ssdef.h SS$_DUPLNAM -- see the ORACLE-PINNED block in
 *        src/libvms/include/ssdef.h). The kernel's own DUPLNAM refusal is
 *        already proven at the ioctl and sys$ layers
 *        (tests/qemu/test_kmod_procnam.c, test_syssvc_procnam.c); this is
 *        the first proof that DCL's OWN error path surfaces it, since
 *        before this item DCL's SET PROCESS/NAME never called the
 *        executive at all and so could never be refused by it.
 * P5:    once the first DCL.EXE exits, the name is free again -- a FOURTH,
 *        independent DCL.EXE's SET PROCESS/NAME=<same name> ACTUALLY SETS
 *        the name in the executive's own row (verified by this test
 *        process's own vms_kif_procscan(), not just by the absence of a
 *        DUPLNAM message). Proves the lifecycle (vms_proc_reap_dead()) is
 *        reached from the DCL layer too, not just from raw ioctl callers.
 * P6:    a 15-char (VMS_PRCNAM_SIZE-1) name is accepted at the boundary;
 *        a 16-char name is REFUSED with SS$_IVLOGNAM in the oracle's
 *        two-line shape, and the process's pre-existing name is left
 *        UNCHANGED by the refusal -- not silently truncated into a
 *        different, legal-looking name (the round-1 defect: upname was
 *        sized sizeof(ctx->process_name)==16, clipping the oversized name
 *        before the executive ever saw it, so it "succeeded" truncated
 *        with no message).
 *
 * SCOPE: this suite is SET PROCESS/NAME only. It does not touch F$USER,
 * SHOW SYSTEM's other columns (test_syssvc_showproc.c owns those), or
 * SPAWN's row (src/kernel/vms_proctab.c:457) -- see the item's own
 * scope-discipline note.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened -- which happens ONLY in the CI negative-control rig, never in
 * the product (vms-0ff: PID 1 refuses to boot without the executive) -- it
 * exercises the no-fabricated-success checks in device_absent_checks() and
 * exits with EXIT_SKIP (77), never a fake pass.
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
#include <time.h>
#include <poll.h>

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

/* Unique across the whole initramfs run: every suite in tests/qemu shares
 * one booted executive and one process table. */
#define HOLDER_NAME    "OVMXFBEHOLD"
#define SETUP_FAIL_MARK "OVMXFBESETUPFAIL"

/* Oracle-pinned TWO-LINE shape (vms-fbe round 2, src/kernel/vms_ioctl.h:
 * 653-658, VAX1, OpenVMS VAX V7.3): SET PROCESS/NAME wraps the underlying
 * system-service failure in the command's own generic
 * "%SET-E-NOTSET, error modifying process name", then reports the specific
 * status on a CONTINUATION line ("-SYSTEM-F-<ident>, <text>"). Round 1 of
 * this suite pinned a bare "%SYSTEM-F-DUPLNAM, duplicate name" single line
 * -- that shape was never observed on VAX1; it was this suite's own guess,
 * and the item that opened round 2 named it explicitly as the wrong shape
 * to correct. F$MESSAGE(148)/F$MESSAGE(340) (vms-8019, ssdef.h) still
 * supply the ident/text pinning for DUPLNAM/IVLOGNAM themselves. */
#define MSG_NOTSET "%SET-E-NOTSET, error modifying process name"
#define MSG_DUPLNAM MSG_NOTSET "\n-SYSTEM-F-DUPLNAM, duplicate name"
#define MSG_IVLOGNAM MSG_NOTSET "\n-SYSTEM-F-IVLOGNAM, invalid logical name"

/* The executive-UNREACHABLE report, used ONLY by device_absent_checks().
 *
 * This one is NOT oracle-pinned and MUST NOT be: it is deliberately not a
 * VMS message. src/vmsdcl/dcl_cmd_set.c:597 emits it through
 * dcl_error("OVMX", 2, "SETPRNFAIL", ...) precisely because "the executive
 * is not there" is a condition OpenVMS never faces and therefore has no
 * message for -- inventing a "%SYSTEM-F-..." for it would be the Rule 10
 * self-certification this whole epic exists to kill. The OVMX facility
 * prefix is the honest label; the raw status is printed as the only fact
 * known, and is NOT matched here (its value comes from vms_kif's closed
 * errno mapping, not from anything $SETPRN decided).
 *
 * The text below is the leading, status-free part of that message as
 * OBSERVED by execution, not read off the format string: a device-absent
 * DCL.EXE fed "SET PROCESS/NAME=<name>\nLOGOUT\n" printed
 *   %OVMX-E-SETPRNFAIL, SET PROCESS/NAME could not reach the executive (status %X000002A4)
 *   ...logged out at...
 * and exited 0. See the device_absent_checks() header. */
#define MSG_UNREACHABLE \
    "%OVMX-E-SETPRNFAIL, SET PROCESS/NAME could not reach the executive"

/* ---------------------------------------------------------------------
 * Small helpers.
 * --------------------------------------------------------------------- */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_full(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

/* sys_row_for - the SHOW SYSTEM row naming `name`, or NULL.
 * Row shape "%08X %-15s %s" -- see test_syssvc_showproc.c's oracle-pinned
 * column positions. Only the name field is needed here. */
static const char *sys_row_for(const char *text, const char *name)
{
    const char *line = text;
    size_t namelen = strlen(name);

    while (line && *line) {
        int i;
        for (i = 0; i < 8; i++)
            if (!isxdigit((unsigned char)line[i]))
                break;
        if (i == 8 && line[8] == ' ' &&
            strncmp(line + 9, name, namelen) == 0)
            return line;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

static uint32_t pid_on_line(const char *line, int col)
{
    char buf[9];

    if (!line) return 0;
    for (int i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)line[col + i])) return 0;
        buf[i] = line[col + i];
    }
    buf[8] = '\0';
    return (uint32_t)strtoul(buf, NULL, 16);
}

/*
 * run_dcl - feed `script` to /bin/DCL.EXE, capture its output to
 * completion (the child must exit -- LOGOUT the script), and return.
 *
 * DCL.EXE is staged into the initramfs at /bin by tests/qemu/Dockerfile
 * (absence there is a FATAL image-build error, not a skip).
 */
static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    /*
     * Flush every stdio buffer -- including this process's own (parent)
     * stdout -- before forking. Without this, the child inherits a copy
     * of the PARENT's unflushed stdout buffer; if execl() ever fails,
     * the child's own fflush(stdout) after dup2'ing the capture pipe
     * would write that inherited parent data into the child's pipe,
     * splicing a prior run_dcl() call's transcript into this call's
     * capture. Latent when stdout is line-buffered (console); live the
     * moment stdout is fully buffered (redirected to a file) -- see
     * vms-cdb.
     */
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        printf(SETUP_FAIL_MARK " exec\n");
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    (void)write_full(in_pipe[1], script, strlen(script));
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
 * spawn_holder - start a DCL.EXE that runs SET PROCESS/NAME=HOLDER_NAME and
 * then BLOCKS reading its next command (stdin stays open, not closed), so
 * the process stays alive and named until the caller sends LOGOUT and
 * closes the pipe. Returns the Linux pid, and leaves *stdin_wr open for the
 * caller to write to and eventually close.
 */
static pid_t spawn_holder(int *stdin_wr)
{
    int in_pipe[2], out_pipe[2];

    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    /*
     * Flush every stdio buffer -- including this process's own (parent)
     * stdout -- before forking. Without this, the child inherits a copy
     * of the PARENT's unflushed stdout buffer; if execl() ever fails,
     * the child's own fflush(stdout) after dup2'ing the capture pipe
     * would write that inherited parent data into the child's pipe.
     * Latent when stdout is line-buffered (console); live the moment
     * stdout is fully buffered (redirected to a file) -- see vms-cdb.
     */
    fflush(NULL);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        printf(SETUP_FAIL_MARK " exec\n");
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    close(out_pipe[0]);  /* holder's stdout/stderr are not read by design:
                           * this process is kept alive by NOT reaching
                           * EOF on stdin, and its output is not part of
                           * any assertion -- what other processes can
                           * read about it through the executive is. */

    const char *script = "SET PROCESS/NAME=" HOLDER_NAME "\n";
    if (write_full(in_pipe[1], script, strlen(script)) != 0) {
        close(in_pipe[1]);
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
        return -1;
    }

    *stdin_wr = in_pipe[1];
    return pid;
}

/*
 * wait_for_named_row - poll vms_kif_procscan (bounded) for the row
 * belonging to `linux_pid` that ALSO carries a non-empty prcnam, filling
 * *info. Returns 0 on success, -1 on timeout.
 *
 * MEASURED, not assumed to be sufficient: an earlier version returned as
 * soon as a row matched linux_pid at all. DCL.EXE registers with the
 * executive (vms_kif_getjpi_self) at STARTUP, before it has read or
 * executed a single command (src/vmsdcl/dcl_main.c) -- so that version
 * raced the holder's own SET PROCESS/NAME and usually won, observing the
 * row before the name landed and failing P2 even on the FIXED code. Waiting
 * for prcnam[0] as well as the pid match removes the race without weakening
 * what is checked: it is still vms_kif_procscan(), the same reader SHOW
 * SYSTEM uses, and the caller still verifies the name against HOLDER_NAME
 * itself.
 *
 * Polling an OBSERVED executive condition rather than sleeping a fixed
 * interval, so the wait is not paced by a guess about emulated-guest speed.
 */
static int wait_for_named_row(uint32_t linux_pid, struct vms_procinfo *info,
                              int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        uint32_t idx = 0;
        while (vms_kif_procscan(&idx, info) & 1) {
            if (info->linux_pid == linux_pid && info->prcnam[0] != '\0')
                return 0;
        }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* ---------------------------------------------------------------------
 * The negative-control path: with no /dev/vms, NOTHING may report
 * success, and DCL.EXE must not fabricate one either.
 *
 * ROUND 4 REWROTE THE DCL ASSERTION HERE, AND THE REASON IS AN EXECUTED
 * OBSERVATION, NOT A READING OF THE SOURCE.
 *
 * Rounds 1-3 never ran the NEGATIVE_CONTROL=1 build. CI did, and this
 * suite exited 1 instead of the contract's 77, on this assertion:
 *
 *     CHECK(strstr(out, "%DCL-") || strstr(out, "%SYSTEM-") ||
 *           strstr(out, "$"),
 *           "DCL.EXE produced ordinary output with no executive (no crash)");
 *
 * The job's own diagnosis -- "a public sys$ entry point reported SUCCESS
 * with no executive present: a fabricated success" -- is WRONG, and is
 * filed as vms-c9c: rc=1 only means "77 was not reached", and BOTH
 * fabricated-success assertions above this block PASSED in that run.
 *
 * WHAT DCL.EXE ACTUALLY DOES, measured by running the shipped binary on a
 * host with no /dev/vms, fed exactly the script below:
 *
 *     $ printf 'SET PROCESS/NAME=OVMXFBEHOLD\nLOGOUT\n' | DCL.EXE ; echo $?
 *     %OVMX-E-SETPRNFAIL, SET PROCESS/NAME could not reach the executive (status %X000002A4)
 *       SYSTEM      logged out at  2-AUG-2026 00:52:29.96
 *     0
 *
 * So DCL.EXE does NOT crash. It refuses honestly, which is what Rule 9
 * demands. The 139 bytes it prints simply contain no "%DCL-", no
 * "%SYSTEM-" and -- because a piped, non-interactive DCL prints no "$"
 * prompt -- no "$" either. The assertion was a crash PROXY whose three
 * substrings did not include the message the product actually emits; it
 * was testing the harness's guess about DCL's output, not DCL's behaviour.
 * Two of its three disjuncts were also nearly free: "$" alone is satisfied
 * by a bare prompt from a DCL that never ran the command at all.
 *
 * The replacement asserts the observed honest refusal POSITIVELY, so it
 * goes red on a fabricated success (silence), on a crash before the
 * message, and on any invented VMS-branded condition -- see the
 * MSG_UNREACHABLE comment for why an OVMX-branded message is the correct
 * shape here rather than a %SYSTEM-F-... one.
 *
 * That it HAS teeth is not asserted, it was seen: an intermediate run
 * where DCL.EXE was staged without one of its shared libraries produced
 * only a loader error, and this assertion went RED while the
 * SETUP_FAIL_MARK check above it stayed GREEN -- the mark only catches a
 * failed execl(), not a binary that execs and then dies. Staging the
 * missing library and changing nothing else turned it GREEN. The
 * predicate was also evaluated standalone against the real 139-byte
 * capture and five defect shapes; it is GREEN on the real capture only,
 * and the disjunction it replaces was GREEN on a silent userspace
 * fallback that printed a bare "$" prompt -- i.e. on the exact Rule 9
 * defect this gate exists to catch.
 *
 * NO PRODUCT CHANGE WAS MADE OR NEEDED: src/vmsdcl/dcl_cmd_set.c:552-600
 * already routes this status away from the oracle-pinned $SETPRN refusal
 * wrapper and reports it under the OVMX facility. Nothing here resurrects
 * a degraded mode: the product cannot reach this state at all (vms-0ff --
 * PID 1 refuses to boot without an executive), which is exactly why only
 * the deliberately executive-less CI rig can observe it.
 * --------------------------------------------------------------------- */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    struct vms_procinfo info;
    uint32_t index = 0;
    uint32_t st = vms_kif_procscan(&index, &info);
    CHECK(!(st & 1),
          "the process-table scan does not report success with no executive");

    st = vms_kif_setprn(HOLDER_NAME);
    CHECK(!(st & 1),
          "vms_kif_setprn does not report success with no executive");

    static char out[65536];
    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  (no /bin/DCL.EXE -- the SET PROCESS/NAME assertion cannot run here)\n");
    } else if (run_dcl("SET PROCESS/NAME=" HOLDER_NAME "\nLOGOUT\n",
                       out, sizeof(out)) == 0) {
        CHECK(strstr(out, SETUP_FAIL_MARK) == NULL,
              "DCL.EXE ran for the device-absent SET PROCESS/NAME check");
        CHECK(strstr(out, MSG_UNREACHABLE) != NULL,
              "SET PROCESS/NAME reports the executive UNREACHABLE with no "
              "executive, rather than fabricating a success");
        CHECK(strstr(out, MSG_NOTSET) == NULL,
              "SET PROCESS/NAME does not fabricate a $SETPRN refusal "
              "(%SET-E-NOTSET) that no executive was there to return");
    }

    printf("=== test_syssvc_setname: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    static char out[65536];

    /*
     * A BROKEN PIPE MUST BE A NAMED FAILURE, NOT A DEAD TEST.
     *
     * This suite writes a DCL script into a pipe whose only reader is a
     * DCL.EXE it just forked (spawn_holder() and run_dcl()). If that
     * DCL.EXE exits before the script is written, the write lands on a
     * pipe with no reader and the DEFAULT SIGPIPE disposition kills THIS
     * PROCESS -- the suite's verdict becomes rc=141 (128+13) and it
     * attributes nothing: a suite that detected a defect is then
     * indistinguishable from one that fell over.
     *
     * That is not hypothetical here. Under tests/qemu/facility_defects.sh's
     * bind-client-no-register control (the vms-9fc defect restored: kif_bind()
     * stops calling vms_kif_register()), DCL.EXE cannot bind to the executive
     * at startup and dies immediately, so both writers race a corpse. CI's
     * per-facility attribution job reported exactly that:
     *   FAIL: suites OUTSIDE this facility failed: test_syssvc_setname(rc=141)
     * plus three of this suite's assertions reddening unattributed.
     *
     * The remedy is the one tests/qemu/test_syssvc_showterm.c:392-405 already
     * applied for the identical failure under the identical control, and its
     * comment records the same lesson: with the signal ignored the writes fail
     * into their checked branches instead of killing the program. spawn_holder()
     * then returns -1 and "the holder DCL.EXE was started" prints a FAIL naming
     * what it was doing; run_dcl() leaves `out` empty and every assertion keyed
     * on DCL's output goes red with its own text. Nothing is weakened -- a
     * signal death is replaced by named failures, which is strictly more
     * information.
     *
     * Placed before the /dev/vms probe so device_absent_checks(), which also
     * drives DCL.EXE through run_dcl(), is covered by it too.
     */
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_setname: SET PROCESS/NAME writes the executive process table ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    if (access("/bin/DCL.EXE", X_OK) != 0) {
        printf("  FAIL: /bin/DCL.EXE is not staged in this image\n");
        printf("=== test_syssvc_setname: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* -------------------------------------------------------------
     * Start the HOLDER: one DCL.EXE runs SET PROCESS/NAME=HOLDER_NAME
     * and then blocks, alive, waiting for its next command. Every
     * assertion below runs from a DIFFERENT process while it is alive.
     * ------------------------------------------------------------- */
    int holder_stdin = -1;
    pid_t holder_pid = spawn_holder(&holder_stdin);
    CHECK(holder_pid > 0, "the holder DCL.EXE was started");
    if (holder_pid <= 0) {
        printf("=== test_syssvc_setname: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    struct vms_procinfo hinfo;
    memset(&hinfo, 0, sizeof(hinfo));
    int found = wait_for_named_row((uint32_t)holder_pid, &hinfo, 15000) == 0;

    /* ---------------------------------------------------------------
     * P1-P2. THE EXECUTIVE'S OWN ROW, read by THIS process (a third
     *     party to the SET), through vms_kif_procscan() -- the exact
     *     reader SHOW SYSTEM uses.
     * --------------------------------------------------------------- */
    /* negctl-knockon: bind-client-no-register */
    CHECK(found, "the executive's process table row for the holder became named");
    if (found) {
        CHECK(strcmp(hinfo.prcnam, HOLDER_NAME) == 0,
              "the holder's executive row carries the name SET PROCESS/NAME set");
    }

    uint32_t holder_vms_pid = found ? hinfo.vms_pid : 0;

    /* ---------------------------------------------------------------
     * P3. A SECOND, INDEPENDENT DCL.EXE's SHOW SYSTEM names the holder.
     *     THIS IS THE PROPERTY THE ITEM IS ABOUT: before this fix,
     *     ctx->process_name was private to the holder's own DCL
     *     process, so no other process -- including this SHOW SYSTEM --
     *     could ever see it. Reverting the vms_kif_setprn() call in
     *     cmd_set_process() (src/vmsdcl/dcl_cmd_set.c) makes this go RED
     *     while P1/P2 above (which is also satisfied by a per-process
     *     write, since it only reaches into the SAME row via linux_pid)
     *     would still need the executive write to find HOLDER_NAME at
     *     all -- so P1/P2 already fail too under that revert. P3 is kept
     *     as the independent, user-visible confirmation.
     * --------------------------------------------------------------- */
    CHECK(run_dcl("SHOW SYSTEM\nLOGOUT\n", out, sizeof(out)) == 0,
          "SHOW SYSTEM ran under a second, independent DCL.EXE");
    const char *row = sys_row_for(out, HOLDER_NAME);
    /* negctl-knockon: bind-client-no-register */
    CHECK(row != NULL,
          "a SECOND DCL.EXE's SHOW SYSTEM named the holder, which it did not create");
    if (row && holder_vms_pid) {
        CHECK(pid_on_line(row, 0) == holder_vms_pid,
              "the row SHOW SYSTEM printed for the holder carries the holder's own pid");
    }
    if (!row)
        printf("  (SHOW SYSTEM output follows)\n%s\n  (end)\n", out);

    /* ---------------------------------------------------------------
     * P4. A THIRD DCL.EXE's SET PROCESS/NAME=<same name>, while the
     *     holder is STILL ALIVE, is refused THROUGH THE DCL COMMAND
     *     LAYER with the oracle-pinned %SYSTEM-F-DUPLNAM message. The
     *     kernel already refuses the clash (test_kmod_procnam.c,
     *     test_syssvc_procnam.c); this is the first proof that DCL's
     *     own SET PROCESS/NAME error path surfaces it, since before
     *     this item the command never reached the executive and so
     *     could never be refused by it.
     * --------------------------------------------------------------- */
    CHECK(run_dcl("SET PROCESS/NAME=" HOLDER_NAME "\nLOGOUT\n",
                  out, sizeof(out)) == 0,
          "a third DCL.EXE ran SET PROCESS/NAME for the already-held name");
    /* negctl-knockon: bind-client-no-register */
    /* negctl-knockon: proctab-duplicate-name */
    CHECK(strstr(out, MSG_DUPLNAM) != NULL,
          "SET PROCESS/NAME for a name already held refuses with the "
          "oracle-pinned two-line %SET-E-NOTSET / -SYSTEM-F-DUPLNAM shape");

    /* ---------------------------------------------------------------
     * Release the holder: LOGOUT it and wait for it to actually exit,
     * so P5 below is not racing the reap.
     * --------------------------------------------------------------- */
    (void)write_full(holder_stdin, "LOGOUT\n", 7);
    close(holder_stdin);
    int hst = 0;
    while (waitpid(holder_pid, &hst, 0) < 0 && errno == EINTR)
        ;

    /* ---------------------------------------------------------------
     * P5. Once the holder is gone, the name is free again: a FOURTH,
     *     independent DCL.EXE's SET PROCESS/NAME=<same name> succeeds.
     *     Proves vms_proc_reap_dead() is reached from the DCL layer,
     *     not just from a raw ioctl caller.
     *
     *     STRENGTHENED (vms-fbe round 2): the round-1 version only
     *     checked that the SET command's output did not contain the
     *     DUPLNAM message -- an ABSENCE-only assertion, satisfiable by
     *     SET PROCESS/NAME doing NOTHING AT ALL (no output, no call,
     *     no executive write). This version spawns a SECOND holder
     *     (reusing spawn_holder(), which blocks alive after setting
     *     HOLDER_NAME) and reads ITS executive row back through THIS
     *     process's own vms_kif_procscan() -- the same A-writes/B-reads
     *     shape as P1-P2 -- so the assertion is that the name was
     *     ACTUALLY SET, not merely that no error text was seen.
     * --------------------------------------------------------------- */
    int holder2_stdin = -1;
    pid_t holder2_pid = spawn_holder(&holder2_stdin);
    CHECK(holder2_pid > 0,
          "a fourth, independent DCL.EXE ran SET PROCESS/NAME after the holder exited");
    if (holder2_pid > 0) {
        struct vms_procinfo hinfo2;
        memset(&hinfo2, 0, sizeof(hinfo2));
        int found2 = wait_for_named_row((uint32_t)holder2_pid, &hinfo2, 15000) == 0;
        /* negctl-knockon: bind-client-no-register */
        CHECK(found2,
              "the executive's process table row for the second holder became named");
        if (found2) {
            CHECK(strcmp(hinfo2.prcnam, HOLDER_NAME) == 0,
                  "the name freed by the first holder's exit was ACTUALLY SET for the "
                  "second holder (not just DUPLNAM-absent)");
        }
        (void)write_full(holder2_stdin, "LOGOUT\n", 7);
        close(holder2_stdin);
        int h2st = 0;
        while (waitpid(holder2_pid, &h2st, 0) < 0 && errno == EINTR)
            ;
    }

    /* ---------------------------------------------------------------
     * P6. THE BLOCKER THIS ROUND FIXES: an oversized (16-char) name
     *     must be REFUSED by the executive with SS$_IVLOGNAM, in the
     *     oracle's two-line shape, AND the process's PRE-EXISTING name
     *     must be LEFT UNCHANGED -- not truncated into a different,
     *     legal-looking name. Oracle: src/kernel/vms_ioctl.h:653-658
     *     (VAX1, OpenVMS VAX V7.3):
     *       $ SET PROCESS/NAME="IMPL8019NAM15XY"   ! 15 chars -> accepted
     *       $ SET PROCESS/NAME="IMPL8019NAM15XYZ"  ! 16 chars
     *       %SET-E-NOTSET, error modifying process name
     *       -SYSTEM-F-IVLOGNAM, invalid logical name
     *       $ WRITE SYS$OUTPUT F$GETJPI("","PRCNAM") ! old name UNCHANGED
     *       IMPL8019NAM15XY
     *
     *     BOUND_NAME is exactly VMS_PRCNAM_SIZE-1 = 15 characters (a
     *     legal boundary name, mirroring the oracle's first line).
     *     OVER_NAME is exactly 16 characters, and its first 15 bytes
     *     ("OVMXFBELEN16ABC") are DELIBERATELY DIFFERENT from
     *     BOUND_NAME's 15 bytes ("OVMXFBELEN15ABC") -- so a truncation
     *     regression (round 1's actual bug: upname sized at 16 instead
     *     of VMS_PRCNAM_XFER) would show up as a DIFFERENT string in
     *     SHOW SYSTEM, not one that happens to equal BOUND_NAME by
     *     construction. The lengths are asserted at compile time so
     *     the boundary claim cannot silently drift out of sync with
     *     the string literals (Method 4).
     * --------------------------------------------------------------- */
#define BOUND_NAME "OVMXFBELEN15ABC"
#define OVER_NAME  "OVMXFBELEN16ABCD"
    _Static_assert(sizeof(BOUND_NAME) - 1 == 15,
                   "BOUND_NAME must be exactly VMS_PRCNAM_SIZE-1 characters");
    _Static_assert(sizeof(OVER_NAME) - 1 == 16,
                   "OVER_NAME must be exactly one character over the legal limit");

    CHECK(run_dcl("SET PROCESS/NAME=" BOUND_NAME "\n"
                  "SHOW SYSTEM\n"
                  "SET PROCESS/NAME=" OVER_NAME "\n"
                  "SHOW SYSTEM\n"
                  "LOGOUT\n",
                  out, sizeof(out)) == 0,
          "a fifth DCL.EXE ran the boundary/oversized SET PROCESS/NAME script");

    /* negctl-knockon: bind-client-no-register */
    CHECK(strstr(out, BOUND_NAME) != NULL,
          "the 15-char boundary-legal name (VMS_PRCNAM_SIZE-1) was accepted");
    /* negctl-knockon: bind-client-no-register */
    CHECK(strstr(out, MSG_IVLOGNAM) != NULL,
          "the 16-char oversized name is refused with the oracle's two-line "
          "%SET-E-NOTSET / -SYSTEM-F-IVLOGNAM shape, now that upname is sized "
          "VMS_PRCNAM_XFER and reaches the executive intact");
    CHECK(strstr(out, "OVMXFBELEN16ABC") == NULL,
          "the truncated form of the oversized name (round 1's actual bug) "
          "never appears anywhere in the output -- the executive was never "
          "handed a clipped, legal-looking name to silently accept");

    /* The capture holds BOTH SHOW SYSTEM outputs. sys_row_for() scans from
     * byte 0, so searching the whole capture would match the row printed
     * BEFORE the refused rename -- and an implementation whose refusal WIPED
     * the name would still pass. Cut the capture at the refusal message and
     * search only what follows, so the POST-refusal SHOW SYSTEM is the only
     * thing that can satisfy the assertion. */
    const char *post_refusal = strstr(out, MSG_IVLOGNAM);
    if (post_refusal)
        post_refusal += strlen(MSG_IVLOGNAM);
    const char *row2 = post_refusal ? sys_row_for(post_refusal, BOUND_NAME) : NULL;
    /* negctl-knockon: bind-client-no-register */
    CHECK(row2 != NULL,
          "after the refused rename, SHOW SYSTEM still names this process "
          "with its PRE-EXISTING boundary name -- left UNCHANGED by the "
          "refusal, exactly as the oracle transcript shows");
    if (!row2)
        printf("  (P6 SHOW SYSTEM output follows)\n%s\n  (end)\n", out);
#undef BOUND_NAME
#undef OVER_NAME

    /* ---------------------------------------------------------------
     * METHOD 5 (investigative -- OBSERVATIONS, not oracle-pinned
     * verdicts). The item asked whether the upname-truncation defect
     * could have been hiding any OTHER input besides the 16-char case.
     * These two are NOT part of what round 1 broke or what the three
     * required changes above fix, and NEITHER has an oracle transcript
     * in this repo -- so per Rule 10 ("never self-certify a constant
     * value or a message") they are printed as OBSERVED FACTS, not
     * asserted as VMS-correct or -incorrect with CHECK(). Only that
     * the probe script itself ran is a CHECK() -- that part IS a
     * verifiable fact regardless of which behavior VMS turns out to
     * want.
     * --------------------------------------------------------------- */
    CHECK(run_dcl("SET PROCESS/NAME=\"\"\nSHOW SYSTEM\nLOGOUT\n",
                  out, sizeof(out)) == 0,
          "a DCL.EXE ran the empty-name (METHOD 5) investigative probe");
    printf("  OBSERVED (SET PROCESS/NAME=\"\", no oracle pin in this repo -- "
           "NOT asserted right or wrong): %s\n",
           (strstr(out, "NOTSET") || strstr(out, "IVLOGNAM"))
               ? "refused by the executive"
               : "SILENT NO-OP -- dcl_cmd_set.c's "
                 "\"if (name_val && *name_val)\" guard skips the whole "
                 "block before vms_kif_setprn() is ever called, so an "
                 "empty name never reaches the executive to be refused "
                 "or accepted. This guard predates this round and is a "
                 "DIFFERENT code path from the upname buffer this item "
                 "fixes -- flagged, not fixed, here.");

    CHECK(run_dcl("SET PROCESS/NAME=\"OVMXFBE!BAD\"\nSHOW SYSTEM\nLOGOUT\n",
                  out, sizeof(out)) == 0,
          "a DCL.EXE ran the non-alnum-character (METHOD 5) investigative probe");
    printf("  OBSERVED (SET PROCESS/NAME=\"OVMXFBE!BAD\", no oracle pin in "
           "this repo -- NOT asserted right or wrong): %s\n",
           (strstr(out, "NOTSET") || strstr(out, "IVLOGNAM"))
               ? "refused by the executive"
               : "ACCEPTED -- vms_proctab.c's name_is_valid() only checks "
                 "for a NUL within VMS_PRCNAM_SIZE bytes; it does not "
                 "validate character set at all, so '!' and any other "
                 "byte a legal-length name contains is stored as given.");

    printf("=== test_syssvc_setname: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
