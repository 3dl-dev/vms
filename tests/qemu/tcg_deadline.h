/*
 * tcg_deadline.h -- env-scaled inner-deadline multiplier for the qemu syssvc
 * suites (rd vms-5ae).
 *
 * WHY THIS EXISTS. Several suites drive a real DCL.EXE / subprocess cold-start
 * and bound the wait for a result with an INNER deadline (e.g. a 15s procscan
 * poll, a 30s DCL poll). Those bounds are generous on x86_64 (native / KVM,
 * DCL returns in milliseconds) but are hit as a FAILURE under pure-TCG
 * emulation on qemu-system-alpha, where a DCL/subprocess cold-start is orders
 * of magnitude slower. That is a HARNESS calibration gap, not an executive
 * defect: the suite would succeed given proportionally longer inner waits.
 *
 * THE KNOB. OVMX_TEST_DEADLINE_SCALE multiplies every inner deadline wrapped
 * in ovmx_tcg_ms(). It is UNSET on the x86_64/aarch64 path (init.sh /
 * run_tests.sh never export it) so scale == 1 and behaviour there is byte-for-
 * byte unchanged. The Alpha runner (tools/cross-alpha/test-init-syssvc-alpha.c)
 * exports it (e.g. 8) so the slow TCG guest gets proportionally longer waits.
 *
 * WHAT IT DOES NOT DO. It NEVER shortens a deadline and it NEVER converts a
 * timeout into a masked success: a suite that still exceeds its scaled deadline
 * fails LOUDLY (INV-6, prove-or-expose). The scale only buys a slow-but-healthy
 * guest enough time to reach the same PASS/FAIL an x86_64 guest reaches.
 *
 * CONTRACT. Parsed once, clamped to [1, 20]; a missing/blank/<1/unparseable
 * value falls back to the default 1; a value > 20 clamps to 20. The per-suite
 * SIGALRM watchdog in the Alpha runner must sit ABOVE the largest scaled inner
 * deadline (see test-init-syssvc-alpha.c), else the watchdog would kill a
 * healthy-slow suite -- or pre-empt a bounded-poll diagnostic -- before the
 * inner deadline resolves.
 */
#ifndef OVMX_TCG_DEADLINE_H
#define OVMX_TCG_DEADLINE_H

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

/* The clamped multiplier, parsed once from OVMX_TEST_DEADLINE_SCALE. */
static inline long ovmx_tcg_scale(void)
{
    static long cached = -1;
    if (cached < 0) {
        long s = 1;
        const char *e = getenv("OVMX_TEST_DEADLINE_SCALE");
        if (e && *e) {
            char *end = NULL;
            long v = strtol(e, &end, 10);
            if (end != e) {
                if (v > 20) v = 20;
                if (v >= 1) s = v; /* v < 1 keeps the default 1 */
            }
        }
        cached = s;
    }
    return cached;
}

/* Scale one inner deadline (in ms). base_ms unchanged when scale == 1. */
static inline long ovmx_tcg_ms(long base_ms)
{
    return base_ms * ovmx_tcg_scale();
}

/* Milliseconds on a monotonic clock (immune to wall-clock steps). */
static inline long ovmx_tcg_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Return codes for ovmx_tcg_drain_pipe(). */
#define OVMX_TCG_DRAIN_EOF      0   /* child closed stdout within the deadline */
#define OVMX_TCG_DRAIN_TIMEOUT  1   /* deadline expired first (child SIGKILLed) */
#define OVMX_TCG_DRAIN_ERROR  (-1)  /* poll error */

/*
 * ovmx_tcg_drain_pipe -- read `fd` into out[0..outsz-1] until the child closes
 * it (EOF) OR `deadline_ms` (a TOTAL wall bound, not per-poll) elapses. This
 * REPLACES an unbounded blocking read that would otherwise hang forever if a
 * subject (e.g. DCL.EXE) never closes stdout.
 *
 * INV-6 / prove-or-expose: on TIMEOUT it does NOT fabricate completion -- it
 * leaves the PARTIAL bytes captured so far in `out` (NUL-terminated), sets
 * *usedp, SIGKILLs `child` so a following waitpid cannot block on the wedged
 * process, and returns OVMX_TCG_DRAIN_TIMEOUT. The caller is expected to print
 * the partial output + a loud diagnostic and FAIL its check. On clean EOF it
 * returns OVMX_TCG_DRAIN_EOF (the subject completed -- TCG-slow-but-healthy).
 */
static inline int ovmx_tcg_drain_pipe(int fd, char *out, size_t outsz,
                                      long deadline_ms, pid_t child,
                                      size_t *usedp)
{
    size_t used = 0;
    long start = ovmx_tcg_mono_ms();
    int rc = OVMX_TCG_DRAIN_EOF;

    for (;;) {
        long remaining = deadline_ms - (ovmx_tcg_mono_ms() - start);
        if (remaining <= 0) { rc = OVMX_TCG_DRAIN_TIMEOUT; break; }
        if (used >= outsz - 1) { rc = OVMX_TCG_DRAIN_EOF; break; }

        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        int pr = poll(&pfd, 1, (int)remaining);
        if (pr < 0) {
            if (errno == EINTR) continue;
            rc = OVMX_TCG_DRAIN_ERROR;
            break;
        }
        if (pr == 0) { rc = OVMX_TCG_DRAIN_TIMEOUT; break; }

        ssize_t n = read(fd, out + used, outsz - 1 - used);
        if (n <= 0) { rc = OVMX_TCG_DRAIN_EOF; break; }  /* EOF/HUP = subject done */
        used += (size_t)n;
    }

    out[used] = '\0';
    if (usedp) *usedp = used;
    if (rc == OVMX_TCG_DRAIN_TIMEOUT && child > 0)
        kill(child, SIGKILL);
    return rc;
}

#endif /* OVMX_TCG_DEADLINE_H */
