/*
 * vms/libicb.h - port include-surface shim (vms-8e8c, CHF rung-5).
 *
 * libgcc/config/alpha/vms-unwind.h `#include <vms/libicb.h>` unchanged to reach
 * the LIB$ Invocation Context Block (INVO_CONTEXT_BLK) and the invocation-context
 * walk routines. OVMX already owns the ICB type in the flat, top-level
 * src/libvms/include/libicb.h (the vms-1fa lane); this shim is a plain redirect
 * so the VMS-convention `<vms/libicb.h>` include path resolves without a second
 * source of truth - the same posture vms/ssdef.h uses. See
 * docs/design-gcc-port-surface-gaps-register.md (R5) and
 * docs/design-chf-condition-handling.md (rung-5).
 */
#ifndef __VMS_LIBICB_H
#define __VMS_LIBICB_H

#include "../libicb.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Invocation-context walk routines, VMS-convention (uppercase) spelling.
 *
 * vms-unwind.h calls LIB$GET_INVO_HANDLE / LIB$GET_INVO_CONTEXT /
 * LIB$GET_PREV_INVO_CONTEXT to unwind across a condition-dispatcher frame.
 * OVMX implements these in src/libvms/rtl/lib_invo.c under the lowercase C
 * spellings (lib$get_invo_handle, ...); on a native VMS DEC C compile the
 * names are case-blind, so the port source references the uppercase form. The
 * prototypes below let the port source typecheck against our cross cc1.
 * COMPILE-SURFACE (vms-8e8c): the include-surface proof compiles vms-unwind.h
 * to assembly (-S) and does not link, so these declarations are what the
 * increment needs; binding the uppercase names to the lowercase definitions at
 * link time is part of the deferred EH-link runtime work.
 */
extern int LIB$GET_INVO_HANDLE(INVO_CONTEXT_BLK *icb);
extern int LIB$GET_INVO_CONTEXT(int invo_handle, INVO_CONTEXT_BLK *icb);
extern int LIB$GET_PREV_INVO_CONTEXT(INVO_CONTEXT_BLK *icb);

#ifdef __cplusplus
}
#endif

#endif /* __VMS_LIBICB_H */
