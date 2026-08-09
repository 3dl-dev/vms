/*
 * test_imgact_bind.c - the .vms$imp import-binding-to-RESIDENT-shareable
 * mechanism, proven in ISOLATION (vms-db2, docs/design-in-process-activation.md
 * Part II §A.2.2 + §A.8 remainder item 1).
 *
 * THE PROPERTY. On OpenVMS a shareable image (LIBVMS$SHR, DECC$SHR) is mapped
 * ONCE per process; every image the process activates binds to that ONE
 * resident copy through the symbol vector, which is why a $CRELNM/DEFINE by a
 * RUN'd image lands in the SAME libvms state DCL uses and flows back. OVMX's
 * in-process activator must bind an activated image's .vms$imp imports to the
 * ALREADY-RESIDENT producer, NOT a private copy -- a private copy is the LARP
 * the authenticity invariants forbid (the image would look activated but share
 * nothing). imgact_bind_imports_resident() (src/libvms/syssvc/imgact_prodreg.c)
 * is that binding; imgact_register_producer() is how the resident producer is
 * made findable.
 *
 * WHY THIS TEST NEEDS NO /dev/vms, AND WHY THAT IS HONEST. Binding an import to
 * a resident producer is pure userspace address arithmetic over the OVMX
 * symbol-vector format -- it never touches the executive. The P0-window map,
 * Supervisor->User transition and image rundown that WRAP a full in-process
 * activation ARE executive facilities (proven separately, against a real
 * /dev/vms, by test_syssvc_imgact_inproc). This suite isolates the NEW piece --
 * the resident-shareable binding -- and proves genuine sharing without a fork
 * and without the executive, so it runs and asserts in EVERY environment
 * (including the CI host build), not just under QEMU.
 *
 * NAMING (main-red fix, was test_syssvc_imgact_bind through #225). The
 * kernel-executive-negative-control CI job (.github/workflows/ci.yml) derives
 * its "must be a real executive suite" policed set from the test_syssvc_*
 * glob and requires every member to return the honest-skip code 77 when
 * /dev/vms is absent -- because a test_syssvc_* suite that returns 0 with no
 * executive is the LARP shape that job exists to catch. This suite was
 * originally given the test_syssvc_ prefix (it does drive a public sys$-
 * adjacent mechanism) but, per the paragraph above, never touches /dev/vms
 * and legitimately returns 0 whether or not the executive is present -- so
 * under that job it was misclassified and tripped as a false LARP positive.
 * test_imgact_bind is the userspace name; it is built, run, and gated
 * (tests/qemu/CMakeLists.txt add_test()) like any host test, and it still
 * runs inside the QEMU harness (tests/qemu/init.sh, tests/qemu/Dockerfile)
 * so run_facility_negctl.sh's consumer-import-not-bound-to-resident proof
 * keeps working -- it is simply excluded from ci.yml's test_syssvc_*-must-
 * skip contract, which only ever applied to suites that need the executive.
 *
 * THE ANTI-LARP CONSTRUCTION. A "resident producer" holds shared internal state
 * (a counter in THIS process). A "consumer image" imports the producer's
 * bump-the-counter universal by (soname, vector index) through a real .vms$imp
 * table and calls it through the GOT cell the binding patches. The consumer's
 * call must reach the SAME counter the test itself sees: if binding had pointed
 * the consumer at a private copy (or left it at its pre-bind stub), the counter
 * the consumer advanced and the counter the test reads would diverge. The
 * negctl consumer-import-not-bound-to-resident skips the one patch store and
 * this divergence is exactly what goes red.
 *
 * Rule 8: the .vms$sv/.vms$imp format is OVMX's own (ovmx_image.h); no VMS byte
 * layout is claimed. The resident-once, bind-by-vector-position SEMANTICS are
 * public VMS (Linker Utility Manual, shareable images installed /SHARED).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ssdef.h"
#include "ovmx_image.h"
#include "imgact_prodreg.h"

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* ---- The RESIDENT PRODUCER: shared internal state + one exported universal. */

/* The producer's process-permanent state. A genuine shared producer owns state
 * that BOTH the process (DCL) and any image it activates mutate through the one
 * resident instance -- the stand-in for libvms's process-logical-name table. */
static long g_prod_counter;

/* Vector index 0: increment the shared counter, report its new value. This is
 * what the consumer imports and what the test also calls directly -- the same
 * function, the same counter, one instance. */
static long prod_bump(void)
{
    g_prod_counter++;
    return g_prod_counter;
}

/*
 * Build the producer's .vms$sv symbol vector in a static buffer: header + one
 * PROCEDURE entry whose value is prod_bump's ABSOLUTE address (so the producer
 * registers with load base 0 -- its symbol-vector values are already run-time
 * addresses, exactly as a producer whose relatives the loader has applied).
 */
static const struct ovmx_sv_header *build_producer_sv(void)
{
    static uint8_t buf[sizeof(struct ovmx_sv_header) +
                       sizeof(struct ovmx_sv_entry) + 16];
    struct ovmx_sv_header *h = (struct ovmx_sv_header *)buf;
    struct ovmx_sv_entry *e =
        (struct ovmx_sv_entry *)(buf + sizeof(struct ovmx_sv_header));
    char *names = (char *)(e + 1);

    memset(buf, 0, sizeof buf);
    h->magic = OVMX_SV_MAGIC;
    h->count = 1;
    h->gsmatch_kind = OVMX_GSMATCH_ALWAYS;   /* any version accepted */
    h->gsmatch_major = 1;
    h->gsmatch_minor = 0;
    h->names_off = (uint32_t)((uint8_t *)names - buf);
    h->names_size = 10;
    e[0].value = (uint64_t)(uintptr_t)&prod_bump;  /* absolute; load base 0 */
    e[0].kind = OVMX_SV_PROCEDURE;
    e[0].name_off = 0;
    memcpy(names, "prod_bump", 10);
    return h;
}

/* ---- The CONSUMER IMAGE: a GOT cell + a .vms$imp naming (producer, index 0). */

/* The consumer's GOT cell, pre-initialised to a SAFE stub that touches nothing
 * (so the negctl's skipped-bind leaves a callable, non-crashing pointer whose
 * distinctive return cleanly fails the sharing assertion instead of segv'ing). */
static long stub_unbound(void) { return -777; }

typedef long (*bumpfn)(void);

/*
 * Build a consumer .vms$imp importing the producer's index-0 universal into a
 * caller-provided GOT cell. patch_off is the cell's offset from `base`; here
 * base is the cell's own address and patch_off 0 (a one-cell "image"), which
 * exercises the exact base+patch_off store the loader does for a mapped image.
 */
static const struct ovmx_imp_header *build_consumer_imp(const char *soname,
                                                        uint32_t sv_index,
                                                        uint32_t req_major,
                                                        uint32_t req_minor)
{
    static uint8_t buf[sizeof(struct ovmx_imp_header) +
                       sizeof(struct ovmx_imp_entry) + 64];
    struct ovmx_imp_header *h = (struct ovmx_imp_header *)buf;
    struct ovmx_imp_entry *e =
        (struct ovmx_imp_entry *)(buf + sizeof(struct ovmx_imp_header));
    char *names = (char *)(e + 1);
    size_t n = strlen(soname) + 1;

    memset(buf, 0, sizeof buf);
    h->magic = OVMX_IMP_MAGIC;
    h->count = 1;
    h->names_off = (uint32_t)((uint8_t *)names - buf);
    h->names_size = (uint32_t)n;
    e[0].producer_off = 0;
    e[0].sv_index = sv_index;
    e[0].patch_off = 0;               /* GOT cell at base+0 */
    e[0].req_major = req_major;
    e[0].req_minor = req_minor;
    memcpy(names, soname, n);
    return h;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_imgact_bind (.vms$imp -> RESIDENT shareable, vms-db2) ===\n");

    const struct ovmx_sv_header *sv = build_producer_sv();

    /* ---- 1. Registry round-trips ------------------------------------- */
    imgact_prodreg_reset();
    CHECK(imgact_find_producer("RESIDENTPROD.EXE", NULL, NULL) == 0,
          "registry is empty by default -- no producer is resident until mapped");
    CHECK(imgact_register_producer("RESIDENTPROD.EXE", 0, sv) == SS$_NORMAL,
          "registered the resident producer (soname, base, .vms$sv)");
    {
        uint64_t b = 1; const struct ovmx_sv_header *got = NULL;
        CHECK(imgact_find_producer("RESIDENTPROD.EXE", &b, &got) == 1 &&
              b == 0 && got == sv,
              "the resident producer is found again by soname (base + .vms$sv)");
    }

    /* ---- 2. THE PROPERTY: bind a consumer's import to the RESIDENT
     * producer and prove its call reaches the SAME instance --------------- */
    {
        volatile bumpfn cell = stub_unbound;   /* GOT cell, pre-bind stub  */
        const struct ovmx_imp_header *imp =
            build_consumer_imp("RESIDENTPROD.EXE", 0, 1, 0);

        g_prod_counter = 0;
        uint32_t st = imgact_bind_imports_resident((uint64_t)(uintptr_t)&cell, imp);
        CHECK(st == SS$_NORMAL,
              "imgact_bind_imports_resident bound the import to the resident producer");
        CHECK((void *)cell == (void *)&prod_bump,
              "the consumer's GOT cell now holds the RESIDENT producer's address "
              "(base + resolved vector value), not its pre-bind stub");

        /* The consumer calls its imported universal through the bound cell. */
        long seen = cell();
        /* negctl: consumer-import-not-bound-to-resident */
        CHECK(seen == 1,
              "the consumer's imported call reached the RESIDENT producer -- it "
              "returned the resident counter value 1, not the unbound stub");

        /* The test (as DCL) calls the SAME producer directly. If the consumer
         * had bound to a private copy, its increment would not be visible here
         * and this direct call would still see 1. */
        long after = prod_bump();
        /* negctl: consumer-import-not-bound-to-resident */
        CHECK(after == 2,
              "the test sees the consumer's increment to the SAME resident "
              "counter (genuine sharing, not a private copy)");
    }

    /* ---- 3. GSMATCH is enforced on the resident bind ------------------- */
    {
        volatile bumpfn cell = stub_unbound;
        /* Producer publishes an EQUAL-match vector at (major 1, minor 0). */
        static uint8_t svbuf[sizeof(struct ovmx_sv_header) +
                             sizeof(struct ovmx_sv_entry) + 16];
        struct ovmx_sv_header *he = (struct ovmx_sv_header *)svbuf;
        struct ovmx_sv_entry *ee =
            (struct ovmx_sv_entry *)(svbuf + sizeof(struct ovmx_sv_header));
        memset(svbuf, 0, sizeof svbuf);
        he->magic = OVMX_SV_MAGIC; he->count = 1;
        he->gsmatch_kind = OVMX_GSMATCH_EQUAL;
        he->gsmatch_major = 1; he->gsmatch_minor = 0;
        he->names_off = (uint32_t)((uint8_t *)(ee + 1) - svbuf);
        ee[0].value = (uint64_t)(uintptr_t)&prod_bump; ee[0].kind = OVMX_SV_PROCEDURE;
        imgact_register_producer("GSMATCHED.EXE", 0, he);

        /* Consumer linked against (major 1, minor 5) -- EQUAL requires exact
         * minor, so this must be refused (image cannot activate in-process). */
        const struct ovmx_imp_header *imp =
            build_consumer_imp("GSMATCHED.EXE", 0, 1, 5);
        uint32_t st = imgact_bind_imports_resident((uint64_t)(uintptr_t)&cell, imp);
        CHECK(st == SS$_UNSUPPORTED,
              "a GSMATCH minor mismatch is refused (SS$_UNSUPPORTED) -- the image "
              "cannot bind to the resident producer, so the caller keeps forking");
        CHECK((void *)cell == (void *)stub_unbound,
              "the GOT cell is untouched on a GSMATCH failure (no partial bind)");
    }

    /* ---- 4. A named producer that is NOT resident is refused ----------- */
    {
        volatile bumpfn cell = stub_unbound;
        const struct ovmx_imp_header *imp =
            build_consumer_imp("NOTRESIDENT.EXE", 0, 0, 0);
        uint32_t st = imgact_bind_imports_resident((uint64_t)(uintptr_t)&cell, imp);
        CHECK(st == SS$_UNSUPPORTED,
              "an import naming a producer that is NOT resident is refused "
              "(SS$_UNSUPPORTED) -- never silently bound to a copy or to garbage");
    }

    /* ---- 5. A malformed .vms$imp is rejected -------------------------- */
    {
        struct ovmx_imp_header bad;
        memset(&bad, 0, sizeof bad);
        bad.magic = 0xdeadbeef;
        CHECK(imgact_bind_imports_resident(0, &bad) == SS$_BADPARAM,
              "a .vms$imp with a bad magic is rejected (SS$_BADPARAM)");
    }

    printf("=== test_imgact_bind: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
