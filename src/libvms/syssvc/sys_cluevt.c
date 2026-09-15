/*
 * sys_cluevt.c - Cluster-Event System Services ($SETCLUEVT / $CLRCLUEVT)
 *
 * A VMS process asks to be told when the cluster it belongs to gains or loses
 * a member: $SETCLUEVT arms a completion AST for a CLUEVT$C_ADD / CLUEVT$C_REMOVE
 * event, $CLRCLUEVT disarms it. On real VMS the connection manager queues the
 * AST from kernel context the moment a membership transition commits.
 *
 * THE REGISTRATION LIVES IN THE EXECUTIVE (src/kernel-core/vms_cnxman.c, reached
 * through /dev/vms by vms_kif_cluster_setcluevt in libvmssys, FC-P3.8). This
 * file holds NO cluster-event state of its own -- it is a translation layer and
 * nothing else: VMS arguments in, the kif wrapper's ioctl out, the executive's
 * SS$_ status back. The AST slot, its arming, the CNXMAN membership diff that
 * fires it and the teardown at process death are all the executive's. Like
 * sys_ast.c, there is NO per-process fallback here, ever -- not when /dev/vms
 * cannot be opened (the kif returns SS$_NOSUCHDEV and we return it), not "to
 * keep a unit test running". A registration the executive cannot honour is
 * refused, never faked (INV-6).
 *
 * WHAT THE EXECUTIVE DOES AND DOES NOT PROVIDE (said plainly). FC-P3.8's
 * registration is SINGLE-SLOT: one arming per process, one AST routine, one
 * event mask, last-arm-wins (src/kernel/vms_ioctl.h's
 * vms_cluster_setcluevt_args and vms_cnxman.c's cluevt_* slot). It has no
 * per-registration HANDLE and no TEST-OCCURRENCE query. So:
 *   - the VMS `handle` argument is accepted for source compatibility and, on a
 *     successful arm, written with the event code as an opaque cookie -- it is
 *     NOT a key into multiple concurrent registrations (the slot is single), and
 *     $CLRCLUEVT clears the process's one slot regardless of the handle it is
 *     given. A program that arms both ADD and REMOVE re-arms the one slot; that
 *     is the executive's real behaviour, not hidden here.
 *   - the VMS `acmode` argument is accepted and maximised to the caller's mode
 *     the way VMS does; the executive delivers the completion AST at user mode
 *     (FC-P3.8). It is not threaded through the kif, which has no acmode.
 *   - $TSTCLUEVT is deliberately NOT implemented: the executive has no
 *     test-occurrence query to back it, and answering "did an event occur"
 *     without one would be a fabricated status. It stays absent (and the
 *     tier1 corpus example that also calls it stays link-fail) until the
 *     executive grows a real query -- an honest omission, not a stub.
 *
 * OVMX service register (rd vms-d89) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-PARTIAL: sys$setcluevt (vms-733) -- exec: the one cluster-event AST
 *     registration is the executive's (vms_cnxman_cluevt_set via
 *     VMS_IOCTL_CLUSTER_SETCLUEVT), and the SS$_NOSUCHDEV refusal is proven to
 *     be the executive's cl->cnxman==NULL guard, not a userspace decision
 *     (test_syssvc_cluevt.c, negctl setcluevt-registers-without-cnxman).
 * OVMX-LOCAL: sys$setcluevt -- the returned handle is a source-compatibility
 *     cookie synthesized in this file (the executive's single-slot registration
 *     has no per-handle identity), and acmode is accepted but not carried to
 *     the executive; the full arm -> membership-change -> AST-delivered
 *     residency is UNPROVEN by the standalone harness (it needs a live cluster,
 *     vms-1ee's lab) -- the same self-limited proof sys$dclast declares.
 * OVMX-PARTIAL: sys$clrcluevt (vms-733) -- exec: disarms that same executive
 *     slot through the same ioctl (event_mask 0), returning the executive's
 *     status; this file keeps no slot of its own.
 * OVMX-LOCAL: sys$clrcluevt -- handle/event/acmode are accepted for source
 *     compatibility but not honoured for per-registration selection (the
 *     executive clears the one slot regardless), and no A-writes/B-reads test
 *     names the delivery path either.
 */
#include <stdint.h>
#include <stddef.h>

#include "starlet.h"
#include "ssdef.h"
#include "cluevtdef.h"
#include "vms_kif.h"

/*
 * sys$setcluevt - Set Cluster Event: arm (or, with astadr 0 / event 0, disarm)
 * the completion AST for a cluster membership event.
 *
 * Parameters:
 *   event  - CLUEVT$C_ADD or CLUEVT$C_REMOVE (the executive treats the value as
 *            the delivery mask; 0 disarms)
 *   astadr - AST routine to call on the event, carried to the executive as an
 *            opaque value (0 disarms)
 *   astprm - parameter passed to the AST routine
 *   acmode - access mode (accepted for source compatibility; the executive
 *            delivers at user mode -- see the file header)
 *   handle - optional quadword; on a successful arm it receives the event code
 *            as a source-compatibility cookie (single-slot -- see the header)
 *
 * Returns SS$_NORMAL on success, SS$_NOSUCHDEV when the connection manager is
 * not started, or the executive's status otherwise.
 */
uint32_t sys$setcluevt(unsigned int event,
                       void (*astadr)(void *),
                       uint64_t astprm,
                       unsigned int acmode,
                       unsigned int *handle) {
    uint32_t status;

    (void)acmode;   /* executive delivers at user mode (FC-P3.8) */

    status = vms_kif_cluster_setcluevt(event,
                                       (uint64_t)(uintptr_t)astadr,
                                       astprm);

    /* Populate the VMS output handle only on a real arm the executive
     * accepted -- an opaque single-slot cookie, never a fabricated success. */
    if (handle != NULL && status == SS$_NORMAL && event != 0u &&
        astadr != NULL) {
        handle[0] = event;
        handle[1] = 0u;
    }
    return status;
}

/*
 * sys$clrcluevt - Clear Cluster Event: disarm the process's cluster-event AST.
 *
 * The executive's registration is single-slot, so this clears that one slot
 * whether it is addressed by `handle` (from a prior $SETCLUEVT) or by `event`
 * (the CLUEVT$C_ code) -- both are accepted for source compatibility and both
 * reach the same executive disarm (event mask 0, astadr 0).
 *
 * Parameters:
 *   handle - optional quadword handle returned by $SETCLUEVT (accepted, not
 *            required -- the slot is single)
 *   acmode - access mode (accepted for source compatibility)
 *   event  - optional CLUEVT$C_ code to clear (accepted for source compat)
 *
 * Returns SS$_NORMAL on success, SS$_NOSUCHDEV when the connection manager is
 * not started, or the executive's status otherwise.
 */
uint32_t sys$clrcluevt(unsigned int *handle,
                       unsigned int acmode,
                       unsigned int event) {
    (void)handle;
    (void)acmode;
    (void)event;

    /* Disarm via the same re-arm-by-recall path the executive exposes. */
    return vms_kif_cluster_setcluevt(0u, 0u, 0u);
}
