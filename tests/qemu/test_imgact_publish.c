/*
 * test_imgact_publish.c - PUBLISHING the resident-producer registry at
 * runtime, proven in ISOLATION (vms-db2, docs/design-in-process-activation.md
 * Part II §A.8 remainder gap 1: "publish the registry at runtime").
 *
 * THE PROBLEM. imgact_bind_imports_resident() (proven by test_imgact_bind)
 * can bind a RUN'd image's .vms$imp imports to an ALREADY-RESIDENT producer --
 * but ONLY if the registry knows which producers are resident. At runtime the
 * registry was EMPTY: IMGACT.EXE maps LIBVMS$SHR/DECC$SHR into the process, keeps
 * their bases in its OWN private g_prods[], and DISCARDED them at hand-off. So a
 * real image's imports had nothing to bind against and the caller always forked.
 * imgact_publish_producers() is the missing hand-across: IMGACT resolves it from
 * the resident LIBVMS$SHR symbol vector by name and calls it once with the
 * producers it mapped, so imgact_activate() (also in LIBVMS$SHR) can then bind an
 * activated image to the SAME resident copies DCL uses -- the prerequisite for a
 * $CRELNM/DEFINE by a RUN'd image to flow back.
 *
 * WHY THIS TEST NEEDS NO /dev/vms, AND WHY THAT IS HONEST. Populating the
 * registry from a producer list is pure userspace bookkeeping over the OVMX
 * symbol-vector format -- it never touches the executive. The P0-window map,
 * Supervisor->User transition and image rundown that WRAP a full in-process
 * activation ARE executive facilities (test_syssvc_imgact_inproc, real /dev/vms).
 * This suite isolates the NEW piece -- runtime registry publication -- and proves
 * that publishing is what makes a consumer's later bind reach the resident
 * producer, so it runs and asserts in EVERY environment, not just under QEMU.
 * (The IMGACT-side glue that marshals g_prods[] and resolves this symbol by name
 * is the thin, runtime-only remainder whose end-to-end proof rides on the
 * native-link runtime -- vms-0b8; no class is flipped here, fork fallback stays.)
 *
 * NAMING. Landed directly as test_imgact_publish (not test_syssvc_imgact_
 * publish, the name the original PR proposed): the same main-red classification
 * bug that renamed test_syssvc_imgact_bind -> test_imgact_bind (see that file's
 * header) applies here for the identical reason -- this suite never touches
 * /dev/vms and legitimately returns 0 whether or not the executive is present,
 * so it must stay outside ci.yml's test_syssvc_*-must-honest-skip-77 contract.
 *
 * THE ANTI-LARP CONSTRUCTION. A "resident producer" holds shared internal state
 * (a counter in THIS process). PUBLISH records it; a consumer imports its
 * bump-the-counter universal and calls it through the bound GOT cell. The
 * consumer's call must reach the SAME counter the test itself sees: if publish
 * had recorded nothing (the registry stayed empty), the consumer's bind would be
 * refused (SS$_UNSUPPORTED) and the shared-counter assertion would go red. The
 * negctl publish-does-not-populate-registry makes exactly that mutation.
 *
 * Rule 8: the .vms$sv/.vms$imp format is OVMX's own (ovmx_image.h); no VMS byte
 * layout is claimed. The resident-once, published-once, bind-by-vector-position
 * SEMANTICS are public VMS (Linker Utility Manual, shareable images /SHARED).
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

static long g_prod_counter;

static long prod_bump(void)
{
    g_prod_counter++;
    return g_prod_counter;
}

/* Build the producer's .vms$sv: header + one PROCEDURE entry whose value is
 * prod_bump's ABSOLUTE address (so the producer registers with load base 0 --
 * its symbol-vector values are already run-time addresses). */
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
    h->gsmatch_kind = OVMX_GSMATCH_ALWAYS;
    h->gsmatch_major = 1;
    h->gsmatch_minor = 0;
    h->names_off = (uint32_t)((uint8_t *)names - buf);
    h->names_size = 10;
    e[0].value = (uint64_t)(uintptr_t)&prod_bump;
    e[0].kind = OVMX_SV_PROCEDURE;
    e[0].name_off = 0;
    memcpy(names, "prod_bump", 10);
    return h;
}

/* ---- The CONSUMER IMAGE: a GOT cell + a .vms$imp naming (producer, index 0). */

static long stub_unbound(void) { return -777; }
typedef long (*bumpfn)(void);

static const struct ovmx_imp_header *build_consumer_imp(const char *soname,
                                                        uint32_t sv_index)
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
    e[0].req_major = 1;
    e[0].req_minor = 0;
    memcpy(names, soname, n);
    return h;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_imgact_publish (publish resident registry at runtime, vms-db2) ===\n");

    const struct ovmx_sv_header *sv = build_producer_sv();

    /* ---- 1. Before publish the registry is empty (honest default) ----- */
    imgact_prodreg_reset();
    CHECK(imgact_find_producer("RESIDENTPROD.EXE", NULL, NULL) == 0,
          "registry is empty before publish -- the runtime state IMGACT left it in");

    /* ---- 2. THE MECHANISM: publish a producer list in one call -------- */
    {
        struct imgact_prod_pub list[1];
        list[0].soname = "RESIDENTPROD.EXE";
        list[0].base   = 0;               /* sv values already absolute */
        list[0].sv     = sv;

        CHECK(imgact_publish_producers(list, 1) == SS$_NORMAL,
              "imgact_publish_producers recorded the resident producer list");

        uint64_t b = 1; const struct ovmx_sv_header *got = NULL;
        CHECK(imgact_find_producer("RESIDENTPROD.EXE", &b, &got) == 1 &&
              b == 0 && got == sv,
              "publish made the resident producer findable by soname (base + .vms$sv)");
    }

    /* ---- 3. THE PROPERTY: publish is what lets a consumer bind to the
     * resident producer and reach the SAME instance ---------------------- */
    {
        volatile bumpfn cell = stub_unbound;
        const struct ovmx_imp_header *imp =
            build_consumer_imp("RESIDENTPROD.EXE", 0);

        g_prod_counter = 0;
        uint32_t st = imgact_bind_imports_resident((uint64_t)(uintptr_t)&cell, imp);
        CHECK(st == SS$_NORMAL,
              "with the producer published, the consumer's import binds to it "
              "(a registry left empty by a skipped publish would refuse this)");

        long seen = cell();
        /* negctl: publish-does-not-populate-registry */
        CHECK(seen == 1,
              "the consumer's imported call reached the PUBLISHED resident producer "
              "-- it returned the resident counter value 1, not the unbound stub");

        long after = prod_bump();
        /* negctl-knockon: publish-does-not-populate-registry */
        CHECK(after == 2,
              "the test sees the consumer's increment to the SAME resident counter "
              "publish made reachable (genuine sharing, not a private copy)");
    }

    /* ---- 4. Publishing several producers at once, all findable -------- */
    {
        imgact_prodreg_reset();
        struct imgact_prod_pub list[3];
        list[0].soname = "DECC$SHR.EXE";        list[0].base = 0x1000; list[0].sv = sv;
        list[1].soname = "LIBVMS$SHR.EXE";      list[1].base = 0x2000; list[1].sv = sv;
        list[2].soname = "LIBVMSPROCESS$SHR.EXE"; list[2].base = 0x3000; list[2].sv = sv;
        CHECK(imgact_publish_producers(list, 3) == SS$_NORMAL,
              "publishing the whole producer graph in one call succeeds");
        uint64_t b = 0;
        CHECK(imgact_find_producer("LIBVMS$SHR.EXE", &b, NULL) == 1 && b == 0x2000,
              "each published producer keeps its own distinct resident base");
        CHECK(imgact_find_producer("DECC$SHR.EXE", &b, NULL) == 1 && b == 0x1000,
              "DECC$SHR is published with its own base (not overwritten by a sibling)");
    }

    /* ---- 5. Malformed publish arguments are rejected ------------------ */
    CHECK(imgact_publish_producers(NULL, 1) == SS$_BADPARAM,
          "a NULL producer list is rejected (SS$_BADPARAM)");
    CHECK(imgact_publish_producers((const struct imgact_prod_pub *)&pass, -1) == SS$_BADPARAM,
          "a negative producer count is rejected (SS$_BADPARAM)");

    printf("=== test_imgact_publish: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
