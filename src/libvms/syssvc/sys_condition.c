/*
 * sys_condition.c - Condition Handling System Services
 *
 * Implements SYS$UNWIND, the system-service counterpart to the LIB$
 * condition-handling routines (lib$establish/lib$signal/lib$stop/
 * lib$revert - see rtl/lib_signal.c).
 *
 * rung-2 (vms-8802, docs/design-chf-condition-handling.md): SYS$UNWIND is
 * now a real machine-frame-transfer unwind. Like real VMS, it does NOT
 * transfer control immediately - it records a deferred unwind request and
 * returns to the calling handler; the CHF dispatcher (lib_signal.c) performs
 * the transfer when the handler returns, running each intervening handler
 * once with CHF$V_UNWINDING set and longjmp-ing to the target frame's armed
 * resume anchor (VMS$UNWIND_ANCHOR), honouring newpc, abandoning the
 * intervening machine frames.
 *
 * Compatibility: when depadr is NULL, or the named target frame armed no
 * resume anchor, or sys$unwind is called outside an active dispatch, the
 * historical pop-only handler-chain contract is preserved (rung-1 /
 * test_lib_fb3). The resume-into-an-un-anchored ancestor frame case (the
 * real Alpha invocation-context walk) is rung-3 (vms-1fa).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$unwind (vms-f90) -- records a deferred unwind against
 *     the process-local frame-handler chain kept in lib_signal.c and, on the
 *     handler's return, transfers control (setjmp/longjmp) to the target
 *     frame's armed anchor. There is no executive condition-dispatch frame,
 *     so nothing outside this process participates in or observes the unwind.
 */

#include <stdint.h>
#include "starlet.h"

/* Imported from rtl/lib_signal.c - see the doc comments there. */
extern uint32_t vms$$unwind_request(const uint32_t *depadr, void *newpc);

/*
 * sys$unwind - Unwind the condition-handler chain, transferring control to
 *              a target frame (rung-2).
 *
 * Records the deferred unwind (target depth from depadr, newpc) and returns
 * SS$_NORMAL. The actual frame transfer is performed by the dispatcher when
 * the requesting handler returns. See the file comment above and
 * lib_signal.c:perform_unwind / vms$$unwind_request.
 */
uint32_t sys$unwind(const uint32_t *depadr, void *newpc)
{
    return vms$$unwind_request(depadr, newpc);
}
