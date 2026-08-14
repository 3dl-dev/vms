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

int main(void)
{
    test_repro_without_barrier();
    test_fixed_with_barrier();

    if (failures) {
        fprintf(stderr, "\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall prompt/echo ordering checks passed\n");
    return 0;
}
