/*
 * lib_signal.c - LIB$SIGNAL / LIB$STOP / LIB$SIG_TO_RET
 *
 * VMS condition handling routines. Implements the full Condition
 * Handling Facility (CHF) with a thread-local handler chain.
 *
 * On VMS, conditions are signalled via lib$signal() and can be caught
 * by condition handlers established on the call stack. Each handler
 * receives signal and mechanism arrays and can choose to:
 *   - SS$_RESIGNAL: pass to the next handler in the chain
 *   - SS$_CONTINUE: resume execution after the signal
 *
 * This implementation maintains a thread-local stack of handlers and
 * walks the chain when conditions are signaled.
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

/* ================================================================
 * Thread-local handler stack
 * ================================================================ */

#define MAX_HANDLERS 64

static _Thread_local chf$handler_t handler_stack[MAX_HANDLERS];
static _Thread_local int handler_count = 0;

/* ================================================================
 * lib$establish - Establish a condition handler
 *
 * Registers a condition handler for the current call frame.
 * Handlers are stored in a thread-local LIFO stack.
 *
 * Parameters:
 *   handler - Pointer to handler function
 *
 * Returns:
 *   Address of previously established handler (or NULL if none)
 *
 * The handler function signature is:
 *   uint32_t handler(chf$signal_array *sig, chf$mech_array *mech)
 * ================================================================ */

void *lib$establish(void *handler) {
    if (handler_count >= MAX_HANDLERS) {
        /* Stack overflow - can't establish more handlers */
        fprintf(stderr, "%%SYSTEM-F-EXASTLM, exceeded AST limit\n");
        return NULL;
    }

    /* Save the previous handler (top of stack, or NULL) */
    void *previous = (handler_count > 0)
                     ? (void *)handler_stack[handler_count - 1]
                     : NULL;

    /* Push new handler onto stack */
    handler_stack[handler_count++] = (chf$handler_t)handler;

    return previous;
}

/* ================================================================
 * lib$revert - Revert to previous condition handler
 *
 * Removes the most recently established handler from the stack.
 *
 * Returns:
 *   SS$_NORMAL on success
 * ================================================================ */

uint32_t lib$revert(void) {
    if (handler_count > 0) {
        handler_count--;
    }
    return SS$_NORMAL;
}

/* ================================================================
 * lib$signal - Signal a condition
 *
 * Signals a condition by invoking the handler chain. If no handler
 * claims the condition, a default handler formats and prints the
 * message and may exit the process depending on severity.
 *
 * Parameters:
 *   condition - The condition value (SS$_ code)
 *   ...       - Optional FAO arguments for message formatting
 *
 * Returns:
 *   SS$_NORMAL if a handler claimed the condition (SS$_CONTINUE)
 *   Does not return if severity is SEVERE and no handler claimed it
 * ================================================================ */

uint32_t lib$signal(uint32_t condition, ...) {
    va_list ap;
    uint32_t sigarray_buf[32];  /* Signal array storage */
    struct chf$signal_array *sigarray = (struct chf$signal_array *)sigarray_buf;
    struct chf$mech_array mecharray;

    /* Build signal array from variadic arguments.
     * VMS convention: lib$signal(cond, num_fao_args, arg1, ..., argN)
     * The first vararg is the count of FAO arguments that follow.
     * If called with just a condition (no extra args), num_fao_args is 0. */
    va_start(ap, condition);

    sigarray->chf$is_sig_name = condition;
    sigarray->chf$is_sig_args = 1;  /* Start with condition itself */

    uint32_t num_fao = va_arg(ap, uint32_t);
    if (num_fao > 29) num_fao = 29;  /* Cap to buffer capacity */

    uint32_t *arg_ptr = &sigarray->chf$is_sig_arg1;
    for (uint32_t i = 0; i < num_fao; i++) {
        *arg_ptr++ = va_arg(ap, uint32_t);
        sigarray->chf$is_sig_args++;
    }

    va_end(ap);

    /* Build mechanism array (simplified) */
    memset(&mecharray, 0, sizeof(mecharray));
    mecharray.chf$is_mch_args = 5;  /* Standard size */
    mecharray.chf$is_mch_flags = 0;
    mecharray.chf$ph_mch_frame = NULL;
    mecharray.chf$is_mch_depth = handler_count;
    mecharray.chf$is_mch_savr0 = 0;
    mecharray.chf$is_mch_savr1 = 0;

    /* Walk the handler chain from top to bottom */
    for (int i = handler_count - 1; i >= 0; i--) {
        chf$handler_t handler = handler_stack[i];
        if (handler == NULL) {
            continue;
        }

        /* Invoke handler */
        uint32_t result = handler(sigarray, &mecharray);

        if (result == SS$_CONTINUE) {
            /* Handler claimed the condition - resume execution */
            return SS$_NORMAL;
        } else if (result == SS$_RESIGNAL) {
            /* Try next handler */
            continue;
        }
        /* Any other return value: treat as resignal */
    }

    /* No handler claimed the condition - default handling */
    uint32_t sev = $VMS_STATUS_SEVERITY(condition);
    char buf[256];

    vms$format_status(condition, buf, sizeof(buf));
    fprintf(stderr, "%s\n", buf);

    const char *msg = vms$status_message(condition);
    if (msg) {
        fprintf(stderr, "  %s\n", msg);
    }

    /* Action based on severity */
    switch (sev) {
        case STS$K_SEVERE:
            /* Severe/fatal error - exit immediately */
            exit(condition);
            break;

        case STS$K_ERROR:
            /* Error - message already printed, continue execution */
            break;

        case STS$K_WARNING:
        case STS$K_INFO:
            /* Warning/info - message already printed, continue */
            break;

        case STS$K_SUCCESS:
            /* Success - no message needed, just continue */
            break;

        default:
            /* Unknown severity - treat as error */
            break;
    }

    return SS$_NORMAL;
}

/* ================================================================
 * lib$stop - Signal a condition and force process exit
 *
 * Like lib$signal but always terminates the process if no handler
 * claims the condition, regardless of severity. This is used for
 * unrecoverable errors.
 *
 * Parameters:
 *   condition - The condition value
 *   ...       - Optional FAO arguments
 *
 * Returns:
 *   Does not return unless a handler claims the condition
 * ================================================================ */

uint32_t lib$stop(uint32_t condition, ...) {
    va_list ap;
    uint32_t sigarray_buf[32];
    struct chf$signal_array *sigarray = (struct chf$signal_array *)sigarray_buf;
    struct chf$mech_array mecharray;

    /* Build signal array from variadic arguments.
     * VMS convention: lib$stop(cond, num_fao_args, arg1, ..., argN) */
    va_start(ap, condition);

    sigarray->chf$is_sig_name = condition;
    sigarray->chf$is_sig_args = 1;

    uint32_t num_fao = va_arg(ap, uint32_t);
    if (num_fao > 29) num_fao = 29;

    uint32_t *arg_ptr = &sigarray->chf$is_sig_arg1;
    for (uint32_t i = 0; i < num_fao; i++) {
        *arg_ptr++ = va_arg(ap, uint32_t);
        sigarray->chf$is_sig_args++;
    }

    va_end(ap);

    /* Build mechanism array */
    memset(&mecharray, 0, sizeof(mecharray));
    mecharray.chf$is_mch_args = 5;
    mecharray.chf$is_mch_flags = 0;
    mecharray.chf$ph_mch_frame = NULL;
    mecharray.chf$is_mch_depth = handler_count;
    mecharray.chf$is_mch_savr0 = 0;
    mecharray.chf$is_mch_savr1 = 0;

    /* Walk the handler chain */
    for (int i = handler_count - 1; i >= 0; i--) {
        chf$handler_t handler = handler_stack[i];
        if (handler == NULL) {
            continue;
        }

        uint32_t result = handler(sigarray, &mecharray);

        if (result == SS$_CONTINUE) {
            /* Handler claimed it - return normally */
            return SS$_NORMAL;
        }
    }

    /* No handler claimed it - always exit for lib$stop */
    char buf[256];
    vms$format_status(condition, buf, sizeof(buf));
    fprintf(stderr, "%s\n", buf);

    const char *msg = vms$status_message(condition);
    if (msg) {
        fprintf(stderr, "  %s\n", msg);
    }

    exit(condition);
    return condition;  /* Never reached */
}

/* ================================================================
 * lib$sig_to_ret - Convert signal to return status
 *
 * A condition handler that converts a signaled condition into
 * a return status for the caller. The handler extracts the
 * condition value from the signal array and stores it in the
 * mechanism array's saved R0 field.
 *
 * Usage pattern:
 *   lib$establish(lib$sig_to_ret);
 *   status = some_function();  // If this signals, handler converts to return
 *   lib$revert();
 *   // Now 'status' contains the condition value
 *
 * Parameters:
 *   signal_args    - Pointer to signal array
 *   mechanism_args - Pointer to mechanism array
 *
 * Returns:
 *   SS$_CONTINUE to resume execution with the return value set
 * ================================================================ */

uint32_t lib$sig_to_ret(void *signal_args, void *mechanism_args) {
    struct chf$signal_array *sigarray = (struct chf$signal_array *)signal_args;
    struct chf$mech_array *mecharray = (struct chf$mech_array *)mechanism_args;

    if (sigarray == NULL || mecharray == NULL) {
        return SS$_RESIGNAL;
    }

    /* Extract the condition value from the signal array */
    uint32_t condition = sigarray->chf$is_sig_name;

    /* Store it as the return value in the mechanism array */
    mecharray->chf$is_mch_savr0 = condition;

    /* Continue execution - the calling function will return the condition */
    return SS$_CONTINUE;
}
