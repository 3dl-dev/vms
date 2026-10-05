/*
 * PDSCDEF.H - Alpha Calling Standard procedure-descriptor (PDSC) and
 * register-save-area (RSA) definitions for the invocation-context walk
 * (vms-1fa, CHF rung-3).
 *
 * The OpenVMS Alpha invocation-context primitives (LIB$GET_INVO_CONTEXT /
 * LIB$GET_PREV_INVO_CONTEXT / LIB$GET_INVO_HANDLE, src/libvms/rtl/lib_invo.c)
 * walk the GENUINE Alpha call chain rather than the lib$establish side chain.
 * Walking one frame back means:
 *
 *   1. find the PROCEDURE DESCRIPTOR (PDSC) for the frame's current PC,
 *   2. read its KIND - a REGISTER-frame procedure keeps its caller's return
 *      address in a register and shares the caller's stack; a STACK-frame
 *      procedure allocates a frame and, if it is not a leaf, saves the return
 *      address and any preserved registers into a REGISTER SAVE AREA (RSA) at
 *      a fixed offset from the frame base,
 *   3. restore the caller's registers (PC, FP=R29, SP=R30, and any preserved
 *      register) from the RSA per the descriptor's register-save mask.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). These layouts are reconstructed from the
 * PUBLIC "Alpha/OpenVMS Calling Standard" (DEC/Compaq/HP, the "Procedure
 * Descriptor", "Register Save Area" and "Invocation Context" chapters) and
 * from what the OVMX-Alpha (alpha-dec-vms GCC backend) toolchain itself emits
 * for a procedure descriptor - never from VSI/HPE source. The field-by-field
 * byte offsets are validated against the toolchain's emitted descriptors on
 * the real qemu-alpha runtime by the deferred child (vms-cc8 bracket / the
 * Alpha-rig oracle); on the host the walk engine is proven against constructed
 * descriptors of this shape (tests/libvms/test_invo_context.c).
 *
 * Reference: Alpha/OpenVMS Calling Standard, "Procedure Descriptors",
 *            "Register Save Areas", "Invocation Context Block".
 */

#ifndef __PDSCDEF_H
#define __PDSCDEF_H

#include <stdint.h>
#include "libicb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Alpha integer register-number conventions (Calling Standard).
 * ================================================================ */

#define ALPHA_REG_RA        26      /* return-address register (default) */
#define ALPHA_REG_FP        29      /* frame pointer */
#define ALPHA_REG_SP        30      /* stack pointer */
#define ALPHA_REG_COUNT     32

/* ================================================================
 * Procedure kinds (low nibble of pdsc$w_flags -> pdsc$v_kind).
 *
 * PDSC$K_KIND_FP_STACK    : a stack-frame procedure. Establishes a frame;
 *                           saves its return address + preserved registers in
 *                           an RSA at pdsc$w_rsa_offset from the frame base.
 * PDSC$K_KIND_FP_REGISTER : a register-frame procedure. Keeps its caller's
 *                           return address in a register (pdsc$b_save_ra) and
 *                           does not build an RSA; walking to the caller is a
 *                           register read, the stack pointer is unchanged.
 * PDSC$K_KIND_NULL        : a null-frame procedure (treated like a register
 *                           frame for the purpose of the walk).
 * ================================================================ */

/* The OpenVMS Alpha Calling Standard (procedure descriptors; R29 = FP, which
 * locates the PDSC) holds only for code built by the alpha-dec-vms compiler --
 * the GCC-port images and their OVMX shareables. OVMX userland built by
 * alpha-linux-gnu follows the Linux/Alpha ABI, where R29 is the GP, so the
 * genuine walk/capture/transfer (vms-ed1) is compiled only for the former. */
#if (defined(__alpha) || defined(__alpha__)) && (defined(__VMS) || defined(__vms) || defined(__VMS__))
#define OVMX_ALPHA_VMS_ABI 1
#else
#define OVMX_ALPHA_VMS_ABI 0
#endif

#define PDSC$K_KIND_NULL            8
#define PDSC$K_KIND_FP_STACK        9
#define PDSC$K_KIND_FP_REGISTER     10

/* ================================================================
 * pdsc$w_flags bit fields.
 * ================================================================ */

#define PDSC$M_KIND                 0x000F  /* <3:0> procedure kind */
#define PDSC$V_HANDLER_VALID        0x0010  /* pdsc$q_handler is present */
#define PDSC$V_HANDLER_REINVOKABLE  0x0020
#define PDSC$V_BASE_REG_IS_FP       0x0080  /* frame base = FP(R29), else SP(R30) */

#define PDSC$KIND(flags)            ((flags) & PDSC$M_KIND)

/* ================================================================
 * Procedure Descriptor (PDSC$).
 *
 * The essential Calling-Standard fields the invocation-context walk needs.
 * A stack-frame descriptor carries pdsc$w_rsa_offset / pdsc$l_size /
 * pdsc$l_ireg_mask; a register-frame descriptor carries pdsc$b_save_ra. Both
 * carry pdsc$q_entry (entry code address) and, when PDSC$V_HANDLER_VALID,
 * pdsc$q_handler (the frame's established condition handler - this is what
 * makes libicb$v_handler_present authentic).
 * ================================================================ */

struct pdsc_descriptor {
    uint16_t pdsc$w_flags;          /* off  0: kind (<3:0>) + flag bits      */
    union {                         /* off  2: (kind-dependent)              */
        uint16_t pdsc$w_rsa_offset; /*   stack frame: frame base -> RSA       */
        struct {                    /*   register frame:                      */
            uint8_t pdsc$b_save_fp; /*     register holding the caller's FP   */
            uint8_t pdsc$b_save_ra; /*     register holding the return addr   */
        };
    };
    uint8_t  pdsc$b_reserved;       /* off  4                                 */
    uint8_t  pdsc$b_type;           /* off  5: procedure type (low nibble)    */
    uint16_t pdsc$w_signature_offset; /* off 6                                */
    uint64_t pdsc$q_entry;          /* off  8: procedure entry code address   */
    uint32_t pdsc$l_size;           /* off 16: fixed frame size in bytes      */
    uint16_t pdsc$w_reserved;       /* off 20                                 */
    uint16_t pdsc$w_entry_length;   /* off 22: prologue length                */
    uint32_t pdsc$l_ireg_mask;      /* off 24: integer regs saved in the RSA (stack) */
    uint32_t pdsc$l_freg_mask;      /* off 28: FP regs saved in the RSA (stack)      */
    uint64_t pdsc$q_handler;        /* off 32: condition handler, if HANDLER_VALID   */
    uint64_t pdsc$q_handler_data;   /* off 40: handler data                          */
};
/* This is the descriptor the alpha-dec-vms back end actually emits (gas .pdesc;
 * observed in a cross-compiled object: flags 0x3089 = kind FP_STACK with
 * BASE_REG_IS_FP, rsa_offset 8 at +2, entry at +8, frame size at +16,
 * ireg_mask at +24) -- the OpenVMS Alpha Calling Standard PDSC. A layout that
 * disagrees mis-reads every real frame (vms-ed1), so it is pinned here. */
_Static_assert(__builtin_offsetof(struct pdsc_descriptor, pdsc$q_entry) == 8, "PDSC entry at +8");
_Static_assert(__builtin_offsetof(struct pdsc_descriptor, pdsc$l_size) == 16, "PDSC size at +16");
_Static_assert(__builtin_offsetof(struct pdsc_descriptor, pdsc$l_ireg_mask) == 24, "PDSC ireg_mask at +24");
_Static_assert(__builtin_offsetof(struct pdsc_descriptor, pdsc$q_handler) == 32, "PDSC handler at +32");


/* ================================================================
 * Register Save Area (RSA) layout, as built by a stack-frame procedure.
 *
 * At (frame_base + pdsc$w_rsa_offset):
 *   [RSA$Q_SAVED_RETURN]  quadword : the saved return address (caller PC)
 *   then, one quadword each in ASCENDING register number, the saved value of
 *   every integer register whose bit is set in pdsc$l_ireg_mask (this is where
 *   the caller's FP=R29 and any preserved R2..R15 are recovered), then the
 *   saved FP registers per pdsc$l_freg_mask.
 * ================================================================ */

#define RSA$Q_SAVED_RETURN          0   /* byte offset of the saved return PC */
#define RSA$K_REG_SLOT_SIZE         8   /* each saved register is a quadword */

/* ================================================================
 * Invocation handle. Per the Calling Standard an invocation handle uniquely
 * identifies one invocation on the current stack; OVMX models it as the
 * frame's stack-pointer value (monotonic and unique per live invocation).
 * LIBICB$K_INVO_HANDLE_NULL marks "no such invocation" / bottom of stack.
 * ================================================================ */

typedef uint64_t INVO_HANDLE;
#define LIBICB$K_INVO_HANDLE_NULL   ((INVO_HANDLE)0)

/* ================================================================
 * OVMX-local non-success sentinel returned by LIB$GET_PREV_INVO_CONTEXT /
 * _HANDLE when the walk has run off the outermost frame (there is no caller
 * to produce). Warning severity (low 3 bits == STS$K_WARNING == 0 -> not a
 * success per $VMS_STATUS_SUCCESS), clean-room, not oracle-pinned.
 * ================================================================ */

#define LIBICB$_NOMOREFRAMES        0x00000010u

/* ================================================================
 * PDSC resolver seam.
 *
 * Mapping a PC to its procedure descriptor is image-linkage specific. On the
 * real Alpha runtime the default resolver consults the activated image's
 * procedure-descriptor tables (deferred child vms-cc8 wires the genuine
 * lookup); the host walk-engine test injects a resolver over its constructed
 * descriptors. A resolver returns the descriptor for `pc`, or NULL when `pc`
 * is outside known code (the base of the chain / bottom of stack).
 * ================================================================ */

typedef const struct pdsc_descriptor *(*vms$$pdsc_resolver_fn)(uint64_t pc,
                                                               void *user);

/* Register (or clear, fn==NULL) the thread-local PDSC resolver. */
void vms$$invo_set_pdsc_resolver(vms$$pdsc_resolver_fn fn, void *user);

/* Inject the thread-local "current context" LIB$GET_CURR_INVO_CONTEXT returns
 * (NULL clears, restoring the generic/real capture). On the real Alpha runtime
 * this is never set (capture is genuine); the host walk-engine test uses it to
 * seed the innermost frame of a constructed chain so the handle-based routines
 * (which walk out from the current context) are exercisable off Alpha. */
void vms$$invo_set_curr_context(const INVO_CONTEXT_BLK *icb);

/* Internal walk engine shared with the CHF dispatcher (lib_signal.c). Walks
 * `icb` one frame outward in place; returns SS$_NORMAL for a produced caller
 * (libicb$v_bottom_of_stack set when it is the outermost frame) or
 * LIBICB$_NOMOREFRAMES when there is no caller to produce. */
uint32_t vms$$invo_walk_prev(INVO_CONTEXT_BLK *icb);

/* ================================================================
 * SYS$UNWIND anchorless-frame support (CHF rung-3 wiring, lib_signal.c).
 *
 * When a handler unwinds to a target frame that armed NO VMS$UNWIND_ANCHOR
 * (the literal "return to main", a bare caller, a libgcc EH landing pad),
 * perform_unwind() reconstructs that frame's real saved context by walking the
 * current invocation chain to the frame whose PC matches the target's
 * establisher return PC, then asks vms$$invo_transfer() to resume there.
 * ================================================================ */

/* Walk the current invocation chain to the frame whose program counter matches
 * `target_pc` and copy its reconstructed context into `out`. Returns SS$_NORMAL
 * on a match, else LIBICB$_NOMOREFRAMES. */
uint32_t vms$$invo_reconstruct_target(uint64_t target_pc, INVO_CONTEXT_BLK *out);

/* Transfer control into a reconstructed frame: restore its saved integer
 * registers and resume at `newpc` (or the frame's own PC when newpc is NULL).
 * Returns 1 and DOES NOT RETURN when it transferred; returns 0 when it cannot
 * (no Alpha machine context to restore into - the register-restore + jump is
 * the Alpha-runtime child vms-cc8 / vms-8e8c). See lib_invo.c. */
int vms$$invo_transfer(const INVO_CONTEXT_BLK *icb, void *newpc);

#ifdef __cplusplus
}
#endif

#endif /* __PDSCDEF_H */
