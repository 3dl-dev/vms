/*
 * imgact_prodreg.h - resident-producer registry + resident import binding for
 * in-process image activation (vms-db2, docs/design-in-process-activation.md
 * Part II §A.2.2 and §A.8 remainder item 1).
 *
 * THE PROBLEM THIS SOLVES. On OpenVMS a shareable image (LIBVMS$SHR, DECC$SHR,
 * ...) is mapped ONCE per process and every image the process activates binds
 * to that ONE resident copy through the symbol vector -- which is why a
 * $CRELNM/DEFINE by a RUN'd image lands in the SAME libvms state DCL uses and
 * flows back. OVMX's in-process activator (imgact_activate,
 * src/libvms/syssvc/sys_imgact.c) must do the same: bind an activated image's
 * .vms$imp imports to the ALREADY-RESIDENT producer, NOT a private copy (a
 * private copy is the LARP the authenticity invariants forbid -- the image
 * would look activated but share nothing).
 *
 * The freestanding IMGACT.EXE (src/imgact/imgact.c, the PT_INTERP loader) maps
 * producers into a fresh process and keeps their bases + symbol vectors in its
 * OWN private `static g_prods[]`, then discards that knowledge at hand-off:
 * nothing exported, no /proc parse, no dl_iterate_phdr -- an in-process
 * activator has no way to find a resident producer. This registry is that
 * missing mechanism: a process-permanent table of (soname, load base, .vms$sv)
 * for the producers resident in THIS process, published once (by IMGACT at the
 * process's own activation, in the eventual real-image flip) and consumed by
 * imgact_bind_imports_resident() to bind a later in-process image's imports to
 * them. It re-homes imgact.c's bind_imports() as library code whose producer
 * source is the registry (resident) instead of a fresh mmap.
 *
 * SCOPE (vms-db2, the flagged §A.8 remainder landed as a proven sub-step). This
 * is the import-binding-to-resident-shareable mechanism, proven in isolation
 * (tests/qemu/test_imgact_bind.c: a resident producer with shared
 * internal state, a consumer bound to it by vector index, the consumer's call
 * reaching the SAME resident instance -- genuine sharing, not a copy). What is
 * NOT here (still deferred, fork fallback intact -- real images do NOT activate
 * in-process): IMGACT publishing this registry at DCL startup; entering a real
 * LINK.EXE image through its SysV auxv/stack _start ABI + intercepting its
 * SYS$EXIT to return to DCL; sharing the resident DECC$SHR's musl TLS. See
 * sys_imgact.c and the design's §A.8 remainder.
 *
 * Rule 8: the .vms$sv/.vms$imp format is OVMX's own (ovmx_image.h); no VMS byte
 * layout is claimed. The resident-once, bind-by-vector-position SEMANTICS are
 * public VMS (shareable images installed /SHARED; Linker Utility Manual).
 */
#ifndef _IMGACT_PRODREG_H
#define _IMGACT_PRODREG_H

#include <stdint.h>
#include "ovmx_image.h"   /* struct ovmx_sv_header / ovmx_imp_header */

/* Upper bound on resident producers tracked (matches imgact.c's g_prods[32]:
 * the whole OVMX producer graph -- DECC$SHR, the 5 LIBVMS* shareables, RMS --
 * is well under this). */
#define IMGACT_MAX_RESIDENT 32

/*
 * Register an already-resident producer shareable so later in-process
 * activations can bind to it. `soname` is the producer image name a consumer's
 * .vms$imp records (e.g. "LIBVMS$SHR.EXE"); `base` is its run-time load bias
 * (0 if its symbol-vector values are already absolute); `sv` points at its
 * mapped .vms$sv header. Re-registering the same soname updates it in place.
 *
 * Returns SS$_NORMAL, SS$_BADPARAM (null/oversized args), or SS$_INSFMEM (table
 * full). This is a pure userspace record -- no /dev/vms -- but it is meaningful
 * ONLY inside a process that genuinely has the named producer mapped; a caller
 * that registers a producer it did not map would be faking residency, which the
 * isolation test guards against by mutating shared producer state.
 */
uint32_t imgact_register_producer(const char *soname, uint64_t base,
                                  const struct ovmx_sv_header *sv);

/*
 * Look up a registered resident producer by soname. On success returns 1 and
 * fills base and sv; returns 0 if not registered. Either out-pointer may be NULL.
 */
int imgact_find_producer(const char *soname, uint64_t *base,
                         const struct ovmx_sv_header **sv);

/*
 * One resident producer as IMGACT.EXE hands it across to LIBVMS$SHR at the
 * process's own activation (vms-db2, §A.8 remainder item 1 -- "publish the
 * registry at runtime"). This is the ABI of the IMGACT->LIBVMS$SHR publish call:
 * IMGACT keeps the producers it mapped in its OWN private g_prods[] (soname +
 * run-time base + mapped .vms$sv) and, having no way to reach into the resident
 * LIBVMS$SHR's data, marshals them into an array of these and calls
 * imgact_publish_producers() -- a LIBVMS$SHR universal it resolves from that
 * shareable's symbol vector BY NAME -- to hand the bases across. See
 * src/imgact/imgact.c publish_resident_producers() and
 * docs/design-in-process-activation.md Part II §A.8.
 */
struct imgact_prod_pub {
    const char                  *soname;  /* producer image name (.vms$imp records this) */
    uint64_t                     base;    /* its run-time load bias */
    const struct ovmx_sv_header *sv;      /* its mapped .vms$sv header */
};

/*
 * Publish a set of already-resident producers into the registry in ONE call --
 * the entry point IMGACT.EXE invokes at the process's own activation so that
 * later in-process activations (imgact_bind_imports_resident) can bind a real
 * image's .vms$imp imports to the SAME resident LIBVMS$SHR/DECC$SHR the process
 * already holds. Registering each in turn via imgact_register_producer(); stops
 * and returns the first non-success status. Returns SS$_NORMAL on success,
 * SS$_BADPARAM for a null list or negative count.
 *
 * This is the substantive, isolation-testable half of §A.8-remainder gap 1: the
 * registry-population routine LIVES here in LIBVMS$SHR (host-testable, no
 * /dev/vms, proven by tests/qemu/test_imgact_publish.c -- publish makes a
 * consumer's later bind reach the resident producer; skip it and the bind is
 * refused and the caller forks). The IMGACT-side glue that resolves this symbol
 * by name and marshals g_prods[] into the list is the thin, runtime-only
 * remainder (its end-to-end proof rides on the native-link runtime, vms-0b8).
 * Registering a producer this process did not actually map would be faking
 * residency -- the isolation test guards against that by mutating shared
 * producer state through the published base.
 */
uint32_t imgact_publish_producers(const struct imgact_prod_pub *prods, int n);

/*
 * Empty the registry. The honest default state is EMPTY (no producer is
 * resident until something maps and registers one); used by tests for a clean
 * per-case slate.
 */
void imgact_prodreg_reset(void);

/*
 * Bind a mapped consumer image's .vms$imp imports against the registered
 * resident producers. `base` is the consumer's load bias; `imp` its mapped
 * .vms$imp header. For each import record: find the named producer in the
 * registry, GSMATCH+index-resolve the universal (ovmx_sv_resolve), and write
 * the resolved RESIDENT address into the consumer's GOT cell at base+patch_off.
 *
 * Returns:
 *   SS$_NORMAL      every import bound to a resident producer
 *   SS$_BADPARAM    malformed .vms$imp (bad magic)
 *   SS$_UNSUPPORTED a named producer is not resident, or its GSMATCH/vector
 *                   index does not resolve -- the image cannot be activated
 *                   in-process (the caller keeps the fork fallback). Mapped to
 *                   SS$_UNSUPPORTED (not a new %IMGACT-F-NOSHRIMG/-SHRIDMISMAT
 *                   status) deliberately: inventing ungrounded SS$_ constants is
 *                   barred by the VMS-purity guardrail, and SS$_UNSUPPORTED is
 *                   exactly the "fall through to fork" signal dcl_activate_image
 *                   already routes on. The real-image flip (a follow-on) will
 *                   ground+cite the proper hard-error codes.
 */
uint32_t imgact_bind_imports_resident(uint64_t base,
                                      const struct ovmx_imp_header *imp);

#endif /* _IMGACT_PRODREG_H */
