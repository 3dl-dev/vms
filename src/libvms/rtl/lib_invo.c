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
 *     where frame_base is FP(R29) if PDSC$M_BASE_REG_IS_FP else SP(R30). The
 *     caller PC is RSA$Q_SAVED_RETURN; every integer register whose bit is set
 *     in pdsc$l_ireg_mask is restored from the RSA in ascending register order
 *     (this recovers the caller's FP=R29 and any preserved registers); the
 *     caller SP = frame_base + pdsc$l_size.
 *   - libicb$v_handler_present reflects PDSC$M_HANDLER_VALID; the walk stops
 *     (libicb$v_bottom_of_stack) when the next-out PC is 0 or unresolvable.
 *
 * WHERE IT RUNS (vms-ed1). On the OpenVMS Alpha ABI (alpha-dec-vms code,
 * OVMX_ALPHA_VMS_ABI) the primitives are genuine:
 *   - the PDSC is found the Calling-Standard way, through each frame's FP:
 *     0(FP) holds the procedure value for a stack frame, FP is the PDSC for a
 *     register frame -- validated (kind, NATIVE, PC within the procedure) so the
 *     walk stops cleanly at a non-VMS frame such as the image activator;
 *   - LIB$GET_CURR_INVO_CONTEXT stores the live integer register file and
 *     walks one frame out, so the ICB is the caller's state at its call;
 *   - vms$$invo_transfer loads a reconstructed frame's registers and jumps.
 * Proven on qemu-system-alpha + /dev/vms by the invo-gate (an anchorless
 * SYS$UNWIND returns to main with R0 from the mechanism array). Off that ABI
 * (host builds, Linux/Alpha userland) the WALK ENGINE is exercised through
 * the resolver/context seams against constructed frames
 * (tests/libvms/test_invo_context.c); capture there is a best-effort generic
 * frame and transfer reports "not transferred".
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

#if OVMX_ALPHA_VMS_ABI
/* The genuine lookup (vms-ed1), per the Alpha Calling Standard: a procedure's
 * frame pointer (R29) identifies its procedure descriptor. A stack-frame
 * procedure stores its procedure value (the PDSC address) at 0(FP); a
 * register-frame procedure sets FP to the PDSC itself. So *FP is 8-byte
 * aligned for a stack frame (a pointer) and is the descriptor's first quadword
 * (flags word, kind 8..10, not 8-byte aligned) for a register frame. A frame
 * that is not a VMS-standard one (the OVMX image activator below crt0) yields
 * no valid NATIVE descriptor whose entry precedes the PC: the walk stops. */
#define PDSC$M_NATIVE 0x1000    /* flags bit: native (not a translated) procedure */
static const struct pdsc_descriptor *fp_pdsc(uint64_t pc, uint64_t fp)
{
    if (fp == 0 || (fp & 7) != 0) {
        return NULL;
    }
    uint64_t q = *(const uint64_t *)(uintptr_t)fp;
    const struct pdsc_descriptor *pd =
        (q != 0 && (q & 7) == 0) ? (const struct pdsc_descriptor *)(uintptr_t)q
                                 : (const struct pdsc_descriptor *)(uintptr_t)fp;
    unsigned kind = PDSC$KIND(pd->pdsc$w_flags);
    if (kind != PDSC$K_KIND_FP_STACK && kind != PDSC$K_KIND_FP_REGISTER &&
        kind != PDSC$K_KIND_NULL) {
        return NULL;
    }
    if (!(pd->pdsc$w_flags & PDSC$M_NATIVE)) {
        return NULL;
    }
    if (pd->pdsc$q_entry == 0 || pc < pd->pdsc$q_entry ||
        pc - pd->pdsc$q_entry > 0x1000000) {
        return NULL;   /* the PC does not lie in this procedure */
    }
    return pd;
}
#endif

static const struct pdsc_descriptor *resolve_pdsc(uint64_t pc, uint64_t fp)
{
    if (pc == 0) {
        return NULL;
    }
    if (g_resolver != NULL) {
        return g_resolver(pc, g_resolver_user);   /* host test seam */
    }
#if OVMX_ALPHA_VMS_ABI
    return fp_pdsc(pc, fp);
#else
    (void)fp;
    return NULL;
#endif
}

/* ================================================================
 * Peek the return address a frame at `pc`/`frame_base` would restore, WITHOUT
 * mutating an ICB. Used for the one-level look-ahead that sets
 * libicb$v_bottom_of_stack on the last real frame (its caller PC is 0 or
 * unresolvable). Returns 1 if a non-zero caller PC exists, else 0.
 * ================================================================ */

static int frame_has_caller(uint64_t pc, uint64_t fp, uint64_t sp)
{
    const struct pdsc_descriptor *pd = resolve_pdsc(pc, fp);
    if (pd == NULL) {
        return 0;
    }
    unsigned kind = PDSC$KIND(pd->pdsc$w_flags);
    if (kind == PDSC$K_KIND_FP_STACK) {
        uint64_t base = (pd->pdsc$w_flags & PDSC$M_BASE_REG_IS_FP) ? fp : sp;
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

/* vms-bfd03 (lib_signal.c): the condition-dispatcher frame records. */
void *vms$$chfctx_for_frame(uint64_t fp);
const INVO_CONTEXT_BLK *vms$$chf_signal_context(uint64_t fp);

uint32_t vms$$invo_walk_prev(INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        return SS$_BADPARAM;
    }
    if (icb->libicb$v_bottom_of_stack) {
        return LIBICB$_NOMOREFRAMES;   /* already at the base */
    }

    uint64_t pc = icb->libicb$q_program_counter;
    uint64_t fp = icb->libicb$q_ireg[ALPHA_REG_FP];
    uint64_t sp = icb->libicb$q_ireg[ALPHA_REG_SP];

#if OVMX_ALPHA_VMS_ABI
    /* vms-bfd03: a condition-dispatcher frame (one calling a handler) is
     * followed, outward, by the SIGNALLING POINT -- as on VMS, where the frame
     * before SYS$CALL_HANDL's is the one that signalled: its full register
     * state is in the dispatch record (captured by LIB$SIGNAL). */
    {
        const INVO_CONTEXT_BLK *sigp = vms$$chf_signal_context(fp);
        if (sigp != NULL) {
            *icb = *sigp;
            icb->libicb$ph_chfctx_addr = vms$$chfctx_for_frame(
                icb->libicb$q_ireg[ALPHA_REG_FP]);
            return SS$_NORMAL;
        }
    }
#endif
    const struct pdsc_descriptor *pd = resolve_pdsc(pc, fp);
    if (pd == NULL) {
        icb->libicb$v_bottom_of_stack = 1;
        return LIBICB$_NOMOREFRAMES;
    }
    unsigned kind = PDSC$KIND(pd->pdsc$w_flags);

    uint64_t caller_pc, caller_fp, caller_sp;

    if (kind == PDSC$K_KIND_FP_STACK) {
        /* Stack-frame procedure: recover the caller from the RSA. */
        uint64_t base = (pd->pdsc$w_flags & PDSC$M_BASE_REG_IS_FP) ? fp : sp;
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
        /* A register-frame procedure keeps the caller's FP in pdsc$b_save_fp
         * (its own FP holds its PDSC); a null frame never changed FP. */
        caller_fp = (kind == PDSC$K_KIND_FP_REGISTER && pd->pdsc$b_save_fp &&
                     pd->pdsc$b_save_fp != ALPHA_REG_FP)
                        ? icb->libicb$q_ireg[pd->pdsc$b_save_fp] : fp;
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
#if OVMX_ALPHA_VMS_ABI
    /* vms-bfd03: a condition-dispatcher frame carries its CHF context. */
    icb->libicb$ph_chfctx_addr = vms$$chfctx_for_frame(caller_fp);
#endif

    /* Report the caller's established condition handler, if any. */
    const struct pdsc_descriptor *cpd = resolve_pdsc(caller_pc, caller_fp);
    icb->libicb$v_handler_present =
        (cpd && (cpd->pdsc$w_flags & PDSC$M_HANDLER_VALID)) ? 1 : 0;

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

#if OVMX_ALPHA_VMS_ABI
    /* Genuine capture (vms-ed1): store this procedure's live integer register
     * file, take a PC inside this procedure, then walk ONE frame outward with
     * the Calling-Standard walk -- which restores, from this frame's RSA, every
     * preserved register this procedure saved -- so the ICB describes the
     * CALLER exactly as it stands at its call to LIB$GET_CURR_INVO_CONTEXT. */
    {
        uint64_t *r = icb->libicb$q_ireg;
        __asm__ __volatile__(
            "stq $0,0(%0)\n\tstq $1,8(%0)\n\tstq $2,16(%0)\n\tstq $3,24(%0)\n\t"
            "stq $4,32(%0)\n\tstq $5,40(%0)\n\tstq $6,48(%0)\n\tstq $7,56(%0)\n\t"
            "stq $8,64(%0)\n\tstq $9,72(%0)\n\tstq $10,80(%0)\n\tstq $11,88(%0)\n\t"
            "stq $12,96(%0)\n\tstq $13,104(%0)\n\tstq $14,112(%0)\n\tstq $15,120(%0)\n\t"
            "stq $16,128(%0)\n\tstq $17,136(%0)\n\tstq $18,144(%0)\n\tstq $19,152(%0)\n\t"
            "stq $20,160(%0)\n\tstq $21,168(%0)\n\tstq $22,176(%0)\n\tstq $23,184(%0)\n\t"
            "stq $24,192(%0)\n\tstq $25,200(%0)\n\tstq $26,208(%0)\n\tstq $27,216(%0)\n\t"
            "stq $28,224(%0)\n\tstq $29,232(%0)\n\tstq $30,240(%0)\n\tstq $31,248(%0)"
            : : "r"(r) : "memory");
        uint64_t self_pc;
        __asm__ __volatile__("br %0,1f\n1:" : "=r"(self_pc));
        icb->libicb$q_program_counter = self_pc;
        icb->libicb$q_stack_pointer = r[ALPHA_REG_SP];
        uint32_t st = vms$$invo_walk_prev(icb);
        icb->libicb$ih_pc = (void *)(uintptr_t)icb->libicb$q_program_counter;
        icb->libicb$ih_ip = icb->libicb$ih_pc;
        return $VMS_STATUS_SUCCESS(st) ? SS$_NORMAL : st;
    }
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
#if OVMX_ALPHA_VMS_ABI
    /* On the Alpha runtime the handle is the frame pointer, as on OpenVMS:
     * callers (libgcc's vms-unwind.h among them) build an ICB holding only FP
     * and ask for its handle. */
    return (INVO_HANDLE)icb->libicb$q_ireg[ALPHA_REG_FP];
#else
    return (INVO_HANDLE)icb->libicb$q_ireg[ALPHA_REG_SP];
#endif
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
#if OVMX_ALPHA_VMS_ABI
    /* vms-ed1: resume in the reconstructed frame. Load its integer register
     * file (R0..R26, R29 FP, R30 SP) and jump to newpc, or to the frame's own
     * PC -- the instruction after its call, as if that call had returned. R27
     * (PV) and R28 (AT) carry the base and the target and are not restored:
     * VMS-standard code reloads R27 from its frame before using it. Integer
     * state only: the caller's preserved FP registers F2-F9 are not part of an
     * OVMX ICB. */
    if (icb == NULL) {
        return 0;
    }
    static _Thread_local uint64_t regs[32];
    for (int i = 0; i < 32; i++) {
        regs[i] = icb->libicb$q_ireg[i];
    }
    uint64_t target = newpc ? (uint64_t)(uintptr_t)newpc
                            : icb->libicb$q_program_counter;
    __asm__ __volatile__(
        ".set noat\n\t"
        "mov %0,$27\n\tmov %1,$28\n\t"
        "ldq $0,0($27)\n\tldq $1,8($27)\n\tldq $2,16($27)\n\tldq $3,24($27)\n\t"
        "ldq $4,32($27)\n\tldq $5,40($27)\n\tldq $6,48($27)\n\tldq $7,56($27)\n\t"
        "ldq $8,64($27)\n\tldq $9,72($27)\n\tldq $10,80($27)\n\tldq $11,88($27)\n\t"
        "ldq $12,96($27)\n\tldq $13,104($27)\n\tldq $14,112($27)\n\tldq $15,120($27)\n\t"
        "ldq $16,128($27)\n\tldq $17,136($27)\n\tldq $18,144($27)\n\tldq $19,152($27)\n\t"
        "ldq $20,160($27)\n\tldq $21,168($27)\n\tldq $22,176($27)\n\tldq $23,184($27)\n\t"
        "ldq $24,192($27)\n\tldq $25,200($27)\n\tldq $26,208($27)\n\t"
        "ldq $29,232($27)\n\tldq $30,240($27)\n\t"
        "jmp $31,($28)\n\t"
        ".set at"
        : : "r"(regs), "r"(target) : "memory");
    __builtin_unreachable();
#else
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
#endif
}

/* ================================================================
 * VMS-convention (uppercase) entry points -- the spellings the GCC port's
 * libgcc unwinder (libgcc/config/alpha/vms-unwind.h, through vms/libicb.h)
 * calls. On OpenVMS the RTL exports these names and an invocation handle is a
 * longword (the stack lives in P1, below 2 GB). The handle here is the
 * low-order longword of the frame pointer. IMGACT runs an image on a user stack
 * in P1 (vms-ce5), so for the image's own frames that longword IS the frame
 * pointer; LIB$GET_INVO_CONTEXT matches a handle against the low longword of
 * each frame's FP, which also identifies a frame on any other single stack.
 * (vms-4d0)
 * ================================================================ */

int LIB$GET_INVO_HANDLE(INVO_CONTEXT_BLK *icb)
{
    return (int)(uint32_t)lib$get_invo_handle(icb);
}

int LIB$GET_INVO_CONTEXT(int invo_handle, INVO_CONTEXT_BLK *icb)
{
    if (icb == NULL) {
        return SS$_BADPARAM;
    }
    INVO_CONTEXT_BLK scratch;
    uint32_t st = lib$get_curr_invo_context(&scratch);   /* this frame */
    while ($VMS_STATUS_SUCCESS(st)) {
        if ((uint32_t)lib$get_invo_handle(&scratch) == (uint32_t)invo_handle) {
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

int LIB$GET_PREV_INVO_CONTEXT(INVO_CONTEXT_BLK *icb)
{
    return (int)lib$get_prev_invo_context(icb);
}
