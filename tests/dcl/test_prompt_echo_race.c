/*
 * test_prompt_echo_race.c - the DCL interactive prompt/echo race (vms-195)
 *
 * THE DEFECT. On the live console login the demo runs, DCL's SYS$INPUT is the
 * terminal but SYS$OUTPUT is diverted to a mailbox served by an async writer
 * thread (dcl_mbx.c: fd 1 -> pipe -> writer -> IO$_WRITEVBLK -> mailbox). DCL's
 * newline-less "$ " prompt therefore rides that async path, while the KERNEL
 * echoes the user's next keystroke to the terminal synchronously. The echo can
 * reach the console before the still-in-flight prompt, producing the observed
 * interleaved "d$ ir".
 *
 * THE FIX (vms-195). After DCL writes and fflush()es the prompt, it calls
 * dcl_mbx_output_drain_sync(), which blocks until the writer thread has pushed
 * every byte then in the output pipe out through the mailbox -- so the prompt is
 * fully emitted before DCL issues the read that arms the terminal echo.
 *
 * WHY THIS IS HERMETIC. CI has no /dev/vms, so the real mailbox executive path
 * cannot run here. The dcl_mbx test hooks start the SAME writer-thread + drain
 * machinery against a plain pipe standing in for the mailbox far end (the merged
 * console). A test-only writer lag makes the race deterministic: with the lag,
 * an echo written straight to the far end beats the delayed prompt UNLESS the
 * drain barrier holds the read back. We assert both directions on that one
 * far-end stream -- exactly "the prompt bytes precede any echoed input".
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>

#include "dcl/dcl_mbx.h"

/* Test hooks compiled into dcl_mbx.c under -DDCL_MBX_TEST_HOOKS. */
int  dcl_mbx__test_start_output(int sink_fd, int *out_write_fd);
void dcl_mbx__test_set_sink_delay(unsigned usec);
void dcl_mbx__test_stop_output(void);

static int failures = 0;

#define CHECK(cond, msg) do {                                   \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
    else         { printf("ok: %s\n", (msg)); }                 \
} while (0)

/* Read until EOF (all sink write ends closed) or the buffer fills. */
static size_t read_to_eof(int fd, char *buf, size_t cap)
{
    size_t off = 0;
    while (off < cap) {
        ssize_t r = read(fd, buf + off, cap - off);
        if (r > 0)              off += (size_t)r;
        else if (r == 0)        break;              /* EOF */
        else if (errno == EINTR) continue;
        else                    break;
    }
    buf[off < cap ? off : cap - 1] = '\0';
    return off;
}

static const unsigned WRITER_LAG_US = 80000;   /* 80 ms: makes the race certain */

/*
 * Without the barrier: DCL writes the prompt to fd 1 (which the LAGGED writer
 * has not yet forwarded) and then arms the read, whereupon the terminal echo
 * lands on the console first. Modeled by writing the echo straight to the far
 * end. Expect the echo to precede the prompt -- the "d$ ir" bug reproduced.
 */
static void test_repro_without_barrier(void)
{
    int mbx[2];
    if (pipe(mbx) != 0) { CHECK(0, "repro: pipe()"); return; }

    int dcl_fd1 = -1;
    if (dcl_mbx__test_start_output(mbx[1], &dcl_fd1) != 0) {
        CHECK(0, "repro: start_output()");
        return;
    }
    dcl_mbx__test_set_sink_delay(WRITER_LAG_US);

    /* DCL emits the prompt onto its (diverted) stdout. */
    (void)!write(dcl_fd1, "$ ", 2);
    /* No drain: DCL immediately reads, so the synchronous terminal echo of the
     * user's "ir" keystrokes reaches the console ahead of the lagged prompt. */
    (void)!write(mbx[1], "ir", 2);

    close(dcl_fd1);
    dcl_mbx__test_stop_output();     /* joins the writer -> it emits "$ " late */
    close(mbx[1]);                   /* last write end -> reader sees EOF      */

    char buf[64];
    read_to_eof(mbx[0], buf, sizeof buf);
    close(mbx[0]);
    dcl_mbx__test_set_sink_delay(0);

    char *pe = strstr(buf, "ir");
    char *pp = strstr(buf, "$ ");
    CHECK(pe && pp && pe < pp,
          "repro: without the barrier the echo precedes the prompt (bug present)");
}

/*
 * With the barrier: after fflush()ing the prompt DCL calls
 * dcl_mbx_output_drain_sync(), which blocks through the writer lag until the
 * prompt has reached the far end; only then is the read armed and the echo
 * delivered. Expect the prompt to precede the echo on the far-end stream.
 */
static void test_fixed_with_barrier(void)
{
    int mbx[2];
    if (pipe(mbx) != 0) { CHECK(0, "fixed: pipe()"); return; }

    int dcl_fd1 = -1;
    if (dcl_mbx__test_start_output(mbx[1], &dcl_fd1) != 0) {
        CHECK(0, "fixed: start_output()");
        return;
    }
    dcl_mbx__test_set_sink_delay(WRITER_LAG_US);

    (void)!write(dcl_fd1, "$ ", 2);
    dcl_mbx_output_drain_sync();     /* holds until "$ " is out through the sink */

    /* Positive guarantee: the prompt is ALREADY at the far end -- a
     * non-blocking read sees the full "$ " with no further waiting. */
    int fl = fcntl(mbx[0], F_GETFL, 0);
    if (fl >= 0) (void)fcntl(mbx[0], F_SETFL, fl | O_NONBLOCK);
    char pk[8] = {0};
    ssize_t got = read(mbx[0], pk, sizeof pk);
    CHECK(got == 2 && pk[0] == '$' && pk[1] == ' ',
          "fixed: after drain the full prompt is already emitted");
    if (fl >= 0) (void)fcntl(mbx[0], F_SETFL, fl);   /* restore blocking */

    /* Now the read is armed and the terminal echo arrives. */
    (void)!write(mbx[1], "ir", 2);

    close(dcl_fd1);
    dcl_mbx__test_stop_output();
    close(mbx[1]);

    char buf[64];
    read_to_eof(mbx[0], buf, sizeof buf);
    close(mbx[0]);
    dcl_mbx__test_set_sink_delay(0);

    /* The prompt was consumed above, so what remains is the echo only, in
     * order after it -- assert no echo-before-prompt inversion occurred. */
    CHECK(strstr(buf, "$ ") == NULL,
          "fixed: prompt already drained before echo (no interleave)");
    CHECK(strncmp(buf, "ir", 2) == 0,
          "fixed: the echo follows the fully-emitted prompt");
}

/*
 * test_two_lines_one_read_become_two_messages (vms-d26f) - the SYS$OUTPUT
 * relay's line-boundary fix.
 *
 * THE BUG. A Unix pipe carries no message boundaries: dcl_mbx.c's writer
 * thread used to forward each read() of DCL's stdout pipe as ONE mailbox
 * message verbatim. If a foreign command's own exit-time stdio flush and
 * DCL's own immediately-following end-of-command status echo (the
 * "MMK____status=" marker MMK's persistent-DCL build protocol keys
 * completion on, tests/corpus/tier3-mmk/build_target.c) both land in the
 * pipe before the writer thread's read() runs -- entirely plausible under
 * host scheduling contention -- ONE read() returns both concatenated, and
 * the old code relayed that as ONE mailbox message. MMK's echo_ast only
 * recognizes the marker when it is the FIRST thing in a message, so the
 * marker silently failed to match, was echoed as ordinary output instead of
 * consumed, and the command that owned it never completed --
 * test_syssvc_mmk_build's "pristine run" flake (LINK never runs because the
 * PRECEDING command's own completion marker was the one lost this way).
 *
 * THE PROOF. Write two newline-terminated lines in ONE write() call, so they
 * are certain to sit contiguously in the pipe (well under PIPE_BUF, so the
 * kernel write is atomic) by the time the writer thread's read() executes --
 * reproducing "one read() spans two logically distinct writes" without
 * depending on real scheduling timing. The far end is a SOCK_SEQPACKET
 * socket, which (unlike a pipe) preserves the boundary of each individual
 * write() the relay makes, so this can assert MESSAGE COUNT directly: the
 * fix must produce exactly two records, each holding exactly one line.
 */
static void test_two_lines_one_read_become_two_messages(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        CHECK(0, "two-lines: socketpair()");
        return;
    }

    int dcl_fd1 = -1;
    if (dcl_mbx__test_start_output(sv[1], &dcl_fd1) != 0) {
        CHECK(0, "two-lines: start_output()");
        close(sv[0]); close(sv[1]);
        return;
    }

    /* One write() call: both lines are certain to be contiguous in the pipe
     * (atomic, well under PIPE_BUF) whenever the writer thread's read() next
     * runs -- the exact "one read() spans two distinct writes" shape the bug
     * needed, reproduced without racing real process scheduling. */
    static const char two_lines[] = "%LIBRAR-S-CREATED, object library made\nMMK____status=1\n";
    if (write(dcl_fd1, two_lines, sizeof(two_lines) - 1) != (ssize_t)sizeof(two_lines) - 1) {
        CHECK(0, "two-lines: write() of both lines");
    }

    close(dcl_fd1);
    dcl_mbx__test_stop_output();     /* joins the writer: both lines are out */
    close(sv[1]);

    char rec1[128] = {0}, rec2[128] = {0};
    ssize_t n1 = recv(sv[0], rec1, sizeof(rec1) - 1, 0);
    ssize_t n2 = recv(sv[0], rec2, sizeof(rec2) - 1, 0);
    close(sv[0]);

    CHECK(n1 == (ssize_t)strlen("%LIBRAR-S-CREATED, object library made\n") &&
          strcmp(rec1, "%LIBRAR-S-CREATED, object library made\n") == 0,
          "two-lines: first record is the foreign command's own line alone");
    CHECK(n2 == (ssize_t)strlen("MMK____status=1\n") &&
          strcmp(rec2, "MMK____status=1\n") == 0,
          "two-lines: second record is DCL's status marker alone, "
          "starting at byte 0 -- echo_ast's marker match would have found it");
}

int main(void)
{
    test_repro_without_barrier();
    test_fixed_with_barrier();
    test_two_lines_one_read_become_two_messages();

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall prompt/echo ordering checks passed\n");
    return 0;
}
