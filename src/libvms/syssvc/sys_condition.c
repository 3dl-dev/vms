/*
 * sys_condition.c - Condition Handling System Services
 *
 * Implements SYS$UNWIND, the system-service counterpart to the LIB$
 * condition-handling routines (lib$establish/lib$signal/lib$stop/
 * lib$revert - see rtl/lib_signal.c).
 *
 * See the sys$unwind doc comment in starlet.h for the honest
 * simplification this implementation makes: OVMX's condition-handling
 * model invokes established handlers as ordinary in-process function
 * calls from within lib$signal/lib$stop rather than a hardware
 * exception dispatch with saved per-frame register/PC state, so there
 * is no machine frame to transfer control to at an arbitrary newpc.
 * What this DOES do, matching real SYS$UNWIND's documented side
 * effect, is pop condition handlers off the handler chain maintained
 * by lib_signal.c down to the target depth.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$unwind (vms-5b4) -- unwinds the process-local handler
 *     stack kept in handler_count/handler_stack in src/libvms/rtl/lib_signal.c;
 *     there is no executive condition-dispatch frame, so nothing outside this
 *     process participates in or observes the unwind.
 */

#include <stdint.h>
#include "starlet.h"

/* Imported from rtl/lib_signal.c - see the doc comment there. */
extern int vms$$handler_depth(void);
extern int vms$$handler_unwind_to(int target_depth);

/*
 * sys$unwind - Unwind the condition-handler chain to a target depth.
 */
uint32_t sys$unwind(const uint32_t *depadr, void *newpc)
{
    (void)newpc;  /* no machine frame to transfer control to - see above */

    int current = vms$$handler_depth();
    int target;

    if (depadr) {
        target = (int)*depadr;
        if (target < 0) target = 0;
        if (target > current) target = current;  /* can't unwind outward */
    } else {
        /* NULL depadr: unwind one level (pop the currently executing
         * handler, returning to the frame that established it). */
        target = (current > 0) ? current - 1 : 0;
    }

    vms$$handler_unwind_to(target);

    return SS$_NORMAL;
}
