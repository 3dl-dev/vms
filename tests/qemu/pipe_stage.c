/*
 * pipe_stage.c -> PIPESTAGE.EXE - a representative OVMX pipeline-stage image
 *                 (vms-e9a B2, docs/design-libspawn-ovmx.md §3c/§5)
 *
 * The small "reads SYS$INPUT, writes SYS$OUTPUT, records a $STATUS" image the
 * B2 pipeline proof (tests/qemu/test_syssvc_spawn_pipeline.c) chains as a stand-
 * in for a real GCC pipeline stage (cpp / cc1 / as / ld) -- which is not yet
 * buildable on OVMX (vms-fd1, blocked). It is $CREPRC'd with SYS$INPUT/SYS$OUTPUT
 * redirected to scratch files (fd 0 / fd 1 by the $CREPRC dup2), so it just
 * reads stdin and writes stdout.
 *
 * WHAT MAKES IT A FAITHFUL STAGE, not a bash filter:
 *
 *   1. It records a genuine executive completion $STATUS ($EXIT) via
 *      VMS_IOCTL_SETEXIT on /dev/vms before it exits. That single call does two
 *      load-bearing things at once: it fires the parent's B1 /NOWAIT completion
 *      (the executive sets the driver's event flag), and it records the $STATUS
 *      the driver then reads by VMS PID to decide whether to launch the next
 *      stage. A per-process userspace fake could carry neither across to the
 *      separate driver process (Rule 9 / INV-6). Its PCB row already exists --
 *      $CREPRC's forked child registered this subprocess (a fresh VMS PID, keyed
 *      by tgid, which execve preserves) before activating this image -- so
 *      SETEXIT records on that row with no re-registration.
 *
 *   2. Its output DEPENDS ON its input, so a chain proves each stage genuinely
 *      consumed the previous stage's output: it copies stdin verbatim to stdout
 *      and appends one "PIPE_STAGE_OK n=<k>" line, where k is one more than the
 *      number of such lines already present in the input. After a 4-stage chain
 *      the final output carries n=1..n=4 in order -- only possible if each stage
 *      read the previous stage's output.
 *
 *   3. FAILURE INJECTION for the driver's $STATUS gate:
 *        - "FAILHERE" in the input  -> this stage records a FAILURE condition
 *          (SS$_ABORT, even) and produces no valid handoff.
 *        - "EMITFAIL" in the input  -> this stage SUCCEEDS, but strips that
 *          one-shot token and appends "FAILHERE" to its OUTPUT, so the NEXT
 *          stage fails. This lets the proof show the driver stops the chain at a
 *          MID-pipeline stage (an earlier stage having run and succeeded) and
 *          never launches the stages after it.
 *
 * No /dev/vms -> the SETEXIT records nothing and the stage exits nonzero; it
 * never fabricates a completion (INV-6). It takes no argv ($CREPRC execs images
 * with none), so all behaviour is driven by the input content.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"

#define COND_SUCCESS    1u      /* SS$_NORMAL */
#define COND_FAILURE    44u     /* SS$_ABORT  */

/* Record `condition` as this process's image-completion $STATUS via the real
 * executive. Returns 0 iff the executive recorded it (and thereby fired any
 * armed parent /NOWAIT completion). */
static int record_exit_status(uint32_t condition)
{
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return -1;                       /* no executive: honest failure */

    struct vms_exit_args a;
    memset(&a, 0, sizeof(a));
    a.condition = condition;
    int rc = ioctl(fd, VMS_IOCTL_SETEXIT, &a);
    close(fd);
    if (rc != 0 || a.status != COND_SUCCESS)
        return -1;
    return 0;
}

static void write_all(const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(STDOUT_FILENO, buf + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        if (w == 0) break;
        off += (size_t)w;
    }
}

int main(void)
{
    /* Read all of SYS$INPUT (fd 0). */
    static char in[1 << 20];
    size_t n = 0;
    for (;;) {
        if (n >= sizeof(in) - 1)
            break;
        ssize_t g = read(STDIN_FILENO, in + n, sizeof(in) - 1 - n);
        if (g < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (g == 0) break;
        n += (size_t)g;
    }
    in[n] = '\0';

    /* A stage that receives the failure sentinel records a FAILURE $STATUS and
     * produces no valid handoff -- the driver's $STATUS gate stops here. */
    if (strstr(in, "FAILHERE") != NULL) {
        (void)record_exit_status(COND_FAILURE);
        return 1;
    }

    /* Count existing stage markers so the appended one is monotonic across the
     * chain (proves each stage read the previous stage's output). */
    int k = 1;
    for (const char *p = in; (p = strstr(p, "PIPE_STAGE_OK")) != NULL; p += 13)
        k++;

    /* Copy the input forward. A one-shot "EMITFAIL" token is stripped (blanked)
     * on the way, and its presence arms this stage to append "FAILHERE" so the
     * NEXT stage fails -- while THIS stage still completes successfully. */
    char *emit = strstr(in, "EMITFAIL");
    if (emit) {
        memset(emit, ' ', 8);            /* strip the one-shot token */
        write_all(in, n);
        write_all("\nFAILHERE\n", 10);   /* poison the next stage's input */
    } else {
        write_all(in, n);
    }

    char marker[64];
    int mlen = snprintf(marker, sizeof(marker), "PIPE_STAGE_OK n=%d\n", k);
    write_all(marker, (size_t)(mlen > 0 ? mlen : 0));

    /* Flush the handoff to the scratch file BEFORE recording exit: the driver
     * launches the next stage as soon as the completion fires, and that stage
     * must see this stage's complete output. */
    fsync(STDOUT_FILENO);

    /* Record the success $STATUS -- fires the parent's B1 completion. If the
     * executive is absent, exit nonzero rather than fake a completion. */
    if (record_exit_status(COND_SUCCESS) != 0)
        return 1;
    return 0;
}
