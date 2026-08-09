/*
 * test_syssvc_imgact_inproc.c - RUN activates an image IN DCL's process, not a
 * fork (vms-68f increment iv), proven against a real /dev/vms.
 *
 * THE PROPERTY. On OpenVMS, RUN / a foreign command / a DCL utility maps the
 * image into the CURRENT process's P0 region and runs it there -- SAME PID --
 * then image rundown returns control to DCL in the same process. OVMX has
 * always fork()+execve()'d a fresh Linux process per image. imgact_activate()
 * (src/libvms/syssvc/sys_imgact.c), dispatched by dcl_activate_image(), runs an
 * in-process-eligible image WITHOUT forking: mapped into a P0 window, entered
 * in User mode across a vms.ko Supervisor->User transition,
 * with DCL's critical-P1 pages mprotect'd read-only for the duration.
 *
 * WHAT THIS SUITE ASSERTS against a real /dev/vms:
 *   1. The in-process image RUNS and produces its output, and the VMS PID and
 *      the Linux PID are IDENTICAL before and after -- no process was created.
 *   2. Control RETURNS to the caller and the executive is back in Supervisor
 *      with no image active and the P0 extent cleared (rundown happened).
 *   3. A critical-P1 page mprotect'd read-only while the image runs is NOT
 *      corrupted by an image that tries to scribble it: the write faults, the
 *      image is run down with SS$_ACCVIO, and the sentinel is intact (the
 *      enforced half of the access-mode model, design §A.2.3(b)). THIS is the
 *      anti-LARP anchor: skip the p1_protect and the scribble succeeds.
 *   4. A real image (no OVMX in-process marker / a PT_INTERP) is REFUSED the
 *      in-process path with SS$_UNSUPPORTED, so RUN of a real image still uses
 *      fork -- the in-process path did not hijack every activation.
 *   5. THE PAYOFF (vms-68f.v): a side effect the in-process image leaves in
 *      DCL's own address space -- a store into a writable P1 cell -- is visible
 *      to DCL AFTER the image runs down, at the SAME PID. This is the exact
 *      thing Option B (the fork) could never do (line 149 of the design: the
 *      child's write landed in a separate, dead address space), and it is the
 *      mechanism behind a process-permanent DEFINE flowing back to DCL.
 *   6. vms_kif_p1_map (wired in this increment by dcl_p1_init) round-trips: a
 *      registered P1 extent is reported back by $GETJPI. This is the wrapper->
 *      kernel->$GETJPI path DCL's startup P1 registration depends on.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no executive it exits
 * EXIT_SKIP (77), never a fake pass: imgact_activate() fails SS$_NOSUCHDEV and
 * refuses to run the image at all (INV-6).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "imgact_activate.h"

#define EXIT_SKIP 77
#define PGSZ 4096UL

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *testimg_path(void)
{
    const char *p = getenv("OVMX_TESTIMG");
    return p ? p : "/tests/TESTIMG.EXE";
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/*
 * Run imgact_activate() with fd 1 redirected into a pipe so the image's
 * banner (written to fd 1) is captured rather than spliced into this suite's
 * assertion stream. Returns the activation status; *ran is set if the banner
 * was seen.
 */
static uint32_t activate_capture(const char *path, long mode, long addr,
                                 const struct imgact_critp1 *crit,
                                 int *ran, int *image_rc)
{
    int pfd[2];
    char buf[256];
    int saved;
    uint32_t st;
    ssize_t n;

    *ran = 0;
    if (pipe(pfd) < 0)
        return SS$_ABORT;
    saved = dup(1);
    fflush(stdout);
    dup2(pfd[1], 1);

    st = imgact_activate(path, mode, addr, crit, image_rc);

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(pfd[1]);
    n = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "OVMX-INPROC-IMAGE-RAN"))
            *ran = 1;
    }
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   /* vms-b5b */
    printf("=== test_syssvc_imgact_inproc (RUN runs in-process, no fork, vms-68f iv) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_imgact_inproc: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    const char *img = testimg_path();
    struct vms_procinfo before, after;
    int ran = 0, image_rc = -1;
    uint32_t st;

    /* DCL runs in Supervisor; imgact_activate()'s ENTER_IMAGE requires it.
     * The interactive/system context that RUNs an image holds CMKRNL, so the
     * ascent to Supervisor is legitimate (a QEMU test runs with CAP_SYS_ADMIN,
     * which the executive maps to the enforced set including CMKRNL/CMEXEC). */
    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS$_NORMAL,
          "the activating context is in Supervisor mode (as DCL is)");

    memset(&before, 0, sizeof before);
    CHECK((vms_kif_getjpi_self(&before) & 1) != 0,
          "read this process's VMS identity before activation");
    pid_t linux_before = getpid();

    /* A critical-P1 page with a sentinel, to be protected while the image
     * runs (a stand-in for DCL's crown-jewel P1 structures). */
    void *crit = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(crit != MAP_FAILED, "allocated a critical-P1 sentinel page");
    *(volatile long *)crit = 0x5AFE5AFE;
    struct imgact_critp1 cp1 = { (uint64_t)(uintptr_t)crit,
                                 (uint64_t)(uintptr_t)crit + PGSZ };

    /* --- 1. BENIGN in-process activation: same PID, ran, returned -------- */
    st = activate_capture(img, 0 /*benign*/, 0, &cp1, &ran, &image_rc);
    CHECK(st == SS$_NORMAL, "imgact_activate ran the image in-process and returned SS$_NORMAL");
    CHECK(ran, "the in-process image produced its output");
    CHECK(image_rc == 0, "the image entry returned 0");

    memset(&after, 0, sizeof after);
    CHECK((vms_kif_getjpi_self(&after) & 1) != 0,
          "read this process's VMS identity after activation");
    CHECK(before.vms_pid == after.vms_pid,
          "the VMS PID is UNCHANGED across the RUN -- no new process (the crux)");
    CHECK(getpid() == linux_before,
          "the Linux PID is UNCHANGED across the RUN -- the image did not fork");
    CHECK(after.current_mode == PSL_C_SUPER,
          "the executive is back in Supervisor after image rundown");
    CHECK(after.p0_base == 0 && after.p0_limit == 0,
          "the P0 extent was cleared at rundown (P0 deleted, image-less)");
    CHECK(*(volatile long *)crit == 0x5AFE5AFE,
          "the benign image left the critical-P1 sentinel intact");

    /* --- 2. HOSTILE image scribbling protected P1: faults, DCL survives -- */
    ran = 0; image_rc = -1;
    st = activate_capture(img, 1 /*scribble crit*/, (long)(uintptr_t)crit,
                          &cp1, &ran, &image_rc);
    CHECK(ran, "the hostile image still ran and produced output before scribbling");
    CHECK(st == SS$_ACCVIO,
          "the image's wild write to the protected P1 page was an access violation");
    /* negctl: imgact-p1-not-protected */
    CHECK(*(volatile long *)crit == 0x5AFE5AFE,
          "the critical-P1 sentinel is INTACT -- the User-mode image could not "
          "corrupt DCL's P1 (the protection held at the MMU)");

    memset(&after, 0, sizeof after);
    vms_kif_getjpi_self(&after);
    CHECK(before.vms_pid == after.vms_pid,
          "the VMS PID is still unchanged after the faulting image ran down");
    CHECK(after.current_mode == PSL_C_SUPER,
          "the executive returned to Supervisor after the ACCVIO rundown");
    CHECK(after.p0_base == 0 && after.p0_limit == 0,
          "the P0 extent was cleared after the faulting image's rundown");

    /* --- 3. A real image (no marker / a PT_INTERP) is NOT hijacked ------- */
    {
        int r2 = 0, rc2 = -1;
        uint32_t su = imgact_activate("/tests/DCL.EXE", 0, 0, NULL, &rc2);
        (void)r2;
        CHECK(su == SS$_UNSUPPORTED,
              "a real image is REFUSED the in-process path (SS$_UNSUPPORTED) so "
              "RUN still forks for it -- the in-process path did not hijack it");
    }

    /* --- 4. THE PAYOFF: a side effect the image leaves in DCL's address
     * space flows back after rundown, same PID (vms-68f.v). A writable P1
     * cell DCL owns -- NOT the protected critical page -- is zeroed; the
     * in-process image (mode 2) stores a sentinel into it; after the image
     * runs down DCL reads the sentinel. Under Option B (a forked child) the
     * store landed in the child's separate address space and was gone the
     * moment control returned -- this assertion is exactly what A buys and B
     * could not. */
    {
        void *p1cell = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(p1cell != MAP_FAILED, "allocated a writable P1 cell DCL owns");
        *(volatile long *)p1cell = 0;               /* DCL's value before */

        ran = 0; image_rc = -1;
        st = activate_capture(img, 2 /*flow-back store*/,
                              (long)(uintptr_t)p1cell, &cp1, &ran, &image_rc);
        CHECK(st == SS$_NORMAL, "the flow-back image ran in-process and returned");
        CHECK(ran, "the flow-back image produced its output");

        memset(&after, 0, sizeof after);
        vms_kif_getjpi_self(&after);
        CHECK(before.vms_pid == after.vms_pid,
              "the VMS PID is UNCHANGED across the flow-back RUN -- same process");
        CHECK(getpid() == linux_before,
              "the Linux PID is UNCHANGED -- no fork happened");
        /* THE PAYOFF ASSERTION. If activation forked (Option B), this cell
         * would be untouched (the write hit a dead child address space). */
        CHECK(*(volatile long *)p1cell == 0x0F10B4C0L,
              "the value the in-process image stored into DCL's P1 cell is "
              "VISIBLE to DCL after rundown -- the payoff a fork could never "
              "deliver (process-permanent DEFINE flows back)");
        CHECK(after.p0_base == 0 && after.p0_limit == 0,
              "the flow-back image's P0 was torn down at rundown");
        munmap(p1cell, PGSZ);
    }

    /* --- 5. vms_kif_p1_map round-trips to $GETJPI (the path dcl_p1_init uses
     * to register DCL's P1 control region at startup, vms-68f.v). Register a
     * page as this process's P1 extent and confirm $GETJPI reports it. */
    {
        void *p1 = mmap(NULL, PGSZ * 2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(p1 != MAP_FAILED, "allocated a P1 control-region block");
        uint64_t p1b = (uint64_t)(uintptr_t)p1;
        uint64_t p1l = p1b + PGSZ * 2;
        CHECK(vms_kif_p1_map(p1b, p1l) == SS$_NORMAL,
              "vms_kif_p1_map registered the P1 extent (the call dcl_p1_init makes)");
        struct vms_procinfo pj;
        memset(&pj, 0, sizeof pj);
        vms_kif_getjpi_self(&pj);
        CHECK(pj.p1_base == p1b && pj.p1_limit == p1l,
              "$GETJPI reports the registered P1 extent -- the wiring is live");
        munmap(p1, PGSZ * 2);
    }

    munmap(crit, PGSZ);
    printf("=== test_syssvc_imgact_inproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
