/*
 * test_syssvc_mmk_drive.c - the SHIPPED MMK.EXE drives a REAL build through its
 * persistent mailbox-driven DCL subprocess, against a real /dev/vms (vms-b23,
 * self-host spine #4).
 *
 * ============================================================
 * THE POINT OF vms-b23. MMK builds by keeping ONE persistent DCL subprocess and
 * streaming resolved command lines into it over a VMS mailbox, learning each
 * command finished by a write-attention AST that interrupts a $HIBER, draining
 * the result with a non-blocking IO$M_NOW read, and reading $STATUS from an
 * end-of-command marker (build_target.c's send_cmd_and_wait / echo_ast, driven
 * by tests/corpus/tier3-mmk/ovmx/ovmx_mmk_sp.c's sp_open/sp_send/sp_receive).
 * Until this item that drive was an honest SS$_UNSUPPORTED stub. This suite
 * proves the WHOLE composition end to end, by running the actual MMK.EXE image
 * against a real descrip.mms -- not a hand-rolled stand-in for the protocol.
 *
 * The composed facilities each have their own /dev/vms proof (lib$spawn vms-98c,
 * mailbox IPC vms-e0b, write-attention AST vms-9003, $HIBER-interrupting async
 * AST delivery vms-feb, IO$M_NOW vms-5df, DCL-over-mailbox vms-786); this suite
 * proves MMK.EXE ITSELF, through ovmx_mmk_sp.c, composes them into a working
 * build drive.
 *
 * INDEPENDENT ORACLE (CLAUDE.md Rule 11 / the veracity crux, à la vms-786's
 * OVMX786:42). The descrip.mms action does not echo a constant. It makes DCL
 * COMPUTE  ANSWER = 6 * 7  and WRITE "OVMXB23:''ANSWER'" to SYS$OUTPUT. The
 * marker the parent asserts, "OVMXB23:42", exists nowhere unless the persistent
 * DCL MMK spawned actually READ the action line out of its SYS$INPUT mailbox,
 * EVALUATED the arithmetic and the symbol substitution, and DELIVERED the
 * computed line back over its SYS$OUTPUT mailbox -- which MMK's write-attention
 * AST then drained (waking its $HIBER) and echoed to its own SYS$OUTPUT. The
 * parent knows 6*7==42 but never computed it inside the drive; there is nowhere
 * local for "OVMXB23:42" to come from. And MMK completing at all (not hanging in
 * $HIBER) proves it detected the "MMK____status=" end-of-command marker -- the
 * $STATUS-capture half of the protocol.
 *
 * RENDEZVOUS / NO SHARED HANDLE. MMK.EXE is a genuinely separate image the test
 * fork+execs; it makes its own PCB, $CREMBXes its command + result mailboxes
 * (publishing them as SYS$INPUT/SYS$OUTPUT), and lib$spawns SYS$SYSTEM:DCL.EXE,
 * which reaches those mailboxes only by the published logical names (dcl_mbx.c).
 * The test shares nothing with MMK but a work directory of files on the SYSDISK.
 *
 * NO EXECUTIVE (honest-failure branch, run on the host before vms.ko is loaded,
 * exactly as test_syssvc_mbx_dcldrv.c does): $CREMBX must fail SS$_NOSUCHDEV,
 * never fabricate a private per-process mailbox (Rule 9 / INV-6). This suite
 * returns EXIT_SKIP (77) there -- the contract ci.yml's
 * kernel-executive-negative-control job holds every test_syssvc_* suite to.
 *
 * NEGATIVE CONTROL (facility_defects.sh mmk-drive-command-not-sent): the
 * end-to-end assertion is anchored by a mutation that makes ovmx_mmk_sp.c's
 * sp_send forward ZERO bytes of each command into the SYS$INPUT mailbox. Under
 * it the spawned DCL receives no action line, computes nothing and delivers
 * nothing -- MMK produces no "OVMXB23:42" and the driven-build assertion reddens
 * -- while a no-executive run still honest-skips.
 *
 * BOUNDED, NO SLEEPS. The parent poll()s MMK's stdout with a generous failure
 * bound and reaps MMK with a bounded loop, so a wedged drive is a NAMED FAIL
 * here rather than an unattributable VM-level timeout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* MMK.EXE is staged at SYS$SYSTEM (tests/qemu/Dockerfile). OVMX_MMK overrides. */
#define MMK_PATH_DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE"

/* Failure bound (ms) on how long MMK may take to spawn a DCL, drive one action
 * command through the mailbox, and echo the computed result -- milliseconds
 * under TCG; this is a failure ceiling, not pacing. Kept well under half
 * run_tests.sh's 120s QEMU timeout so that BOTH bounded waits below (marker
 * scan, then reap) fit even when the drive is deliberately broken (the negctl,
 * which wedges MMK in $HIBER): a wedged drive becomes a named FAIL here, not an
 * unattributable VM-level timeout. */
#define DRIVE_TIMEOUT_MS 25000

/* The computed marker the driven DCL must PRODUCE (6*7 evaluated in DCL). */
#define EXPECT_MARKER  "OVMXB23:42"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *mmk_path(void)
{
    const char *p = getenv("OVMX_MMK");
    return (p && p[0]) ? p : MMK_PATH_DEFAULT;
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Write a file, return 0 on success. */
static int write_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(contents);
    int ok = (fwrite(contents, 1, n, f) == n);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_mmk_drive (shipped MMK.EXE drives a real build over its mailbox-driven DCL, vms-b23) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        /* NO EXECUTIVE: $CREMBX must fail honestly (Rule 9 / INV-6). MMK's own
         * sp_open would return that same SS$_NOSUCHDEV; assert the primitive
         * here so the suite honest-skips exactly like its siblings. */
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc("SYS$INPUT");
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mmk_drive: %d passed, %d failed (SKIPPED: no /dev/vms -- MMK build drive not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* The shipped MMK image must be present to drive. */
    struct stat stbuf;
    if (stat(mmk_path(), &stbuf) != 0) {
        printf("  FAIL: shipped MMK image not found at %s\n", mmk_path());
        return 1;
    }

    /* A hermetic work directory ON THE SYSDISK (/vms). MMK opens the description
     * file through OVMX RMS; with a real executive present RMS resolves a bare
     * filespec against SYS$DISK (the /vms system disk), NOT the Linux cwd, so the
     * descrip must live under /vms to be found. The child chdir()s here before
     * exec so a bare "OVMXB23.MMS" resolves to this directory. */
    char workdir[] = "/vms/mmkb23_XXXXXX";
    if (!mkdtemp(workdir)) {
        printf("  FAIL: mkdtemp() under /vms failed\n");
        return 1;
    }

    /* A real MMS description file: OVMXB23.OUT depends on OVMXB23.IN, with a
     * tab-indented action that makes DCL COMPUTE the independent oracle and
     * write it to SYS$OUTPUT (which rides back over MMK's result mailbox and is
     * echoed to MMK's own SYS$OUTPUT). MMK requires the leading TAB. */
    char path[256];
    snprintf(path, sizeof(path), "%s/OVMXB23.MMS", workdir);
    if (write_file(path,
            "OVMXB23.OUT : OVMXB23.IN\n"
            "\tANSWER = 6 * 7\n"
            "\tWRITE SYS$OUTPUT \"OVMXB23:''ANSWER'\"\n") != 0) {
        printf("  FAIL: could not write descrip.mms\n");
        return 1;
    }
    /* The dependency source must exist so MMK resolves the rule. */
    snprintf(path, sizeof(path), "%s/OVMXB23.IN", workdir);
    if (write_file(path, "vms-b23 spine-4 build input\n") != 0) {
        printf("  FAIL: could not write OVMXB23.IN\n");
        return 1;
    }
    /* /RULES defaults to MMS$RULES; an empty one keeps the run's status clean. */
    snprintf(path, sizeof(path), "%s/MMS$RULES", workdir);
    if (write_file(path, "! empty default rules (vms-b23 drive test)\n") != 0) {
        printf("  FAIL: could not write MMS$RULES\n");
        return 1;
    }

    /* Capture MMK's SYS$OUTPUT so we can scan for the driven oracle. */
    int outpipe[2];
    if (pipe(outpipe) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        /* Child = the shipped MMK. Run the REAL build (NOT /NOACTION): MMK opens
         * the persistent DCL and drives the action over the mailbox. */
        if (chdir(workdir) != 0) _exit(120);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(outpipe[1], STDERR_FILENO);
        close(outpipe[0]); close(outpipe[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        setenv("VMS_FOREIGN_CMD", "/DESCRIPTION=OVMXB23.MMS OVMXB23.OUT", 1);
        execl(mmk_path(), mmk_path(), (char *)NULL);
        _exit(127);
    }
    close(outpipe[1]);

    /* Bounded scan of MMK's output for the DCL-computed marker. */
    char acc[16384];
    size_t acclen = 0;
    int got_marker = 0;
    int waited = 0;
    acc[0] = '\0';
    while (waited < DRIVE_TIMEOUT_MS && !got_marker) {
        struct pollfd pfd = { .fd = outpipe[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        waited += 200;
        if (pr <= 0) {
            if (pr < 0) break;
            continue;
        }
        ssize_t n = read(outpipe[0], acc + acclen,
                         (acclen < sizeof(acc) - 1) ? (sizeof(acc) - 1 - acclen) : 0);
        if (n <= 0) break;   /* MMK closed SYS$OUTPUT (exited) */
        acclen += (size_t)n;
        acc[acclen] = '\0';
        if (strstr(acc, EXPECT_MARKER) != NULL)
            got_marker = 1;
    }

    /* Surface exactly what MMK echoed so a drive that completes without the
     * oracle is debuggable from the CI transcript. */
    printf("  DIAG: MMK echoed %zu bytes: <<<%s>>>\n", acclen, acc);

    /* THE POINT OF THE ITEM. MMK.EXE spawned a persistent DCL, streamed the
     * action command into its SYS$INPUT mailbox, and -- through the
     * write-attention AST that interrupted its $HIBER and the IO$M_NOW drain --
     * received the DCL-computed result over the SYS$OUTPUT mailbox. */
    /* negctl: mmk-drive-command-not-sent */
    CHECK(got_marker,
          "MMK.EXE drove a real build: its spawned DCL computed 6*7 and delivered OVMXB23:42 back over the mailbox drive (spawn + mailbox + write-attention AST + $HIBER + IO$M_NOW + $STATUS marker)");

    /* Bounded reap: a working drive detects the end-of-command marker and MMK
     * exits; a broken drive hangs in $HIBER and is caught here. */
    int wstatus = 0;
    int reaped = 0;
    for (int w = 0; w < DRIVE_TIMEOUT_MS; w += 20) {
        pid_t r = waitpid(pid, &wstatus, WNOHANG);
        if (r == pid) { reaped = 1; break; }
        if (r < 0) break;
        usleep(20000);
    }
    CHECK(reaped,
          "MMK.EXE completed (it detected the MMK____status= end-of-command marker and exited, rather than deadlocking in $HIBER)");
    if (reaped) {
        if (WIFEXITED(wstatus))
            printf("  DIAG: MMK exit status = %d (WEXITSTATUS)\n", WEXITSTATUS(wstatus));
        else if (WIFSIGNALED(wstatus))
            printf("  DIAG: MMK killed by signal %d\n", WTERMSIG(wstatus));
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, &wstatus, 0);
    }

    /* Drain any remaining output for the transcript (bounded, best-effort). */
    for (int i = 0; i < 8; i++) {
        struct pollfd pfd = { .fd = outpipe[0], .events = POLLIN };
        if (poll(&pfd, 1, 50) <= 0) break;
        char tmp[1024];
        ssize_t n = read(outpipe[0], tmp, sizeof(tmp));
        if (n <= 0) break;
    }
    close(outpipe[0]);

    /* Cleanup the work directory. */
    snprintf(path, sizeof(path), "%s/OVMXB23.MMS", workdir); unlink(path);
    snprintf(path, sizeof(path), "%s/OVMXB23.IN",  workdir); unlink(path);
    snprintf(path, sizeof(path), "%s/OVMXB23.OUT", workdir); unlink(path);
    snprintf(path, sizeof(path), "%s/MMS$RULES",   workdir); unlink(path);
    rmdir(workdir);

    printf("=== test_syssvc_mmk_drive: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
