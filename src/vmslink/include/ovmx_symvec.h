/*
 * ovmx_symvec.h — OVMX symbol-vector resolution + GSMATCH (bead vms-8d5).
 *
 * The single source of truth for how a universal symbol is bound by VECTOR
 * POSITION and how GSMATCH is enforced. Shared, byte-for-byte, between:
 *   - IMGACT.EXE (SYS$IMGACT) at activation time, and
 *   - OVMXDUMP / the LINK.EXE test harness.
 *
 * Header-only + freestanding-safe: pure computation over the mapped structures
 * in ovmx_image.h, no libc calls. The caller locates the `.vms$sv` header (via
 * the ELF section header table) and passes it in; the resolver returns the
 * image-relative value of a universal symbol, to which the caller adds the load
 * bias to get the run-time address.
 *
 * GSMATCH semantics (public VMS, grounded — docs/design-link-native-toolchain.md
 * §5.4): major must match unless ALWAYS; EQUAL ⇒ minor exact; LEQUAL ⇒ the
 * image's minor >= the minor the consumer was linked against.
 */
#ifndef OVMX_SYMVEC_H
#define OVMX_SYMVEC_H

#include <stdint.h>
#include "ovmx_image.h"

/* Entries follow the header contiguously. */
static inline const struct ovmx_sv_entry *
ovmx_sv_entries(const struct ovmx_sv_header *h)
{
    return (const struct ovmx_sv_entry *)((const uint8_t *)h + sizeof(*h));
}

static inline const char *
ovmx_sv_names(const struct ovmx_sv_header *h)
{
    return (const char *)((const uint8_t *)h + h->names_off);
}

/*
 * Look up universal symbol by VECTOR INDEX. Returns the entry, or NULL if the
 * index is out of the vector's bounds (the VMS "bad symbol vector index"
 * failure) or the slot has been RETIRED (PRIVATE_* — not publicly bound).
 */
static inline const struct ovmx_sv_entry *
ovmx_sv_at(const struct ovmx_sv_header *h, uint32_t index)
{
    if (h->magic != OVMX_SV_MAGIC || index >= h->count)
        return 0;
    const struct ovmx_sv_entry *e = &ovmx_sv_entries(h)[index];
    if (e->kind == OVMX_SV_RETIRED)
        return 0;
    return e;
}

/*
 * GSMATCH check: is the producer image (whose header carries the match-control
 * and its actual major/minor) acceptable to a consumer that was linked against
 * (req_major, req_minor)?
 */
static inline int
ovmx_gsmatch_ok(const struct ovmx_sv_header *h,
                uint32_t req_major, uint32_t req_minor)
{
    switch (h->gsmatch_kind) {
    case OVMX_GSMATCH_ALWAYS:
        return 1;
    case OVMX_GSMATCH_EQUAL:
        return h->gsmatch_major == req_major && h->gsmatch_minor == req_minor;
    case OVMX_GSMATCH_LEQUAL:
        return h->gsmatch_major == req_major && h->gsmatch_minor >= req_minor;
    default:
        return 0;
    }
}

/*
 * Full resolve: GSMATCH-check the producer, then return the RUN-TIME address of
 * universal symbol `index` given the image's load bias. Returns 0 on GSMATCH
 * failure or bad index (caller emits %IMGACT-F-GSMATCH / -SYMVECMIS).
 */
static inline uint64_t
ovmx_sv_resolve(const struct ovmx_sv_header *h, uint32_t index,
                uint64_t load_bias, uint32_t req_major, uint32_t req_minor)
{
    if (!ovmx_gsmatch_ok(h, req_major, req_minor))
        return 0;
    const struct ovmx_sv_entry *e = ovmx_sv_at(h, index);
    if (!e)
        return 0;
    return load_bias + e->value;
}

#endif /* OVMX_SYMVEC_H */
