/*
 * test_syssvc_bg_fork_inherit.c - does a forked/exec'd child inherit a BG channel
 * its parent created? (rd vms-0cd, the OpenSSH sshd-over-BGn: port; the 3b
 * fork-model make-or-break.)
 *
 * WHY. OpenSSH sshd's MASTER accepts an inbound connection, then fork()+exec's
 * sshd-session (privilege separation) to handle it. The accepted connection is a
 * BGn: veneer handle = an executive BG channel. The executive keys channel
 * ownership on the CALLING process (src/kernel-core/vms_bg.c bgchan_lookup walks
 * the caller's proc->bg_channels), so a child process -- a NEW executive proc --
 * does NOT see the parent's channel: $QIO on it returns SS$_IVCHAN. On a real OS
 * a forked child inherits the parent's open channels/fds; the OVMX executive
 * SHOULD too, so that STOCK forking servers (sshd, inetd) work UNCHANGED over
 * BGn: (the faithful fix -- not bending the daemon to a missing OS capability).
 *
 * This test SPECIFIES that end state: a child operates a channel its parent
 * created. It FAILS today (proving the gap the executive fork-inheritance feature
 * closes) and PASSES once that feature lands. Two cases: fork() alone, and
 * fork()+exec() (which additionally loses the userspace veneer handle map -- see
 * CASE 2). Honest Rule-9 skip with no /dev/vms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "  FAIL: test_syssvc_bg_fork_inherit timed out\n";
    (void)!write(1, m, sizeof(m) - 1);
    _exit(1);
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* A child operates the inherited channel and reports the VMS status back over a
 * pipe (low byte of the status; odd = success). SS$_IVCHAN (even) means the
 * executive did not share the parent's channel with the child. */
static uint32_t child_status_over_pipe(uint32_t exec_chan, int do_exec, char *self)
{
    int p[2];
    if (pipe(p) != 0) return 0;
    pid_t c = fork();
    if (c == 0) {
        char sbuf[16];
        uint32_t st;
        close(p[0]);
        if (do_exec) {
            /* CASE 2: re-exec ourselves with the channel number; the exec'd image
             * has a FRESH address space (the userspace veneer handle map is gone),
             * so it drives the raw kif with the inherited channel NUMBER directly. */
            char cbuf[16], wbuf[16];
            snprintf(cbuf, sizeof(cbuf), "%u", exec_chan);
            snprintf(wbuf, sizeof(wbuf), "%d", p[1]);
            execl(self, self, "--child-op", cbuf, wbuf, (char *)NULL);
            _exit(127);
        }
        st = vms_kif_bg_setmode(exec_chan);  /* benign op on the inherited channel */
        snprintf(sbuf, sizeof(sbuf), "%u", st);
        (void)!write(p[1], sbuf, strlen(sbuf));
        _exit(0);
    }
    close(p[1]);
    char rb[16] = {0};
    (void)!read(p[0], rb, sizeof(rb) - 1);
    close(p[0]);
    int cst = 0;
    waitpid(c, &cst, 0);
    return (uint32_t)strtoul(rb, NULL, 10);
}

int main(int argc, char **argv)
{
    /* Re-exec helper (CASE 2): operate the passed channel, report status, exit. */
    if (argc == 4 && strcmp(argv[1], "--child-op") == 0) {
        uint32_t exec_chan = (uint32_t)strtoul(argv[2], NULL, 10);
        int wfd = (int)strtol(argv[3], NULL, 10);
        char sbuf[16];
        uint32_t st = vms_kif_bg_setmode(exec_chan);
        snprintf(sbuf, sizeof(sbuf), "%u", st);
        (void)!write(wfd, sbuf, strlen(sbuf));
        return 0;
    }

    signal(SIGALRM, watchdog);
    alarm(30);

    printf("test_syssvc_bg_fork_inherit: executive BG-channel fork/exec inheritance\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms (executive absent) -- honest Rule-9 skip\n");
        return EXIT_SKIP;
    }

    uint32_t exec_chan = 0, unit = 0;
    uint32_t st = vms_kif_bg_create(&exec_chan, &unit, NULL, 0);
    CHECK(st & 1, "$ASSIGN TCPIP$DEVICE: creates a BG channel in the parent");
    if (!(st & 1)) return 1;

    st = vms_kif_bg_setmode(exec_chan);
    CHECK(st & 1, "the PARENT can operate its own BG channel (IO$_SETMODE)");

    /* CASE 1: fork() -- the child is a new executive proc; it must still see the
     * parent's channel for a stock forking server to work. */
    uint32_t cfork = child_status_over_pipe(exec_chan, 0, argv[0]);
    CHECK(cfork & 1,
          "a FORKED child can operate the BG channel its parent created (executive fork-inheritance)");

    /* CASE 2: fork()+exec() -- OpenSSH sshd's privsep re-execs sshd-session. */
    uint32_t cexec = child_status_over_pipe(exec_chan, 1, argv[0]);
    CHECK(cexec & 1,
          "a fork()+exec()'d child can operate the inherited BG channel (sshd privsep sshd-session)");

    vms_kif_bg_dassgn(exec_chan);

    printf("=== test_syssvc_bg_fork_inherit: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
