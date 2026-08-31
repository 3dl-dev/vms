/*
 * sys_setexv.c - SYS$SETEXV: Set Exception Vector (vms-2e72 rung-1)
 *
 * Establishes one of the three per-access-mode software exception
 * vectors that the OpenVMS condition dispatcher consults around the
 * call-frame handler search:
 *
 *   CHF$K_PRIMARY_VECTOR      - searched BEFORE the frame-handler chain
 *   CHF$K_SECONDARY_VECTOR    - searched AFTER  the frame-handler chain
 *   CHF$K_LAST_CHANCE_VECTOR  - the final handler before the catch-all
 *
 * This is the authentic exception-vector half of the Condition Handling
 * Facility (CHF). Before this service the OVMX dispatcher had ONLY the
 * lib$establish frame-handler chain (rtl/lib_signal.c) and no notion of
 * the vectored handlers a debugger, a run-time library, or a program
 * that wants a process-wide "last chance" handler relies on. The
 * dispatcher in lib_signal.c now walks these vectors in the authentic
 * order (see docs/design-chf-condition-handling.md).
 *
 * FAITHFULNESS NOTES
 *   - Real SYS$SETEXV takes an access-mode argument and keeps a
 *     separate vector per access mode (a more-privileged mode's vector
 *     is reached before a less-privileged one). OVMX condition handling
 *     is a single-mode userspace model today, so acmode is validated
 *     and recorded but only the effective (least-privileged) slot
 *     participates in dispatch. This is the same honest single-mode
 *     simplification the rest of the executive userspace surface makes;
 *     it is NOT a per-process fake of a shared facility (Rule 9 / INV-6)
 *     - the vectors are real, consulted by the real dispatcher, and
 *     observable by their return effect.
 *   - The vectors are thread-local, mirroring the thread-local
 *     lib$establish chain in lib_signal.c. On real VMS the exception
 *     vectors are per-access-mode and per-process; a future rung that
 *     moves condition dispatch into the executive (/dev/vms) will move
 *     these with it.
 *
 * Reference: OpenVMS System Services Reference Manual, $SETEXV;
 *            OpenVMS Calling Standard, "Exception Vectors".
 */

#include <stdint.h>
#include "starlet.h"
#include "ssdef.h"
#include "chfdef.h"

/*
 * Thread-local software exception vectors. Index by CHF$K_*_VECTOR.
 * NULL means "no handler established for this vector".
 */
static _Thread_local void *exc_vector[CHF$K_VECTOR_COUNT] = { 0, 0, 0 };

/*
 * sys$setexv - Set (or clear) a software exception vector.
 *
 * @param vector  Which vector: CHF$K_PRIMARY_VECTOR / _SECONDARY_ /
 *                _LAST_CHANCE_VECTOR.
 * @param addres  New handler address, or NULL to clear the vector.
 *                Handler prototype is the standard
 *                uint32_t h(struct chf$signal_array *,
 *                           struct chf$mech_array *).
 * @param acmode  Access mode to associate (validated; recorded for the
 *                honest single-mode model - see file header).
 * @param prvhnd  Optional out-parameter; receives the previously
 *                established handler for this vector (NULL if none).
 *
 * @return SS$_NORMAL on success; SS$_BADPARAM for an out-of-range
 *         vector selector.
 */
uint32_t sys$setexv(uint32_t vector, void *addres, uint32_t acmode,
                    void **prvhnd)
{
    (void)acmode;   /* single-mode model - see file header */

    if (vector >= (uint32_t)CHF$K_VECTOR_COUNT) {
        if (prvhnd) {
            *prvhnd = (void *)0;
        }
        return SS$_BADPARAM;
    }

    void *previous = exc_vector[vector];
    exc_vector[vector] = addres;

    if (prvhnd) {
        *prvhnd = previous;
    }

    return SS$_NORMAL;
}

/*
 * vms$$exc_vector_get - internal accessor for the condition dispatcher
 * in rtl/lib_signal.c. Returns the handler currently established for
 * the given vector (NULL if none). This mirrors the vms$$handler_depth
 * / vms$$handler_unwind_to internal-helper convention used to share the
 * lib_signal.c handler chain with sys_condition.c.
 */
void *vms$$exc_vector_get(int which)
{
    if (which < 0 || which >= CHF$K_VECTOR_COUNT) {
        return (void *)0;
    }
    return exc_vector[which];
}
