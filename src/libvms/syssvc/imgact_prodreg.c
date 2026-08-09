/*
 * imgact_prodreg.c - resident-producer registry + resident import binding
 * (vms-db2). See src/libvms/include/imgact_prodreg.h for the model, the
 * problem it solves, and the scope (a proven sub-step of the §A.8 remainder:
 * the import-binding-to-resident-shareable mechanism; the real-image flip and
 * IMGACT-side registry population are deferred, fork fallback intact).
 *
 * This TU is part of LIBVMS$SHR (src/libvms/CMakeLists.txt AND
 * src/vmslink/mk_libvms_shr.sh's LIST -- the two sources of truth kept in
 * sync). Every symbol it references is a DECC$SHR universal (strlen/strncmp/
 * strncpy/memcmp) or a header-only inline (ovmx_symvec.h), so it links into the
 * VMS-native DCL.EXE (LINK.EXE --executable --use {DECC$SHR,...}) unchanged --
 * the #222 build-context trap (a non-universal symbol breaking the DCL link).
 * It touches no /dev/vms: the registry and the GOT-cell patching are pure
 * userspace address arithmetic over the OVMX symbol-vector format (ovmx_image.h,
 * Rule 8 -- OVMX's own format, no VMS byte layout claimed).
 */
#include <stdint.h>
#include <string.h>

#include "ssdef.h"
#include "ovmx_image.h"
#include "ovmx_symvec.h"
#include "imgact_prodreg.h"

/* soname is bounded so the registry is a flat, allocation-free table (VMS
 * activation is single-threaded per process, so no locking -- exactly as
 * imgact.c's g_prods[] is a plain file-scope array). */
#define IMGACT_SONAME_MAX 128

struct imgact_resident {
    char                         soname[IMGACT_SONAME_MAX];
    uint64_t                     base;
    const struct ovmx_sv_header *sv;
};

static struct imgact_resident g_resident[IMGACT_MAX_RESIDENT];
static int                    g_nresident;   /* honest default: 0 resident */

uint32_t imgact_register_producer(const char *soname, uint64_t base,
                                  const struct ovmx_sv_header *sv)
{
    if (!soname || !sv || soname[0] == '\0')
        return SS$_BADPARAM;
    if (strlen(soname) >= IMGACT_SONAME_MAX)
        return SS$_BADPARAM;

    /* Re-register in place if the soname is already known (a producer mapped
     * once, re-published). */
    for (int i = 0; i < g_nresident; i++) {
        if (strncmp(g_resident[i].soname, soname, IMGACT_SONAME_MAX) == 0) {
            g_resident[i].base = base;
            g_resident[i].sv = sv;
            return SS$_NORMAL;
        }
    }
    if (g_nresident >= IMGACT_MAX_RESIDENT)
        return SS$_INSFMEM;

    struct imgact_resident *r = &g_resident[g_nresident];
    strncpy(r->soname, soname, IMGACT_SONAME_MAX - 1);
    r->soname[IMGACT_SONAME_MAX - 1] = '\0';
    r->base = base;
    r->sv = sv;
    g_nresident++;
    return SS$_NORMAL;
}

int imgact_find_producer(const char *soname, uint64_t *base,
                         const struct ovmx_sv_header **sv)
{
    if (!soname)
        return 0;
    for (int i = 0; i < g_nresident; i++) {
        if (strncmp(g_resident[i].soname, soname, IMGACT_SONAME_MAX) == 0) {
            if (base) *base = g_resident[i].base;
            if (sv)   *sv = g_resident[i].sv;
            return 1;
        }
    }
    return 0;
}

void imgact_prodreg_reset(void)
{
    g_nresident = 0;
}

uint32_t imgact_publish_producers(const struct imgact_prod_pub *prods, int n)
{
    if (!prods || n < 0)
        return SS$_BADPARAM;

    /* Register each resident producer in turn. This loop IS the "publish the
     * registry at runtime" mechanism (§A.8 gap 1): before it runs the registry
     * is empty and a real image's imports cannot resolve in-process (they fork);
     * after it runs the process's resident LIBVMS$SHR/DECC$SHR are findable so
     * imgact_bind_imports_resident can bind an activated image to them. A
     * failure to record any one producer is surfaced, not swallowed -- a
     * half-published registry would bind some imports to the resident copy and
     * leave others unresolved, exactly the partial-share the invariants forbid. */
    for (int i = 0; i < n; i++) {
        uint32_t st = imgact_register_producer(prods[i].soname, prods[i].base, prods[i].sv);
        if (!(st & 1))
            return st;
    }
    return SS$_NORMAL;
}

uint32_t imgact_bind_imports_mapped(uint64_t base,
                                    const struct ovmx_imp_header *imp,
                                    imgact_mapper_fn map)
{
    if (!imp || imp->magic != OVMX_IMP_MAGIC)
        return SS$_BADPARAM;

    /* Entries follow the header contiguously; the producer-soname blob is at
     * imp->names_off. Identical layout walk to imgact.c's bind_imports(), but
     * the producer source is the resident registry, not a fresh mmap. */
    const struct ovmx_imp_entry *ie =
        (const struct ovmx_imp_entry *)((const uint8_t *)imp + sizeof(*imp));
    const char *names = (const char *)imp + imp->names_off;

    for (uint32_t k = 0; k < imp->count; k++) {
        const char *soname = names + ie[k].producer_off;

        uint64_t prod_base = 0;
        const struct ovmx_sv_header *sv = 0;
        if (!imgact_find_producer(soname, &prod_base, &sv)) {
            /* Not yet resident. With a mapper, ask it to map+register the
             * producer in-process (the NON-RESIDENT producer sub-step); then
             * look again. Without a mapper (resident-only bind), or if the
             * mapper refuses (a producer we cannot safely map in-process --
             * PT_TLS / C-RTL bootstrap, the deferred remainder), the import
             * stays unbound and the caller forks. */
            if (!map || !(map(soname) & 1) ||
                !imgact_find_producer(soname, &prod_base, &sv))
                return SS$_UNSUPPORTED;   /* not resident/mappable -> forks */
        }

        uint64_t addr = ovmx_sv_resolve(sv, ie[k].sv_index, prod_base,
                                        ie[k].req_major, ie[k].req_minor);
        if (!addr)
            return SS$_UNSUPPORTED;   /* GSMATCH / bad vector index */

        /* Write the RESIDENT producer's address into the consumer's GOT cell.
         * patch_off is image-relative (the LINK.EXE contract, ovmx_image.h);
         * base+patch_off lands inside the consumer's mapped image. This one
         * store is the whole anti-LARP crux: it points the consumer at the
         * ALREADY-RESIDENT producer, so its call reaches the same instance DCL
         * uses -- skip it and the image shares nothing (negctl anchor
         * consumer-import-not-bound-to-resident). */
        uint64_t *cell = (uint64_t *)(uintptr_t)(base + ie[k].patch_off);
        *cell = addr;
    }
    return SS$_NORMAL;
}

uint32_t imgact_bind_imports_resident(uint64_t base,
                                      const struct ovmx_imp_header *imp)
{
    /* Resident-only bind: no mapper, so a not-resident producer forks. The
     * isolation test (test_imgact_bind) exercises exactly this path. */
    return imgact_bind_imports_mapped(base, imp, 0);
}
