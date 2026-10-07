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

/* pdsc$w_flags bit PDSC$V_BASE_REG_IS_FP (0x0080): frame base = FP(R29) vs
 * SP(R30). libgcc/config/alpha/vms-unwind.h spells the mask PDSC$M_BASE_REG_IS_FP
 * ("M" = mask); the internal src/libvms/include/pdscdef.h defines the same
 * 0x0080 mask (PDSC$V_BASE_REG_IS_FP is the bit NUMBER, 7). This shim provides the "M" alias the port
 * source references. */
#ifndef PDSC$M_BASE_REG_IS_FP
#define PDSC$M_BASE_REG_IS_FP    0x0080
#endif

/* Procedure Descriptor (PDSC$). The field-for-field layout MIRRORS the internal
 * src/libvms/include/pdscdef.h `struct pdsc_descriptor` exactly (same offsets):
 * that header is the vms-1fa invocation-context lane's oracle-tracked shape, so
 * matching it keeps the two spellings of the descriptor interchangeable when the
 * deferred runtime wiring reads real descriptors. vms-gcc_shell_handler.c reads
 * only pdsc$w_flags (offset 0, unchanged) plus a HARD-CODED handler_data offset
 * of 40 for stack frames - which lands exactly on pdsc$q_handler_data below.
 * vms-unwind.h additionally reads pdsc$w_rsa_offset, pdsc$l_size, pdsc$l_ireg_mask,
 * pdsc$q_entry, pdsc$b_save_ra and pdsc$b_save_fp. */
struct _pdscdef {
    uint16_t pdsc$w_flags;          /* off  0: kind (<3:0>) + flag bits      */
    union {                         /* off  2: (kind-dependent)              */
        uint16_t pdsc$w_rsa_offset; /*   stack frame: frame base -> RSA       */
        struct {                    /*   register frame:                      */
            uint8_t pdsc$b_save_fp; /*     register holding the caller's FP   */
            uint8_t pdsc$b_save_ra; /*     register holding the return addr   */
        };
    };
    uint8_t  pdsc$b_reserved;       /* off  4                                 */
    uint8_t  pdsc$b_type;           /* off  5: procedure type (low nibble)    */
    uint16_t pdsc$w_signature_offset; /* off 6                                */
    uint64_t pdsc$q_entry;          /* off  8: procedure entry code address   */
    uint32_t pdsc$l_size;           /* off 16: fixed frame size in bytes      */
    uint16_t pdsc$w_reserved;       /* off 20                                 */
    uint16_t pdsc$w_entry_length;   /* off 22: prologue length                */
    uint32_t pdsc$l_ireg_mask;      /* off 24: integer regs saved in the RSA (stack) */
    uint32_t pdsc$l_freg_mask;      /* off 28: FP regs saved in the RSA (stack)      */
    uint64_t pdsc$q_handler;        /* off 32: condition handler, if HANDLER_VALID   */
    uint64_t pdsc$q_handler_data;   /* off 40: handler data                          */
};
/* This is the descriptor the alpha-dec-vms back end actually emits (gas .pdesc;
 * observed in a cross-compiled object: flags 0x3089 = kind FP_STACK with
 * BASE_REG_IS_FP, rsa_offset 8 at +2, entry at +8, frame size at +16,
 * ireg_mask at +24) -- the OpenVMS Alpha Calling Standard PDSC. A layout that
 * disagrees mis-reads every real frame (vms-ed1), so it is pinned here. */
_Static_assert(__builtin_offsetof(struct _pdscdef, pdsc$q_entry) == 8, "PDSC entry at +8");
_Static_assert(__builtin_offsetof(struct _pdscdef, pdsc$l_size) == 16, "PDSC size at +16");
_Static_assert(__builtin_offsetof(struct _pdscdef, pdsc$l_ireg_mask) == 24, "PDSC ireg_mask at +24");
_Static_assert(__builtin_offsetof(struct _pdscdef, pdsc$q_handler) == 32, "PDSC handler at +32");

typedef struct _pdscdef PDSCDEF;

#ifdef __cplusplus
}
#endif

#endif /* __VMS_PDSCDEF_H */
