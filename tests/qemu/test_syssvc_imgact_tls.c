/*
 * test_syssvc_imgact_tls.c - OWN-PT_TLS in-process activation: a REAL image with
 * its OWN __thread data runs IN DCL's process, its TLS DISTINCT and correct while
 * DCL's thread pointer + TLS and the resident universals' TLS survive untouched,
 * proven against a real /dev/vms (vms-db2, docs/design-in-process-activation.md
 * Part II §A.8 remainder item 4 -- the DTV-append / TLS-absorb case #236 fenced).
 *
 * THE PROPERTY. The realimg/extern/nonres flips run a REAL image in-process but
 * REFUSE any image with its own PT_TLS (SS$_UNSUPPORTED -> fork), because setting
 * up the image's TLS the fresh-process way reprograms the thread pointer and
 * would CLOBBER DCL's own TLS. This proves the own-PT_TLS class runs in-process:
 * imgact_setup_own_tls gives the image its OWN TLS block and biases its .vms$tls
 * TLSDESC entries against DCL's ALREADY-PROGRAMMED thread pointer, so the image's
 * __thread data is its own while TP is never reprogrammed.
 *
 * WHAT THIS SUITE ASSERTS against a real /dev/vms:
 *   1. The image RUNS (banner) and returns SS$_NORMAL with its SYS$EXIT code --
 *      and the image reached that code ONLY after reading its own __thread init
 *      magic, bumping the resident TLS, and reading back a value it WROTE to its
 *      own TLS (§ testtls_inproc.c). A wrong image exit code means one of those
 *      own-TLS steps failed.
 *   2. The VMS PID and Linux PID are IDENTICAL before/after -- no fork.
 *   3. Back in Supervisor with the P0 extent cleared (rundown ran).
 *   4. OWN TLS DISTINCT: the resident __thread counter the image bumped moved by
 *      exactly TLS_DELTA and did NOT become the image's sentinel -- the image's
 *      own-TLS write landed in ITS OWN block, not the resident one.
 *   5. NO CLOBBER (the crux): DCL's thread pointer is UNCHANGED across the RUN,
 *      and a DCL __thread datum set before the RUN is UNCHANGED after -- the
 *      image's own-TLS setup did not perturb DCL's TLS.
 *   6. FLOWS BACK: a process-permanent event flag the image set is visible to DCL.
 *   7. NEGATIVE CONTROL (test-internal): with the resident producer NOT published
 *      the image's imports cannot bind and it is refused (SS$_UNSUPPORTED) -- the
 *      flip is not faked. (The registered facility_defects anchor for this suite
 *      is tlsdesc-offset-not-biased, on assertion 1.)
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no executive it exits
 * EXIT_SKIP (77), never a fake pass (INV-6): imgact_activate() fails SS$_NOSUCHDEV
 * and refuses to run the image at all.
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

/* Must match testtls_inproc.c's fixed work parameters. */
#define TLS_INIT_MAGIC   0x00ABCDEF01234567L
#define TLS_SENTINEL     0x0BADF00D5EED1234L
#define TLS_EFN          41
#define TLS_DELTA        9L
#define TLS_EXIT_CODE    0
#define DCL_TLS_MARK     0x1122334455667788L

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *testtls_path(void)
{
    const char *p = getenv("OVMX_TESTTLS");
    return p ? p : "/tests/TESTTLS.EXE";
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/* Read the process thread pointer without changing it, exactly as the activator
 * does (x86_64 %fs:0 self-pointer / aarch64 TPIDR_EL0). Used to prove DCL's TP is
 * unchanged across the in-process RUN. */
static unsigned long read_tp(void)
{
#if defined(__x86_64__)
    unsigned long tp;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(tp));
    return tp;
#elif defined(__aarch64__)
    unsigned long tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
#else
    return 0;
#endif
}

/* ---- The resident producer this suite stands in for LIBVMS$SHR ----------
 * Identical to test_syssvc_imgact_realimg's: three universals by vector index
 * (0 SYS$EXIT, 1 SETEF, 2 TLS-bump). g_resident_tls_counter is the "resident
 * DECC$SHR TLS" the image bumps through the UNCHANGED TP; g_dcl_tls_untouched is
 * a DCL __thread datum the image's own-TLS setup must NOT perturb. */

static __thread long g_resident_tls_counter;   /* the "resident DECC$SHR TLS"    */
static __thread long g_dcl_tls_untouched;       /* a DCL TLS datum, must survive  */

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
        if (strstr(b, "OVMX-TLSIMG-RAN"))
            *ran = 1;
    }
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_imgact_tls (own-PT_TLS image runs in-process, no clobber, vms-db2) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI executive-absent rig, not the product\n");
        printf("=== test_syssvc_imgact_tls: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    const char *img = testtls_path();
    const struct ovmx_sv_header *sv = build_producer_sv();
    struct vms_procinfo before, after;
    int ran = 0, image_rc = -1;
    uint32_t st;

    CHECK(vms_kif_setmode(PSL_C_SUPER) == SS$_NORMAL,
          "the activating context is in Supervisor mode (as DCL is)");

    vms_kif_clref(TLS_EFN);

    memset(&before, 0, sizeof before);
    CHECK((vms_kif_getjpi_self(&before) & 1) != 0,
          "read this process's VMS identity before activation");
    pid_t linux_before = getpid();
    unsigned long tp_before = read_tp();
    g_dcl_tls_untouched = DCL_TLS_MARK;   /* a DCL TLS datum the image must not touch */
    long tls_before = g_resident_tls_counter;

    /* --- NEGATIVE CONTROL first: producer NOT published -> imports cannot bind
     * -> the own-PT_TLS image is REFUSED the in-process path (would fork). --- */
    imgact_prodreg_reset();
    {
        int r0 = 0, rc0 = -1;
        uint32_t su = activate_capture(img, &r0, &rc0);
        CHECK(su == SS$_UNSUPPORTED,
              "with the resident producer UNPUBLISHED, the own-PT_TLS image's "
              "imports do not bind and imgact_activate refuses it "
              "(SS$_UNSUPPORTED -> caller forks) -- the flip is not faked");
        CHECK(!r0, "the refused image did not run in-process (no banner)");
    }

    /* --- THE FLIP: publish the resident producer, then activate in-process. --- */
    publish_resident(sv);
    st = activate_capture(img, &ran, &image_rc);

    CHECK(st == SS$_NORMAL,
          "the own-PT_TLS image ran in-process and returned SS$_NORMAL");
    CHECK(ran, "the image produced its banner (it actually ran)");
    /* The image reaches TLS_EXIT_CODE only after: argc==1, its own __thread datum
     * read back TLS_INIT_MAGIC (the activator copied its .tdata into the image's
     * OWN block via a correctly biased .vms$tls offset), the resident bump, and a
     * write+readback of its own TLS. A wrong/unbiased TP offset makes the init
     * read land in DCL's TLS -> the image's distinct bad-init exit code -> this
     * fails. This is the negctl anchor's target assertion. */
    /* negctl: tlsdesc-offset-not-biased */
    CHECK(image_rc == TLS_EXIT_CODE,
          "SYS$EXIT returned control to DCL with the image's success code -- the "
          "image read its OWN __thread init magic, wrote and re-read its own TLS, "
          "and bumped the resident TLS, all before exiting (own TLS correct)");

    memset(&after, 0, sizeof after);
    CHECK((vms_kif_getjpi_self(&after) & 1) != 0,
          "read this process's VMS identity after activation");
    CHECK(before.vms_pid == after.vms_pid,
          "the VMS PID is UNCHANGED across the RUN of an own-PT_TLS image -- no fork");
    CHECK(getpid() == linux_before,
          "the Linux PID is UNCHANGED across the RUN -- the image did not fork");
    CHECK(after.current_mode == PSL_C_SUPER,
          "the executive is back in Supervisor after own-PT_TLS rundown");
    CHECK(after.p0_base == 0 && after.p0_limit == 0,
          "the P0 extent was cleared at rundown (P0 deleted, image-less)");

    /* --- OWN TLS DISTINCT: the resident counter moved by exactly TLS_DELTA and
     * did NOT become the image's sentinel. If the image's own-TLS write had
     * landed on the resident block (bias broken toward the resident TLS), this
     * counter would read TLS_SENTINEL, not tls_before + TLS_DELTA. --- */
    CHECK(g_resident_tls_counter == tls_before + TLS_DELTA,
          "the RESIDENT __thread counter the image bumped moved by exactly "
          "TLS_DELTA -- the resident universal reached the RESIDENT TLS");
    CHECK(g_resident_tls_counter != TLS_SENTINEL,
          "the image's own-TLS write did NOT land on the resident TLS -- the "
          "image's TLS block is DISTINCT from the resident one");

    /* --- NO CLOBBER (the crux): DCL's thread pointer and a DCL __thread datum
     * survive the image's own-TLS setup untouched. --- */
    CHECK(read_tp() == tp_before,
          "DCL's thread pointer is UNCHANGED across the RUN -- the own-PT_TLS "
          "image was given its own block, NOT allowed to reprogram TP");
    CHECK(g_dcl_tls_untouched == DCL_TLS_MARK,
          "a DCL __thread datum set before the RUN is UNCHANGED after -- the "
          "image's own TLS did not clobber DCL's TLS (the no-clobber crux)");

    /* --- FLOWS BACK: the event flag the in-process image set is visible. --- */
    {
        uint32_t state = 0;
        vms_kif_readef(TLS_EFN, &state);
        CHECK((state & (1u << (TLS_EFN % 32))) != 0,
              "a process-permanent event flag SET by the in-process own-PT_TLS "
              "image is visible to DCL after rundown (executive flows-back)");
    }

    printf("=== test_syssvc_imgact_tls: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
