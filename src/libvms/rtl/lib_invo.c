/*
 * lib_invo.c - Alpha invocation-context primitives (vms-1fa, CHF rung-3).
 *
 *   LIB$GET_CURR_INVO_CONTEXT  - fill an ICB from the current context
 *   LIB$GET_PREV_INVO_CONTEXT  - walk the ICB one frame outward (to the caller)
 *   LIB$GET_INVO_CONTEXT       - fill an ICB from an invocation handle
 *   LIB$GET_INVO_HANDLE        - the handle for the frame an ICB describes
 *   LIB$GET_PREV_INVO_HANDLE   - the handle of the caller of a given handle
 *
 * WHY THIS EXISTS (docs/design-chf-condition-handling.md rung-3, gap G5).
 * Rung-1/rung-2 gave CHF a real 4-stage search and a real frame-transfer
 * SYS$UNWIND, but the frame transfer can only resume into a target frame that
 * armed an explicit VMS$UNWIND_ANCHOR (a setjmp). Resuming into an ANCHORLESS
 * ancestor - VMS's literal "return to main", a bare caller of the establisher
 * with no OVMX hook, and every libgcc/GCC-port EH landing pad - needs the
 * runtime to reconstruct that frame's saved context by walking the GENUINE
 * Alpha call chain: procedure descriptors (PDSC) + register save areas (RSA)
 * per the Alpha Calling Standard. These primitives are that walk, and
 * perform_unwind() (lib_signal.c) consults them on the anchorless path.
 *
 * WALK MODEL (Alpha Calling Standard, see pdscdef.h):
 *   - Resolve the PDSC for the current PC (via a registered resolver - the
 *     image-linkage lookup on real Alpha, a constructed table under the host
 *     walk-engine test).
 *   - REGISTER-frame procedure: the caller's return address is in a register
 *     (pdsc$b_save_ra, default R26); the caller shares this frame's stack.
 *     Caller PC = ireg[save_ra]; FP/SP unchanged.
 *   - STACK-frame procedure: locate the RSA at (frame_base + pdsc$w_rsa_offset)
 *     where frame_base is FP(R29) if PDSC$V_BASE_REG_IS_FP else SP(R30). The
 *     caller PC is RSA$Q_SAVED_RETURN; every integer register whose bit is set
 *     in pdsc$l_ireg_mask is restored from the RSA in ascending register order
 *     (this recovers the caller's FP=R29 and any preserved registers); the
 *     caller SP = frame_base + pdsc$l_size.
 *   - libicb$v_handler_present reflects PDSC$V_HANDLER_VALID; the walk stops
 *     (libicb$v_bottom_of_stack) when the next-out PC is 0 or unresolvable.
 *
 * HOST SCOPE (honest, no silent drop). The WALK ENGINE above is pure control
 * flow over the descriptors/RSA and is proven on the host against constructed
 * Alpha frames (tests/libvms/test_invo_context.c) - byte-for-byte the 64-bit
 * layout the Alpha runtime uses (host x86_64 is LP64 like Alpha). What is
 * inherently Alpha-runtime and deferred to the children:
 *   - LIB$GET_CURR_INVO_CONTEXT capturing a REAL Alpha register file (the host
 *     has no Alpha context; off-Alpha it seeds a best-effort generic frame and
 *     the walk halts at the first unresolved PC) - proven on qemu-alpha by the
 *     Alpha-rig oracle (child vms-cc8 bracket);
 *   - the actual MACHINE transfer into a reconstructed anchorless frame (a
 *     register restore + jump) that perform_unwind() performs on Alpha - the
 *     reconstruction (building the target ICB) is host-testable; the register
 *     restore is Alpha-only (child vms-cc8 / vms-8e8c libgcc EH).
 *
 * CLEAN-ROOM (Rule 8): from the public Alpha/OpenVMS Calling Standard and the
 * OVMX-Alpha toolchain's own emitted descriptors; never VSI/HPE source.
 */

#include <stdint.h>
#include <string.h>
#include "ssdef.h"
#include "libicb.h"
#include "pdscdef.h"
#include "lib$routines.h"

/* ================================================================
 * Thread-local PDSC resolver (pc -> procedure descriptor).
 * ================================================================ */

static _Thread_local vms$$pdsc_resolver_fn g_resolver = NULL;
static _Thread_local void                 *g_resolver_user = NULL;

/* Injected current-context seam (see pdscdef.h). NULL => genuine capture. */
static _Thread_local int              g_curr_valid = 0;
static _Thread_local INVO_CONTEXT_BLK g_curr_ctx;

void vms$$invo_set_pdsc_resolver(vms$$pdsc_resolver_fn fn, void *user)
{
    g_resolver = fn;
    g_resolver_user = user;
}

void vms$$invo_set_curr_context(const INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        g_curr_valid = 0;
        return;
    }
    g_curr_ctx = *icb;
    g_curr_valid = 1;
}

static const struct pdsc_descriptor *resolve_pdsc(uint64_t pc)
{
    if (pc == 0 || g_resolver == NULL) {
        return NULL;
    }
    return g_resolver(pc, g_resolver_user);
}

/* ================================================================
 * Peek the return address a frame at `pc`/`frame_base` would restore, WITHOUT
 * mutating an ICB. Used for the one-level look-ahead that sets
 * libicb$v_bottom_of_stack on the last real frame (its caller PC is 0 or
 * unresolvable). Returns 1 if a non-zero caller PC exists, else 0.
 * ================================================================ */

static int frame_has_caller(uint64_t pc, uint64_t fp, uint64_t sp)
{
    const struct pdsc_descriptor *pd = resolve_pdsc(pc);
    if (pd == NULL) {
        return 0;
    }
    unsigned kind = PDSC$KIND(pd->pdsc$w_flags);
    if (kind == PDSC$K_KIND_FP_STACK) {
        uint64_t base = (pd->pdsc$w_flags & PDSC$V_BASE_REG_IS_FP) ? fp : sp;
        const uint64_t *rsa =
            (const uint64_t *)(uintptr_t)(base + pd->pdsc$w_rsa_offset
                                          + RSA$Q_SAVED_RETURN);
        return (*rsa != 0);
    }
    /* register / null frame: caller PC is in the save-ra register - we cannot
     * read it here without the ICB, so assume a caller exists (the walk itself
     * detects a 0 return). */
    return 1;
}

/* ================================================================
 * vms$$invo_walk_prev - walk `icb` one frame outward, in place.
 *
 * Precondition: icb->libicb$q_program_counter and the integer register file
 * (in particular FP=R29 and SP=R30) describe the current frame. On success the
 * ICB is updated to describe the caller and libicb$v_bottom_of_stack is set
 * iff the caller is the outermost frame. Returns LIBICB$_NOMOREFRAMES when
 * there is no caller to produce.
 * ================================================================ */

uint32_t vms$$invo_walk_prev(INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        return SS$_BADPARAM;
    }
    if (icb->libicb$v_bottom_of_stack) {
        return LIBICB$_NOMOREFRAMES;   /* already at the base */
    }

    uint64_t pc = icb->libicb$q_program_counter;
    const struct pdsc_descriptor *pd = resolve_pdsc(pc);
    if (pd == NULL) {
        icb->libicb$v_bottom_of_stack = 1;
        return LIBICB$_NOMOREFRAMES;
    }

    uint64_t fp = icb->libicb$q_ireg[ALPHA_REG_FP];
    uint64_t sp = icb->libicb$q_ireg[ALPHA_REG_SP];
    unsigned kind = PDSC$KIND(pd->pdsc$w_flags);

    uint64_t caller_pc, caller_fp, caller_sp;

    if (kind == PDSC$K_KIND_FP_STACK) {
        /* Stack-frame procedure: recover the caller from the RSA. */
        uint64_t base = (pd->pdsc$w_flags & PDSC$V_BASE_REG_IS_FP) ? fp : sp;
        uint64_t rsa  = base + pd->pdsc$w_rsa_offset;

        const uint64_t *saved_ret =
            (const uint64_t *)(uintptr_t)(rsa + RSA$Q_SAVED_RETURN);
        caller_pc = *saved_ret;

        /* Restore each integer register saved in the RSA, in ascending
         * register order, one quadword each after the saved return address.
         * This recovers the caller's FP (R29) and any preserved registers. */
        uint64_t slot = rsa + RSA$Q_SAVED_RETURN + RSA$K_REG_SLOT_SIZE;
        for (int r = 0; r < ALPHA_REG_COUNT; r++) {
            if (pd->pdsc$l_ireg_mask & (1u << r)) {
                icb->libicb$q_ireg[r] =
                    *(const uint64_t *)(uintptr_t)slot;
                slot += RSA$K_REG_SLOT_SIZE;
            }
        }

        caller_fp = icb->libicb$q_ireg[ALPHA_REG_FP];
        caller_sp = base + pd->pdsc$l_size;   /* deallocate this frame */
    } else {
        /* Register-frame / null-frame procedure: the caller's return address
         * is live in a register; the caller shares this stack. */
        unsigned ra = pd->pdsc$b_save_ra ? pd->pdsc$b_save_ra : ALPHA_REG_RA;
        caller_pc = icb->libicb$q_ireg[ra];
        caller_fp = fp;
        caller_sp = sp;
    }

    if (caller_pc == 0) {
        /* No caller: this frame is the base of the chain. Mark bottom but do
         * not advance (the current frame remains the outermost produced). */
        icb->libicb$v_bottom_of_stack = 1;
        return LIBICB$_NOMOREFRAMES;
    }

    /* Commit the caller context. */
    icb->libicb$q_program_counter = caller_pc;
    icb->libicb$q_ireg[ALPHA_REG_FP] = caller_fp;
    icb->libicb$q_ireg[ALPHA_REG_SP] = caller_sp;
    icb->libicb$q_stack_pointer = caller_sp;

    /* Report the caller's established condition handler, if any. */
    const struct pdsc_descriptor *cpd = resolve_pdsc(caller_pc);
    icb->libicb$v_handler_present =
        (cpd && (cpd->pdsc$w_flags & PDSC$V_HANDLER_VALID)) ? 1 : 0;

    /* One-level look-ahead: is the caller itself the outermost frame? */
    icb->libicb$v_bottom_of_stack =
        frame_has_caller(caller_pc, caller_fp, caller_sp) ? 0 : 1;

    return SS$_NORMAL;
}

/* ================================================================
 * lib$get_curr_invo_context - fill an ICB from the current context.
 *
 * On Alpha this captures the live register file + PC. Off Alpha there is no
 * Alpha context to capture: seed a best-effort generic frame (the caller's
 * return address and frame address) so a walk is well-defined and halts at the
 * first unresolved PC. Real capture is proven on qemu-alpha (child vms-cc8).
 * ================================================================ */

uint32_t lib$get_curr_invo_context(INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        return SS$_BADPARAM;
    }

    if (g_curr_valid) {
        /* Test/seed seam: return the injected current context verbatim. */
        *icb = g_curr_ctx;
        return SS$_NORMAL;
    }

    memset(icb, 0, sizeof(*icb));

#if defined(__alpha) || defined(__alpha__)
    /* The genuine Alpha capture (register file + PC) is wired by the
     * Alpha-runtime child; until then seed from the generic builtins below so
     * the primitive is defined on every arch and the walk engine is exercised
     * against real descriptors on the rig. */
#endif
    uint64_t here = (uint64_t)(uintptr_t)__builtin_return_address(0);
    uint64_t frame = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    icb->libicb$q_program_counter = here;
    icb->libicb$q_ireg[ALPHA_REG_FP] = frame;
    icb->libicb$q_ireg[ALPHA_REG_SP] = frame;
    icb->libicb$q_stack_pointer = frame;
    icb->libicb$ih_pc = (void *)(uintptr_t)here;
    icb->libicb$ih_ip = (void *)(uintptr_t)here;
    icb->libicb$v_bottom_of_stack = 0;
    icb->libicb$v_handler_present = 0;
    return SS$_NORMAL;
}

/* ================================================================
 * lib$get_prev_invo_context - walk to the caller (in place).
 * ================================================================ */

uint32_t lib$get_prev_invo_context(INVO_CONTEXT_BLK *icb)
{
    uint32_t st = vms$$invo_walk_prev(icb);
    if (icb) {
        /* Keep the arch-specific PC mirrors coherent for corpus consumers. */
        icb->libicb$ih_pc = (void *)(uintptr_t)icb->libicb$q_program_counter;
        icb->libicb$ih_ip = (void *)(uintptr_t)icb->libicb$q_program_counter;
    }
    return st;
}

/* ================================================================
 * lib$get_invo_handle - the invocation handle for the frame an ICB describes.
 *
 * OVMX models the handle as the frame's stack-pointer value (unique per live
 * invocation). A bottom-of-stack ICB has the NULL handle.
 * ================================================================ */

INVO_HANDLE lib$get_invo_handle(INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        return LIBICB$K_INVO_HANDLE_NULL;
    }
    return (INVO_HANDLE)icb->libicb$q_ireg[ALPHA_REG_SP];
}

/* ================================================================
 * lib$get_invo_context - fill an ICB for the frame named by a handle.
 *
 * Walks the current chain until it reaches the frame whose handle matches, so
 * a handle obtained from an earlier get_invo_handle round-trips back to its
 * context. Returns SS$_NORMAL when found, LIBICB$_NOMOREFRAMES otherwise.
 * ================================================================ */

uint32_t lib$get_invo_context(INVO_HANDLE handle, INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        return SS$_BADPARAM;
    }
    if (handle == LIBICB$K_INVO_HANDLE_NULL) {
        return LIBICB$_NOMOREFRAMES;
    }

    INVO_CONTEXT_BLK scratch;
    uint32_t st = lib$get_curr_invo_context(&scratch);
    while ($VMS_STATUS_SUCCESS(st)) {
        if (lib$get_invo_handle(&scratch) == handle) {
            *icb = scratch;
            return SS$_NORMAL;
        }
        if (scratch.libicb$v_bottom_of_stack) {
            break;
        }
        st = lib$get_prev_invo_context(&scratch);
    }
    return LIBICB$_NOMOREFRAMES;
}

/* ================================================================
 * lib$get_prev_invo_handle - the handle of the caller of a given invocation.
 *
 * Walks the current chain to the frame named by `handle`, steps out one more
 * frame, and returns that caller's handle (NULL at the base of the stack).
 * ================================================================ */

INVO_HANDLE lib$get_prev_invo_handle(INVO_HANDLE handle)
{
    INVO_CONTEXT_BLK icb;
    if (lib$get_invo_context(handle, &icb) != SS$_NORMAL) {
        return LIBICB$K_INVO_HANDLE_NULL;
    }
    if (icb.libicb$v_bottom_of_stack) {
        return LIBICB$K_INVO_HANDLE_NULL;
    }
    if (!$VMS_STATUS_SUCCESS(lib$get_prev_invo_context(&icb))) {
        return LIBICB$K_INVO_HANDLE_NULL;
    }
    return lib$get_invo_handle(&icb);
}

/* ================================================================
 * SYS$UNWIND anchorless-frame support (CHF rung-3 wiring - see pdscdef.h).
 * ================================================================ */

uint32_t vms$$invo_reconstruct_target(uint64_t target_pc, INVO_CONTEXT_BLK *out)
{
    if (out == NULL || target_pc == 0) {
        return LIBICB$_NOMOREFRAMES;
    }

    INVO_CONTEXT_BLK icb;
    uint32_t st = lib$get_curr_invo_context(&icb);
    if (!$VMS_STATUS_SUCCESS(st)) {
        return LIBICB$_NOMOREFRAMES;
    }

    for (;;) {
        if (icb.libicb$q_program_counter == target_pc) {
            *out = icb;
            return SS$_NORMAL;
        }
        if (icb.libicb$v_bottom_of_stack) {
            break;
        }
        st = lib$get_prev_invo_context(&icb);
        if (!$VMS_STATUS_SUCCESS(st)) {
            break;
        }
    }
    return LIBICB$_NOMOREFRAMES;
}

int vms$$invo_transfer(const INVO_CONTEXT_BLK *icb, void *newpc)
{
    (void)icb;
    (void)newpc;
    /* The real machine transfer - restore the reconstructed frame's saved
     * integer registers (from icb->libicb$q_ireg[]) and jump to newpc (or
     * icb->libicb$q_program_counter) - is an Alpha register-restore sequence
     * that resumes execution in an arbitrary ancestor frame. That is the
     * Alpha-runtime child (vms-cc8 bracket / vms-8e8c libgcc EH); there is no
     * host machine context to restore into, so we report "not transferred" and
     * perform_unwind() keeps the rung-1 pop-only contract. The RECONSTRUCTION
     * that precedes this (vms$$invo_reconstruct_target) is host-proven. */
    return 0;
}
