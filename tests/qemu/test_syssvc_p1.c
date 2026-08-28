/*
 * test_syssvc_p1.c - P1 control-region persistence + the P0/P1 distinction
 * (vms-68f.ii, increment (ii) of the Option A in-process image activation
 * design, docs/design-in-process-activation.md Part II §A.1.1, §A.2.1).
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT. This increment does not lay DCL's
 * actual process-permanent state into a real P1 window, does not activate
 * an image into P0, and does not do access-mode transitions or real image
 * rundown -- those are increments (iii)-(v). What it does:
 *
 *   1. VMS_IOCTL_P1_MAP lets a process register a [base, limit) P1 extent,
 *      reflected back through $GETJPI (struct vms_procinfo.p1_base/
 *      p1_limit) -- observable by the process itself AND by a different
 *      process reading this one's row (A-WRITES/B-READS, CLAUDE.md rule
 *      11), same discipline as test_syssvc_p0.c.
 *   2. THE KEY FAITHFUL PROPERTY: "P0 deleted on rundown, P1 survives".
 *      A process registers BOTH a P1 extent and a P0 extent, then cycles
 *      P0 map/unmap repeatedly -- and P1 is unchanged by every single
 *      cycle. This is the executive fact increments (iv)/(v) will build
 *      real image activation/rundown on top of.
 *
 * NEGATIVE CONTROL RIG: under NEGATIVE_CONTROL=1 (tests/qemu/Dockerfile
 * boots without insmod'ing vms.ko) there is no /dev/vms to open and this
 * program fails at the first line of main() saying so -- no per-process
 * fallback (INV-6).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "vms_kif.h"

/*
 * Status values. SS$_NORMAL is 1 in $SSDEF on the reference lab (OpenVMS
 * VAX V7.3, VAX1). SS$_BADPARAM is this tree's existing
 * src/libvmssys/vms_errno.h value (20) -- not re-derived here, same
 * discipline test_syssvc_p0.c uses for its own borrowed statuses.
 */
#define SS_NORMAL       1
#define SS_BADPARAM     20

/* Real PROT_NONE reservations, same shape design §A.2.1 describes for
 * DCL's P0/P1 windows -- large enough to be unmistakable extents, small
 * enough to mmap without fuss. Two separate windows so P0 and P1 extents
 * are never numerically confusable with each other. */
#define WIN_SIZE        (4UL * 1024 * 1024)

/* test_syssvc_* device-absent contract (vms-d40, ci.yml kernel-executive
 * negative control): with no /dev/vms the executive is absent and there is
 * nothing this suite can exercise or fabricate -- it MUST exit exactly 77
 * (honest SKIP), never 0 and never a plain 1. Reachable only on the
 * executive-absent rig; under a real /dev/vms the assertions below run. */
#define EXIT_SKIP       77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process B reports back to process A over a pipe. */
struct cross_report {
    uint32_t a_status;      /* B's VMS_IOCTL_GETJPI(PID=A) status */
    uint64_t a_p1_base;
    uint64_t a_p1_limit;
    uint32_t b_status;      /* B's own VMS_IOCTL_GETJPI(SELF) status */
    uint64_t b_p1_base;
    uint64_t b_p1_limit;
};

static int open_and_register(uint32_t *vms_pid)
{
    if (vms_kif_open() < 0)
        return EXIT_SKIP;
    if (vms_kif_register(vms_pid) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        return -1;
    }
    return 0;
}

/*
 * Process B. A separate Linux process, registered as a SEPARATE VMS
 * process (plain vms_kif_register(), not REGISTER_CONTINUE) -- it knows A
 * only by the VMS process id A already registered its P1 extent under,
 * sent down via a pipe. It has no access to A's memory.
 */
static void cross_reader(int wfd, uint32_t a_vms_pid)
{
    struct cross_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    if (vms_kif_open() < 0 || vms_kif_register(NULL) != SS_NORMAL)
        _exit(3);

    memset(&info, 0, sizeof(info));
    rep.a_status = vms_kif_getjpi_pid(a_vms_pid, &info);
    rep.a_p1_base = info.p1_base;
    rep.a_p1_limit = info.p1_limit;

    memset(&info, 0, sizeof(info));
    rep.b_status = vms_kif_getjpi_self(&info);
    rep.b_p1_base = info.p1_base;
    rep.b_p1_limit = info.p1_limit;

    if (write(wfd, &rep, sizeof(rep)) != (ssize_t)sizeof(rep))
        _exit(4);

    vms_kif_close();
    _exit(0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct vms_procinfo info;
    struct cross_report rep;
    uint32_t status, my_vms_pid = 0;
    void *p1_win, *p0_win;
    uint64_t p1_base, p1_limit, p0_base, p0_limit, old_base, old_limit;
    int pipefd[2];
    pid_t child;
    ssize_t n;
    int cycle;

    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_p1: P1 control-region persistence (vms-68f.ii) ===\n");

    int oar = open_and_register(&my_vms_pid);
    if (oar == EXIT_SKIP) {
        printf("=== test_syssvc_p1: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- executive absent) ===\n");
        return EXIT_SKIP;
    }
    if (oar < 0) {
        printf("=== test_syssvc_p1: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* ---- a fresh process has no P1 extent -------------------------- */

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL && info.p1_base == 0 && info.p1_limit == 0,
          "a process with no P1 extent registered reports none via GETJPI");

    /* ---- degenerate extents are refused, and refused cleanly -------- */

    CHECK(vms_kif_p1_map(0, 0x1000) == SS_BADPARAM,
          "P1_MAP refuses a null base");
    CHECK(vms_kif_p1_map(0x2000, 0x2000) == SS_BADPARAM,
          "P1_MAP refuses a limit that does not exceed base");
    CHECK(vms_kif_p1_map(0x3000, 0x2000) == SS_BADPARAM,
          "P1_MAP refuses a limit below base");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL && info.p1_base == 0 && info.p1_limit == 0,
          "a refused P1_MAP left no extent registered");

    /* ---- register a real P1 window ----------------------------------- */

    p1_win = mmap(NULL, WIN_SIZE, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p1_win != MAP_FAILED,
          "reserved a real PROT_NONE P1 window in this process's address space");
    p1_base = (uint64_t)(uintptr_t)p1_win;
    p1_limit = p1_base + WIN_SIZE;

    status = vms_kif_p1_map(p1_base, p1_limit);
    CHECK(status == SS_NORMAL, "VMS_IOCTL_P1_MAP registers the window");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    /* negctl: p1-map-not-recorded */
    CHECK(status == SS_NORMAL && info.p1_base == p1_base &&
          info.p1_limit == p1_limit,
          "GETJPI reflects the registered P1 extent: p1_base/p1_limit match "
          "what VMS_IOCTL_P1_MAP just registered");

    /* ---- A-writes / B-reads ------------------------------------------ */

    if (pipe(pipefd) != 0) {
        CHECK(0, "pipe()");
        goto cycle_p0;
    }
    child = fork();
    if (child < 0) {
        CHECK(0, "fork()");
        close(pipefd[0]); close(pipefd[1]);
        goto cycle_p0;
    }
    if (child == 0) {
        close(pipefd[0]);
        cross_reader(pipefd[1], my_vms_pid);
        _exit(5); /* unreachable */
    }
    close(pipefd[1]);

    memset(&rep, 0, sizeof(rep));
    n = read(pipefd[0], &rep, sizeof(rep));
    close(pipefd[0]);
    waitpid(child, NULL, 0);

    CHECK(n == (ssize_t)sizeof(rep) && rep.a_status == SS_NORMAL,
          "a second process (B) could read A's row at all");
    /* negctl-knockon: p1-map-not-recorded */
    CHECK(rep.a_status == SS_NORMAL && rep.a_p1_base == p1_base &&
          rep.a_p1_limit == p1_limit,
          "A-WRITES/B-READS: B reads A's registered P1 extent out of A's "
          "row -- a fact a per-process notion of P1 could not show");
    CHECK(rep.b_status == SS_NORMAL && rep.b_p1_base == 0 &&
          rep.b_p1_limit == 0,
          "B's own row still carries no P1 extent, so A's extent is not "
          "B's state echoed back");

cycle_p0:
    /* ---- THE KEY PROPERTY: P0 deleted on rundown, P1 survives -------- *
     * Register a P0 extent, then cycle map/unmap several times. P1 must
     * be unaffected by every single cycle -- this is the executive fact
     * that makes P0 (per-image, transient) and P1 (process-permanent)
     * genuinely distinct rather than two names for the same bookkeeping.
     */

    p0_win = mmap(NULL, WIN_SIZE, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p0_win != MAP_FAILED,
          "reserved a real PROT_NONE P0 window, separate from the P1 window");
    p0_base = (uint64_t)(uintptr_t)p0_win;
    p0_limit = p0_base + WIN_SIZE;

    for (cycle = 0; cycle < 3; cycle++) {
        status = vms_kif_p0_map(p0_base, p0_limit);
        CHECK(status == SS_NORMAL, "VMS_IOCTL_P0_MAP registers this cycle's P0 window");

        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_self(&info);
        /* negctl-knockon: p0-unmap-clears-p1 */
        /* negctl-knockon: p1-map-not-recorded */
        CHECK(status == SS_NORMAL && info.p1_base == p1_base &&
              info.p1_limit == p1_limit && info.p0_base == p0_base &&
              info.p0_limit == p0_limit,
              "after P0_MAP: P1 extent still matches what was registered "
              "before any P0 activity, and P0 is now also present");

        old_base = old_limit = 0xdeadbeefULL;
        status = vms_kif_p0_unmap(&old_base, &old_limit);
        CHECK(status == SS_NORMAL && old_base == p0_base && old_limit == p0_limit,
              "VMS_IOCTL_P0_UNMAP (rundown) succeeds and reports the exact "
              "P0 extent it just freed");

        memset(&info, 0, sizeof(info));
        status = vms_kif_getjpi_self(&info);
        CHECK(status == SS_NORMAL && info.p0_base == 0 && info.p0_limit == 0,
              "P0-DELETED-ON-RUNDOWN: GETJPI shows no P0 extent after unmap");
        /* negctl: p0-unmap-clears-p1 */
        /* negctl-knockon: p1-map-not-recorded */
        CHECK(status == SS_NORMAL && info.p1_base == p1_base &&
              info.p1_limit == p1_limit,
              "P1-SURVIVES: GETJPI still reflects the SAME P1 extent after "
              "this P0 teardown -- the property this increment exists to "
              "prove, not just once but across a repeated P0 map/unmap cycle");
    }

    munmap(p0_win, WIN_SIZE);
    munmap(p1_win, WIN_SIZE);

    vms_kif_close();
    printf("=== test_syssvc_p1: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
