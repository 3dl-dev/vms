/*
 * CHFDEF.H - Condition Handling Facility Definitions
 *
 * OpenVMX compatibility layer - Defines the data structures used
 * by the VMS condition handling mechanism: signal arrays and
 * mechanism arrays passed to condition handlers.
 *
 * On VMS, when a condition is signaled (via LIB$SIGNAL or hardware
 * exception), the system searches the call stack for condition
 * handlers. Each handler receives two arguments:
 *   1. Signal array - describes what happened
 *   2. Mechanism array - describes where it happened and how to respond
 *
 * Handler return values:
 *   SS$_RESIGNAL  - pass to next handler
 *   SS$_CONTINUE  - continue execution after signal
 *
 * Reference: OpenVMS Programming Concepts Manual, Chapter 9
 *            OpenVMS Calling Standard
 */

#ifndef __CHFDEF_H
#define __CHFDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Signal Array (CHF$SIGNAL_ARRAY / chf$signal_array)
 *
 * Passed as the first argument to a condition handler.
 *
 * Layout:
 *   chf$is_sig_args  - Number of additional longwords in the array
 *   chf$is_sig_name  - Condition value (the SS$_ or facility code)
 *   chf$is_sig_arg1  - First signal-specific argument (FAO arg)
 *   ...              - Additional arguments
 *   chf$is_sig_argN  - Last argument
 *
 * The condition value is always at offset 1 (index [1]).
 * FAO arguments for message formatting follow at offsets 2..N.
 * The PC at the point of signal is at offset N (last element).
 * ================================================================ */

struct chf$signal_array {
    uint32_t    chf$is_sig_args;    /* Argument count */
    uint32_t    chf$is_sig_name;    /* Condition value */
    uint32_t    chf$is_sig_arg1;    /* First FAO argument */
    /* Additional arguments follow dynamically.
     * In practice, access via: ((uint32_t *)sigarray)[index] */
};

/* Convenience macros for accessing signal array elements */
#define CHF$L_SIG_ARGS      0   /* Offset: argument count */
#define CHF$L_SIG_NAME      1   /* Offset: condition value */
#define CHF$L_SIG_ARG1      2   /* Offset: first FAO argument */

/* ================================================================
 * Mechanism Array (CHF$MECH_ARRAY / chf$mech_array)
 *
 * Passed as the second argument to a condition handler.
 *
 * Provides context about where the condition was signaled and
 * allows the handler to modify the return behavior.
 *
 * On Alpha/IA64, the mechanism array contains the saved register
 * state. On OVMX, we provide a simplified version that captures
 * the essential fields.
 * ================================================================ */

struct chf$mech_array {
    uint32_t    chf$is_mch_args;    /* Argument count */
    uint32_t    chf$is_mch_flags;   /* Flags */
    void       *chf$ph_mch_frame;   /* Frame pointer of establisher */
    uint32_t    chf$is_mch_depth;   /* Call depth of establisher */
    uint32_t    chf$is_mch_savr0;   /* Saved function return value (R0) */
    uint32_t    chf$is_mch_savr1;   /* Saved R1 */
};

/* Mechanism array offsets */
#define CHF$L_MCH_ARGS      0
#define CHF$L_MCH_FLAGS     1
#define CHF$L_MCH_FRAME     2
#define CHF$L_MCH_DEPTH     3
#define CHF$L_MCH_SAVR0     4
#define CHF$L_MCH_SAVR1     5

/* ================================================================
 * Condition Handler Function Type
 *
 * A condition handler is a function that takes:
 *   - Pointer to signal array
 *   - Pointer to mechanism array
 * and returns a VMS status code (SS$_RESIGNAL or SS$_CONTINUE).
 * ================================================================ */

typedef uint32_t (*chf$handler_t)(
    struct chf$signal_array *sigarray,
    struct chf$mech_array *mecharray
);

/* ================================================================
 * Condition Handler Control Block
 *
 * Used internally to maintain the handler stack.
 * Each call frame that establishes a handler has one of these.
 * ================================================================ */

struct chf$handler_block {
    struct chf$handler_block   *chf$l_link;     /* Forward link */
    chf$handler_t               chf$a_handler;  /* Handler routine */
    uint32_t                    chf$l_depth;     /* Stack depth */
};

/* ================================================================
 * Condition value severity codes (from stsdef.h, repeated for
 * convenience since CHF users often need them)
 * ================================================================ */

#ifndef STS$K_WARNING
#define STS$K_WARNING   0   /* Warning */
#define STS$K_SUCCESS   1   /* Success */
#define STS$K_ERROR     2   /* Error */
#define STS$K_INFO      3   /* Informational */
#define STS$K_SEVERE    4   /* Severe/Fatal */
#endif

/* ================================================================
 * Handler action constants
 * ================================================================ */

#define CHF$K_RESIGNAL      0   /* Resignal to next handler */
#define CHF$K_CONTINUE      1   /* Continue execution */
#define CHF$K_UNWIND        2   /* Unwind the call stack */

/* Maximum number of condition handlers in the chain */
#define CHF$K_MAX_HANDLERS  64

/* ================================================================
 * Mechanism-array flag bits (chf$is_mch_flags)
 *
 * OpenVMS Calling Standard mechanism-array flags. A handler tests
 * CHF$V_UNWINDING to distinguish a normal signal delivery (search
 * for a handler that will resume) from an unwind delivery (the
 * dispatcher is unwinding the stack and is calling this handler one
 * last time so it can release resources before its frame is
 * abandoned). See rung-2 in docs/design-chf-condition-handling.md.
 * ================================================================ */

#define CHF$V_UNWINDING     0           /* bit 0: an unwind is in progress */
#define CHF$M_UNWINDING     (1u << CHF$V_UNWINDING)

/* ================================================================
 * VMS$UNWIND_ANCHOR - arm a machine-frame-transfer resume point (vms-8802
 * rung-2).
 *
 * A procedure that establishes a condition handler and wants SYS$UNWIND to
 * be able to transfer control back INTO its own frame (abandoning the
 * intervening frames between the point of signal and this frame) places
 * VMS$UNWIND_ANCHOR() immediately after lib$establish(). It arms a resume
 * anchor for the most recently established handler's frame and evaluates,
 * setjmp-style, to:
 *
 *   0        - on the arming call (fall through to the protected region);
 *   non-zero - when a later SYS$UNWIND transferred control back to this
 *              point. Read vms$$unwind_newpc() for the newpc that was
 *              requested (NULL when the unwind passed newpc == 0).
 *
 * Usage:
 *
 *   (void)lib$establish(my_handler);
 *   if (VMS$UNWIND_ANCHOR()) {
 *       // control returned here via SYS$UNWIND: intervening frames abandoned
 *   } else {
 *       // protected region; nested calls may signal a condition
 *   }
 *
 * The macro MUST expand in the establisher's own frame (that is what makes
 * the setjmp resumable), which is why it is a macro over setjmp rather than
 * a library call. On the host this is the resume mechanism; the real Alpha
 * invocation-context walk (resuming into an ancestor that armed no anchor)
 * is rung-3 (vms-1fa). Include <setjmp.h> before using it.
 * ================================================================ */

extern void *vms$$unwind_anchor_buf(void);  /* really a jmp_buf* (see setjmp.h) */
extern void *vms$$unwind_newpc(void);

#define VMS$UNWIND_ANCHOR() \
    (setjmp(*(jmp_buf *)vms$$unwind_anchor_buf()))

/* ================================================================
 * SYS$SETEXV software exception vector selectors (vms-2e72 rung-1)
 *
 * The OpenVMS exception dispatcher consults three per-access-mode
 * software exception vectors in a fixed order relative to the
 * call-frame handler search:
 *
 *   1. PRIMARY      vector  (searched BEFORE the frame-handler chain)
 *   2. call-frame handlers  (innermost frame -> outermost frame)
 *   3. SECONDARY    vector  (searched AFTER  the frame-handler chain)
 *   4. LAST-CHANCE  vector  (the final handler before the catch-all)
 *
 * A vectored handler has the same (signal-array, mechanism-array)
 * prototype as a frame handler and returns SS$_CONTINUE / SS$_RESIGNAL
 * identically. These selector codes are passed as SYS$SETEXV's first
 * argument. Reference: OpenVMS Calling Standard, "Exception Vectors";
 * OpenVMS System Services Reference, $SETEXV.
 * ================================================================ */

#define CHF$K_PRIMARY_VECTOR        0   /* primary exception vector */
#define CHF$K_SECONDARY_VECTOR      1   /* secondary exception vector */
#define CHF$K_LAST_CHANCE_VECTOR    2   /* last-chance exception vector */
#define CHF$K_VECTOR_COUNT          3

#ifdef __cplusplus
}
#endif

#endif /* __CHFDEF_H */
