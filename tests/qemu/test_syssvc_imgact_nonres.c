/*
 * test_syssvc_imgact_nonres.c - THE NON-RESIDENT-PRODUCER FLIP: a REAL image
 * whose .vms$imp names a producer DCL does NOT hold resident runs IN DCL's
 * process, no fork, proven against a real /dev/vms (vms-db2,
 * docs/design-in-process-activation.md Part II §A.8 remainder item 1).
 *
 * THE PROPERTY. The realimg/extern flips bind a real image only to the
 * ALREADY-RESIDENT producer. This proves the OTHER case the design names: an
 * image imports a universal (prod_bump) from TESTPROD.EXE, a shareable the
 * process does NOT hold. imgact_activate MAPS TESTPROD into DCL's process
 * (imgact_map_producer -- re-homed from IMGACT.EXE's load_ovmx_producer as an
 * in-process library routine), registers it, and binds the consumer's import to
 * that mapped instance -- a genuinely shared single instance, never a
 * per-consumer private copy (the LARP the authenticity invariants forbid).
 *
 * WHAT THIS SUITE ASSERTS against a real /dev/vms:
 *   1. TESTPROD is NOT resident before the run (the producer really is absent).
 *   2. The consumer RUNS in-process and returns SS$_NORMAL with the bump result
 *      as its SYS$EXIT code (SYS$EXIT returned control to DCL).
 *   3. The VMS PID and Linux PID are IDENTICAL across the run -- no fork.
 *   4. Control is back in Supervisor with the P0 extent cleared (rundown ran).
 *   5. TESTPROD is NOW resident (the activator mapped it), and the consumer's
 *      bump landed in that mapped instance (counter == 1).
 *   6. SHARED INSTANCE (the anti-LARP crux): the suite mutates the mapped
 *      producer's counter to 100; a SECOND run's bump returns 101 -- reaching
 *      the SAME instance the suite mutated, not a fresh private copy (which
 *      would return 1 again). TESTPROD stays mapped ONCE (registry dedup).
 *
 * NEGATIVE CONTROL (facility_defects nonres-producer-not-mapped): with
 * imgact_map_producer refusing to register the mapped producer, the consumer's
 * import cannot bind, imgact_activate returns SS$_UNSUPPORTED, and the flip does
 * not happen (the caller forks). If a "flip" reported success there it would be
 * a LARP (an image that looks activated but binds to nothing).
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
#include "ovmx_symvec.h"

#define EXIT_SKIP 77

/* TESTPROD.EXE's .vms$sv indices (must match testprod_shr.c). */
#define PROD_IDX_BUMP    0
#define PROD_IDX_COUNTER 1
#define PROD_SONAME      "TESTPROD.EXE"

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *testnonres_path(void)
{
    const char *p = getenv("OVMX_TESTNONRES");
    return p ? p : "/tests/TESTNONRES.EXE";
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/* ---- The RESIDENT producer standing in for LIBVMS$SHR ---------------------
 * The consumer imports SYS$EXIT (imgact_image_exit) from LIBVMS$SHR by vector
 * index 0. In the real flip that lives in the resident LIBVMS$SHR DCL holds;
 * here it is imgact_image_exit's address in THIS statically-linked process --
 * the SAME resident routine the in-process consumer must reach so its SYS$EXIT
 * returns to DCL. (prod_bump, by contrast, is NOT here -- it lives in the
 * NON-RESIDENT TESTPROD.EXE the activator maps.) */
static const struct ovmx_sv_header *build_libvms_sv(void)
{
    static uint8_t buf[sizeof(struct ovmx_sv_header) +
                       1 * sizeof(struct ovmx_sv_entry) + 8];
    struct ovmx_sv_header *h = (struct ovmx_sv_header *)buf;
    struct ovmx_sv_entry *e =
        (struct ovmx_sv_entry *)(buf + sizeof(struct ovmx_sv_header));

    memset(buf, 0, sizeof buf);
    h->magic = OVMX_SV_MAGIC;
    h->count = 1;
    h->gsmatch_kind = OVMX_GSMATCH_ALWAYS;
    h->gsmatch_major = 1;
    h->gsmatch_minor = 0;
    h->names_off = (uint32_t)(sizeof(struct ovmx_sv_header) +
                              1 * sizeof(struct ovmx_sv_entry));
    h->names_size = 1;
    e[0].value = (uint64_t)(uintptr_t)&imgact_image_exit;
    e[0].kind = OVMX_SV_PROCEDURE;
    return h;
}

static void publish_libvms(const struct ovmx_sv_header *sv)
{
    struct imgact_prod_pub list[1];
    list[0].soname = "LIBVMS$SHR.EXE";
    list[0].base   = 0;               /* sv values are absolute addresses */
    list[0].sv     = sv;
    imgact_prodreg_reset();
    imgact_publish_producers(list, 1);
}

/* The mapped TESTPROD producer's shared counter, reached through the registry
 * (the SAME instance the consumer bound to): base + .vms$sv[COUNTER].value. */
static volatile long *mapped_counter(void)
{
    uint64_t pbase = 0;
    const struct ovmx_sv_header *psv = 0;
    if (!imgact_find_producer(PROD_SONAME, &pbase, &psv) || !psv)
        return 0;
    const struct ovmx_sv_entry *e = ovmx_sv_entries(psv);
    return (volatile long *)(uintptr_t)(pbase + e[PROD_IDX_COUNTER].value);
}

/* Activate with fd 1 into a pipe so the image's banner is captured. */
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
        if (strstr(b, "OVMX-NONRES-RAN"))
            *ran = 1;
    }
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_imgact_nonres (non-resident producer mapped in-process, the flip, vms-db2) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_imgact_nonres: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    const char *img = testnonres_path();
    const struct ovmx_sv_header *sv = build_libvms_sv();
    struct vms_procinfo before, after;
    int ran = 0, image_rc = -1;
    uint32_t st;

    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS$_NORMAL,
          "the activating context is in Supervisor mode (as DCL is)");

    /* Publish ONLY the resident LIBVMS$SHR; TESTPROD is deliberately absent so
     * the activator must MAP it. imgact_prodreg_reset (inside publish) clears
     * any TESTPROD a prior test left resident. */
    publish_libvms(sv);
    CHECK(!imgact_find_producer(PROD_SONAME, 0, 0),
          "TESTPROD is NOT resident before the run (the producer really is absent)");

    memset(&before, 0, sizeof before);
    CHECK((vms_kif_getjpi_self(&before) & 1) != 0,
          "read this process's VMS identity before activation");
    pid_t linux_before = getpid();

    /* --- FIRST RUN: the consumer's prod_bump import names a non-resident
     * producer, so the activator maps TESTPROD and binds to it. --- */
    st = activate_capture(img, &ran, &image_rc);

    CHECK(st == SS$_NORMAL,
          "TESTNONRES ran in-process and returned SS$_NORMAL");
    CHECK(ran, "the consumer produced its banner (it actually ran)");
    CHECK(image_rc == 1,
          "the consumer's bump returned 1 -- its import reached the freshly "
          "mapped non-resident producer and incremented its counter");

    memset(&after, 0, sizeof after);
    CHECK((vms_kif_getjpi_self(&after) & 1) != 0,
          "read this process's VMS identity after activation");
    CHECK(before.vms_pid == after.vms_pid,
          "the VMS PID is UNCHANGED across the RUN -- no fork (the crux)");
    CHECK(getpid() == linux_before,
          "the Linux PID is UNCHANGED across the RUN -- the image did not fork");
    CHECK(after.current_mode == PSL_C_SUPER,
          "the executive is back in Supervisor after rundown");
    CHECK(after.p0_base == 0 && after.p0_limit == 0,
          "the P0 extent was cleared at rundown (P0 deleted, image-less)");

    /* --- The producer was genuinely MAPPED, and the consumer's bump landed in
     * THAT instance (the counter the registry points at reads 1). --- */
    volatile long *counter = mapped_counter();
    CHECK(counter != 0,
          "TESTPROD is NOW resident -- the activator mapped the non-resident "
          "producer into DCL's process");
    CHECK(counter && *counter == 1,
          "the mapped producer's counter reads 1 -- the consumer's bump landed "
          "in the instance the registry points at (not a private copy)");

    /* --- SHARED INSTANCE (anti-LARP crux): mutate the mapped counter, then a
     * SECOND run must observe THAT value -- proving it reached the SAME instance
     * the suite mutated, and that the producer was mapped ONCE (dedup). --- */
    uint64_t pbase1 = 0;
    imgact_find_producer(PROD_SONAME, &pbase1, 0);
    if (counter)
        *counter = 100;

    ran = 0; image_rc = -1;
    st = activate_capture(img, &ran, &image_rc);
    CHECK(st == SS$_NORMAL && ran,
          "the second run activated in-process too");
    /* negctl: nonres-producer-not-mapped */
    CHECK(image_rc == 101,
          "the second run's bump returned 101 -- it reached the SAME mapped "
          "producer instance the suite set to 100 (a per-consumer private copy "
          "would have returned 1) -- genuine single-instance sharing");

    uint64_t pbase2 = 0;
    CHECK(imgact_find_producer(PROD_SONAME, &pbase2, 0) && pbase2 == pbase1,
          "TESTPROD stayed mapped ONCE across both runs (registry dedup -- one "
          "resident instance, not one per activation)");

    printf("=== test_syssvc_imgact_nonres: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
