/*
 * test_syssvc_p0.c - the executive records a process's P0 program-region
 * extent (vms-68f.i, increment (i) of the Option A in-process image
 * activation design, docs/design-in-process-activation.md Part II §A.2.1).
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT. This increment does not activate
 * an image into P0, does not replace the RUN fork, and does not do access-
 * mode transitions or rundown -- those are increments (ii)-(vi). What it
 * does: VMS_IOCTL_P0_MAP/P0_UNMAP let a process register and clear a
 * [base, limit) virtual-address extent, and that extent is reflected back
 * through $GETJPI (struct vms_procinfo.p0_base/p0_limit) -- observable by
 * the process itself AND, exactly like every other identity field this
 * table carries, by a different process reading this one's row.
 *
 * THE DECISIVE TEST IS A-WRITES / B-READS (CLAUDE.md rule 11), same
 * discipline as tests/qemu/test_kmod_setterm.c. A per-process notion of
 * "my P0 extent" would pass every single-process assertion in this file
 * perfectly; only a SECOND process reading it out of the executive proves
 * the extent is executive-resident rather than a local variable this
 * program remembers about itself.
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
 * discipline test_kmod_setterm.c uses for its own borrowed statuses.
 */
#define SS_NORMAL       1
#define SS_BADPARAM     20

/* A real PROT_NONE reservation, the same shape design §A.2.1 describes
 * for DCL's P0 window -- large enough to be an unmistakable extent, small
 * enough to mmap without fuss. */
#define WIN_SIZE        (4UL * 1024 * 1024)

/* test_syssvc_* device-absent contract (vms-d40, ci.yml kernel-executive
 * negative control): with no /dev/vms the executive is absent and there is
 * nothing this suite can exercise or fabricate -- it MUST exit exactly 77
 * (honest SKIP), never 0 (fake pass) and never a plain 1. Only reachable on
 * the executive-absent rig; under a real /dev/vms open succeeds and the
 * assertions below run for real. */
#define EXIT_SKIP       77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* What process B reports back to process A over a pipe. */
struct cross_report {
    uint32_t a_status;      /* B's VMS_IOCTL_GETJPI(PID=A) status */
    uint64_t a_p0_base;
    uint64_t a_p0_limit;
    uint32_t b_status;      /* B's own VMS_IOCTL_GETJPI(SELF) status */
    uint64_t b_p0_base;
    uint64_t b_p0_limit;
};

/* Returns 0 on success, EXIT_SKIP (77) when /dev/vms is absent (honest skip),
 * or -1 when the device IS present but REGISTER was rejected (a real fault). */
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
 * only by the VMS process id A already mapped its extent under, sent down
 * argv/a pipe. It has no access to A's memory and A told it nothing about
 * P0 beyond that one number.
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
    rep.a_p0_base = info.p0_base;
    rep.a_p0_limit = info.p0_limit;

    memset(&info, 0, sizeof(info));
    rep.b_status = vms_kif_getjpi_self(&info);
    rep.b_p0_base = info.p0_base;
    rep.b_p0_limit = info.p0_limit;

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
    void *win;
    uint64_t base, limit, old_base, old_limit;
    int pipefd[2];
    pid_t child;
    ssize_t n;

    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_p0: P0 program-region map/free (vms-68f.i) ===\n");

    int oar = open_and_register(&my_vms_pid);
    if (oar == EXIT_SKIP) {
        printf("=== test_syssvc_p0: 0 passed, 0 failed "
               "(SKIPPED: no /dev/vms -- executive absent) ===\n");
        return EXIT_SKIP;
    }
    if (oar < 0) {
        printf("=== test_syssvc_p0: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* ---- a fresh process has no P0 extent -------------------------- */

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL && info.p0_base == 0 && info.p0_limit == 0,
          "a process with no P0 extent registered reports none via GETJPI");

    /* ---- degenerate extents are refused, and refused cleanly -------- */

    CHECK(vms_kif_p0_map(0, 0x1000) == SS_BADPARAM,
          "P0_MAP refuses a null base");
    CHECK(vms_kif_p0_map(0x2000, 0x2000) == SS_BADPARAM,
          "P0_MAP refuses a limit that does not exceed base");
    CHECK(vms_kif_p0_map(0x3000, 0x2000) == SS_BADPARAM,
          "P0_MAP refuses a limit below base");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL && info.p0_base == 0 && info.p0_limit == 0,
          "a refused P0_MAP left no extent registered");

    /* ---- map a real window ------------------------------------------ */

    win = mmap(NULL, WIN_SIZE, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(win != MAP_FAILED,
          "reserved a real PROT_NONE window in this process's address space");
    base = (uint64_t)(uintptr_t)win;
    limit = base + WIN_SIZE;

    status = vms_kif_p0_map(base, limit);
    CHECK(status == SS_NORMAL, "VMS_IOCTL_P0_MAP registers the window");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    /* negctl: p0-map-not-recorded */
    CHECK(status == SS_NORMAL && info.p0_base == base && info.p0_limit == limit,
          "GETJPI reflects the mapped P0 extent: p0_base/p0_limit match what "
          "VMS_IOCTL_P0_MAP just registered");

    /* ---- A-writes / B-reads ------------------------------------------ */

    if (pipe(pipefd) != 0) {
        CHECK(0, "pipe()");
        goto unmap;
    }
    child = fork();
    if (child < 0) {
        CHECK(0, "fork()");
        close(pipefd[0]); close(pipefd[1]);
        goto unmap;
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
    /* negctl-knockon: p0-map-not-recorded */
    CHECK(rep.a_status == SS_NORMAL && rep.a_p0_base == base &&
          rep.a_p0_limit == limit,
          "A-WRITES/B-READS: B reads A's mapped P0 extent out of A's row -- "
          "a fact a per-process notion of P0 could not show");
    CHECK(rep.b_status == SS_NORMAL && rep.b_p0_base == 0 &&
          rep.b_p0_limit == 0,
          "B's own row still carries no P0 extent, so A's extent is not "
          "B's state echoed back");

unmap:
    /* ---- unmap releases it, and reports what it released ------------- */

    old_base = old_limit = 0xdeadbeefULL;
    status = vms_kif_p0_unmap(&old_base, &old_limit);
    CHECK(status == SS_NORMAL, "VMS_IOCTL_P0_UNMAP succeeds");
    /* negctl-knockon: p0-map-not-recorded */
    CHECK(old_base == base && old_limit == limit,
          "P0_UNMAP reports the exact extent it just freed");

    memset(&info, 0, sizeof(info));
    status = vms_kif_getjpi_self(&info);
    CHECK(status == SS_NORMAL && info.p0_base == 0 && info.p0_limit == 0,
          "GETJPI shows no P0 extent after unmap");

    /* ---- unmap is idempotent: nothing left to free the second time --- */

    old_base = old_limit = 0xdeadbeefULL;
    status = vms_kif_p0_unmap(&old_base, &old_limit);
    CHECK(status == SS_NORMAL && old_base == 0 && old_limit == 0,
          "unmapping an already-unmapped process succeeds and reports "
          "nothing to free");

    munmap(win, WIN_SIZE);

    vms_kif_close();
    printf("=== test_syssvc_p0: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
