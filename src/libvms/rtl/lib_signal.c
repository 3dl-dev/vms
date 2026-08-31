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
 *   rung-2 (NOT yet): true machine-frame-transfer SYS$UNWIND (honouring
 *   newpc and running intervening handlers in CHF$V_UNWINDING mode). Today
 *   handlers are still invoked as ordinary nested calls from within
 *   lib$signal, so an unwind pops the chain but does not abandon
 *   intervening machine frames. See the design doc for the plan.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "ssdef.h"
#include "stsdef.h"
#include "descrip.h"
#include "chfdef.h"
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
};

static _Thread_local struct handler_record handler_stack[MAX_HANDLERS];
static _Thread_local int handler_count = 0;

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

    handler_stack[handler_count].handler   = (chf$handler_t)handler;
    handler_stack[handler_count].est_frame = est_frame;
    handler_stack[handler_count].est_pc    = est_pc;
    handler_stack[handler_count].active    = 0;
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

    /* 1. PRIMARY exception vector. */
    uint32_t r = invoke_vector(CHF$K_PRIMARY_VECTOR, sigarray, &mecharray);
    if (r == SS$_CONTINUE) {
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
        uint32_t result = rec->handler(sigarray, &mecharray);
        rec->active = 0;

        if (result == SS$_CONTINUE) {
            return SS$_CONTINUE;
        }
        /* SS$_RESIGNAL (or anything else) -> next handler outward. */
    }

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
