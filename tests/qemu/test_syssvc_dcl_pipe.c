/*
 * test_syssvc_dcl_pipe.c - a DCL PIPE segment is a SUBPROCESS its DCL created
 * (rd vms-1f8, vms-9f32 design note 3.1/3.4).
 *
 * On VMS each segment of a PIPE runs in a subprocess of the CLI's process,
 * carrying the CLI's identity and CURRENT privileges. OVMX runs each segment
 * in a forked task; DCL makes it a VMS process (VMS_IOCTL_CREPRC_PCB,
 * SUBPROCESS) while it waits, before it runs anything. The discriminating
 * property is the privilege mask: a subprocess gets its creator's CURRENT
 * mask, so a privilege DCL gave up is gone in the segment too -- a task that
 * registered itself instead would derive its mask afresh and have it back.
 *
 * The suite creates the DCL it drives as its own subprocess
 * (tests/qemu/ovmx_test_fork.h). In one session DCL drops WORLD, then a PIPE
 * segment shows its privileges: WORLD must be absent. In another the segment
 * must report a VMS process ID that is not DCL's own.
 *
 * negctl: dcl-pipe-segment-not-created
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }
    fflush(NULL);
    pid_t pid = ovmx_fork_subprocess();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl(DCL_PATH, "DCL.EXE", (char *)NULL);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    (void)!write(in_pipe[1], script, strlen(script));
    close(in_pipe[1]);
    size_t used = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        struct timespec tn;
        clock_gettime(CLOCK_MONOTONIC, &tn);
        long left = 60000 - ((tn.tv_sec - t0.tv_sec) * 1000 + (tn.tv_nsec - t0.tv_nsec) / 1000000);
        struct pollfd pfd = { .fd = out_pipe[0], .events = POLLIN };
        if (left <= 0 || poll(&pfd, 1, (int)left) == 0) { kill(pid, SIGKILL); break; }
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/* Is `name` listed in the "Process privileges:" block that starts at or after
 * `from`? (SHOW PROCESS/PRIVILEGES prints " NAME   description".) */
static int lists_priv(const char *from, const char *name)
{
    const char *hdr = from ? strstr(from, "Process privileges:") : NULL;
    if (!hdr) return 0;
    const char *end = strstr(hdr + 1, "Process privileges:");
    char needle[48];
    snprintf(needle, sizeof needle, "\n %s ", name);
    const char *hit = strstr(hdr, needle);
    return hit && (!end || hit < end);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_pipe: a DCL PIPE segment is a subprocess its DCL created (rd vms-1f8) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_pipe: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    static char out[65536], out2[65536];
    /* Session 1: the segment's privileges. The ONLY privilege listing in this
     * transcript is the segment's (DCL's and the segment's output interleave,
     * so the parent prints none). */
    const char *script1 =
        "SET PROCESS/PRIVILEGES=NOWORLD\n"
        "PIPE SHOW PROCESS/PRIVILEGES\n"
        "LOGOUT\n";
    /* Session 2: the segment's VMS process ID against DCL's own. */
    const char *script2 =
        "WRITE SYS$OUTPUT \"OVMX1F8:DCL:\", F$GETJPI(\"\",\"PID\")\n"
        "PIPE WRITE SYS$OUTPUT \"OVMX1F8:SEG:\", F$GETJPI(\"\",\"PID\")\n"
        "LOGOUT\n";
    CHECK(run_dcl(script1, out, sizeof out) == 0 && run_dcl(script2, out2, sizeof out2) == 0,
          "DCL.EXE ran both PIPE scripts");
    const char *blk = strstr(out, "Process privileges:");
    CHECK(blk != NULL && strstr(blk + 1, "Process privileges:") == NULL,
          "the PIPE segment ran and listed its privileges");
    /* negctl: dcl-pipe-segment-not-created */
    CHECK(blk && lists_priv(out, "CMKRNL") && !lists_priv(out, "WORLD"),
          "the segment carries DCL's CURRENT privileges -- WORLD, which DCL gave up, is gone there too");
    const char *dclpid = strstr(out2, "OVMX1F8:DCL:");
    const char *segpid = strstr(out2, "OVMX1F8:SEG:");
    CHECK(segpid != NULL, "the PIPE segment ran and reported its VMS process ID");
    {
        unsigned a = 0, b = 0;
        if (dclpid) sscanf(dclpid + strlen("OVMX1F8:DCL:"), "%x", &a);
        if (segpid) sscanf(segpid + strlen("OVMX1F8:SEG:"), "%x", &b);
        CHECK(a != 0 && b != 0 && a != b,
              "the segment is a VMS process of its own (a VMS process ID that is not DCL's)");
    }
    if (fail)
        printf("  (DCL transcripts follow)\n%s\n----\n%s\n  (end)\n", out, out2);
    printf("=== test_syssvc_dcl_pipe: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
