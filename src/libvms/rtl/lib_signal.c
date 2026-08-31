/*
 * lib_signal.c - LIB$SIGNAL / LIB$STOP / LIB$SIG_TO_RET
 *
 * VMS condition handling routines. Implements the Condition Handling
 * Facility (CHF) dispatcher: the frame-handler chain established by
 * lib$establish PLUS the software exception vectors established by
 * SYS$SETEXV (sys_setexv.c), searched in the authentic OpenVMS order.
 *
 * On VMS, when a condition is signalled (via lib$signal or a hardware
 * exception mapped to an SS$_ code) the dispatcher searches for a
 * condition handler in this order:
 *
 *   1. the PRIMARY software exception vector (SYS$SETEXV)
 *   2. the established frame-handler chain, innermost frame -> outermost
 *   3. the SECONDARY software exception vector
 *   4. the LAST-CHANCE software exception vector
 *   5. the default catch-all (format the message; act on severity)
 *
 * Each handler receives a signal array and a mechanism array and returns
 *   - SS$_CONTINUE  : it handled the condition; resume execution
 *   - SS$_RESIGNAL  : pass to the next handler in the search order
 *
 * FAITHFULNESS / SCOPE (vms-2e72, docs/design-chf-condition-handling.md)
 *   rung-1 (this change): the search order and vectors above are real;
 *   the mechanism array carries the REAL establisher frame pointer and a
 *   real chain depth (captured by lib$establish via the compiler's frame
 *   builtins), not the NULL/placeholder values the previous handler-stack
 *   emulation reported.
 *
 *   rung-2 (this change): true machine-frame-transfer SYS$UNWIND. When a
 *   handler calls sys$unwind(depadr, newpc) naming a target frame that has
 *   armed a resume anchor (VMS$UNWIND_ANCHOR, see chfdef.h), the dispatcher
 *   defers the unwind (as real VMS does - $UNWIND takes effect when the
 *   handler returns to the dispatcher), then walks the chain from the
 *   requesting handler's frame outward to the target: it calls each
 *   intervening established handler ONCE with CHF$V_UNWINDING set, pops the
 *   chain, and TRANSFERS control (setjmp/longjmp) to the target frame's
 *   armed anchor - abandoning the intervening machine frames, honouring
 *   newpc. When no anchor is armed for the target (or depadr is NULL /
 *   sys$unwind is called outside an active dispatch) the historical
 *   pop-only contract is preserved for source/behaviour compatibility
 *   (test_lib_fb3). See docs/design-chf-condition-handling.md rung-2.
 *
 *   Host scope note: the target frame must have armed a resume anchor. The
 *   real Alpha machine invocation-context walk (resuming into an ancestor
 *   frame that armed no anchor, e.g. a bare caller of the establisher) is
 *   rung-3 (vms-1fa, LIB$GET_INVO_*); until then a target frame declares
 *   its resumability with VMS$UNWIND_ANCHOR().
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include "ssdef.h"
#include "stsdef.h"
#include "descrip.h"
#include "chfdef.h"
#include "libicb.h"
#include "pdscdef.h"
#include "lib$routines.h"

/* Imported from status.c */
extern uint32_t vms$format_status(uint32_t status, char *buf, size_t buflen);
extern const char *vms$status_message(uint32_t code);

/* Imported from syssvc/sys_setexv.c - the SYS$SETEXV vector table. */
extern void *vms$$exc_vector_get(int which);

/* ================================================================
 * Thread-local frame-handler chain
 *
 * Each entry records not just the handler routine but the REAL call
 * frame that established it, so the mechanism array handed to the
 * handler carries a genuine establisher frame pointer and depth rather
 * than the placeholder NULL/handler_count the old emulation reported.
 * ================================================================ */

#define MAX_HANDLERS 64

struct handler_record {
    chf$handler_t   handler;        /* established handler routine */
    void           *est_frame;      /* real frame address of establisher */
    void           *est_pc;         /* return address into establisher */
    int             active;         /* re-entrancy guard: handler running */
    int             has_anchor;     /* rung-2: resume_ctx armed for transfer */
    jmp_buf         resume_ctx;     /* rung-2: setjmp'd in establisher frame */
};

static _Thread_local struct handler_record handler_stack[MAX_HANDLERS];
static _Thread_local int handler_count = 0;

/* ================================================================
 * rung-2: deferred-unwind request state (thread-local).
 *
 * Real VMS SYS$UNWIND does not transfer control immediately; it marks the
 * unwind and the dispatcher performs it when the handler returns. We mirror
 * that: sys$unwind records the request here and returns to the handler; the
 * dispatcher (dispatch_condition) acts on it after the handler returns.
 * ================================================================ */

static _Thread_local int   uw_dispatching = 0;  /* inside dispatch_condition */
static _Thread_local int   uw_pending     = 0;  /* an unwind was requested */
static _Thread_local int   uw_target      = 0;  /* target depth (handlers kept) */
static _Thread_local void *uw_newpc       = NULL;
static _Thread_local int   uw_requester   = -1; /* index of requesting handler */

/* ================================================================
 * lib$establish - Establish a condition handler
 *
 * Registers a condition handler for the current call frame and captures
 * the establisher's real frame address and return PC so the mechanism
 * array can report them. Handlers form a thread-local LIFO chain.
 *
 * Returns the address of the previously established handler (or NULL).
 * ================================================================ */

void *lib$establish(void *handler) {
    if (handler_count >= MAX_HANDLERS) {
        /* Chain overflow - can't establish more handlers */
        fprintf(stderr, "%%SYSTEM-F-EXASTLM, exceeded AST limit\n");
        return NULL;
    }

    void *previous = (handler_count > 0)
                     ? (void *)handler_stack[handler_count - 1].handler
                     : NULL;

    /* Capture the REAL establisher context. lib$establish is a genuine
     * (non-inlined, exported) function, so frame-address(1)/return-
     * address(0) name the caller - the frame that is establishing the
     * handler. These populate the mechanism array's establisher frame
     * and let a handler's frame pointer be non-fabricated. */
    void *est_frame = __builtin_frame_address(1);
    void *est_pc    = __builtin_return_address(0);

    handler_stack[handler_count].handler    = (chf$handler_t)handler;
    handler_stack[handler_count].est_frame  = est_frame;
    handler_stack[handler_count].est_pc     = est_pc;
    handler_stack[handler_count].active     = 0;
    handler_stack[handler_count].has_anchor = 0;
    handler_count++;

    return previous;
}

/* ================================================================
 * lib$revert - Remove the most recently established handler.
 * ================================================================ */

uint32_t lib$revert(void) {
    if (handler_count > 0) {
        handler_count--;
    }
    return SS$_NORMAL;
}

/* ================================================================
 * Signal-array construction from lib$signal / lib$stop varargs.
 *
 * VMS convention: lib$signal(cond, num_fao_args, arg1, ..., argN).
 * The first vararg is the count of FAO arguments that follow.
 * ================================================================ */

static void build_signal_array(struct chf$signal_array *sigarray,
                               uint32_t condition, va_list ap)
{
    sigarray->chf$is_sig_name = condition;
    sigarray->chf$is_sig_args = 1;  /* the condition value itself */

    uint32_t num_fao = va_arg(ap, uint32_t);
    if (num_fao > 29) num_fao = 29;  /* cap to signal-array capacity */

    uint32_t *arg_ptr = &sigarray->chf$is_sig_arg1;
    for (uint32_t i = 0; i < num_fao; i++) {
        *arg_ptr++ = va_arg(ap, uint32_t);
        sigarray->chf$is_sig_args++;
    }
}

/* ================================================================
 * invoke_vector - call a SYS$SETEXV software exception vector, if any.
 *
 * Returns the handler's status (SS$_CONTINUE / SS$_RESIGNAL / other) or,
 * when no handler is established for that vector, SS$_RESIGNAL so the
 * dispatcher moves on to the next stage.
 * ================================================================ */

static uint32_t invoke_vector(int which,
                              struct chf$signal_array *sigarray,
                              struct chf$mech_array *mecharray)
{
    chf$handler_t vec = (chf$handler_t)vms$$exc_vector_get(which);
    if (vec == NULL) {
        return SS$_RESIGNAL;
    }
    /* The vectored handler sees a mechanism array whose establisher frame
     * is the vector itself (there is no call frame that "established" a
     * SYS$SETEXV vector); report the dispatcher context rather than a
     * fabricated one. */
    mecharray->chf$ph_mch_frame = NULL;
    mecharray->chf$is_mch_depth = (uint32_t)handler_count;
    return vec(sigarray, mecharray);
}

/* ================================================================
 * dispatch_condition - the authentic CHF search.
 *
 * Walks, in order: primary vector -> frame-handler chain (innermost to
 * outermost) -> secondary vector -> last-chance vector. Returns
 *   SS$_CONTINUE : some handler claimed the condition (resume)
 *   SS$_RESIGNAL : nobody claimed it (caller applies default handling)
 * ================================================================ */

/* ================================================================
 * perform_unwind - execute a deferred SYS$UNWIND request (rung-2).
 *
 * Called by the dispatcher once the handler that requested the unwind has
 * returned. Walks the frame-handler chain from the requesting handler's
 * frame outward toward the target depth, calling each intervening,
 * not-currently-active established handler ONCE with CHF$V_UNWINDING set so
 * it can release resources, then pops the chain to the target depth and
 * TRANSFERS control to the target frame's armed resume anchor
 * (setjmp/longjmp) - abandoning the intervening machine frames. Does not
 * return when an anchor is armed (longjmp). Returns normally (pop-only)
 * when the target frame armed no anchor.
 * ================================================================ */

static void perform_unwind(struct chf$signal_array *sigarray)
{
    int target    = uw_target;
    int requester = uw_requester;

    /* Snapshot the target frame's resume anchor BEFORE popping. The record
     * memory in handler_stack[] persists across the pop (only the count is
     * decremented), so the jmp_buf stays valid to longjmp into. */
    jmp_buf *anchor = NULL;
    if (target >= 0 && target < handler_count && handler_stack[target].has_anchor) {
        anchor = &handler_stack[target].resume_ctx;
    }

    /* Call each intervening handler once in unwind mode: from the frame just
     * below the requester's frame down to (but not including) the target
     * frame, which survives. The requester itself is skipped (it is the
     * running handler and its frame is being abandoned by the transfer). */
    struct chf$mech_array mech;
    memset(&mech, 0, sizeof(mech));
    mech.chf$is_mch_args  = 5;
    mech.chf$is_mch_flags = CHF$M_UNWINDING;   /* CHF$V_UNWINDING set */

    for (int i = requester - 1; i > target; i--) {
        struct handler_record *rec = &handler_stack[i];
        if (rec->handler == NULL || rec->active) {
            continue;
        }
        mech.chf$ph_mch_frame = rec->est_frame;
        mech.chf$is_mch_depth = (uint32_t)i;
        mech.chf$is_mch_flags = CHF$M_UNWINDING;
        rec->active = 1;
        (void)rec->handler(sigarray, &mech);   /* return value ignored in unwind */
        rec->active = 0;
    }

    /* Pop the chain down to the target depth (the abandoned frames' handlers
     * are gone; the target frame will re-establish if it wants one). */
    if (target < 0) target = 0;
    while (handler_count > target) {
        handler_count--;
    }

    /* Clear the pending state before transferring. */
    void *newpc = uw_newpc;
    uw_pending   = 0;
    uw_requester = -1;
    uw_newpc     = NULL;

    if (anchor) {
        /* Transfer control to the target frame. newpc is stashed so the
         * resume site can read it via vms$$unwind_newpc(); longjmp delivers
         * a fixed non-zero token so the VMS$UNWIND_ANCHOR() setjmp returns
         * non-zero (0 is reserved for the arming call). */
        uw_newpc = newpc;   /* readable at the resume site */
        longjmp(*anchor, 1);
        /* not reached */
    }

    /* rung-3 (vms-1fa): the target frame armed NO resume anchor - the literal
     * "return to main", a bare caller of the establisher, a libgcc EH landing
     * pad. Reconstruct that frame's real saved context by walking the genuine
     * Alpha invocation chain (procedure descriptors + register save areas) to
     * the frame whose PC matches the target establisher's return PC, then ask
     * vms$$invo_transfer() to restore its registers and resume there. On the
     * Alpha runtime this IS the anchorless transfer; on the host there is no
     * Alpha machine context to restore into, so vms$$invo_transfer() reports
     * "not transferred" and the rung-1 pop-only unwind (already done above)
     * stands. The reconstruction itself is host-proven (test_invo_context). */
    if (target >= 0 && target < MAX_HANDLERS && handler_stack[target].est_pc) {
        INVO_CONTEXT_BLK ticb;
        uint64_t target_pc =
            (uint64_t)(uintptr_t)handler_stack[target].est_pc;
        if (vms$$invo_reconstruct_target(target_pc, &ticb) == SS$_NORMAL) {
            uw_newpc = newpc;               /* readable at the resume site */
            if (vms$$invo_transfer(&ticb, newpc)) {
                /* not reached: control resumed in the reconstructed frame */
            }
            /* else: fall through to the pop-only contract below. */
        }
    }
    /* No anchor armed: pop-only unwind already done above (rung-1 contract). */
    (void)newpc;
}

static uint32_t dispatch_condition(struct chf$signal_array *sigarray)
{
    struct chf$mech_array mecharray;

    memset(&mecharray, 0, sizeof(mecharray));
    mecharray.chf$is_mch_args  = 5;   /* standard mechanism-array size */
    mecharray.chf$is_mch_flags = 0;   /* not unwinding (rung-1) */
    mecharray.chf$ph_mch_frame = NULL;
    mecharray.chf$is_mch_depth = (uint32_t)handler_count;
    mecharray.chf$is_mch_savr0 = 0;
    mecharray.chf$is_mch_savr1 = 0;

    int prev_dispatching = uw_dispatching;
    uw_dispatching = 1;
    uw_requester = -1;   /* no frame handler running yet */

    /* 1. PRIMARY exception vector. */
    uint32_t r = invoke_vector(CHF$K_PRIMARY_VECTOR, sigarray, &mecharray);
    if (uw_pending) {
        perform_unwind(sigarray);   /* vector requested a transfer */
        uw_dispatching = prev_dispatching;
        return SS$_CONTINUE;
    }
    if (r == SS$_CONTINUE) {
        uw_dispatching = prev_dispatching;
        return SS$_CONTINUE;
    }

    /* 2. Established frame-handler chain, innermost (top) to outermost. */
    for (int i = handler_count - 1; i >= 0; i--) {
        struct handler_record *rec = &handler_stack[i];
        if (rec->handler == NULL || rec->active) {
            continue;   /* skip a handler already running (re-entrancy) */
        }

        /* Report the REAL establisher context for this frame. */
        mecharray.chf$ph_mch_frame = rec->est_frame;
        mecharray.chf$is_mch_depth = (uint32_t)i;
        mecharray.chf$is_mch_flags = 0;

        rec->active = 1;
        uw_requester = i;   /* frame the handler could unwind from */
        uint32_t result = rec->handler(sigarray, &mecharray);
        rec->active = 0;

        /* rung-2: the handler may have called sys$unwind, which deferred a
         * transfer. Perform it now (as real VMS does on handler return). If
         * an anchor is armed, perform_unwind() does not return (longjmp). */
        if (uw_pending) {
            perform_unwind(sigarray);
            /* pop-only fallthrough (no anchor): resume the search as CONTINUE
             * so the establishing frame regains control normally. */
            uw_dispatching = prev_dispatching;
            return SS$_CONTINUE;
        }

        if (result == SS$_CONTINUE) {
            uw_dispatching = prev_dispatching;
            return SS$_CONTINUE;
        }
        /* SS$_RESIGNAL (or anything else) -> next handler outward. */
    }
    uw_dispatching = prev_dispatching;

    /* 3. SECONDARY exception vector. */
    mecharray.chf$ph_mch_frame = NULL;
    r = invoke_vector(CHF$K_SECONDARY_VECTOR, sigarray, &mecharray);
    if (r == SS$_CONTINUE) {
        return SS$_CONTINUE;
    }

    /* 4. LAST-CHANCE exception vector. */
    r = invoke_vector(CHF$K_LAST_CHANCE_VECTOR, sigarray, &mecharray);
    if (r == SS$_CONTINUE) {
        return SS$_CONTINUE;
    }

    return SS$_RESIGNAL;   /* unhandled: caller applies default handling */
}

/* ================================================================
 * default_report - format and print an unhandled condition's message.
 * ================================================================ */

static void default_report(uint32_t condition)
{
    char buf[256];
    vms$format_status(condition, buf, sizeof(buf));
    fprintf(stderr, "%s\n", buf);

    const char *msg = vms$status_message(condition);
    if (msg) {
        fprintf(stderr, "  %s\n", msg);
    }
}

/* ================================================================
 * lib$signal - Signal a condition.
 *
 * Searches the handler chain + exception vectors. If nobody claims the
 * condition, applies default handling based on severity.
 * ================================================================ */

uint32_t lib$signal(uint32_t condition, ...) {
    va_list ap;
    uint32_t sigarray_buf[32];
    struct chf$signal_array *sigarray = (struct chf$signal_array *)sigarray_buf;

    va_start(ap, condition);
    build_signal_array(sigarray, condition, ap);
    va_end(ap);

    if (dispatch_condition(sigarray) == SS$_CONTINUE) {
        return SS$_NORMAL;   /* a handler claimed it */
    }

    /* No handler claimed the condition - default handling. */
    default_report(condition);

    uint32_t sev = $VMS_STATUS_SEVERITY(condition);
    if (sev == STS$K_SEVERE) {
        exit(condition);     /* severe/fatal: terminate the image */
    }
    /* error/warning/info/success: message printed, execution continues */

    return SS$_NORMAL;
}

/* ================================================================
 * lib$stop - Signal a condition and force process exit.
 *
 * Like lib$signal but always terminates the process if no handler
 * claims the condition, regardless of severity.
 * ================================================================ */

uint32_t lib$stop(uint32_t condition, ...) {
    va_list ap;
    uint32_t sigarray_buf[32];
    struct chf$signal_array *sigarray = (struct chf$signal_array *)sigarray_buf;

    va_start(ap, condition);
    build_signal_array(sigarray, condition, ap);
    va_end(ap);

    if (dispatch_condition(sigarray) == SS$_CONTINUE) {
        return SS$_NORMAL;   /* a handler claimed it */
    }

    /* No handler claimed it - lib$stop always exits. */
    default_report(condition);
    exit(condition);
    return condition;        /* never reached */
}

/* ================================================================
 * lib$sig_to_ret - Convert a signalled condition into a return status.
 *
 * A handler that stores the condition value in the mechanism array's
 * saved-R0 field and returns SS$_CONTINUE, so the establishing function
 * returns the condition instead of the signal propagating.
 * ================================================================ */

uint32_t lib$sig_to_ret(void *signal_args, void *mechanism_args) {
    struct chf$signal_array *sigarray = (struct chf$signal_array *)signal_args;
    struct chf$mech_array *mecharray = (struct chf$mech_array *)mechanism_args;

    if (sigarray == NULL || mecharray == NULL) {
        return SS$_RESIGNAL;
    }

    mecharray->chf$is_mch_savr0 = sigarray->chf$is_sig_name;
    return SS$_CONTINUE;
}

/* ================================================================
 * Internal accessors for SYS$UNWIND (sys_condition.c)
 *
 * SYS$UNWIND pops handlers off this file's thread-local chain down to a
 * target depth. See the sys$unwind doc comment in starlet.h and rung-2
 * in docs/design-chf-condition-handling.md for the machine-frame-
 * transfer this does NOT yet perform.
 * ================================================================ */

int vms$$handler_depth(void) {
    return handler_count;
}

int vms$$handler_unwind_to(int target_depth) {
    if (target_depth < 0) target_depth = 0;
    while (handler_count > target_depth) {
        handler_count--;
    }
    return handler_count;
}

/* ================================================================
 * rung-2 frame-transfer SYS$UNWIND support (used by sys_condition.c and
 * the VMS$UNWIND_ANCHOR() macro in chfdef.h).
 * ================================================================ */

/*
 * vms$$unwind_anchor_buf - arm a resume anchor for the current innermost
 * (most recently established) frame handler and hand back its jmp_buf so
 * the caller's VMS$UNWIND_ANCHOR() macro can setjmp into it, IN the
 * establisher's own frame. Returns a throwaway static buffer if no handler
 * is established (the setjmp is then a harmless no-op that never receives a
 * transfer).
 */
void *vms$$unwind_anchor_buf(void)
{
    static _Thread_local jmp_buf dummy;
    if (handler_count <= 0) {
        return &dummy;
    }
    struct handler_record *rec = &handler_stack[handler_count - 1];
    rec->has_anchor = 1;
    return &rec->resume_ctx;
}

/*
 * vms$$unwind_newpc - the newpc argument passed to the SYS$UNWIND that
 * transferred control to the current resume site (NULL if none / newpc==0).
 * A resume site reads this after VMS$UNWIND_ANCHOR() returns non-zero to
 * honour the target PC selection.
 */
void *vms$$unwind_newpc(void)
{
    return uw_newpc;
}

/*
 * vms$$unwind_request - record a deferred SYS$UNWIND (called by sys$unwind).
 *
 * Deferred-unwind model (real VMS): the transfer happens when the handler
 * returns to the dispatcher, not here. We only decide whether this call is a
 * real frame-transfer request (armed target anchor + inside an active
 * dispatch) or the historical pop-only contract, then either arm the pending
 * state or pop immediately.
 *
 *   depadr == NULL : pop one level (rung-1 contract), never a transfer.
 *   depadr != NULL : target depth = *depadr. If a frame handler at that depth
 *                    has an armed anchor and we are inside a dispatch, defer a
 *                    real frame transfer; otherwise pop to the target depth.
 */
uint32_t vms$$unwind_request(const uint32_t *depadr, void *newpc)
{
    int current = handler_count;
    int target;

    if (depadr) {
        target = (int)*depadr;
        if (target < 0) target = 0;
        if (target > current) target = current;   /* cannot unwind outward */
    } else {
        target = (current > 0) ? current - 1 : 0;  /* pop one level */
    }

    int can_transfer =
        uw_dispatching && depadr &&
        target >= 0 && target < current &&
        handler_stack[target].has_anchor;

    if (can_transfer) {
        /* Defer: the dispatcher performs the transfer when the requesting
         * handler returns (perform_unwind). */
        uw_pending   = 1;
        uw_target    = target;
        uw_newpc     = newpc;
        /* uw_requester is set by the dispatcher to the running frame index. */
        return SS$_NORMAL;
    }

    /* Pop-only contract (no armed target / NULL depadr / outside dispatch). */
    vms$$handler_unwind_to(target);
    return SS$_NORMAL;
}
