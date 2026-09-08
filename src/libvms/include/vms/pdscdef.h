/*
 * vms/pdscdef.h - port include-surface shim (vms-714c, R5).
 *
 * The alpha-dec-vms GCC port's own host sources (e.g.
 * libgcc/config/alpha/vms-gcc_shell_handler.c) `#include <vms/pdscdef.h>`
 * unchanged and dereference a `PDSCDEF *` (the Alpha Calling-Standard
 * procedure descriptor) via `pdsc$w_flags` and the PDSC$K_KIND_* constants.
 * See docs/design-gcc-port-surface-gaps-register.md S1.2 row R5.
 *
 * CLEAN-ROOM (Rule 8): reconstructed from the public "Alpha/OpenVMS Calling
 * Standard" ("Procedure Descriptor" chapter), never from VSI/HPE source - the
 * same source basis src/libvms/include/pdscdef.h already documents. The
 * PDSC$K_KIND_* values (8/9/10) match that header for cross-codebase
 * consistency.
 *
 * DELIBERATELY SEPARATE from src/libvms/include/pdscdef.h (owned by the
 * vms-1fa invocation-context lane, `struct pdsc_descriptor` there): this
 * shim exists only so the port's host sources resolve the VMS-convention
 * `<vms/pdscdef.h>` name and compile unchanged, and is not wired into
 * OVMX's own invocation-context walk.
 */
#ifndef __VMS_PDSCDEF_H
#define __VMS_PDSCDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PDSC$K_KIND_NULL         8
#define PDSC$K_KIND_FP_STACK     9
#define PDSC$K_KIND_FP_REGISTER  10

/* The essential Procedure Descriptor fields the port's condition-handler
 * shell (vms-gcc_shell_handler.c) reads: the kind/flags word, and (by
 * pointer arithmetic keyed off PDSC$K_KIND_*) the handler_data field. */
struct _pdscdef {
    uint16_t pdsc$w_flags;        /* kind (low nibble) + flag bits */
    uint16_t pdsc$w_rsa_offset;   /* frame-base -> register-save-area offset */
    uint32_t pdsc$l_size;         /* fixed stack-frame size (stack-kind) */
    uint64_t pdsc$q_entry;        /* procedure entry code address */
    uint64_t pdsc$q_handler;      /* established condition handler, if any */
    uint64_t pdsc$q_handler_data; /* handler_data field the shell handler reads */
};
typedef struct _pdscdef PDSCDEF;

#ifdef __cplusplus
}
#endif

#endif /* __VMS_PDSCDEF_H */
