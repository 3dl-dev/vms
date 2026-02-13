/*
 * lib_signal.c - LIB$SIGNAL / LIB$STOP / LIB$SIG_TO_RET
 *
 * VMS condition handling routines. On VMS, conditions are signalled
 * and can be caught by condition handlers established on the call
 * stack. This implementation formats and prints the condition message
 * and aborts for severe/fatal conditions.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "ssdef.h"
#include "stsdef.h"
#include "descrip.h"
#include "lib$routines.h"

/* Imported from status.c */
extern uint32_t vms$format_status(uint32_t status, char *buf, size_t buflen);
extern const char *vms$status_message(uint32_t code);

/*
 * lib$signal - Signal a condition.
 *
 * Formats the condition value as a human-readable message and prints
 * it to stderr. If the severity is fatal (STS$K_SEVERE), the process
 * is aborted. For error severity, the message is printed but execution
 * continues (a real VMS system would search for condition handlers).
 *
 * Parameters:
 *   condition_value - VMS condition code
 *   ...            - Additional FAO arguments (ignored for now)
 */
uint32_t lib$signal(uint32_t condition, ...) {
    char buf[256];
    vms$format_status(condition, buf, sizeof(buf));

    const char *msg = vms$status_message(condition);
    fprintf(stderr, "%s\n", buf);
    if (msg) {
        fprintf(stderr, "  %s\n", msg);
    }

    /* If severity is severe/fatal, abort the process */
    uint32_t sev = $VMS_STATUS_SEVERITY(condition);
    if (sev == STS$K_SEVERE) {
        exit(condition);
    }

    return SS$_NORMAL;
}

/*
 * lib$stop - Signal a condition and force process exit.
 *
 * Like lib$signal but always terminates the process regardless of
 * condition severity. This is used for unrecoverable errors.
 */
uint32_t lib$stop(uint32_t condition, ...) {
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

/*
 * lib$sig_to_ret - Convert a signal to a return status.
 *
 * In a VMS condition handler, this routine converts the signalled
 * condition into a return status for the caller. This is a stub
 * implementation that simply returns SS$_NORMAL.
 */
uint32_t lib$sig_to_ret(void *signal_args, void *mechanism_args) {
    (void)signal_args;
    (void)mechanism_args;
    /* In a full implementation, this would extract the condition
     * value from signal_args and set it as the return value in
     * mechanism_args. */
    return SS$_NORMAL;
}
