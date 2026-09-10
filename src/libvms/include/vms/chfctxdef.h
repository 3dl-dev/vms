/*
 * vms/chfctxdef.h - port include-surface shim (vms-8e8c, CHF rung-5).
 *
 * The alpha-dec-vms GCC port's own libgcc EH source
 * libgcc/config/alpha/vms-unwind.h `#include <vms/chfctxdef.h>` unchanged and
 * dereferences a `CHFCTX *` - the OpenVMS Condition Handling Facility (CHF)
 * context block the executive builds when it dispatches a condition, and which
 * a signal-frame unwinder reads to recover the machine state at the point the
 * condition was raised. See docs/design-gcc-port-surface-gaps-register.md
 * (R5 residual, folded here from vms-714c) and docs/design-chf-condition-
 * handling.md (rung-5, libgcc EH).
 *
 * The three fields vms-unwind.h reads:
 *   chfctx$q_sigarglst - address of the CHF signal-args array (the condition
 *                        value + FAO args + the condition PC), cast to
 *                        CHF$SIGNAL_ARRAY *.
 *   chfctx$q_mcharglst - address of the CHF mechanism-args array (the saved
 *                        callee-register block + establisher frame), cast to
 *                        CHF$MECH_ARRAY *.
 *   chfctx$q_expt_fp   - the frame pointer (R29) live at the condition point;
 *                        vms-unwind.h takes its ADDRESS as the save-location of
 *                        the caller's FP.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): there is NO internal CHFCTX header in OVMX
 * today (grep chfctx = this file only) and NO VSI/HPE source was consulted.
 * This layout is reconstructed from the PUBLIC "HP OpenVMS Calling Standard"
 * (condition-handling chapter: the signal/mechanism argument arrays and the
 * per-condition context the dispatcher threads them through) and from what
 * vms-unwind.h itself dereferences. Exactly as src/libvms/include/pdscdef.h
 * documents for the PDSC, the field-by-field BYTE OFFSETS here are NOT yet
 * oracle-pinned: this is COMPILE-SURFACE (the port source resolves its
 * `<vms/chfctxdef.h>` include and typechecks), and the real dispatcher wiring
 * that populates a genuine CHFCTX at these offsets is the deferred runtime
 * child (gated on the Alpha rail + vms-6fe/vms-e16). The leading link fields
 * and headroom below reserve space for that block's full width without
 * asserting the interior offsets are byte-authentic yet.
 *
 * DELIBERATELY SEPARATE from OVMX's own condition-handling representation
 * (src/libvms/rtl/lib_signal.c, owned by the vms-1fa/vms-2e72 lane): this shim
 * exists ONLY so the port's EH source compiles UNCHANGED against our cross cc1;
 * it is not wired into OVMX's own SYS$SETEXV dispatch path.
 */
#ifndef __VMS_CHFCTXDEF_H
#define __VMS_CHFCTXDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CHFCTX - Condition Handling Facility context block (Alpha, quadword fields).
 *
 * Field order below places the two argument-list pointers and the exception
 * frame pointer at fixed quadword slots after a pair of self-relative queue
 * links (the CHF context is threaded on the exec's per-thread condition list),
 * with trailing headroom so the block is wide enough for the full documented
 * context without this shim over-committing to interior offsets it cannot yet
 * validate against a running dispatcher.
 */
struct _chfctx {
    uint64_t chfctx$q_flink;        /* forward queue link                    */
    uint64_t chfctx$q_blink;        /* backward queue link                   */
    uint64_t chfctx$q_sigarglst;    /* -> CHF signal-args array              */
    uint64_t chfctx$q_mcharglst;    /* -> CHF mechanism-args array           */
    uint64_t chfctx$q_expt_fp;      /* frame pointer (R29) at condition point */
    uint64_t chfctx$q_expt_pc;      /* PC at condition point                 */
    uint64_t chfctx$q_expt_ps;      /* PS at condition point                 */
    uint64_t chfctx$q_headroom[9];  /* reserved: full-context width headroom */
};
typedef struct _chfctx CHFCTX;

#ifdef __cplusplus
}
#endif

#endif /* __VMS_CHFCTXDEF_H */
