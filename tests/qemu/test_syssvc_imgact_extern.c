/*
 * test_syssvc_imgact_extern.c - THE EXTERNAL-IMAGE FLIP: a genuinely external
 * LINK.EXE image (carrying a PT_INTERP that names the OVMX loader, SysV auxv
 * _start entry, resident-bound imports, SYS$EXIT) runs IN DCL's process, no
 * fork, proven against a real /dev/vms (vms-db2, docs/design-in-process-
 * activation.md Part II §A.8 remainder item 2).
 *
 * THE PROPERTY. The previous flip (test_syssvc_imgact_realimg) proved a REAL
 * image (auxv entry, resident imports) runs in-process -- but only for an image
 * with NO PT_INTERP. A genuinely EXTERNAL linker-produced executable carries a
 * PT_INTERP naming its loader (LINK.EXE writes IMGACT.EXE, src/vmslink/link.c);
 * before this flip imgact_activate rejected ANY PT_INTERP and dcl_activate_image
 * forked. The flip: in-process activation IS that loader, so imgact_activate now
 * accepts a PT_INTERP whose basename is IMGACT.EXE and runs the image IN DCL's
 * process; a FOREIGN interp still forks. TESTEXTERN.EXE is the real-image
 * subject PLUS that PT_INTERP -- the defining trait of a real executable.
 *
 * WHAT THIS SUITE ASSERTS against a real /dev/vms:
 *   INTERP DISCRIMINATION (anti-over-acceptance): TESTEXTERN_FOREIGN.EXE -- an
 *     image in-process-eligible in every other respect but named by a FOREIGN
 *     interpreter -- is REFUSED (SS$_UNSUPPORTED, no banner). Accepting a
 *     PT_INTERP naming our loader must NOT mean accepting a foreign one (that
 *     would be a LARP: running an image whose real loader we skipped).
 *   ANTI-LARP BINDING (unpublished producer): with the resident producer NOT
 *     published, the external image's imports cannot bind and it is refused
 *     SS$_UNSUPPORTED -- the flip's success below MUST depend on genuine
 *     resident binding.
 *   THE FLIP: with the producer published, TESTEXTERN.EXE (PT_INTERP = the OVMX
 *     loader) runs in-process and returns SS$_NORMAL with its SYS$EXIT code; the
 *     VMS and Linux PID are UNCHANGED (no fork -- the crux); control is back in
 *     Supervisor with the P0 extent cleared (rundown ran); a process-permanent
 *     event flag the image SET flows back to DCL (executive, the fork never
 *     could); the resident __thread datum it bumped is visible here (shared TLS).
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no executive it exits
 * EXIT_SKIP (77), never a fake pass (INV-6).
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

/* Must match testextern_inproc.c's fixed work parameters. */
#define EXT_EFN          40   /* a LOCAL event flag (cluster 1, 32-63): no ascefc */
#define EXT_TLS_DELTA    7L
#define EXT_EXIT_CODE    0

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *testextern_path(void)
{
    const char *p = getenv("OVMX_TESTEXTERN");
    return p ? p : "/tests/TESTEXTERN.EXE";
}
static const char *testextern_foreign_path(void)
{
    const char *p = getenv("OVMX_TESTEXTERN_FOREIGN");
    return p ? p : "/tests/TESTEXTERN_FOREIGN.EXE";
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
 * Identical to test_syssvc_imgact_realimg: three universals by vector index
 * (0 SYS$EXIT, 1 SETEF, 2 TLS-bump), the SAME resident routines the in-process
 * image reaches. */
static __thread long g_resident_tls_counter;

static long tls_bump(long delta)
{
    g_resident_tls_counter += delta;
    return g_resident_tls_counter;
}

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
    h->names_size = 1;
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
    list[0].base   = 0;
    list[0].sv     = sv;
    imgact_prodreg_reset();
    imgact_publish_producers(list, 1);
}

/*
 * Activate with fd 1 into a pipe so the image's banner is captured. `want` is
 * the banner string to look for; *ran set if seen. Returns the activation status.
 */
static uint32_t activate_capture(const char *path, const char *want,
                                 int *ran, int *image_rc)
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
        if (strstr(b, want))
            *ran = 1;
    }
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_imgact_extern (EXTERNAL image, PT_INTERP, runs in-process, vms-db2) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_imgact_extern: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    const char *img = testextern_path();
    const char *foreign = testextern_foreign_path();
    const struct ovmx_sv_header *sv = build_producer_sv();
    struct vms_procinfo before, after;
    int ran = 0, image_rc = -1;
    uint32_t st;

    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS$_NORMAL,
          "the activating context is in Supervisor mode (as DCL is)");

    /* Start the event flag CLEAR so a later WASSET is unambiguously the image's. */
    vms_kif_clref(EXT_EFN);

    /* --- INTERP DISCRIMINATION: an image named by a FOREIGN interpreter, but
     * otherwise in-process-eligible (OVMX marker, auxv ABI, no PT_TLS, imports
     * nothing), is REFUSED the in-process path. Publish the producer first so
     * the ONLY reason it is refused is the foreign interp, not an unbound import.
     * --- */
    publish_resident(sv);
    {
        int rf = 0, rcf = -1;
        uint32_t su = activate_capture(foreign, "OVMX-FOREIGN-RAN", &rf, &rcf);
        /* negctl: extern-interp-check-rejects-ours (see below) also guards the
         * inverse; this is the in-test guard against OVER-acceptance. */
        CHECK(su == SS$_UNSUPPORTED,
              "an image named by a FOREIGN interpreter is refused the in-process "
              "path (SS$_UNSUPPORTED -> caller forks) even though it is otherwise "
              "eligible -- accepting our loader's name does not accept a foreign one");
        CHECK(!rf, "the foreign-interp image did NOT run in-process (no banner)");
    }

    memset(&before, 0, sizeof before);
    CHECK((vms_kif_getjpi_self(&before) & 1) != 0,
          "read this process's VMS identity before activation");
    pid_t linux_before = getpid();
    long tls_before = g_resident_tls_counter;

    /* --- ANTI-LARP BINDING: producer NOT published -> the external image's
     * imports cannot bind -> it is REFUSED (would fork). The flip's success
     * below MUST depend on genuine resident binding. --- */
    imgact_prodreg_reset();
    {
        int r0 = 0, rc0 = -1;
        uint32_t su = activate_capture(img, "OVMX-EXTERN-RAN", &r0, &rc0);
        CHECK(su == SS$_UNSUPPORTED,
              "with the resident producer UNPUBLISHED, the external image's imports "
              "do not bind and imgact_activate refuses it (SS$_UNSUPPORTED -> fork) "
              "-- the flip is not faked");
        CHECK(!r0, "the refused external image did not run in-process (no banner)");
    }

    /* --- THE FLIP: publish the producer, then activate the EXTERNAL image
     * (PT_INTERP = the OVMX loader) in-process. --- */
    publish_resident(sv);
    st = activate_capture(img, "OVMX-EXTERN-RAN", &ran, &image_rc);

    /* If imgact_interp_is_ours is mutated to reject the OVMX loader's own name
     * (extern-interp-check-rejects-ours), this external image -- which carries a
     * PT_INTERP -- is refused SS$_UNSUPPORTED and never runs in-process, so this
     * assertion (and every one below) FAILS. The PT_INTERP acceptance of our own
     * loader is load-bearing, not decorative. */
    /* negctl: extern-interp-check-rejects-ours */
    CHECK(st == SS$_NORMAL,
          "the EXTERNAL image (with a PT_INTERP naming the OVMX loader) ran "
          "in-process and returned SS$_NORMAL");
    CHECK(ran, "the external image produced its banner (it actually ran)");
    CHECK(image_rc == EXT_EXIT_CODE,
          "SYS$EXIT returned control to DCL with the image's exit code -- the "
          "command loop resumed in the same process (not a fault, not a process exit)");

    memset(&after, 0, sizeof after);
    CHECK((vms_kif_getjpi_self(&after) & 1) != 0,
          "read this process's VMS identity after activation");
    /* negctl-knockon: extern-interp-check-rejects-ours */
    CHECK(before.vms_pid == after.vms_pid,
          "the VMS PID is UNCHANGED across the RUN of an EXTERNAL image -- no fork (the crux)");
    CHECK(getpid() == linux_before,
          "the Linux PID is UNCHANGED across the RUN -- the external image did not fork");
    CHECK(after.current_mode == PSL_C_SUPER,
          "the executive is back in Supervisor after external-image rundown");
    CHECK(after.p0_base == 0 && after.p0_limit == 0,
          "the P0 extent was cleared at rundown (P0 deleted, image-less)");

    /* --- FLOWS BACK (executive): the event flag the in-process image set is
     * visible to DCL after the image ran down. --- */
    {
        uint32_t state = 0;
        vms_kif_readef(EXT_EFN, &state);
        CHECK((state & (1u << (EXT_EFN % 32))) != 0,
              "a process-permanent event flag SET by the in-process external image "
              "is visible to DCL after rundown -- executive flows-back the fork "
              "never could");
    }

    /* --- SHARED TLS: the resident __thread counter the image bumped moved by
     * exactly EXT_TLS_DELTA -- the image reached DCL's resident TLS. --- */
    CHECK(g_resident_tls_counter == tls_before + EXT_TLS_DELTA,
          "the resident __thread datum the in-process external image bumped is "
          "visible here -- it shares DCL's resident TLS (no private copy)");

    printf("=== test_syssvc_imgact_extern: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
