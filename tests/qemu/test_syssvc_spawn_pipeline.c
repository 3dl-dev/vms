/*
 * test_syssvc_spawn_pipeline.c - a MULTI-STAGE subprocess pipeline runs over the
 * executive, chained by /NOWAIT completion + $STATUS (vms-e9a B2,
 * docs/design-libspawn-ovmx.md §3c/§5), proven against a real /dev/vms.
 *
 * WHAT THIS SUITE PROVES
 *
 * B0 gave OVMX one executive-registered creation primitive ($CREPRC) and B1 gave
 * it the /NOWAIT process-exit completion facility (VMS_IOCTL_SPAWN_NOTIFY: the
 * executive sets the parent's event flag / queues its AST when a subprocess
 * records its exit). B2 is the PAYOFF those two rungs exist for: the GCC driver's
 * cpp -> cc1 -> as -> ld pipeline, where a driver spawns a chain of image stages,
 * each stage's output feeding the next, and the driver waits on each stage's
 * completion and checks its $STATUS before launching the next.
 *
 * The reusable orchestration is src/gcc_host/ovmx_spawn_pipeline.c (the GCC-lane
 * authored VMS-host layer, NOT src/libvms -- design §5). Because the real
 * alpha-dec-vms GCC port is not yet buildable on OVMX (vms-fd1, blocked), the
 * stages here are a representative stand-in image, PIPESTAGE.EXE
 * (tests/qemu/pipe_stage.c): it reads SYS$INPUT, writes SYS$OUTPUT, and records a
 * genuine executive $STATUS ($EXIT) -- the same three things a real compiler
 * stage does. The mechanism is generic; only the payload images are stand-ins.
 *
 * THE PROOF IS OVER THE EXECUTIVE, NOT A UNIX PIPE (Rule 9 / INV-6):
 *   - each stage is created by $CREPRC (a genuine, executive-registered VMS
 *     subprocess with its own VMS PID), not fork/exec of a bash filter;
 *   - the driver advances on the B1 event flag the EXECUTIVE sets when the stage
 *     records its exit -- it does not poll waitpid as the completion signal;
 *   - the per-stage $STATUS the driver gates on is read from the EXECUTIVE by VMS
 *     PID (vms_kif_getexit_pid), the condition value the stage itself recorded;
 *   - inter-stage output rides an RMS scratch file (§3c default transport), read
 *     back as the next stage's SYS$INPUT.
 * A per-process userspace fake could carry none of that from a stage process to
 * the separate driver process -- which is exactly why this needs a real
 * /dev/vms, and honest-skips (77) without one rather than fake a pass.
 *
 * ASSERTIONS
 *   A) A 4-stage all-success pipeline runs to completion; the final output shows
 *      each stage saw the previous stage's output (monotonic n=1..n=4, in order,
 *      atop the original input) -- only possible if the chain really chained.
 *   B) A pipeline where a MID-chain stage records a FAILURE $STATUS stops there:
 *      the driver reports the failing stage index and status, the stages after
 *      it are NEVER launched, and the final output file is never created. This is
 *      the $STATUS gate that a real driver needs (a failed cc1 must not run ld).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "ovmx_spawn_pipeline.h"

#define EXIT_SKIP 77
#define PIPESTAGE "/bin/PIPESTAGE.EXE"

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                                 \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }   \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }   \
    } while (0)

/* A real bug in the completion-delivery path could hang $WAITFR forever and
 * stall the whole suite; bound the run and fail honestly if it fires. */
static void on_alarm(int sig)
{
    (void)sig;
    const char *m = "  FAIL: test_syssvc_spawn_pipeline timed out (a stage completion never delivered)\n";
    (void)!write(STDOUT_FILENO, m, strlen(m));
    _exit(3);
}

static int write_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t len = strlen(data), off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        if (w == 0) break;
        off += (size_t)w;
    }
    close(fd);
    return 0;
}

static long read_file(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = 0;
    for (;;) {
        ssize_t g = read(fd, buf + n, bufsz - 1 - (size_t)n);
        if (g < 0) { if (errno == EINTR) continue; break; }
        if (g == 0) break;
        n += g;
        if ((size_t)n >= bufsz - 1) break;
    }
    buf[n > 0 ? n : 0] = '\0';
    close(fd);
    return n;
}

/* Two markers appear IN ORDER iff the first occurs before the second. */
static int in_order(const char *body, const char *a, const char *b)
{
    const char *pa = strstr(body, a);
    const char *pb = pa ? strstr(pa, b) : NULL;
    return pa && pb;
}

/* No /dev/vms: the pipeline's first $CREPRC must fail honestly (no executive to
 * register the subprocess) and the driver must NOT fabricate a completed
 * pipeline (design §3d, INV-6). */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    struct ovmx_pipe_stage stages[2] = {
        { PIPESTAGE, "PIPE_S0" },
        { PIPESTAGE, "PIPE_S1" },
    };
    struct ovmx_pipe_result r;
    memset(&r, 0, sizeof(r));
    uint32_t st = ovmx_spawn_pipeline(stages, 2, NULL, "/tmp/ovmx_pipe_out.tmp",
                                      "/tmp", &r);
    CHECK(!(st & 1),
          "the pipeline reports no success with no executive (honest fail)");
    CHECK(r.stages_run == 0,
          "no stage is reported run when the executive is absent");

    printf("=== test_syssvc_spawn_pipeline: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_spawn_pipeline (vms-e9a B2: /NOWAIT-chained pipeline) ===\n");

    signal(SIGALRM, on_alarm);
    alarm(60);

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    if (access(PIPESTAGE, X_OK) != 0) {
        printf("  (no %s staged -- cannot drive the pipeline) SKIP\n", PIPESTAGE);
        return EXIT_SKIP;
    }

    /* ---- A) 4-stage all-success pipeline (cpp -> cc1 -> as -> ld model) ---- */
    printf("--- 4-stage pipeline runs to completion, each stage feeding the next ---\n");
    {
        const char *IN  = "/tmp/ovmx_pipe_src.tmp";
        const char *OUT = "/tmp/ovmx_pipe_final.tmp";
        unlink(OUT);
        CHECK(write_file(IN, "HELLO_PIPELINE\n") == 0,
              "seed the pipeline's initial SYS$INPUT");

        struct ovmx_pipe_stage stages[4] = {
            { PIPESTAGE, "PIPE_CPP" },
            { PIPESTAGE, "PIPE_CC1" },
            { PIPESTAGE, "PIPE_AS"  },
            { PIPESTAGE, "PIPE_LD"  },
        };
        struct ovmx_pipe_result r;
        memset(&r, 0, sizeof(r));
        uint32_t st = ovmx_spawn_pipeline(stages, 4, IN, OUT, "/tmp", &r);

        CHECK(st == SS$_NORMAL,
              "the 4-stage pipeline completes with overall success $STATUS");
        CHECK(r.stages_run == 4, "all four stages ran");
        CHECK(r.failed_index == -1, "no stage failed");

        char body[8192];
        long n = read_file(OUT, body, sizeof(body));
        CHECK(n > 0, "the pipeline produced a final SYS$OUTPUT file");
        CHECK(n > 0 && strstr(body, "HELLO_PIPELINE") != NULL,
              "the original input transited the whole pipeline to the final output");
        /* Each stage appends the next n=; their presence AND order prove each
         * stage genuinely consumed the previous stage's output. */
        CHECK(strstr(body, "PIPE_STAGE_OK n=1") != NULL, "stage 1 marker present");
        CHECK(strstr(body, "PIPE_STAGE_OK n=2") != NULL, "stage 2 marker present");
        CHECK(strstr(body, "PIPE_STAGE_OK n=3") != NULL, "stage 3 marker present");
        CHECK(strstr(body, "PIPE_STAGE_OK n=4") != NULL, "stage 4 marker present");
        CHECK(in_order(body, "PIPE_STAGE_OK n=1", "PIPE_STAGE_OK n=2") &&
              in_order(body, "PIPE_STAGE_OK n=2", "PIPE_STAGE_OK n=3") &&
              in_order(body, "PIPE_STAGE_OK n=3", "PIPE_STAGE_OK n=4"),
              "the stage markers appear in pipeline order (each stage saw the prior output)");
        unlink(IN);
        unlink(OUT);
    }

    /* ---- B) mid-chain failure stops the pipeline; later stages never run ---- */
    printf("--- a mid-chain stage failure stops the pipeline (the $STATUS gate) ---\n");
    {
        const char *IN  = "/tmp/ovmx_pipe_failsrc.tmp";
        const char *OUT = "/tmp/ovmx_pipe_failfinal.tmp";
        unlink(OUT);
        /* Stage 0 sees EMITFAIL: it SUCCEEDS but poisons its output with the
         * failure sentinel, so stage 1 records a FAILURE $STATUS. Stage 2 (the
         * last, which would write OUT) must never be launched. */
        CHECK(write_file(IN, "EMITFAIL please\n") == 0,
              "seed an input that makes stage 1 fail");

        struct ovmx_pipe_stage stages[3] = {
            { PIPESTAGE, "PIPE_F0" },
            { PIPESTAGE, "PIPE_F1" },
            { PIPESTAGE, "PIPE_F2" },
        };
        struct ovmx_pipe_result r;
        memset(&r, 0, sizeof(r));
        uint32_t st = ovmx_spawn_pipeline(stages, 3, IN, OUT, "/tmp", &r);

        CHECK(!(st & 1),
              "the pipeline returns a failure $STATUS when a stage fails");
        CHECK(r.failed_index == 1,
              "the driver reports the MID-chain stage (index 1) as the failure");
        CHECK(r.failed_status == SS$_ABORT,
              "the failing stage's actual $STATUS (SS$_ABORT) is surfaced");
        CHECK(r.stages_run == 2,
              "exactly two stages ran (stage 0 succeeded, stage 1 failed)");
        CHECK(access(OUT, F_OK) != 0,
              "the final-stage output was NEVER created (stage 2 was not launched)");
        unlink(IN);
        unlink(OUT);
    }

    alarm(0);
    printf("=== test_syssvc_spawn_pipeline: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
