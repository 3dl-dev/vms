/*
 * test_syssvc_imgact_realimg.c - THE FLIP: a REAL image (SysV auxv _start entry,
 * resident-bound imports, SYS$EXIT) runs IN DCL's process, no fork, proven
 * against a real /dev/vms (vms-db2, docs/design-in-process-activation.md Part II
 * §A.8 remainder gap 2 + the flip).
 *
 * THE PROPERTY. Increment iv proved the (a0,a1) marker class runs in-process.
 * This proves the REAL-image class does: TESTREAL.EXE is entered through the
 * SysV auxv `_start` ABI (imgact_activate builds an argc/argv/envp/auxv stack and
 * jumps to _start on a separate P0 stack), reaches the executive ONLY by binding
 * its .vms$imp imports to the ALREADY-RESIDENT producer (never a private copy),
 * and ends by calling the imported SYS$EXIT (imgact_image_exit), which RETURNS
 * to this process instead of terminating it. dcl_activate_image runs this class
 * in-process (no fork()+execve()).
 *
 * WHAT THIS SUITE ASSERTS against a real /dev/vms:
 *   1. The real image RUNS (its banner) and returns SS$_NORMAL with the image's
 *      SYS$EXIT code -- proving SYS$EXIT returned control to DCL (not a fault,
 *      not a process exit).
 *   2. The VMS PID and the Linux PID are IDENTICAL before and after -- no fork.
 *   3. Control is back in Supervisor with the P0 extent cleared (rundown ran).
 *   4. FLOWS BACK (executive): a process-permanent event flag the in-process
 *      image SET (via the resident vms_kif_setef) is readable by DCL after the
 *      image runs down -- the ONE process's PCB. Option B's forked child had its
 *      OWN event flags (design line 144), so this could never flow back under B.
 *   5. SHARED TLS: a __thread datum in the resident producer that the image
 *      bumped (via the resident tls_bump, through the UNCHANGED thread pointer)
 *      is visible here -- the image reached DCL's resident TLS, not a private
 *      copy (§A.8 gap 2b, the no-own-PT_TLS class).
 *   6. NEGATIVE CONTROL (realimg-import-not-bound): with the resident producer
 *      NOT published, the image's imports cannot bind, imgact_activate returns
 *      SS$_UNSUPPORTED, and the flip does not happen (the caller would fork) --
 *      the anti-LARP anchor. If a "flip" reported success here it would be a
 *      LARP (an image that looks activated but shares nothing).
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
#include <stdint.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "imgact_activate.h"
#include "imgact_prodreg.h"
#include "ovmx_image.h"

#define EXIT_SKIP 77

/* Must match testreal_inproc.c's fixed work parameters. */
#define REAL_EFN         40   /* a LOCAL event flag (cluster 1, 32-63): no ascefc */
#define REAL_TLS_DELTA   7L
#define REAL_EXIT_CODE   0

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *testreal_path(void)
{
    const char *p = getenv("OVMX_TESTREAL");
    return p ? p : "/tests/TESTREAL.EXE";
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/* ---- The resident producer this suite stands in for LIBVMS$SHR ----------
 * TESTREAL imports three universals by vector index (0 SYS$EXIT, 1 SETEF,
 * 2 TLS-bump). In the real flip these live in the resident LIBVMS$SHR/libvmssys
 * DCL already holds; here they are addresses in THIS statically-linked process
 * -- imgact_image_exit (libvms) and vms_kif_setef (libvmssys) are the SAME
 * resident routines the in-process image must reach, and tls_bump touches a
 * __thread datum resident in this process to prove the shared thread pointer. */

static __thread long g_resident_tls_counter;   /* the "resident DECC$SHR TLS" */

/* PROCEDURE universal index 2: bump the resident __thread counter. Reached by
 * the in-process image through the unchanged TP -- so its effect lands on THIS
 * counter (shared TLS), not a private image copy. */
static long tls_bump(long delta)
{
    g_resident_tls_counter += delta;
    return g_resident_tls_counter;
}

/* Build the producer .vms$sv: 3 PROCEDURE entries at absolute addresses (base 0),
 * GSMATCH ALWAYS 1.0 -- exactly what TESTREAL's .vms$imp records requested. */
static const struct ovmx_sv_header *build_producer_sv(void)
{
    static uint8_t buf[sizeof(struct ovmx_sv_header) +
                       3 * sizeof(struct ovmx_sv_entry) + 8];
    struct ovmx_sv_header *h = (struct ovmx_sv_header *)buf;
    struct ovmx_sv_entry *e =
        (struct ovmx_sv_entry *)(buf + sizeof(struct ovmx_sv_header));

    memset(buf, 0, sizeof buf);
    h->magic = OVMX_SV_MAGIC;
    h->count = 3;
    h->gsmatch_kind = OVMX_GSMATCH_ALWAYS;
    h->gsmatch_major = 1;
    h->gsmatch_minor = 0;
    h->names_off = (uint32_t)(sizeof(struct ovmx_sv_header) +
                              3 * sizeof(struct ovmx_sv_entry));
    h->names_size = 1;   /* one NUL: names are diagnostics only, bind is by index */
    e[0].value = (uint64_t)(uintptr_t)&imgact_image_exit;
    e[0].kind = OVMX_SV_PROCEDURE;
    e[1].value = (uint64_t)(uintptr_t)&vms_kif_setef;
    e[1].kind = OVMX_SV_PROCEDURE;
    e[2].value = (uint64_t)(uintptr_t)&tls_bump;
    e[2].kind = OVMX_SV_PROCEDURE;
    return h;
}

static void publish_resident(const struct ovmx_sv_header *sv)
{
    struct imgact_prod_pub list[1];
    list[0].soname = "LIBVMS$SHR.EXE";
    list[0].base   = 0;               /* sv values are absolute addresses */
    list[0].sv     = sv;
    imgact_prodreg_reset();
    imgact_publish_producers(list, 1);
}

/*
 * Activate with fd 1 into a pipe so the image's banner is captured, not spliced
 * into the assertion stream. Returns the activation status; *ran set if seen.
 */
static uint32_t activate_capture(const char *path, int *ran, int *image_rc)
{
    int pfd[2];
    char b[256];
    int saved;
    uint32_t st;
    ssize_t n;

    *ran = 0;
    if (pipe(pfd) < 0)
        return SS$_ABORT;
    saved = dup(1);
    fflush(stdout);
    dup2(pfd[1], 1);

    st = imgact_activate(path, 0, 0, NULL, image_rc);

    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    close(pfd[1]);
    n = read(pfd[0], b, sizeof(b) - 1);
    close(pfd[0]);
    if (n > 0) {
        b[n] = '\0';
        if (strstr(b, "OVMX-REALIMG-RAN"))
            *ran = 1;
    }
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_imgact_realimg (REAL image runs in-process, the flip, vms-db2) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_imgact_realimg: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    const char *img = testreal_path();
    const struct ovmx_sv_header *sv = build_producer_sv();
    struct vms_procinfo before, after;
    int ran = 0, image_rc = -1;
    uint32_t st;

    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS$_NORMAL,
          "the activating context is in Supervisor mode (as DCL is)");

    /* Start the event flag CLEAR so a later WASSET is unambiguously the image's. */
    vms_kif_clref(REAL_EFN);

    memset(&before, 0, sizeof before);
    CHECK((vms_kif_getjpi_self(&before) & 1) != 0,
          "read this process's VMS identity before activation");
    pid_t linux_before = getpid();
    long tls_before = g_resident_tls_counter;

    /* --- NEGATIVE CONTROL first: producer NOT published -> imports cannot bind
     * -> the real image is REFUSED the in-process path (would fork). This is the
     * anti-LARP anchor: the flip's success below MUST depend on genuine binding
     * to the resident producer. --- */
    imgact_prodreg_reset();
    {
        int r0 = 0, rc0 = -1;
        uint32_t su = activate_capture(img, &r0, &rc0);
        /* anti-LARP: the flip's success below MUST depend on genuine resident
         * binding. (Not a facility_defects anchor -- it is a test-internal
         * condition, not a source mutation; the registered anchor for this
         * suite is realimg-auxv-argc-wrong on the exit-code assertion below.) */
        CHECK(su == SS$_UNSUPPORTED,
              "with the resident producer UNPUBLISHED, the real image's imports "
              "do not bind and imgact_activate refuses it (SS$_UNSUPPORTED -> "
              "caller forks) -- the flip is not faked");
        CHECK(!r0, "the refused image did not run in-process (no banner)");
    }

    /* --- THE FLIP: publish the resident producer, then activate in-process. --- */
    publish_resident(sv);
    st = activate_capture(img, &ran, &image_rc);

    CHECK(st == SS$_NORMAL,
          "the REAL image ran in-process and returned SS$_NORMAL");
    CHECK(ran, "the real image produced its banner (it actually ran)");
    /* SYS$EXIT returned to DCL: the image's exit CODE came back through the
     * setjmp/longjmp EXIT path (a fault would have been SS$_ACCVIO, a hang would
     * not return at all). The image only reaches REAL_EXIT_CODE after reading
     * argc == 1 off the constructed auxv stack, so this also proves the SysV
     * entry ABI delivered a correct stack (a wrong stack -> its distinct bad-ABI
     * exit code -> this fails). */
    /* negctl: realimg-auxv-argc-wrong */
    CHECK(image_rc == REAL_EXIT_CODE,
          "SYS$EXIT returned control to DCL with the image's exit code -- the "
          "command loop resumed in the same process (not a fault, not a process exit)");

    memset(&after, 0, sizeof after);
    CHECK((vms_kif_getjpi_self(&after) & 1) != 0,
          "read this process's VMS identity after activation");
    CHECK(before.vms_pid == after.vms_pid,
          "the VMS PID is UNCHANGED across the RUN of a REAL image -- no fork (the crux)");
    CHECK(getpid() == linux_before,
          "the Linux PID is UNCHANGED across the RUN -- the real image did not fork");
    CHECK(after.current_mode == PSL_C_SUPER,
          "the executive is back in Supervisor after real-image rundown");
    CHECK(after.p0_base == 0 && after.p0_limit == 0,
          "the P0 extent was cleared at rundown (P0 deleted, image-less)");

    /* --- FLOWS BACK (executive): the event flag the in-process image set is
     * visible to DCL after the image ran down. --- */
    {
        uint32_t state = 0;
        vms_kif_readef(REAL_EFN, &state);
        /* negctl-knockon: realimg-import-not-bound (unbound -> setef never called) */
        CHECK((state & (1u << (REAL_EFN % 32))) != 0,
              "a process-permanent event flag SET by the in-process real image is "
              "visible to DCL after rundown -- executive flows-back the fork "
              "(Option B, per-child flags) never could");
    }

    /* --- SHARED TLS: the resident __thread counter the image bumped moved by
     * exactly REAL_TLS_DELTA -- the image reached DCL's resident TLS, not a
     * private copy (TP unchanged for a no-own-PT_TLS image). --- */
    CHECK(g_resident_tls_counter == tls_before + REAL_TLS_DELTA,
          "the resident __thread datum the in-process image bumped is visible here "
          "-- the real image shares DCL's resident TLS (no private copy)");

    printf("=== test_syssvc_imgact_realimg: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
